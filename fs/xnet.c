// Transparent forwarding of guest TCP connections to the app's SOCKS5 entry.
// The interface and the guarantees are in fs/xnet.h; this is how.
//
// A hooked connect() leaves the guest's own host socket (real_fd) alone and
// unconnected, opens a second host socket S to the entry, authenticates and
// sends CONNECT <destination> right away (loopback, so this takes well under
// a millisecond), and leaves the wait for the entry's answer to a host
// thread. An unconnected TCP socket is neither readable nor writable and
// never hangs up, on poll() and kqueue alike, so a guest waiting on it in
// poll/epoll/select just keeps waiting: nothing of the handshake can be
// mistaken for the connection being up, and no byte the guest writes can get
// mixed into it.
//
// When the answer comes, S takes real_fd's place with dup2(): same host fd
// number, so the fd table, the guest's flags and every reference to real_fd
// stay valid. dup2() drops the host poll registrations of the old socket, so
// every poll the fd is in is registered again (poll_refresh_real_fd) and
// woken. On success S is the connection; on failure S is shut down, which
// makes it readable, writable and hung up like a socket whose connect failed,
// and SO_ERROR, read and write report the mapped errno once, as Linux does.
//
// Only Darwin hosts (iOS, macOS) hook: on Linux an unconnected TCP socket
// polls as writable and hung up, which would end the guest's wait at once.
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include "kernel/calls.h"
#include "kernel/errno.h"
#include "fs/fd.h"
#include "fs/poll.h"
#include "fs/xnet.h"
#include "fs/xnet_internal.h"
#include "util/sync.h"
#include "debug.h"

#if defined(__APPLE__)
#define XNET_SUPPORTED 1
#else
#define XNET_SUPPORTED 0
#endif

// How long the entry may take to accept and to answer the greeting and the
// password (it is on the loopback; anything slower means it is wedged), and
// how long it may take to answer CONNECT, which is a dial to the destination
// (Darwin's own connect timeout is 75 s).
#define XNET_ENTRY_MS 2000
#define XNET_REPLY_MS 75000
// After the entry is found unreachable, connections go out directly (or
// fail, with fail_closed) for this long before it is tried again.
#define XNET_ENTRY_RETRY_MS 10000

atomic_bool xnet_on;

struct xprefix {
    uint8_t family; // AF_INET or AF_INET6
    uint8_t bits;
    uint8_t addr[16];
};

static lock_t xnet_lock = LOCK_INITIALIZER;
// Guarded by xnet_lock.
static struct {
    uint16_t port;
    char username[256];
    char password[256];
    bool append_id;
} entry;
static struct {
    uint64_t generation;
    struct xprefix *hook, *exempt, *self, *udp_hook, *resolvers;
    size_t hook_count, exempt_count, self_count, udp_hook_count, resolver_count;
    bool fail_closed;
    bool dns_intercept;
} routes;
static long entry_retry_at_ms; // breaker: entry unreachable until then
static bool entry_was_down;

static atomic_uint next_conn_id;

static struct {
    _Atomic uint64_t hooked, pass_no_match, pass_exempt, pass_local, pass_bound,
            pass_entry_down, pass_unconfigured, refused_closed, connected,
            fail_refused, fail_host_unreach, fail_net_unreach, fail_timeout,
            fail_denied, fail_other, fail_entry, cancelled, entry_down_events,
            pass_resolver, dns_tcp_hooked, udp_sent, udp_dns_sent, udp_received,
            udp_dropped, udp_pass_no_match, udp_pass_exempt, udp_pass_local,
            udp_pass_bound, udp_pass_proxy_route, udp_pass_v6only, udp_pass_entry_down,
            udp_refused_closed, udp_too_big, udp_assoc_opened, udp_assoc_failed,
            udp_assoc_renewed;
    atomic_uint pending, udp_assoc_active;
} stats;

enum { XC_SETUP, XC_PENDING, XC_CONNECTED, XC_FAILED };
// What connect() has told the guest, which is Linux's sock->state: only a
// connect() call moves a socket out of "connecting", so after a non-blocking
// connect completes the next connect() returns 0 (or the error) and only the
// one after that EISCONN (or starts over).
enum { GS_CONNECTING, GS_CONNECTED, GS_UNCONNECTED };

struct xnet_conn {
    lock_t lock;
    cond_t done;
    int state;
    int err;           // the guest errno (negative) once XC_FAILED
    bool err_reported; // SO_ERROR, connect, read or write has returned it
    int guest_state;   // GS_*
    unsigned id;
    int side_fd;       // S, until swapped in
    struct fd *fd;     // the waiting thread's reference
    int domain;        // of the guest socket (host AF_)
    struct sockaddr_storage dst; // as the guest asked for it, host form
    socklen_t dst_len;
    int family;        // of the destination; a v4-mapped one is AF_INET
    uint8_t addr[16];
    uint16_t port;     // host order
    bool have_self;
    uint8_t self[16];  // this node's tailnet address of the same family
    char text[64];     // the destination, for the log
    long start_ms;
};

static long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long) ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

static void wipe(void *p, size_t n) {
    volatile unsigned char *v = p;
    while (n--)
        *v++ = 0;
}

// --- logging ---------------------------------------------------------------

// Decisions to let a connection through are logged too (so a log shows
// whether a connection went out directly because of the snapshot or because
// it was never looked at), but a program that connects in a loop must not
// flood the log: at most XNET_PASS_LOG_BURST of them per 10 s.
#define XNET_PASS_LOG_BURST 40
static lock_t log_lock = LOCK_INITIALIZER;
static long pass_log_window;
static unsigned pass_log_count, pass_log_dropped;

static bool pass_log_allowed(void) {
    lock(&log_lock);
    long now = now_ms();
    if (now - pass_log_window >= 10000) {
        if (pass_log_dropped)
            printk("[Net][Xparent] %u pass lines not logged in the last 10s\n", pass_log_dropped);
        pass_log_window = now;
        pass_log_count = pass_log_dropped = 0;
    }
    bool ok = pass_log_count++ < XNET_PASS_LOG_BURST;
    if (!ok)
        pass_log_dropped++;
    unlock(&log_lock);
    return ok;
}

static void format_dst(char *out, size_t size, int family, const uint8_t *addr, uint16_t port) {
    char ip[INET6_ADDRSTRLEN] = "?";
    inet_ntop(family, addr, ip, sizeof(ip));
    snprintf(out, size, family == AF_INET6 ? "[%s]:%u" : "%s:%u", ip, port);
}

static const char *errno_name(int err) {
    switch (err) {
        case _ECONNREFUSED: return "ECONNREFUSED";
        case _EHOSTUNREACH: return "EHOSTUNREACH";
        case _ENETUNREACH: return "ENETUNREACH";
        case _ETIMEDOUT: return "ETIMEDOUT";
        case _EACCES: return "EACCES";
        default: return "E?";
    }
}

// --- prefixes --------------------------------------------------------------

static bool prefix_parse(const char *text, struct xprefix *p) {
    char buf[INET6_ADDRSTRLEN + 8];
    if (text == NULL || strlen(text) >= sizeof(buf))
        return false;
    strcpy(buf, text);
    char *slash = strchr(buf, '/');
    if (slash != NULL)
        *slash = '\0';
    memset(p, 0, sizeof(*p));
    int max;
    if (inet_pton(AF_INET, buf, p->addr) == 1) {
        p->family = AF_INET;
        max = 32;
    } else if (inet_pton(AF_INET6, buf, p->addr) == 1) {
        p->family = AF_INET6;
        max = 128;
    } else {
        return false;
    }
    int bits = max;
    if (slash != NULL) {
        char *end;
        long n = strtol(slash + 1, &end, 10);
        if (slash[1] == '\0' || *end != '\0' || n < 0 || n > max)
            return false;
        bits = (int) n;
    }
    p->bits = bits;
    // Clear the host part, so "10.1.1.7/24" means 10.1.1.0/24.
    for (int i = 0; i < 16; i++) {
        int keep = bits - i * 8;
        if (keep >= 8)
            continue;
        p->addr[i] &= keep <= 0 ? 0 : (uint8_t) (0xff << (8 - keep));
    }
    return true;
}

static bool prefix_covers(const struct xprefix *p, int family, const uint8_t *addr) {
    if (p->family != family)
        return false;
    int full = p->bits / 8, rest = p->bits % 8;
    if (memcmp(p->addr, addr, full) != 0)
        return false;
    if (rest == 0)
        return true;
    uint8_t mask = (uint8_t) (0xff << (8 - rest));
    return (p->addr[full] & mask) == (addr[full] & mask);
}

static bool any_covers(const struct xprefix *list, size_t count, int family, const uint8_t *addr) {
    for (size_t i = 0; i < count; i++)
        if (prefix_covers(&list[i], family, addr))
            return true;
    return false;
}

// Addresses that are never forwarded: they do not leave this host, or name
// no single host, or only mean something on the local link.
static bool addr_is_local(int family, const uint8_t *a) {
    if (family == AF_INET) {
        return a[0] == 127 || a[0] == 0 || (a[0] & 0xf0) == 0xe0 ||
                (a[0] == 255 && a[1] == 255 && a[2] == 255 && a[3] == 255) ||
                (a[0] == 169 && a[1] == 254);
    }
    static const uint8_t zero[16], one[16] = {[15] = 1};
    return memcmp(a, zero, 16) == 0 || memcmp(a, one, 16) == 0 ||
            a[0] == 0xff || (a[0] == 0xfe && (a[1] & 0xc0) == 0x80);
}

// --- the control API -------------------------------------------------------

int ish_xnet_configure(uint16_t entry_port, const char *username, const char *password,
        bool append_conn_id) {
    if (entry_port != 0 && password != NULL &&
            (username == NULL || strlen(username) > 255 || strlen(password) > 255))
        return -EINVAL;
    lock(&xnet_lock);
    wipe(entry.password, sizeof(entry.password));
    if (entry_port == 0 || password == NULL) {
        entry.port = 0;
    } else {
        entry.port = entry_port;
        strcpy(entry.username, username);
        strcpy(entry.password, password);
        entry.append_id = append_conn_id;
    }
    entry_retry_at_ms = 0;
    entry_was_down = false;
    bool configured = entry.port != 0;
    unlock(&xnet_lock);
    printk("[Net][Xparent] entry %s\n", configured ? "configured" : "cleared");
    return 0;
}

