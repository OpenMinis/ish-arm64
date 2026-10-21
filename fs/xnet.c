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
    struct xprefix *hook, *exempt, *self;
    size_t hook_count, exempt_count, self_count;
    bool fail_closed;
} routes;
static long entry_retry_at_ms; // breaker: entry unreachable until then
static bool entry_was_down;

static atomic_uint next_conn_id;

static struct {
    _Atomic uint64_t hooked, pass_no_match, pass_exempt, pass_local, pass_bound,
            pass_entry_down, pass_unconfigured, refused_closed, connected,
            fail_refused, fail_host_unreach, fail_net_unreach, fail_timeout,
            fail_denied, fail_other, fail_entry, cancelled, entry_down_events;
    atomic_uint pending;
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

int ish_xnet_set_routes(uint64_t generation,
        const char *const *hook, size_t hook_count,
        const char *const *exempt, size_t exempt_count,
        bool UNUSED(dns_intercept),
        const char *const *self_addrs, size_t self_count,
        bool fail_closed) {
    struct xprefix *h, *e = NULL, *s = NULL;
    int err = parse_list(hook, hook_count, &h, "hook");
    if (err == 0)
        err = parse_list(exempt, exempt_count, &e, "exempt");
    if (err == 0)
        err = parse_list(self_addrs, self_count, &s, "self");
    if (err < 0) {
        free(h);
        free(e);
        return err;
    }
    lock(&xnet_lock);
    struct xprefix *old_h = routes.hook, *old_e = routes.exempt, *old_s = routes.self;
    routes.generation = generation;
    routes.hook = h;
    routes.hook_count = hook_count;
    routes.exempt = e;
    routes.exempt_count = exempt_count;
    routes.self = s;
    routes.self_count = self_count;
    routes.fail_closed = fail_closed;
    unlock(&xnet_lock);
    free(old_h);
    free(old_e);
    free(old_s);
    printk("[Net][Xparent] routes gen=%llu hook=%zu exempt=%zu self=%zu fail_closed=%d\n",
            (unsigned long long) generation, hook_count, exempt_count, self_count, fail_closed);
    return 0;
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
    if (entry.port == 0 || routes.hook_count == 0) {
        stats.pass_unconfigured++;
        reason = "unconfigured";
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
            snprintf(user, sizeof(user), "%s~x%x", entry.username, c->id);
        else
            strcpy(user, entry.username);
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
    stats.pending++;
    printk("[Net][Xparent] x%x hook pid=%d comm=%.16s dst=%s gen=%llu\n",
            c->id, current->pid, current->comm, c->text, (unsigned long long) generation);
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
    unsigned port = 0;
    int conn_id = 0, fail_closed = 0, enabled = 0;
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
        else if (strcmp(key, "conn_id") == 0) conn_id = atoi(value);
        else if (strcmp(key, "fail_closed") == 0) fail_closed = atoi(value);
        else if (strcmp(key, "enabled") == 0) enabled = atoi(value);
        else if (strcmp(key, "generation") == 0) generation = strtoull(value, NULL, 10);
#undef TEXT
    }
    fclose(f);
    wipe(line, sizeof(line));
    const char *hook[32], *exempt[32], *self[4];
    size_t nh = split_list(hook_buf, hook, 32), ne = split_list(exempt_buf, exempt, 32),
            ns = split_list(self_buf, self, 4);
    int err = ish_xnet_configure((uint16_t) port, user, pass, conn_id);
    wipe(pass, sizeof(pass));
    if (err == 0)
        err = ish_xnet_set_routes(generation, hook, nh, exempt, ne, false, self, ns, fail_closed);
    if (err == 0)
        ish_xnet_set_enabled(enabled);
    return err;
}