static int parse_list(const char *const *texts, size_t count, struct xprefix **out, const char *what) {
    *out = NULL;
    if (count == 0)
        return 0;
    if (texts == NULL)
        return -EINVAL;
    struct xprefix *list = calloc(count, sizeof(*list));
    if (list == NULL)
        return -ENOMEM;
    for (size_t i = 0; i < count; i++) {
        if (!prefix_parse(texts[i], &list[i])) {
            printk("[Net][Xparent] routes rejected: %s entry %zu does not parse\n", what, i);
            free(list);
            return -EINVAL;
        }
    }
    *out = list;
    return 0;
}

int ish_xnet_set_routes2(const struct ish_xnet_routes *r) {
    struct xprefix *h = NULL, *e = NULL, *s = NULL, *u = NULL, *d = NULL;
    int err = parse_list(r->hook, r->hook_count, &h, "hook");
    if (err == 0)
        err = parse_list(r->exempt, r->exempt_count, &e, "exempt");
    if (err == 0)
        err = parse_list(r->self_addrs, r->self_count, &s, "self");
    if (err == 0)
        err = parse_list(r->udp_hook, r->udp_hook_count, &u, "udp_hook");
    if (err == 0)
        err = parse_list(r->resolvers, r->resolver_count, &d, "resolver");
    if (err < 0) {
        free(h);
        free(e);
        free(s);
        free(u);
        return err;
    }
    lock(&xnet_lock);
    struct xprefix *old[] = {routes.hook, routes.exempt, routes.self, routes.udp_hook, routes.resolvers};
    routes.generation = r->generation;
    routes.hook = h;
    routes.hook_count = r->hook_count;
    routes.exempt = e;
    routes.exempt_count = r->exempt_count;
    routes.self = s;
    routes.self_count = r->self_count;
    routes.udp_hook = u;
    routes.udp_hook_count = r->udp_hook_count;
    routes.resolvers = d;
    routes.resolver_count = r->resolver_count;
    routes.dns_intercept = r->dns_intercept;
    routes.fail_closed = r->fail_closed;
    unlock(&xnet_lock);
    for (size_t i = 0; i < sizeof(old) / sizeof(old[0]); i++)
        free(old[i]);
    printk("[Net][Xparent] routes gen=%llu hook=%zu exempt=%zu udp_hook=%zu resolvers=%zu self=%zu"
            " dns_intercept=%d fail_closed=%d\n",
            (unsigned long long) r->generation, r->hook_count, r->exempt_count, r->udp_hook_count,
            r->resolver_count, r->self_count, r->dns_intercept, r->fail_closed);
    return 0;
}

int ish_xnet_set_routes(uint64_t generation,
        const char *const *hook, size_t hook_count,
        const char *const *exempt, size_t exempt_count,
        bool UNUSED(dns_intercept),
        const char *const *self_addrs, size_t self_count,
        bool fail_closed) {
    struct ish_xnet_routes r = {
        .generation = generation,
        .hook = hook, .hook_count = hook_count,
        .exempt = exempt, .exempt_count = exempt_count,
        .self_addrs = self_addrs, .self_count = self_count,
        .fail_closed = fail_closed,
    };
    return ish_xnet_set_routes2(&r);
}

void ish_xnet_set_enabled(bool enabled) {
    if (enabled && !XNET_SUPPORTED) {
        printk("[Net][Xparent] not supported on this host, staying off\n");
        return;
    }
    bool was = atomic_exchange(&xnet_on, enabled);
    if (was != enabled)
        printk("[Net][Xparent] %s\n", enabled ? "on" : "off");
}

bool ish_xnet_enabled(void) {
    return atomic_load(&xnet_on);
}

void ish_xnet_stats(struct ish_xnet_stats *out) {
    memset(out, 0, sizeof(*out));
    out->enabled = atomic_load(&xnet_on);
    out->supported = XNET_SUPPORTED;
    lock(&xnet_lock);
    out->configured = entry.port != 0;
    out->generation = routes.generation;
    out->hook_prefixes = (uint32_t) routes.hook_count;
    out->exempt_prefixes = (uint32_t) routes.exempt_count;
    out->udp_hook_prefixes = (uint32_t) routes.udp_hook_count;
    out->resolvers = (uint32_t) routes.resolver_count;
    out->dns_intercept = routes.dns_intercept;
    unlock(&xnet_lock);
    out->hooked = stats.hooked;
    out->pass_no_match = stats.pass_no_match;
    out->pass_exempt = stats.pass_exempt;
    out->pass_local = stats.pass_local;
    out->pass_bound = stats.pass_bound;
    out->pass_entry_down = stats.pass_entry_down;
    out->pass_unconfigured = stats.pass_unconfigured;
    out->refused_closed = stats.refused_closed;
    out->connected = stats.connected;
    out->fail_refused = stats.fail_refused;
    out->fail_host_unreach = stats.fail_host_unreach;
    out->fail_net_unreach = stats.fail_net_unreach;
    out->fail_timeout = stats.fail_timeout;
    out->fail_denied = stats.fail_denied;
    out->fail_other = stats.fail_other;
    out->fail_entry = stats.fail_entry;
    out->cancelled = stats.cancelled;
    out->entry_down_events = stats.entry_down_events;
    out->pending = stats.pending;
    out->pass_resolver = stats.pass_resolver;
    out->dns_tcp_hooked = stats.dns_tcp_hooked;
    out->udp_sent = stats.udp_sent;
    out->udp_dns_sent = stats.udp_dns_sent;
    out->udp_received = stats.udp_received;
    out->udp_dropped = stats.udp_dropped;
    out->udp_pass_no_match = stats.udp_pass_no_match;
    out->udp_pass_exempt = stats.udp_pass_exempt;
    out->udp_pass_local = stats.udp_pass_local;
    out->udp_pass_bound = stats.udp_pass_bound;
    out->udp_pass_proxy_route = stats.udp_pass_proxy_route;
    out->udp_pass_v6only = stats.udp_pass_v6only;
    out->udp_pass_entry_down = stats.udp_pass_entry_down;
    out->udp_refused_closed = stats.udp_refused_closed;
    out->udp_too_big = stats.udp_too_big;
    out->udp_assoc_opened = stats.udp_assoc_opened;
    out->udp_assoc_failed = stats.udp_assoc_failed;
    out->udp_assoc_renewed = stats.udp_assoc_renewed;
    out->udp_assoc_active = stats.udp_assoc_active;
}

size_t ish_xnet_describe(char *buf, size_t size) {
    struct ish_xnet_stats s;
    ish_xnet_stats(&s);
    int n = snprintf(buf, size,
            "enabled %d\nconfigured %d\nsupported %d\ngeneration %llu\n"
            "hook_prefixes %u\nexempt_prefixes %u\n"
            "hooked %llu\npass_no_match %llu\npass_exempt %llu\npass_local %llu\n"
            "pass_bound %llu\npass_entry_down %llu\npass_unconfigured %llu\nrefused_closed %llu\n"
            "connected %llu\nfail_refused %llu\nfail_host_unreach %llu\nfail_net_unreach %llu\n"
            "fail_timeout %llu\nfail_denied %llu\nfail_other %llu\nfail_entry %llu\n"
            "cancelled %llu\nentry_down_events %llu\npending %u\n",
            s.enabled, s.configured, s.supported, (unsigned long long) s.generation,
            s.hook_prefixes, s.exempt_prefixes,
            (unsigned long long) s.hooked, (unsigned long long) s.pass_no_match,
            (unsigned long long) s.pass_exempt, (unsigned long long) s.pass_local,
            (unsigned long long) s.pass_bound, (unsigned long long) s.pass_entry_down,
            (unsigned long long) s.pass_unconfigured, (unsigned long long) s.refused_closed,
            (unsigned long long) s.connected, (unsigned long long) s.fail_refused,
            (unsigned long long) s.fail_host_unreach, (unsigned long long) s.fail_net_unreach,
            (unsigned long long) s.fail_timeout, (unsigned long long) s.fail_denied,
            (unsigned long long) s.fail_other, (unsigned long long) s.fail_entry,
            (unsigned long long) s.cancelled, (unsigned long long) s.entry_down_events,
            s.pending);
    if (n < 0)
        return 0;
    if ((size_t) n < size) {
        int m = snprintf(buf + n, size - n,
                "dns_intercept %d\nudp_hook_prefixes %u\nresolvers %u\npass_resolver %llu\n"
                "dns_tcp_hooked %llu\nudp_sent %llu\nudp_dns_sent %llu\nudp_received %llu\n"
                "udp_dropped %llu\nudp_pass_no_match %llu\nudp_pass_exempt %llu\nudp_pass_local %llu\n"
                "udp_pass_bound %llu\nudp_pass_proxy_route %llu\nudp_pass_v6only %llu\n"
                "udp_pass_entry_down %llu\nudp_refused_closed %llu\nudp_too_big %llu\n"
                "udp_assoc_opened %llu\nudp_assoc_failed %llu\nudp_assoc_renewed %llu\n"
                "udp_assoc_active %u\n",
                s.dns_intercept, s.udp_hook_prefixes, s.resolvers,
                (unsigned long long) s.pass_resolver, (unsigned long long) s.dns_tcp_hooked,
                (unsigned long long) s.udp_sent, (unsigned long long) s.udp_dns_sent,
                (unsigned long long) s.udp_received, (unsigned long long) s.udp_dropped,
                (unsigned long long) s.udp_pass_no_match, (unsigned long long) s.udp_pass_exempt,
                (unsigned long long) s.udp_pass_local, (unsigned long long) s.udp_pass_bound,
                (unsigned long long) s.udp_pass_proxy_route, (unsigned long long) s.udp_pass_v6only,
                (unsigned long long) s.udp_pass_entry_down, (unsigned long long) s.udp_refused_closed,
                (unsigned long long) s.udp_too_big, (unsigned long long) s.udp_assoc_opened,
                (unsigned long long) s.udp_assoc_failed, (unsigned long long) s.udp_assoc_renewed,
                s.udp_assoc_active);
        if (m > 0)
            n += m;
    }
    return (size_t) n < size ? (size_t) n : size - 1;
}

// --- talking to the entry --------------------------------------------------

// Send or receive exactly len bytes on the non-blocking socket s by the
// deadline. Returns 0, or -1 with errno set (ETIMEDOUT at the deadline,
// ECONNRESET if the entry closed). *cancel, if given, is polled each second.
static int xfer(int s, void *buf, size_t len, bool sending, long deadline,
        bool (*cancel)(void *), void *cancel_arg) {
    char *p = buf;
    while (len > 0) {
        ssize_t n = sending ? send(s, p, len, 0) : recv(s, p, len, 0);
        if (n > 0) {
            p += n;
            len -= (size_t) n;
            continue;
        }
        if (n == 0) {
            errno = ECONNRESET;
            return -1;
        }
        if (errno == EINTR)
            continue;
        if (errno != EAGAIN && errno != EWOULDBLOCK)
            return -1;
        long left = deadline - now_ms();
        if (left <= 0) {
            errno = ETIMEDOUT;
            return -1;
        }
        if (cancel != NULL && cancel(cancel_arg)) {
            errno = ECANCELED;
            return -1;
        }
        struct pollfd pfd = {s, sending ? POLLOUT : POLLIN, 0};
        poll(&pfd, 1, left < 1000 ? (int) left : 1000);
    }
    return 0;
}

// Open S to the entry and authenticate. Returns S, or -1 with errno set and
// *stage naming the step that failed.
static int entry_open(int domain, uint16_t port, const char *user, const char *pass,
        const char **stage) {
    *stage = "socket";
    int s = socket(domain, SOCK_STREAM, 0);
    if (s < 0)
        return -1;
#ifdef SO_NOSIGPIPE
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
    fcntl(s, F_SETFL, fcntl(s, F_GETFL) | O_NONBLOCK);
    long deadline = now_ms() + XNET_ENTRY_MS;

    *stage = "entry-connect";
    int res;
    if (domain == AF_INET6) {
        struct sockaddr_in6 a = {.sin6_family = AF_INET6, .sin6_port = htons(port)};
        inet_pton(AF_INET6, "::ffff:127.0.0.1", &a.sin6_addr);
        res = connect(s, (void *) &a, sizeof(a));
    } else {
        struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(port),
                .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
        res = connect(s, (void *) &a, sizeof(a));
    }
    if (res < 0 && errno == EINPROGRESS) {
        struct pollfd pfd = {s, POLLOUT, 0};
        if (poll(&pfd, 1, XNET_ENTRY_MS) != 1) {
            errno = ETIMEDOUT;
            goto fail;
        }
        int so_error = 0;
        socklen_t so_len = sizeof(so_error);
        getsockopt(s, SOL_SOCKET, SO_ERROR, &so_error, &so_len);
        if (so_error != 0) {
            errno = so_error;
            goto fail;
        }
    } else if (res < 0) {
        goto fail;
    }

    // RFC 1928 greeting, offering username/password only; RFC 1929 auth.
    *stage = "entry-greeting";
    uint8_t hello[3] = {5, 1, 2}, choice[2];
    if (xfer(s, hello, 3, true, deadline, NULL, NULL) < 0 ||
            xfer(s, choice, 2, false, deadline, NULL, NULL) < 0)
        goto fail;
    if (choice[0] != 5 || choice[1] != 2) {
        errno = EPROTO;
        goto fail;
    }
    *stage = "entry-auth";
    size_t ulen = strlen(user), plen = strlen(pass);
    if (ulen > 255 || plen > 255) {
        errno = EINVAL;
        goto fail;
    }
    uint8_t auth[3 + 255 + 255];
    auth[0] = 1;
    auth[1] = (uint8_t) ulen;
    memcpy(auth + 2, user, ulen);
    auth[2 + ulen] = (uint8_t) plen;
    memcpy(auth + 3 + ulen, pass, plen);
    uint8_t verdict[2];
    res = xfer(s, auth, 3 + ulen + plen, true, deadline, NULL, NULL);
    wipe(auth, sizeof(auth));
    if (res < 0 || xfer(s, verdict, 2, false, deadline, NULL, NULL) < 0)
        goto fail;
    if (verdict[1] != 0) {
        errno = EACCES;
        goto fail;
    }
    return s;

fail:;
    int saved = errno;
    close(s);
    errno = saved;
    return -1;
}

static int send_connect_request(int s, struct xnet_conn *c) {
    uint8_t req[4 + 16 + 2] = {5, 1, 0, c->family == AF_INET ? 1 : 4};
    size_t alen = c->family == AF_INET ? 4 : 16;
    memcpy(req + 4, c->addr, alen);
    req[4 + alen] = c->port >> 8;
    req[5 + alen] = c->port & 0xff;
    return xfer(s, req, 6 + alen, true, now_ms() + XNET_ENTRY_MS, NULL, NULL);
}

// The SOCKS5 reply code to the errno Linux gives for the same failure.
static int reply_errno(int rep) {
    switch (rep) {
        case 0x02: stats.fail_denied++; return _EACCES;
        case 0x03: stats.fail_net_unreach++; return _ENETUNREACH;
        case 0x04: stats.fail_host_unreach++; return _EHOSTUNREACH;
        case 0x05: stats.fail_refused++; return _ECONNREFUSED;
        case 0x06: stats.fail_timeout++; return _ETIMEDOUT;
        default: stats.fail_other++; return _ECONNREFUSED;
    }
}

// --- putting S in place ----------------------------------------------------

// Socket options the guest may have set before connect(). Those that differ
// from a fresh socket's are carried over to S, so TCP_NODELAY, keepalive,
// timeouts, buffer sizes and the like survive the swap.
static const struct { int level, name; } carried_options[] = {
    {SOL_SOCKET, SO_KEEPALIVE}, {SOL_SOCKET, SO_LINGER}, {SOL_SOCKET, SO_RCVTIMEO},
    {SOL_SOCKET, SO_SNDTIMEO}, {SOL_SOCKET, SO_OOBINLINE}, {SOL_SOCKET, SO_RCVBUF},
    {SOL_SOCKET, SO_SNDBUF}, {SOL_SOCKET, SO_RCVLOWAT}, {SOL_SOCKET, SO_SNDLOWAT},
    {IPPROTO_TCP, TCP_NODELAY}, {IPPROTO_TCP, TCP_KEEPINTVL}, {IPPROTO_TCP, TCP_KEEPCNT},
    {IPPROTO_TCP, TCP_MAXSEG},
#if defined(__APPLE__)
    {IPPROTO_TCP, TCP_KEEPALIVE}, {IPPROTO_TCP, TCP_NOTSENT_LOWAT},
    {IPPROTO_TCP, TCP_CONNECTIONTIMEOUT}, {IPPROTO_TCP, TCP_RXT_CONNDROPTIME},
#else
    {IPPROTO_TCP, TCP_KEEPIDLE}, {IPPROTO_TCP, TCP_USER_TIMEOUT},
#endif
    {IPPROTO_IP, IP_TOS}, {IPPROTO_IPV6, IPV6_TCLASS},
};

static void carry_options(int from, int to, int domain) {
    int fresh = socket(domain, SOCK_STREAM, 0);
    if (fresh < 0)
        return;
    for (size_t i = 0; i < sizeof(carried_options) / sizeof(carried_options[0]); i++) {
        int level = carried_options[i].level, name = carried_options[i].name;
        if ((level == IPPROTO_IP && domain != AF_INET) || (level == IPPROTO_IPV6 && domain != AF_INET6))
            continue;
        char want[32] = {}, base[32] = {};
        socklen_t want_len = sizeof(want), base_len = sizeof(base);
        if (getsockopt(from, level, name, want, &want_len) < 0 ||
                getsockopt(fresh, level, name, base, &base_len) < 0)
            continue;
        if (want_len == base_len && memcmp(want, base, want_len) == 0)
            continue;
        setsockopt(to, level, name, want, want_len);
    }
    close(fresh);
    // S was made non-blocking for the handshake; give it the guest's mode.
    int flags = fcntl(from, F_GETFL);
    if (flags >= 0)
        fcntl(to, F_SETFL, flags);
}

// Called with c->lock held: make S the guest's socket.
static void swap_in(struct xnet_conn *c, struct fd *fd) {
    int s = c->side_fd;
    c->side_fd = -1;
    carry_options(fd->real_fd, s, c->domain);
    int fd_flags = fcntl(fd->real_fd, F_GETFD);
    if (dup2(s, fd->real_fd) < 0)
        printk("[Net][Xparent] x%x dup2 failed: %d\n", c->id, errno);
    if (fd_flags > 0)
        fcntl(fd->real_fd, F_SETFD, fd_flags);
    close(s);
}

static void finish(struct xnet_conn *c, struct fd *fd, int err) {
    lock(&c->lock);
    if (err != 0)
        shutdown(c->side_fd, SHUT_RDWR);
    swap_in(c, fd);
    c->err = err;
    c->state = err == 0 ? XC_CONNECTED : XC_FAILED;
    // From here on c belongs to the socket: a new connect() after a failure
    // may free it, so the waiting thread must not touch it again.
    c->fd = NULL;
    notify(&c->done);
    unlock(&c->lock);
    // The swapped-in socket has no host poll registrations yet.
    poll_refresh_real_fd(fd);
    poll_wakeup(fd, POLL_READ | POLL_WRITE | POLL_ERR | POLL_HUP);
}

static bool guest_gone(void *arg) {
    // Only the waiting thread's own reference is left: the guest closed it.
    return ((struct fd *) arg)->refcount <= 1;
}

static void *wait_for_reply(void *arg) {
    struct xnet_conn *c = arg;
    struct fd *fd = c->fd;
    long deadline = c->start_ms + XNET_REPLY_MS;
    uint8_t head[4];
    int err = 0;
    int res = xfer(c->side_fd, head, 4, false, deadline, guest_gone, fd);
    if (res == 0 && (head[0] != 5 || (head[3] != 1 && head[3] != 3 && head[3] != 4))) {
        errno = EPROTO;
        res = -1;
    }
    if (res == 0 && head[1] == 0) {
        // Skip the bound address so that the destination's first bytes are
        // left in the socket for the guest.
        uint8_t rest[2 + 255 + 2];
        size_t len = head[3] == 1 ? 4 + 2 : head[3] == 4 ? 16 + 2 : 0;
        if (head[3] == 3) {
            res = xfer(c->side_fd, rest, 1, false, deadline, guest_gone, fd);
            len = rest[0] + 2;
        }
        if (res == 0)
            res = xfer(c->side_fd, rest, len, false, deadline, guest_gone, fd);
    }
    long ms = now_ms() - c->start_ms;
    int saved_errno = errno;
    stats.pending--;
    errno = saved_errno;

    if (res < 0 && errno == ECANCELED) {
        stats.cancelled++;
        printk("[Net][Xparent] x%x cancelled dst=%s: socket closed after %ldms\n", c->id, c->text, ms);
        lock(&c->lock);
        close(c->side_fd);
        c->side_fd = -1;
        c->state = XC_FAILED;
        c->err = _ECONNABORTED;
        c->fd = NULL;
        unlock(&c->lock);
    } else {
        if (res < 0) {
            if (errno == ETIMEDOUT) {
                stats.fail_timeout++;
                err = _ETIMEDOUT;
            } else {
                stats.fail_entry++;
                err = _ECONNREFUSED;
            }
            printk("[Net][Xparent] x%x FAIL stage=reply-io dst=%s err=%d -> %s after %ldms\n",
                    c->id, c->text, errno, errno_name(err), ms);
        } else if (head[1] != 0) {
            err = reply_errno(head[1]);
            printk("[Net][Xparent] x%x FAIL stage=reply dst=%s code=%#x -> %s after %ldms\n",
                    c->id, c->text, head[1], errno_name(err), ms);
        } else {
            stats.connected++;
            printk("[Net][Xparent] x%x connected dst=%s after %ldms\n", c->id, c->text, ms);
        }
        finish(c, fd, err);
    }
    fd_close(fd);
    return NULL;
}

// --- the sock.c side -------------------------------------------------------

static struct xnet_conn *conn_new(struct fd *sock, const struct sockaddr *addr, socklen_t len) {
    struct xnet_conn *c = calloc(1, sizeof(*c));
    if (c == NULL)
        return NULL;
    lock_init(&c->lock);
    cond_init(&c->done);
    c->side_fd = -1;
    c->domain = sock->socket.domain == AF_INET6_ ? AF_INET6 : AF_INET;
    memcpy(&c->dst, addr, len < sizeof(c->dst) ? len : sizeof(c->dst));
    c->dst_len = len;
    if (addr->sa_family == AF_INET) {
        const struct sockaddr_in *in = (const void *) addr;
        c->family = AF_INET;
        memcpy(c->addr, &in->sin_addr, 4);
        c->port = ntohs(in->sin_port);
    } else {
        const struct sockaddr_in6 *in6 = (const void *) addr;
        c->port = ntohs(in6->sin6_port);
        if (IN6_IS_ADDR_V4MAPPED(&in6->sin6_addr)) {
            c->family = AF_INET;
            memcpy(c->addr, &in6->sin6_addr.s6_addr[12], 4);
        } else {
            c->family = AF_INET6;
            memcpy(c->addr, &in6->sin6_addr, 16);
        }
    }
    format_dst(c->text, sizeof(c->text), c->family, c->addr, c->port);
    return c;
}

static void conn_free(struct xnet_conn *c) {
    if (c->side_fd >= 0)
        close(c->side_fd);
    cond_destroy(&c->done);
    free(c);
}

// Whether the guest tied the socket to an interface (SO_BINDTODEVICE) or to
// one of its addresses: then it chose the path, and it is not ours to change.
static bool socket_is_bound(int real_fd, int domain) {
#if defined(__APPLE__)
    int ifindex = 0;
    socklen_t len = sizeof(ifindex);
    if (domain == AF_INET6) {
        if (getsockopt(real_fd, IPPROTO_IPV6, IPV6_BOUND_IF, &ifindex, &len) == 0 && ifindex != 0)
            return true;
    } else if (getsockopt(real_fd, IPPROTO_IP, IP_BOUND_IF, &ifindex, &len) == 0 && ifindex != 0) {
        return true;
    }
#endif
    struct sockaddr_storage local;
    socklen_t local_len = sizeof(local);
    if (getsockname(real_fd, (void *) &local, &local_len) < 0)
        return false;
    if (local.ss_family == AF_INET)
        return ((struct sockaddr_in *) &local)->sin_addr.s_addr != htonl(INADDR_ANY);
    if (local.ss_family == AF_INET6)
        return !IN6_IS_ADDR_UNSPECIFIED(&((struct sockaddr_in6 *) &local)->sin6_addr);
    return false;
}

static void log_pass(const char *reason, const struct xnet_conn *c) {
    if (pass_log_allowed())
        printk("[Net][Xparent] pass reason=%s pid=%d comm=%.16s dst=%s\n",
                reason, current->pid, current->comm, c->text);
}

static bool is_nonblocking(struct fd *sock) {
    int flags = fcntl(sock->real_fd, F_GETFL);
    return flags >= 0 && (flags & O_NONBLOCK);
}

// Wait, as a blocking connect() does, for the entry's answer. The time is
// bounded by SO_SNDTIMEO, as on Linux, after which it is EINPROGRESS.
// Called and returns with c->lock held.
static int wait_settled(struct xnet_conn *c, int real_fd) {
    struct timeval tv = {};
    socklen_t tv_len = sizeof(tv);
    getsockopt(real_fd, SOL_SOCKET, SO_SNDTIMEO, &tv, &tv_len);
    long limit = (long) tv.tv_sec * 1000 + tv.tv_usec / 1000;
    long deadline = limit > 0 ? now_ms() + limit : 0;
    while (c->state == XC_SETUP || c->state == XC_PENDING) {
        long slice = 1000;
        if (deadline != 0) {
            long left = deadline - now_ms();
            if (left <= 0)
                return _EINPROGRESS;
            if (left < slice)
                slice = left;
        }
        struct timespec ts = {slice / 1000, (slice % 1000) * 1000000};
        if (wait_for(&c->done, &c->lock, &ts) == _EINTR)
            return _EINTR;
    }
    return 0;
}

// The result of connect() once the answer is in. Linux reports a failure
// once; a connect() after SO_ERROR (or a read) took it is ECONNABORTED.
static int settled_result(struct xnet_conn *c) {
    if (c->state == XC_CONNECTED) {
        c->guest_state = GS_CONNECTED;
        return 0;
    }
    c->guest_state = GS_UNCONNECTED;
    int err = c->err_reported ? _ECONNABORTED : c->err;
    c->err_reported = true;
    return err;
}

int xnet_connect(struct fd *sock, const struct sockaddr *addr, socklen_t len) {
    if (addr->sa_family != AF_INET && addr->sa_family != AF_INET6)
        return XNET_PASS;
    struct xnet_conn *c = conn_new(sock, addr, len);
    if (c == NULL)
        return XNET_PASS;

    if (addr_is_local(c->family, c->addr)) {
        stats.pass_local++;
        conn_free(c);
        return XNET_PASS; // not logged: the loopback is busy
    }

    lock(&xnet_lock);
    const char *reason = NULL;
    bool fail_closed = routes.fail_closed;
    // Port 53 of the guest's own nameserver is DNS, whatever the hook lists
    // say: intercepted as such, or left alone.
    bool dns = c->port == 53 && any_covers(routes.resolvers, routes.resolver_count, c->family, c->addr);
    if (entry.port == 0 || (routes.hook_count == 0 && !(dns && routes.dns_intercept))) {
        stats.pass_unconfigured++;
        reason = "unconfigured";
    } else if (dns && !routes.dns_intercept) {
        stats.pass_resolver++;
        reason = "resolver";
    } else if (dns) {
        if (now_ms() < entry_retry_at_ms)
            reason = "entry-down";
    } else if (!any_covers(routes.hook, routes.hook_count, c->family, c->addr)) {
        stats.pass_no_match++;
        reason = "no-match";
    } else if (any_covers(routes.exempt, routes.exempt_count, c->family, c->addr)) {
        stats.pass_exempt++;
        reason = "exempt";
    } else if (now_ms() < entry_retry_at_ms) {
        reason = "entry-down";
    }
    uint16_t port = entry.port;
    char user[256 + 16], pass[256];
    c->id = ++next_conn_id;
    if (reason == NULL) {
        if (entry.append_id)
            snprintf(user, sizeof(user), "%s~x%x%s", entry.username, c->id, dns ? "~dns" : "");
        else
            snprintf(user, sizeof(user), "%s%s", entry.username, dns ? "~dns" : "");
        strcpy(pass, entry.password);
        for (size_t i = 0; i < routes.self_count; i++) {
            const struct xprefix *p = &routes.self[i];
            if (p->family == c->family) {
                memcpy(c->self, p->addr, sizeof(c->self));
                c->have_self = true;
                break;
            }
        }
    }
    uint64_t generation = routes.generation;
    unlock(&xnet_lock);

    if (reason != NULL && strcmp(reason, "entry-down") == 0) {
        wipe(pass, sizeof(pass));
        if (fail_closed) {
            stats.refused_closed++;
            log_pass("entry-down(fail-closed: ENETUNREACH)", c);
            conn_free(c);
            return _ENETUNREACH;
        }
        stats.pass_entry_down++;
    }
    if (reason != NULL) {
        log_pass(reason, c);
        conn_free(c);
        return XNET_PASS;
    }
    if (socket_is_bound(sock->real_fd, c->domain)) {
        wipe(pass, sizeof(pass));
        stats.pass_bound++;
        log_pass("bound", c);
        conn_free(c);
        return XNET_PASS;
    }

    // Claim the socket before the handshake, so a second thread's connect()
    // sees a connection in progress, as on Linux.
    c->state = XC_SETUP;
    struct xnet_conn *expected = NULL;
    if (!__atomic_compare_exchange_n(&sock->socket.xnet, &expected, c, false,
                __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        wipe(pass, sizeof(pass));
        conn_free(c);
        return xnet_reconnect(sock);
    }

    c->start_ms = now_ms();
    const char *stage;
    int s = entry_open(c->domain, port, user, pass, &stage);
    wipe(pass, sizeof(pass));
    if (s >= 0 && send_connect_request(s, c) < 0) {
        stage = "request";
        int saved = errno;
        close(s);
        errno = saved;
        s = -1;
    }
    if (s < 0) {
        // The entry is not there (or not taking our password): leave it be
        // for a while and let this connection, and the next ones, go out
        // directly - or fail, if the user asked for that.
        int saved = errno;
        stats.entry_down_events++;
        lock(&xnet_lock);
        entry_retry_at_ms = now_ms() + XNET_ENTRY_RETRY_MS;
        entry_was_down = true;
        unlock(&xnet_lock);
        printk("[Net][Xparent] x%x entry unusable stage=%s err=%d; %s for %ds\n", c->id, stage, saved,
                fail_closed ? "refusing hooked connections" : "going direct", XNET_ENTRY_RETRY_MS / 1000);
        __atomic_store_n(&sock->socket.xnet, NULL, __ATOMIC_RELEASE);
        if (fail_closed) {
            stats.refused_closed++;
            conn_free(c);
            return _ENETUNREACH;
        }
        stats.pass_entry_down++;
        log_pass("entry-down", c);
        conn_free(c);
        return XNET_PASS;
    }
    lock(&xnet_lock);
    if (entry_was_down) {
        entry_was_down = false;
        printk("[Net][Xparent] entry reachable again\n");
    }
    unlock(&xnet_lock);

    stats.hooked++;
    if (dns)
        stats.dns_tcp_hooked++;
    stats.pending++;
    printk("[Net][Xparent] x%x hook%s pid=%d comm=%.16s dst=%s gen=%llu\n",
            c->id, dns ? " dns" : "", current->pid, current->comm, c->text, (unsigned long long) generation);
    bool nonblock = is_nonblocking(sock);
    lock(&c->lock);
    c->side_fd = s;
    c->state = XC_PENDING;
    c->fd = fd_retain(sock);
    unlock(&c->lock);

    pthread_t thread;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&thread, &attr, wait_for_reply, c) != 0) {
        // No thread to wait with: wait right here, uninterruptibly. Rare
        // enough that it is not worth more.
        pthread_attr_destroy(&attr);
        wait_for_reply(c);
        lock(&c->lock);
        int res = settled_result(c);
        unlock(&c->lock);
        return res;
    }
    pthread_attr_destroy(&attr);

    if (nonblock)
        return _EINPROGRESS;
    lock(&c->lock);
    int res = wait_settled(c, sock->real_fd);
    if (res == 0)
        res = settled_result(c);
    unlock(&c->lock);
    return res;
}

// Put a fresh unconnected socket in place of a failed one, for a new
// connect() after the failure was reported. Linux allows that; the host
// socket, shut down, would refuse it.
static void reset_socket(struct fd *sock, struct xnet_conn *c) {
    int fresh = socket(c->domain, SOCK_STREAM, 0);
    if (fresh < 0)
        return;
    carry_options(sock->real_fd, fresh, c->domain);
    int fd_flags = fcntl(sock->real_fd, F_GETFD);
    dup2(fresh, sock->real_fd);
    if (fd_flags > 0)
        fcntl(sock->real_fd, F_SETFD, fd_flags);
    close(fresh);
    poll_refresh_real_fd(sock);
}

int xnet_reconnect(struct fd *sock) {
    struct xnet_conn *c = __atomic_load_n(&sock->socket.xnet, __ATOMIC_ACQUIRE);
    lock(&c->lock);
    int res;
    if (c->guest_state == GS_CONNECTED) {
        res = _EISCONN;
    } else if (c->guest_state == GS_UNCONNECTED) {
        // A new attempt after the failure was reported. The waiting thread
        // let go of c when it settled, so c can go.
        unlock(&c->lock);
        __atomic_store_n(&sock->socket.xnet, NULL, __ATOMIC_RELEASE);
        reset_socket(sock, c);
        conn_free(c);
        return XNET_PASS;
    } else if ((c->state == XC_SETUP || c->state == XC_PENDING) && is_nonblocking(sock)) {
        res = _EALREADY;
    } else {
        // Linux: a blocking socket waits for the outcome again.
        res = wait_settled(c, sock->real_fd);
        if (res == 0)
            res = settled_result(c);
        else if (res == _EINPROGRESS)
            res = _EALREADY;
    }
    unlock(&c->lock);
    return res;
}

int xnet_io_gate(struct fd *sock, bool dontwait) {
    struct xnet_conn *c = __atomic_load_n(&sock->socket.xnet, __ATOMIC_ACQUIRE);
    lock(&c->lock);
    int res = 0;
    if (c->state == XC_SETUP || c->state == XC_PENDING) {
        if (dontwait || is_nonblocking(sock)) {
            res = _EAGAIN;
        } else {
            // A blocking read or write before the connection is up waits for
            // it, with no time limit of its own (SO_RCVTIMEO and SO_SNDTIMEO
            // apply once the host call runs).
            while (c->state == XC_SETUP || c->state == XC_PENDING) {
                struct timespec ts = {1, 0};
                if (wait_for(&c->done, &c->lock, &ts) == _EINTR) {
                    res = _EINTR;
                    break;
                }
            }
        }
    }
    if (res == 0 && c->state == XC_FAILED && !c->err_reported) {
        c->err_reported = true;
        res = c->err;
    }
    unlock(&c->lock);
    return res;
}

bool xnet_so_error(struct fd *sock, int *err) {
    struct xnet_conn *c = __atomic_load_n(&sock->socket.xnet, __ATOMIC_ACQUIRE);
    lock(&c->lock);
    bool handled = true;
    if (c->state == XC_SETUP || c->state == XC_PENDING) {
        *err = 0;
    } else if (c->state == XC_FAILED && !c->err_reported) {
        c->err_reported = true;
        *err = c->err;
    } else {
        handled = false;
    }
    unlock(&c->lock);
    return handled;
}

int xnet_getpeername(struct fd *sock, struct sockaddr_storage *out, socklen_t *len) {
    struct xnet_conn *c = __atomic_load_n(&sock->socket.xnet, __ATOMIC_ACQUIRE);
    lock(&c->lock);
    int res = _ENOTCONN;
    if (c->state == XC_CONNECTED) {
        memcpy(out, &c->dst, sizeof(c->dst));
        *len = c->dst_len;
        res = 0;
    }
    unlock(&c->lock);
    return res;
}

int xnet_getsockname(struct fd *sock, struct sockaddr_storage *out, socklen_t *len) {
    struct xnet_conn *c = __atomic_load_n(&sock->socket.xnet, __ATOMIC_ACQUIRE);
    lock(&c->lock);
    bool connected = c->state == XC_CONNECTED;
    unlock(&c->lock);
    if (!connected)
        return XNET_PASS;
    // The port is S's own local port; the address is this node's tailnet
    // address, as if the connection left through the tailnet interface.
    struct sockaddr_storage host;
    socklen_t host_len = sizeof(host);
    uint16_t port = 0;
    if (getsockname(sock->real_fd, (void *) &host, &host_len) == 0)
        port = host.ss_family == AF_INET6 ? ((struct sockaddr_in6 *) &host)->sin6_port
                : ((struct sockaddr_in *) &host)->sin_port;
    memset(out, 0, sizeof(*out));
    if (c->domain == AF_INET) {
        struct sockaddr_in *in = (void *) out;
        in->sin_family = AF_INET;
        in->sin_port = port;
        if (c->have_self)
            memcpy(&in->sin_addr, c->self, 4);
        *len = sizeof(*in);
    } else {
        struct sockaddr_in6 *in6 = (void *) out;
        in6->sin6_family = AF_INET6;
        in6->sin6_port = port;
        if (c->have_self && c->family == AF_INET) {
            in6->sin6_addr.s6_addr[10] = in6->sin6_addr.s6_addr[11] = 0xff;
            memcpy(&in6->sin6_addr.s6_addr[12], c->self, 4);
        } else if (c->have_self) {
            memcpy(&in6->sin6_addr, c->self, 16);
        }
        *len = sizeof(*in6);
    }
    return 0;
}

void xnet_release(struct fd *sock) {
    struct xnet_conn *c = sock->socket.xnet;
    sock->socket.xnet = NULL;
    // The waiting thread holds a reference, so it is done by now.
    conn_free(c);
}

// --- UDP ---------------------------------------------------------------------
//
// A datagram socket keeps its own host socket for everything. The first time
// it sends to a hooked destination, it gets a struct xnet_udp and, per kind
// (ordinary or DNS), an association: a hidden TCP connection to the entry
// that asked for UDP ASSOCIATE, and the relay port the entry answered with.
// Datagrams to hooked destinations are wrapped in the RFC 1928 UDP header
// and sent to that relay (127.0.0.1) from the guest's own socket; datagrams
// from the relay are unwrapped, and the guest sees the real source. Nothing
// else about the socket changes, so poll/epoll/select see the same host fd.
//
// A socket that never sends to a hooked destination never gets the struct,
// and its datagrams take exactly the host calls they took before.

#define XNET_UDP_HDR_MAX 22 // RSV RSV FRAG ATYP, an IPv6 address, the port
#define XNET_UDP_MAX 65507  // the largest UDP payload over IPv4

enum { XA_MAIN, XA_DNS, XA_KINDS };
static const char *const assoc_kind[XA_KINDS] = {"udp", "dns"};

struct xdst {
    int family; // AF_INET (also for a v4-mapped address) or AF_INET6
    uint8_t addr[16];
    uint16_t port; // host order
};

struct xassoc {
    int ctl;            // the control connection, -1 when there is none
    uint16_t relay;     // the relay's port on 127.0.0.1 (host order), 0 when none
    uint16_t old_relay; // the one before: its late datagrams are dropped
    unsigned id;
};

struct xnet_udp {
    lock_t lock;
    int domain; // of the guest socket (host AF_)
    struct xassoc assoc[XA_KINDS];
    // connect() to a hooked destination. The host socket stays unconnected,
    // or the relay's datagrams would not reach it; iSH keeps the peer and
    // filters what is received, as Linux does for a connected socket.
    bool connected;
    struct sockaddr_storage peer; // host form, as the guest gave it
    socklen_t peer_len;
    struct xdst peer_dst;
    char last_logged[64]; // destination of the last "udp dst" line
};

// Associations that could not be made for a reason other than the entry
// being unreachable (an entry without UDP, say): UDP goes out directly (or
// fails with fail_closed) until then, TCP is not affected.
static long udp_retry_at_ms;

static bool dst_from_sockaddr(const void *sa_, socklen_t len, struct xdst *d) {
    const struct sockaddr *sa = sa_;
    if (sa == NULL || len < sizeof(sa->sa_family))
        return false;
    memset(d, 0, sizeof(*d));
    if (sa->sa_family == AF_INET && len >= sizeof(struct sockaddr_in)) {
        const struct sockaddr_in *in = sa_;
        d->family = AF_INET;
        memcpy(d->addr, &in->sin_addr, 4);
        d->port = ntohs(in->sin_port);
        return true;
    }
    if (sa->sa_family == AF_INET6 && len >= sizeof(struct sockaddr_in6)) {
        const struct sockaddr_in6 *in6 = sa_;
        d->port = ntohs(in6->sin6_port);
        if (IN6_IS_ADDR_V4MAPPED(&in6->sin6_addr)) {
            d->family = AF_INET;
            memcpy(d->addr, &in6->sin6_addr.s6_addr[12], 4);
        } else {
            d->family = AF_INET6;
            memcpy(d->addr, &in6->sin6_addr, 16);
        }
        return true;
    }
    return false;
}

static bool dst_equal(const struct xdst *a, const struct xdst *b) {
    return a->family == b->family && a->port == b->port &&
            memcmp(a->addr, b->addr, a->family == AF_INET ? 4 : 16) == 0;
}

// The sockaddr a guest socket of `domain` sees for d (v4-mapped on IPv6).
static socklen_t dst_to_sockaddr(const struct xdst *d, int domain, struct sockaddr_storage *out) {
    memset(out, 0, sizeof(*out));
    if (domain == AF_INET && d->family == AF_INET) {
        struct sockaddr_in *in = (void *) out;
        in->sin_family = AF_INET;
        in->sin_port = htons(d->port);
        memcpy(&in->sin_addr, d->addr, 4);
#if defined(__APPLE__)
        in->sin_len = sizeof(*in);
#endif
        return sizeof(*in);
    }
    struct sockaddr_in6 *in6 = (void *) out;
    in6->sin6_family = AF_INET6;
    in6->sin6_port = htons(d->port);
    if (d->family == AF_INET) {
        in6->sin6_addr.s6_addr[10] = in6->sin6_addr.s6_addr[11] = 0xff;
        memcpy(&in6->sin6_addr.s6_addr[12], d->addr, 4);
    } else {
        memcpy(&in6->sin6_addr, d->addr, 16);
    }
#if defined(__APPLE__)
    in6->sin6_len = sizeof(*in6);
#endif
    return sizeof(*in6);
}

static bool is_v6only(int real_fd) {
    int v = 0;
    socklen_t len = sizeof(v);
    return getsockopt(real_fd, IPPROTO_IPV6, IPV6_V6ONLY, &v, &len) == 0 && v;
}

static void log_udp(const char *what, const struct xdst *d) {
    if (!pass_log_allowed())
        return;
    char text[64];
    format_dst(text, sizeof(text), d->family, d->addr, d->port);
    printk("[Net][Xparent] udp %s pid=%d comm=%.16s dst=%s\n", what, current->pid, current->comm, text);
}

// Non-zero: assoc_ensure returns 0 for "the association is up".
enum { UR_DIRECT = 1, UR_MAIN, UR_DNS, UR_REFUSE };

// Where a datagram to d goes. Counts the decision; *reason is set for the
// ones worth a log line.
static int udp_decide(int real_fd, int domain, const struct xdst *d, const char **reason) {
    *reason = NULL;
    if (addr_is_local(d->family, d->addr)) {
        stats.udp_pass_local++;
        return UR_DIRECT;
    }
    int route = UR_DIRECT;
    lock(&xnet_lock);
    bool dns = d->port == 53 && any_covers(routes.resolvers, routes.resolver_count, d->family, d->addr);
    if (entry.port == 0) {
        // unconfigured: not counted per datagram
    } else if (dns && !routes.dns_intercept) {
        stats.pass_resolver++;
    } else if (dns) {
        route = UR_DNS;
    } else if (!any_covers(routes.udp_hook, routes.udp_hook_count, d->family, d->addr)) {
        if (any_covers(routes.hook, routes.hook_count, d->family, d->addr) &&
                !any_covers(routes.exempt, routes.exempt_count, d->family, d->addr)) {
            stats.udp_pass_proxy_route++;
            *reason = "udp-direct(proxy-route)";
        } else {
            stats.udp_pass_no_match++;
        }
    } else if (any_covers(routes.exempt, routes.exempt_count, d->family, d->addr)) {
        stats.udp_pass_exempt++;
        *reason = "pass reason=exempt";
    } else {
        route = UR_MAIN;
    }
    if (route != UR_DIRECT) {
        long now = now_ms();
        if (now < entry_retry_at_ms || now < udp_retry_at_ms) {
            if (routes.fail_closed) {
                stats.udp_refused_closed++;
                *reason = "pass reason=entry-down(fail-closed: ENETUNREACH)";
                route = UR_REFUSE;
            } else {
                stats.udp_pass_entry_down++;
                *reason = "pass reason=entry-down";
                route = UR_DIRECT;
            }
        }
    }
    unlock(&xnet_lock);
    if (route == UR_MAIN || route == UR_DNS) {
        if (socket_is_bound(real_fd, domain)) {
            stats.udp_pass_bound++;
            *reason = "pass reason=bound";
            return UR_DIRECT;
        }
        if (domain == AF_INET6 && is_v6only(real_fd)) {
            stats.udp_pass_v6only++;
            *reason = "pass reason=v6only";
            return UR_DIRECT;
        }
    }
    return route;
}

static struct xnet_udp *udp_attach(struct fd *sock) {
    struct xnet_udp *u = __atomic_load_n(&sock->socket.xudp, __ATOMIC_ACQUIRE);
    if (u != NULL)
        return u;
    u = calloc(1, sizeof(*u));
    if (u == NULL)
        return NULL;
    lock_init(&u->lock);
    u->domain = sock->socket.domain == AF_INET6_ ? AF_INET6 : AF_INET;
    for (int k = 0; k < XA_KINDS; k++)
        u->assoc[k].ctl = -1;
    struct xnet_udp *expected = NULL;
    if (!__atomic_compare_exchange_n(&sock->socket.xudp, &expected, u, false,
                __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        free(u);
        return expected;
    }
    return u;
}

// The guest socket's local port, binding it first if it has none: Linux
// binds an unbound socket on its first send or connect() anyway.
static int udp_local_port(int real_fd, int domain) {
    for (int attempt = 0; attempt < 2; attempt++) {
        struct sockaddr_storage a;
        socklen_t len = sizeof(a);
        if (getsockname(real_fd, (void *) &a, &len) < 0)
            return -1;
        uint16_t port = a.ss_family == AF_INET6 ? ((struct sockaddr_in6 *) &a)->sin6_port
                : ((struct sockaddr_in *) &a)->sin_port;
        if (port != 0)
            return ntohs(port);
        struct xdst any = {.family = domain};
        struct sockaddr_storage w;
        socklen_t wlen = dst_to_sockaddr(&any, domain, &w);
        if (domain == AF_INET6)
            ((struct sockaddr_in6 *) &w)->sin6_addr = in6addr_any;
        if (bind(real_fd, (void *) &w, wlen) < 0)
            return -1;
    }
    errno = EADDRNOTAVAIL;
    return -1;
}

// Open an association of `kind` (u->lock held). Returns 0, or -1 with errno,
// *stage and *code (the entry's reply code, or 0) set.
static int assoc_open(struct xnet_udp *u, int real_fd, int kind, const char **stage, int *code) {
    struct xassoc *a = &u->assoc[kind];
    *code = 0;
    *stage = "bind";
    int lport = udp_local_port(real_fd, u->domain);
    if (lport <= 0)
        return -1;
    char user[256 + 16], pass[256];
    lock(&xnet_lock);
    uint16_t port = entry.port;
    unsigned id = ++next_conn_id;
    if (entry.append_id)
        snprintf(user, sizeof(user), "%s~x%x%s", entry.username, id, kind == XA_DNS ? "~dns" : "");
    else
        snprintf(user, sizeof(user), "%s%s", entry.username, kind == XA_DNS ? "~dns" : "");
    strcpy(pass, entry.password);
    unlock(&xnet_lock);
    if (port == 0) {
        wipe(pass, sizeof(pass));
        *stage = "unconfigured";
        errno = ENOTCONN;
        return -1;
    }
    int s = entry_open(AF_INET, port, user, pass, stage);
    wipe(pass, sizeof(pass));
    if (s < 0)
        return -1;
    fcntl(s, F_SETFD, FD_CLOEXEC);

    // UDP ASSOCIATE, declaring where the datagrams come from: the guest
    // socket's port on the loopback (the entry takes no others).
    *stage = "associate";
    long deadline = now_ms() + XNET_ENTRY_MS;
    uint8_t req[10] = {5, 3, 0, 1, 127, 0, 0, 1, (uint8_t) (lport >> 8), (uint8_t) lport};
    uint8_t head[4], addr[16], pb[2];
    if (xfer(s, req, sizeof(req), true, deadline, NULL, NULL) < 0 ||
            xfer(s, head, 4, false, deadline, NULL, NULL) < 0)
        goto fail;
    if (head[0] != 5 || head[1] != 0) {
        *code = head[1];
        errno = head[1] == 0x02 ? EACCES : EPROTO;
        goto fail;
    }
    size_t alen = head[3] == 1 ? 4 : head[3] == 4 ? 16 : 0;
    if (alen == 0) {
        errno = EPROTO;
        goto fail;
    }
    if (xfer(s, addr, alen, false, deadline, NULL, NULL) < 0 ||
            xfer(s, pb, 2, false, deadline, NULL, NULL) < 0)
        goto fail;
    // The relay must be on the IPv4 loopback (or "the address you reached
    // me at", 0.0.0.0), where nothing but the entry can have the port.
    static const uint8_t lo[4] = {127, 0, 0, 1}, unspec[4];
    uint16_t relay = (uint16_t) (pb[0] << 8 | pb[1]);
    if (alen != 4 || (memcmp(addr, lo, 4) != 0 && memcmp(addr, unspec, 4) != 0) || relay == 0) {
        errno = EPROTO;
        goto fail;
    }
    a->ctl = s;
    a->relay = relay;
    a->id = id;
    return 0;

fail:;
    int saved = errno;
    close(s);
    errno = saved;
    return -1;
}

static void assoc_close(struct xassoc *a) {
    if (a->ctl < 0)
        return;
    close(a->ctl);
    a->ctl = -1;
    a->old_relay = a->relay;
    a->relay = 0;
    stats.udp_assoc_active--;
}

// The entry ends an association by closing its control connection (idle,
// over its limit, or restarted). Seen here before each use.
static void assoc_check(struct xassoc *a, int kind) {
    if (a->ctl < 0)
        return;
    char b;
    ssize_t n = recv(a->ctl, &b, 1, MSG_PEEK | MSG_DONTWAIT);
    if (n > 0 || (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)))
        return;
    printk("[Net][Xparent] x%x %s association ended by the entry\n", a->id, assoc_kind[kind]);
    assoc_close(a);
}

// Make sure the association of `kind` is up (u->lock held). Returns 0, or
// UR_DIRECT / UR_REFUSE for what the datagram should do instead.
static int assoc_ensure(struct xnet_udp *u, int real_fd, int kind) {
    struct xassoc *a = &u->assoc[kind];
    assoc_check(a, kind);
    if (a->ctl >= 0)
        return 0;
    bool again = a->old_relay != 0;
    const char *stage;
    int code;
    if (assoc_open(u, real_fd, kind, &stage, &code) == 0) {
        stats.udp_assoc_opened++;
        stats.udp_assoc_active++;
        if (again)
            stats.udp_assoc_renewed++;
        printk("[Net][Xparent] x%x %s association open pid=%d comm=%.16s%s\n", a->id, assoc_kind[kind],
                current->pid, current->comm, again ? " (again)" : "");
        lock(&xnet_lock);
        if (entry_was_down) {
            entry_was_down = false;
            printk("[Net][Xparent] entry reachable again\n");
        }
        unlock(&xnet_lock);
        return 0;
    }
    int saved = errno;
    stats.udp_assoc_failed++;
    // The entry itself not answering stops TCP forwarding too, as a failed
    // connect() does; the entry refusing UDP stops only UDP.
    bool entry_down = strncmp(stage, "entry-", 6) == 0;
    lock(&xnet_lock);
    bool fail_closed = routes.fail_closed;
    if (entry_down) {
        entry_retry_at_ms = now_ms() + XNET_ENTRY_RETRY_MS;
        entry_was_down = true;
    } else {
        udp_retry_at_ms = now_ms() + XNET_ENTRY_RETRY_MS;
    }
    unlock(&xnet_lock);
    if (entry_down)
        stats.entry_down_events++;
    printk("[Net][Xparent] %s association FAIL stage=%s err=%d code=%#x; %s %s for %ds\n",
            assoc_kind[kind], stage, saved, code, entry_down ? "entry unusable," : "UDP",
            fail_closed ? "refusing" : "going direct", XNET_ENTRY_RETRY_MS / 1000);
    if (fail_closed) {
        stats.udp_refused_closed++;
        return UR_REFUSE;
    }
    stats.udp_pass_entry_down++;
    return UR_DIRECT;
}

bool xnet_udp_sendmsg(struct fd *sock, const struct msghdr *msg, int flags, ssize_t *result) {
    struct xnet_udp *u = __atomic_load_n(&sock->socket.xudp, __ATOMIC_ACQUIRE);
    struct xdst d;
    struct sockaddr_storage peer;
    socklen_t peer_len = 0;
    bool to_peer = false;
    if (msg->msg_name == NULL) {
        if (u == NULL)
            return false;
        lock(&u->lock);
        if (u->connected) {
            to_peer = true;
            d = u->peer_dst;
            peer = u->peer;
            peer_len = u->peer_len;
        }
        unlock(&u->lock);
        if (!to_peer)
            return false;
    } else if (!dst_from_sockaddr(msg->msg_name, msg->msg_namelen, &d)) {
        return false;
    }

    const char *reason = NULL;
    int route = UR_DIRECT;
    if (atomic_load(&xnet_on))
        route = udp_decide(sock->real_fd, sock->socket.domain == AF_INET6_ ? AF_INET6 : AF_INET, &d, &reason);
    if (route != UR_DIRECT && route != UR_REFUSE && (u != NULL || (u = udp_attach(sock)) != NULL)) {
        size_t total = 0;
        for (size_t i = 0; i < (size_t) msg->msg_iovlen; i++)
            total += msg->msg_iov[i].iov_len;
        uint8_t *buf = malloc(XNET_UDP_HDR_MAX + total);
        if (buf == NULL) {
            errno = ENOBUFS;
            *result = -1;
            return true;
        }
        size_t alen = d.family == AF_INET ? 4 : 16;
        size_t hlen = 4 + alen + 2;
        if (total > XNET_UDP_MAX - hlen) {
            free(buf);
            stats.udp_too_big++;
            errno = EMSGSIZE;
            *result = -1;
            return true;
        }
        int kind = route == UR_DNS ? XA_DNS : XA_MAIN;
        lock(&u->lock);
        int alt = assoc_ensure(u, sock->real_fd, kind);
        uint16_t relay = u->assoc[kind].relay;
        unsigned id = u->assoc[kind].id;
        bool log_dst = false;
        char text[64];
        if (alt == 0) {
            format_dst(text, sizeof(text), d.family, d.addr, d.port);
            if (strcmp(text, u->last_logged) != 0) {
                strcpy(u->last_logged, text);
                log_dst = true;
            }
        }
        unlock(&u->lock);
        if (alt != 0) {
            free(buf);
            route = alt;
        } else {
            if (log_dst && pass_log_allowed())
                printk("[Net][Xparent] x%x %s dst=%s\n", id, assoc_kind[kind], text);
            buf[0] = buf[1] = buf[2] = 0;
            buf[3] = d.family == AF_INET ? 1 : 4;
            memcpy(buf + 4, d.addr, alen);
            buf[4 + alen] = d.port >> 8;
            buf[5 + alen] = d.port & 0xff;
            size_t off = hlen;
            for (size_t i = 0; i < (size_t) msg->msg_iovlen; i++) {
                memcpy(buf + off, msg->msg_iov[i].iov_base, msg->msg_iov[i].iov_len);
                off += msg->msg_iov[i].iov_len;
            }
            struct xdst r = {.family = AF_INET, .addr = {127, 0, 0, 1}, .port = relay};
            struct sockaddr_storage to;
            socklen_t to_len = dst_to_sockaddr(&r, u->domain, &to);
            ssize_t n = sendto(sock->real_fd, buf, off, flags, (void *) &to, to_len);
            int saved = errno;
            free(buf);
            if (n >= 0) {
                stats.udp_sent++;
                if (kind == XA_DNS)
                    stats.udp_dns_sent++;
                n = (size_t) n >= hlen ? n - (ssize_t) hlen : 0;
            }
            errno = saved;
            *result = n;
            return true;
        }
    }
    if (route == UR_REFUSE) {
        if (reason != NULL)
            log_udp(reason, &d);
        errno = ENETUNREACH;
        *result = -1;
        return true;
    }
    if (reason != NULL)
        log_udp(reason, &d);
    if (!to_peer)
        return false; // the host call, exactly as without forwarding
    // A connect()ed socket whose peer now goes out directly: the host
    // socket is not connected, so name the peer.
    struct msghdr m = *msg;
    m.msg_name = &peer;
    m.msg_namelen = peer_len;
    *result = sendmsg(sock->real_fd, &m, flags);
    return true;
}

bool xnet_udp_recvmsg(struct fd *sock, struct msghdr *msg, int flags, ssize_t *result) {
    struct xnet_udp *u = __atomic_load_n(&sock->socket.xudp, __ATOMIC_ACQUIRE);
    if (u == NULL)
        return false;
    lock(&u->lock);
    uint16_t relay[XA_KINDS], old[XA_KINDS];
    for (int k = 0; k < XA_KINDS; k++) {
        relay[k] = u->assoc[k].relay;
        old[k] = u->assoc[k].old_relay;
    }
    bool connected = u->connected;
    struct xdst peer = u->peer_dst;
    unlock(&u->lock);

    size_t cap = 0;
    for (size_t i = 0; i < (size_t) msg->msg_iovlen; i++)
        cap += msg->msg_iov[i].iov_len;
    // Room for the largest datagram and its header: the guest's own buffer
    // size still decides MSG_TRUNC, as it would without the header.
    size_t size = 65536 + XNET_UDP_HDR_MAX;
    uint8_t *buf = malloc(size);
    if (buf == NULL) {
        errno = ENOBUFS;
        *result = -1;
        return true;
    }
    for (;;) {
        struct sockaddr_storage from;
        struct iovec iov = {buf, size};
        struct msghdr h = {
            .msg_name = &from, .msg_namelen = sizeof(from), .msg_iov = &iov, .msg_iovlen = 1,
            .msg_control = msg->msg_control, .msg_controllen = msg->msg_controllen,
        };
        ssize_t n = recvmsg(sock->real_fd, &h, flags & ~MSG_TRUNC);
        if (n < 0) {
            *result = -1;
            break;
        }
        struct xdst src;
        bool known = dst_from_sockaddr(&from, h.msg_namelen, &src);
        uint8_t *payload = buf;
        size_t plen = (size_t) n;
        bool drop = false, relayed = false;
        static const uint8_t lo[4] = {127, 0, 0, 1};
        if (known && src.family == AF_INET && memcmp(src.addr, lo, 4) == 0 && src.port != 0) {
            for (int k = 0; k < XA_KINDS; k++) {
                if (src.port == relay[k])
                    relayed = true;
                else if (src.port == old[k])
                    drop = true; // a late datagram of an ended association
            }
        }
        if (relayed) {
            // RSV RSV FRAG ATYP ADDR PORT; fragments are not used.
            size_t alen = n >= 4 ? (buf[3] == 1 ? 4 : buf[3] == 4 ? 16 : 0) : 0;
            if (alen == 0 || (size_t) n < 4 + alen + 2 || buf[0] != 0 || buf[1] != 0 || buf[2] != 0) {
                drop = true;
            } else {
                src.family = buf[3] == 1 ? AF_INET : AF_INET6;
                memset(src.addr, 0, sizeof(src.addr));
                memcpy(src.addr, buf + 4, alen);
                src.port = (uint16_t) (buf[4 + alen] << 8 | buf[5 + alen]);
                payload = buf + 4 + alen + 2;
                plen = (size_t) n - (4 + alen + 2);
            }
        }
        if (!drop && connected && (!known || !dst_equal(&src, &peer)))
            drop = true;
        if (drop) {
            stats.udp_dropped++;
            if (flags & MSG_PEEK)
                recv(sock->real_fd, buf, 1, MSG_DONTWAIT); // take it off the queue
            continue;
        }
        if (relayed)
            stats.udp_received++;
        size_t copied = 0;
        for (size_t i = 0; i < (size_t) msg->msg_iovlen && copied < plen; i++) {
            size_t chunk = msg->msg_iov[i].iov_len;
            if (chunk > plen - copied)
                chunk = plen - copied;
            memcpy(msg->msg_iov[i].iov_base, payload + copied, chunk);
            copied += chunk;
        }
        msg->msg_flags = (h.msg_flags & ~MSG_TRUNC) | (plen > cap ? MSG_TRUNC : 0);
        msg->msg_controllen = h.msg_controllen;
        if (msg->msg_name != NULL) {
            struct sockaddr_storage name;
            socklen_t name_len;
            if (relayed) {
                name_len = dst_to_sockaddr(&src, u->domain, &name);
            } else {
                name = from;
                name_len = h.msg_namelen;
            }
            memcpy(msg->msg_name, &name, name_len < msg->msg_namelen ? name_len : msg->msg_namelen);
            msg->msg_namelen = name_len;
        }
        *result = (flags & MSG_TRUNC) ? (ssize_t) plen : (ssize_t) copied;
        break;
    }
    int saved = errno;
    free(buf);
    errno = saved;
    return true;
}

int xnet_udp_connect(struct fd *sock, const struct sockaddr *addr, socklen_t len) {
    struct xnet_udp *u = __atomic_load_n(&sock->socket.xudp, __ATOMIC_ACQUIRE);
    if (addr->sa_family == AF_UNSPEC) {
        if (u == NULL)
            return XNET_PASS;
        lock(&u->lock);
        bool was = u->connected;
        u->connected = false;
        unlock(&u->lock);
        // iSH held the peer, the host socket was never connected.
        return was ? 0 : XNET_PASS;
    }
    struct xdst d;
    if (!dst_from_sockaddr(addr, len, &d))
        return XNET_PASS;
    int domain = sock->socket.domain == AF_INET6_ ? AF_INET6 : AF_INET;
    const char *reason = NULL;
    int route = UR_DIRECT;
    if (atomic_load(&xnet_on))
        route = udp_decide(sock->real_fd, domain, &d, &reason);
    if (route == UR_REFUSE) {
        log_udp(reason, &d);
        return _ENETUNREACH;
    }
    if (route == UR_DIRECT || (u == NULL && (u = udp_attach(sock)) == NULL)) {
        if (reason != NULL)
            log_udp(reason, &d);
        if (u != NULL) {
            lock(&u->lock);
            u->connected = false;
            unlock(&u->lock);
        }
        return XNET_PASS;
    }
    // A host socket connected earlier would not take the relay's datagrams.
    struct sockaddr_storage cur;
    socklen_t cur_len = sizeof(cur);
    if (getpeername(sock->real_fd, (void *) &cur, &cur_len) == 0) {
        struct sockaddr unspec = {.sa_family = AF_UNSPEC};
        connect(sock->real_fd, &unspec, sizeof(unspec));
    }
    if (udp_local_port(sock->real_fd, domain) < 0)
        return errno_map();
    lock(&u->lock);
    u->connected = true;
    memset(&u->peer, 0, sizeof(u->peer));
    memcpy(&u->peer, addr, len < sizeof(u->peer) ? len : sizeof(u->peer));
    u->peer_len = len < sizeof(u->peer) ? len : sizeof(u->peer);
    u->peer_dst = d;
    unlock(&u->lock);
    log_udp(route == UR_DNS ? "connect kind=dns (peer kept by iSH)" : "connect (peer kept by iSH)", &d);
    return 0;
}

int xnet_udp_getpeername(struct fd *sock, struct sockaddr_storage *out, socklen_t *len) {
    struct xnet_udp *u = __atomic_load_n(&sock->socket.xudp, __ATOMIC_ACQUIRE);
    if (u == NULL)
        return XNET_PASS;
    lock(&u->lock);
    int res = XNET_PASS;
    if (u->connected) {
        memcpy(out, &u->peer, sizeof(u->peer));
        *len = u->peer_len;
        res = 0;
    }
    unlock(&u->lock);
    return res;
}

void xnet_udp_release(struct fd *sock) {
    struct xnet_udp *u = sock->socket.xudp;
    sock->socket.xudp = NULL;
    for (int k = 0; k < XA_KINDS; k++) {
        if (u->assoc[k].ctl >= 0)
            printk("[Net][Xparent] x%x %s association closed\n", u->assoc[k].id, assoc_kind[k]);
        assoc_close(&u->assoc[k]);
    }
    free(u);
}

// --- command-line configuration ---------------------------------------------

static size_t split_list(char *value, const char **out, size_t max) {
    size_t n = 0;
    for (char *tok = strtok(value, ","); tok != NULL && n < max; tok = strtok(NULL, ","))
        if (*tok != '\0')
            out[n++] = tok;
    return n;
}

int ish_xnet_load_file(const char *path) {
    FILE *f = fopen(path, "r");
    if (f == NULL)
        return -errno;
    char line[1024], user[256] = "shell", pass[256] = "";
    char hook_buf[1024] = "", exempt_buf[1024] = "", self_buf[256] = "";
    char udp_buf[1024] = "", resolver_buf[256] = "";
    unsigned port = 0;
    int conn_id = 0, fail_closed = 0, enabled = 0, dns_intercept = 0;
    unsigned long long generation = 0;
    while (fgets(line, sizeof(line), f) != NULL) {
        line[strcspn(line, "\r\n")] = '\0';
        char *eq = strchr(line, '=');
        if (line[0] == '#' || eq == NULL)
            continue;
        *eq = '\0';
        const char *key = line, *value = eq + 1;
#define TEXT(name, buf) else if (strcmp(key, name) == 0) snprintf(buf, sizeof(buf), "%s", value)
        if (strcmp(key, "port") == 0) port = (unsigned) atoi(value);
        TEXT("user", user);
        TEXT("password", pass);
        TEXT("hook", hook_buf);
        TEXT("exempt", exempt_buf);
        TEXT("self", self_buf);
        TEXT("udp_hook", udp_buf);
        TEXT("resolvers", resolver_buf);
        else if (strcmp(key, "dns_intercept") == 0) dns_intercept = atoi(value);
        else if (strcmp(key, "conn_id") == 0) conn_id = atoi(value);
        else if (strcmp(key, "fail_closed") == 0) fail_closed = atoi(value);
        else if (strcmp(key, "enabled") == 0) enabled = atoi(value);
        else if (strcmp(key, "generation") == 0) generation = strtoull(value, NULL, 10);
#undef TEXT
    }
    fclose(f);
    wipe(line, sizeof(line));
    const char *hook[32], *exempt[32], *self[4], *udp[32], *resolvers[8];
    struct ish_xnet_routes r = {
        .generation = generation,
        .hook = hook, .hook_count = split_list(hook_buf, hook, 32),
        .exempt = exempt, .exempt_count = split_list(exempt_buf, exempt, 32),
        .self_addrs = self, .self_count = split_list(self_buf, self, 4),
        .udp_hook = udp, .udp_hook_count = split_list(udp_buf, udp, 32),
        .resolvers = resolvers, .resolver_count = split_list(resolver_buf, resolvers, 8),
        .dns_intercept = dns_intercept,
        .fail_closed = fail_closed,
    };
    int err = ish_xnet_configure((uint16_t) port, user, pass, conn_id);
    wipe(pass, sizeof(pass));
    if (err == 0)
        err = ish_xnet_set_routes2(&r);
    if (err == 0)
        ish_xnet_set_enabled(enabled);
    return err;
}
