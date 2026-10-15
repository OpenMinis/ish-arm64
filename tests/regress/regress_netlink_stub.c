// AF_NETLINK stub: no read may block without bound, and what the host can
// tell us is answered for real (fs/sock.c, fs/netlink.c).
//
// The regression this pins: the stub answered a request only when it was
// sent with sendto()/sendmsg(), and its read was a bare blocking read() of
// the backing socketpair that ignored MSG_PEEK / MSG_DONTWAIT / O_NONBLOCK.
// busybox `ip` sends with write(), so `ip addr` waited for a reply that was
// never produced -- for the 900 s tool timeout, or until the user stopped it.
// iproute2 peeks before it reads, and the peek consumed the only reply.
//
// Every case runs in a child under a watchdog, so a hang is a FAIL here and
// not a stuck test run.
//
// Build: aarch64-linux-musl-gcc -static -O0 -o nlstub regress_netlink_stub.c
// Run:   ish -r <rootfs> /tmp/nlstub        (tests/regress/run.sh does both)
//        ISH_NETLINK_ROUTE=empty ish -r <rootfs> /tmp/nlstub route-empty
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <net/if.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int64_t now_us(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t) t.tv_sec * 1000000 + t.tv_nsec / 1000;
}
static void sleep_us(long us) {
    struct timespec t = {us / 1000000, (us % 1000000) * 1000};
    nanosleep(&t, NULL);
}

static int pass, fail;
static const char *only;
static int route_empty; // run with ISH_NETLINK_ROUTE=empty in the host environment

static void run(const char *name, int (*fn)(void), int limit_ms) {
    if (only && !strstr(name, only)) return;
    fflush(stdout);
    int64_t t0 = now_us();
    pid_t pid = fork();
    if (pid == 0) { alarm(20); _exit(fn()); }
    int status = 0, code = -1;
    for (;;) {
        pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid) { code = WIFEXITED(status) ? WEXITSTATUS(status) : 100 + WTERMSIG(status); break; }
        if ((now_us() - t0) / 1000 > limit_ms) { kill(pid, SIGKILL); waitpid(pid, &status, 0); break; }
        sleep_us(5000);
    }
    long ms = (long) ((now_us() - t0) / 1000);
    const char *what = code == 0 ? "ok" : code == -1 ? "HUNG (killed)" : "FAIL";
    printf("%-52s %s in %ldms\n", name, what, ms);
    if (code == 0) pass++; else fail++;
}

#define CHECK(cond, ...) do { if (!(cond)) { printf("    FAIL " __VA_ARGS__); printf("\n"); return 1; } } while (0)

struct req {
    struct nlmsghdr h;
    union {
        struct ifinfomsg ifi;
        struct ifaddrmsg ifa;
        struct rtmsg rt;
        struct rtgenmsg g;
    } u;
};

static int nl_socket(int extra) {
    int fd = socket(AF_NETLINK, SOCK_RAW | extra, NETLINK_ROUTE);
    if (fd < 0) return -1;
    struct sockaddr_nl a = {.nl_family = AF_NETLINK};
    if (bind(fd, (struct sockaddr *) &a, sizeof(a)) < 0) return -1;
    return fd;
}

static void fill(struct req *r, int type, int flags, uint32_t seq, int family, size_t body) {
    memset(r, 0, sizeof(*r));
    r->h.nlmsg_len = NLMSG_LENGTH(body);
    r->h.nlmsg_type = type;
    r->h.nlmsg_flags = NLM_F_REQUEST | flags;
    r->h.nlmsg_seq = seq;
    r->u.g.rtgen_family = family;
}

// What a dump produced: how many messages of `want` there were, and whether
// it ended with NLMSG_DONE or NLMSG_ERROR (err = the error it carried).
struct dump {
    int count, done, error, err, bad_seq, datagrams;
    int saw_lo, saw_lo_v4, lo_index;
};

static void scan(struct dump *d, char *buf, ssize_t n, int want, uint32_t seq) {
    d->datagrams++;
    for (struct nlmsghdr *h = (struct nlmsghdr *) buf; NLMSG_OK(h, n); h = NLMSG_NEXT(h, n)) {
        if (h->nlmsg_seq != seq) d->bad_seq++;
        if (h->nlmsg_type == NLMSG_DONE) { d->done = 1; return; }
        if (h->nlmsg_type == NLMSG_ERROR) {
            d->error = 1;
            d->err = -((struct nlmsgerr *) NLMSG_DATA(h))->error;
            return;
        }
        if (h->nlmsg_type != want) continue;
        d->count++;
        if (want == RTM_NEWLINK) {
            struct ifinfomsg *ifi = NLMSG_DATA(h);
            int len = h->nlmsg_len - NLMSG_LENGTH(sizeof(*ifi));
            for (struct rtattr *a = IFLA_RTA(ifi); RTA_OK(a, len); a = RTA_NEXT(a, len))
                if (a->rta_type == IFLA_IFNAME && strcmp(RTA_DATA(a), "lo") == 0) {
                    d->saw_lo = 1;
                    d->lo_index = ifi->ifi_index;
                }
        } else if (want == RTM_NEWADDR) {
            struct ifaddrmsg *ifa = NLMSG_DATA(h);
            int len = h->nlmsg_len - NLMSG_LENGTH(sizeof(*ifa));
            for (struct rtattr *a = IFA_RTA(ifa); RTA_OK(a, len); a = RTA_NEXT(a, len))
                if (ifa->ifa_family == AF_INET && a->rta_type == IFA_LOCAL &&
                        memcmp(RTA_DATA(a), "\x7f\x00\x00\x01", 4) == 0 && ifa->ifa_prefixlen == 8)
                    d->saw_lo_v4 = 1;
        }
    }
}

// Read with blocking recvmsg() until the dump ends.
static int read_dump(int fd, struct dump *d, int want, uint32_t seq) {
    memset(d, 0, sizeof(*d));
    char buf[8192];
    while (!d->done && !d->error) {
        struct sockaddr_nl from = {0};
        struct iovec iov = {buf, sizeof(buf)};
        struct msghdr m = {.msg_name = &from, .msg_namelen = sizeof(from), .msg_iov = &iov, .msg_iovlen = 1};
        ssize_t n = recvmsg(fd, &m, 0);
        if (n < 0) { if (errno == EINTR) continue; printf("    recvmsg: %s\n", strerror(errno)); return -1; }
        if (m.msg_namelen != sizeof(from) || from.nl_family != AF_NETLINK || from.nl_pid != 0) {
            printf("    bad sender: namelen=%u family=%d pid=%u\n", m.msg_namelen, from.nl_family, from.nl_pid);
            return -1;
        }
        scan(d, buf, n, want, seq);
    }
    return 0;
}

// (a) busybox `ip`: the request goes out with write(), the reply is read with
// a blocking recvmsg(). This is the case that hung.
static int c_write_then_recvmsg(void) {
    int fd = nl_socket(0);
    CHECK(fd >= 0, "socket/bind: %s", strerror(errno));
    struct req r;
    fill(&r, RTM_GETLINK, NLM_F_DUMP, 101, AF_UNSPEC, sizeof(struct rtgenmsg));
    CHECK(write(fd, &r, r.h.nlmsg_len) == (ssize_t) r.h.nlmsg_len, "write: %s", strerror(errno));
    struct dump d;
    int64_t t0 = now_us();
    CHECK(read_dump(fd, &d, RTM_NEWLINK, 101) == 0, "read");
    long ms = (long) ((now_us() - t0) / 1000);
    printf("    NOTE write+recvmsg: %d links in %d datagrams, done=%d, lo=%d (index %d), %ldms\n",
           d.count, d.datagrams, d.done, d.saw_lo, d.lo_index, ms);
    CHECK(d.done && !d.error, "dump did not end with NLMSG_DONE");
    CHECK(d.count >= 1 && d.saw_lo, "no RTM_NEWLINK for lo");
    CHECK(d.bad_seq == 0, "%d replies with the wrong seq", d.bad_seq);
    CHECK(ms < 1000, "took %ldms", ms);
    // writev() takes the same path.
    fill(&r, RTM_GETLINK, NLM_F_DUMP, 102, AF_UNSPEC, sizeof(struct rtgenmsg));
    struct iovec iov[2] = {{&r, sizeof(r.h)}, {&r.u, r.h.nlmsg_len - sizeof(r.h)}};
    CHECK(writev(fd, iov, 2) == (ssize_t) r.h.nlmsg_len, "writev: %s", strerror(errno));
    CHECK(read_dump(fd, &d, RTM_NEWLINK, 102) == 0 && d.done && d.saw_lo, "writev request not answered");
    return 0;
}

// (b) iproute2: send(), then recvmsg(MSG_PEEK|MSG_TRUNC) to size the buffer,
// then the real recvmsg(). The peek must not consume, and must report the
// datagram's real length.
static int c_peek_trunc(void) {
    int fd = nl_socket(0);
    CHECK(fd >= 0, "socket/bind: %s", strerror(errno));
    struct req r;
    fill(&r, RTM_GETLINK, NLM_F_DUMP, 201, AF_UNSPEC, sizeof(struct ifinfomsg));
    CHECK(send(fd, &r, r.h.nlmsg_len, 0) == (ssize_t) r.h.nlmsg_len, "send: %s", strerror(errno));
    struct dump d = {0};
    char big[8192];
    int rounds = 0;
    while (!d.done && !d.error && rounds++ < 500) {
        struct iovec none = {NULL, 0};
        struct msghdr pm = {.msg_iov = &none, .msg_iovlen = 1};
        ssize_t peeked = recvmsg(fd, &pm, MSG_PEEK | MSG_TRUNC);
        CHECK(peeked > 0, "peek returned %zd (%s)", peeked, strerror(errno));
        CHECK(pm.msg_flags & MSG_TRUNC, "peek into an empty buffer did not set MSG_TRUNC");
        // A second peek sees the same datagram: nothing was consumed.
        char one[1];
        ssize_t again = recv(fd, one, sizeof(one), MSG_PEEK | MSG_TRUNC);
        CHECK(again == peeked, "second peek %zd != first %zd", again, peeked);
        struct iovec iov = {big, sizeof(big)};
        struct msghdr m = {.msg_iov = &iov, .msg_iovlen = 1};
        ssize_t n = recvmsg(fd, &m, 0);
        CHECK(n == peeked, "read %zd after peeking %zd", n, peeked);
        CHECK(!(m.msg_flags & MSG_TRUNC), "full read flagged MSG_TRUNC");
        scan(&d, big, n, RTM_NEWLINK, 201);
    }
    printf("    NOTE peek+read: %d links, done=%d\n", d.count, d.done);
    CHECK(d.done && d.saw_lo, "dump incomplete after peeking (done=%d lo=%d)", d.done, d.saw_lo);
    return 0;
}

// (c) addresses: at least the loopback's 127.0.0.1/8, and the family filter.
static int c_getaddr(void) {
    int fd = nl_socket(0);
    CHECK(fd >= 0, "socket/bind: %s", strerror(errno));
    struct req r;
    fill(&r, RTM_GETADDR, NLM_F_DUMP, 301, AF_UNSPEC, sizeof(struct ifaddrmsg));
    CHECK(send(fd, &r, r.h.nlmsg_len, 0) > 0, "send: %s", strerror(errno));
    struct dump all, v4, v6;
    CHECK(read_dump(fd, &all, RTM_NEWADDR, 301) == 0 && all.done, "AF_UNSPEC dump");
    CHECK(all.saw_lo_v4, "no 127.0.0.1/8");
    fill(&r, RTM_GETADDR, NLM_F_DUMP, 302, AF_INET, sizeof(struct ifaddrmsg));
    CHECK(send(fd, &r, r.h.nlmsg_len, 0) > 0, "send");
    CHECK(read_dump(fd, &v4, RTM_NEWADDR, 302) == 0 && v4.done && v4.saw_lo_v4, "AF_INET dump");
    fill(&r, RTM_GETADDR, NLM_F_DUMP, 303, AF_INET6, sizeof(struct ifaddrmsg));
    CHECK(send(fd, &r, r.h.nlmsg_len, 0) > 0, "send");
    CHECK(read_dump(fd, &v6, RTM_NEWADDR, 303) == 0 && v6.done && !v6.saw_lo_v4, "AF_INET6 dump");
    printf("    NOTE addrs: %d total, %d v4, %d v6\n", all.count, v4.count, v6.count);
    CHECK(v4.count + v6.count == all.count, "family filter: %d + %d != %d", v4.count, v6.count, all.count);
    // The C library's getifaddrs() and if_nametoindex() ride on the same dumps.
    struct ifaddrs *list = NULL;
    CHECK(getifaddrs(&list) == 0 && list != NULL, "getifaddrs: %s", strerror(errno));
    int lo = 0;
    for (struct ifaddrs *i = list; i; i = i->ifa_next)
        if (strcmp(i->ifa_name, "lo") == 0 && (i->ifa_flags & IFF_LOOPBACK) && (i->ifa_flags & IFF_UP)) lo = 1;
    freeifaddrs(list);
    CHECK(lo, "getifaddrs() has no up loopback named lo");
    return 0;
}

// (d) what the host cannot tell us is refused at once, never left hanging:
// routes (unless ISH_NETLINK_ROUTE=empty), neighbours, rules, a change.
static int c_refused(void) {
    int fd = nl_socket(0);
    CHECK(fd >= 0, "socket/bind: %s", strerror(errno));
    struct req r;
    struct dump d;
    int64_t t0 = now_us();
    fill(&r, RTM_GETROUTE, NLM_F_DUMP, 401, AF_INET, sizeof(struct rtmsg));
    CHECK(write(fd, &r, r.h.nlmsg_len) > 0, "write");
    CHECK(read_dump(fd, &d, RTM_NEWROUTE, 401) == 0, "route read");
    if (route_empty) {
        CHECK(d.done && !d.error && d.count == 0, "ISH_NETLINK_ROUTE=empty: expected an empty dump");
    } else {
        CHECK(d.error && d.err == EOPNOTSUPP, "route dump: error=%d errno=%d, expected EOPNOTSUPP", d.error, d.err);
    }
    int types[] = {RTM_GETNEIGH, RTM_GETRULE, RTM_GETQDISC, RTM_NEWLINK, RTM_DELADDR};
    for (unsigned i = 0; i < sizeof(types) / sizeof(types[0]); i++) {
        fill(&r, types[i], NLM_F_DUMP, 410 + i, AF_UNSPEC, sizeof(struct rtmsg));
        CHECK(send(fd, &r, r.h.nlmsg_len, 0) > 0, "send");
        CHECK(read_dump(fd, &d, 0, 410 + i) == 0, "read type %d", types[i]);
        CHECK(d.error && d.err == EOPNOTSUPP, "type %d: error=%d errno=%d", types[i], d.error, d.err);
    }
    // A link that does not exist.
    fill(&r, RTM_GETLINK, 0, 420, AF_UNSPEC, sizeof(struct ifinfomsg));
    r.u.ifi.ifi_index = 9999;
    CHECK(send(fd, &r, r.h.nlmsg_len, 0) > 0, "send");
    CHECK(read_dump(fd, &d, RTM_NEWLINK, 420) == 0 && d.error && d.err == ENODEV, "missing link: errno %d", d.err);
    // One link by index: the object alone, no NLMSG_DONE to wait for.
    fill(&r, RTM_GETLINK, 0, 421, AF_UNSPEC, sizeof(struct ifinfomsg));
    r.u.ifi.ifi_index = 1;
    CHECK(send(fd, &r, r.h.nlmsg_len, 0) > 0, "send");
    char buf[4096];
    ssize_t n = recv(fd, buf, sizeof(buf), 0);
    memset(&d, 0, sizeof(d));
    scan(&d, buf, n, RTM_NEWLINK, 421);
    CHECK(n > 0 && d.count == 1 && d.saw_lo && !d.done, "get link 1: n=%zd count=%d lo=%d", n, d.count, d.saw_lo);
    CHECK(recv(fd, buf, sizeof(buf), MSG_DONTWAIT) < 0 && errno == EAGAIN, "something queued after a single get");
    long ms = (long) ((now_us() - t0) / 1000);
    printf("    NOTE refusals: all answered in %ldms (route mode: %s)\n", ms, route_empty ? "empty" : "error");
    CHECK(ms < 1000, "took %ldms", ms);
    // Any other netlink protocol has nothing behind it at all.
    int other = socket(AF_NETLINK, SOCK_RAW, NETLINK_GENERIC);
    if (other >= 0) {
        fill(&r, 0x10, 0, 430, 0, 4);
        CHECK(send(other, &r, r.h.nlmsg_len, 0) > 0, "send generic");
        CHECK(read_dump(other, &d, 0, 430) == 0 && d.error && d.err == EOPNOTSUPP, "NETLINK_GENERIC not refused");
    }
    return 0;
}

// (e) an empty queue is EAGAIN for a non-blocking reader, by every route.
static int c_nonblock(void) {
    char buf[256];
    int fd = nl_socket(0);
    CHECK(fd >= 0, "socket/bind: %s", strerror(errno));
    int64_t t0 = now_us();
    CHECK(recv(fd, buf, sizeof(buf), MSG_DONTWAIT) < 0 && errno == EAGAIN, "recv(MSG_DONTWAIT): %s", strerror(errno));
    struct iovec iov = {buf, sizeof(buf)};
    struct msghdr m = {.msg_iov = &iov, .msg_iovlen = 1};
    CHECK(recvmsg(fd, &m, MSG_DONTWAIT) < 0 && errno == EAGAIN, "recvmsg(MSG_DONTWAIT): %s", strerror(errno));
    CHECK(recvmsg(fd, &m, MSG_DONTWAIT | MSG_PEEK) < 0 && errno == EAGAIN, "recvmsg(DONTWAIT|PEEK): %s", strerror(errno));
    int nb = nl_socket(SOCK_NONBLOCK);
    CHECK(nb >= 0, "SOCK_NONBLOCK socket");
    CHECK(recv(nb, buf, sizeof(buf), 0) < 0 && errno == EAGAIN, "SOCK_NONBLOCK recv: %s", strerror(errno));
    CHECK(recvmsg(nb, &m, 0) < 0 && errno == EAGAIN, "SOCK_NONBLOCK recvmsg: %s", strerror(errno));
    CHECK(read(nb, buf, sizeof(buf)) < 0 && errno == EAGAIN, "SOCK_NONBLOCK read: %s", strerror(errno));
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    CHECK(recv(fd, buf, sizeof(buf), 0) < 0 && errno == EAGAIN, "O_NONBLOCK recv: %s", strerror(errno));
    long ms = (long) ((now_us() - t0) / 1000);
    CHECK(ms < 500, "non-blocking reads took %ldms", ms);
    // poll() agrees: nothing readable until a request has been answered.
    struct pollfd p = {nb, POLLIN, 0};
    CHECK(poll(&p, 1, 0) == 0, "empty socket polls readable");
    struct req r;
    fill(&r, RTM_GETLINK, NLM_F_DUMP, 501, AF_UNSPEC, sizeof(struct rtgenmsg));
    CHECK(send(nb, &r, r.h.nlmsg_len, 0) > 0, "send");
    CHECK(poll(&p, 1, 1000) == 1 && (p.revents & POLLIN), "answered socket does not poll readable");
    return 0;
}

static volatile sig_atomic_t alarms;
static void on_alarm(int s) { (void) s; alarms++; }

// (f) a blocking read with nothing asked and nothing queued waits like on
// Linux -- but a signal gets through, with and without subscribed groups,
// and SO_RCVTIMEO is honoured.
static int c_signal(void) {
    struct sigaction sa = {.sa_handler = on_alarm}; // no SA_RESTART
    sigaction(SIGALRM, &sa, NULL);
    for (int groups = 0; groups <= 1; groups++) {
        int fd = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
        struct sockaddr_nl a = {.nl_family = AF_NETLINK,
                                .nl_groups = groups ? (RTMGRP_LINK | RTMGRP_IPV4_IFADDR) : 0};
        CHECK(fd >= 0 && bind(fd, (struct sockaddr *) &a, sizeof(a)) == 0, "socket/bind: %s", strerror(errno));
        char buf[256];
        struct iovec iov = {buf, sizeof(buf)};
        struct msghdr m = {.msg_iov = &iov, .msg_iovlen = 1};
        alarms = 0;
        int64_t t0 = now_us();
        ualarm(300000, 0);
        ssize_t n = recvmsg(fd, &m, 0);
        int e = errno;
        long ms = (long) ((now_us() - t0) / 1000);
        printf("    NOTE blocking read, groups=%d: r=%zd errno=%d after %ldms, alarms=%d\n", groups, n, e, ms, (int) alarms);
        CHECK(n < 0 && e == EINTR && alarms == 1, "not interrupted by SIGALRM");
        CHECK(ms >= 250 && ms < 2500, "interrupted after %ldms", ms);
        close(fd);
    }
    int fd = nl_socket(0);
    struct timeval tv = {0, 300000};
    CHECK(setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) == 0, "SO_RCVTIMEO: %s", strerror(errno));
    char buf[64];
    int64_t t0 = now_us();
    ssize_t n = recv(fd, buf, sizeof(buf), 0);
    int e = errno;
    long ms = (long) ((now_us() - t0) / 1000);
    printf("    NOTE SO_RCVTIMEO 300ms: r=%zd errno=%d after %ldms\n", n, e, ms);
    CHECK(n < 0 && e == EAGAIN && ms >= 250 && ms < 2500, "SO_RCVTIMEO not honoured");
    return 0;
}

// (g) Go's syscall.NetlinkRIB (tailscale's netmon, net.Interfaces): bind,
// sendto, getsockname, then recvfrom into one page until NLMSG_DONE,
// checking every message's seq and pid against what getsockname reported.
static int c_go_netlinkrib(void) {
    int types[] = {RTM_GETLINK, RTM_GETADDR};
    int news[] = {RTM_NEWLINK, RTM_NEWADDR};
    for (int t = 0; t < 2; t++) {
        int fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE);
        struct sockaddr_nl a = {.nl_family = AF_NETLINK};
        CHECK(fd >= 0 && bind(fd, (struct sockaddr *) &a, sizeof(a)) == 0, "socket/bind: %s", strerror(errno));
        struct req r;
        fill(&r, types[t], NLM_F_DUMP, 1, AF_UNSPEC, sizeof(struct rtgenmsg));
        CHECK(sendto(fd, &r, r.h.nlmsg_len, 0, (struct sockaddr *) &a, sizeof(a)) > 0, "sendto: %s", strerror(errno));
        struct sockaddr_nl self = {0};
        socklen_t len = sizeof(self);
        CHECK(getsockname(fd, (struct sockaddr *) &self, &len) == 0 && len == sizeof(self) && self.nl_family == AF_NETLINK,
              "getsockname");
        char page[4096];
        int done = 0, count = 0;
        while (!done) {
            struct sockaddr_nl from = {0};
            socklen_t flen = sizeof(from);
            ssize_t n = recvfrom(fd, page, sizeof(page), 0, (struct sockaddr *) &from, &flen);
            CHECK(n >= (ssize_t) sizeof(struct nlmsghdr), "recvfrom: %zd (%s)", n, strerror(errno));
            CHECK(flen == sizeof(from) && from.nl_family == AF_NETLINK, "recvfrom sender: len=%u family=%d", flen, from.nl_family);
            for (struct nlmsghdr *h = (struct nlmsghdr *) page; NLMSG_OK(h, n); h = NLMSG_NEXT(h, n)) {
                CHECK(h->nlmsg_seq == 1 && h->nlmsg_pid == self.nl_pid, "seq=%u pid=%u (socket pid %u)",
                      h->nlmsg_seq, h->nlmsg_pid, self.nl_pid);
                CHECK(h->nlmsg_type != NLMSG_ERROR, "NLMSG_ERROR for type %d", types[t]);
                if (h->nlmsg_type == NLMSG_DONE) { done = 1; break; }
                if (h->nlmsg_type == news[t]) count++;
            }
        }
        printf("    NOTE NetlinkRIB(%s): %d messages\n", t == 0 ? "RTM_GETLINK" : "RTM_GETADDR", count);
        CHECK(count >= 1, "empty %s dump", t == 0 ? "link" : "address");
        close(fd);
    }
    // sendmsg + blocking recvmsg, several requests on one socket.
    int fd = nl_socket(0);
    for (uint32_t seq = 700; seq < 705; seq++) {
        struct req r;
        fill(&r, seq % 2 ? RTM_GETADDR : RTM_GETLINK, NLM_F_DUMP, seq, AF_UNSPEC, sizeof(struct rtgenmsg));
        struct iovec iov = {&r, r.h.nlmsg_len};
        struct sockaddr_nl to = {.nl_family = AF_NETLINK};
        struct msghdr m = {.msg_name = &to, .msg_namelen = sizeof(to), .msg_iov = &iov, .msg_iovlen = 1};
        CHECK(sendmsg(fd, &m, 0) == (ssize_t) r.h.nlmsg_len, "sendmsg: %s", strerror(errno));
        struct dump d;
        CHECK(read_dump(fd, &d, seq % 2 ? RTM_NEWADDR : RTM_NEWLINK, seq) == 0 && d.done && d.count >= 1 && d.bad_seq == 0,
              "request %u: done=%d count=%d bad_seq=%d", seq, d.done, d.count, d.bad_seq);
    }
    return 0;
}

// (h) requests sent without reading until the reply queue is full: sending
// never blocks, and the reader still gets to the end -- a dump that was cut
// short is closed with NLMSG_DONE instead of leaving a blocking read hanging.
static int c_queue_full(void) {
    int fd = nl_socket(0);
    CHECK(fd >= 0, "socket/bind: %s", strerror(errno));
    struct req r;
    int64_t t0 = now_us();
    uint32_t last = 0;
    for (uint32_t seq = 1000; seq < 1400; seq++) {
        fill(&r, RTM_GETLINK, NLM_F_DUMP, seq, AF_UNSPEC, sizeof(struct rtgenmsg));
        CHECK(send(fd, &r, r.h.nlmsg_len, 0) == (ssize_t) r.h.nlmsg_len, "send %u: %s", seq, strerror(errno));
        last = seq;
    }
    long send_ms = (long) ((now_us() - t0) / 1000);
    // Drain with blocking reads only. The last request's dump must end.
    char buf[8192];
    long datagrams = 0;
    int ended = 0;
    while (!ended) {
        ssize_t n = recv(fd, buf, sizeof(buf), 0);
        CHECK(n > 0, "blocking recv: %zd (%s)", n, strerror(errno));
        datagrams++;
        for (struct nlmsghdr *h = (struct nlmsghdr *) buf; NLMSG_OK(h, n); h = NLMSG_NEXT(h, n))
            if (h->nlmsg_type == NLMSG_DONE && h->nlmsg_seq == last) ended = 1;
    }
    long ms = (long) ((now_us() - t0) / 1000);
    printf("    NOTE 400 unread dumps: sent in %ldms, %ld datagrams read, last dump ended, %ldms total\n",
           send_ms, datagrams, ms);
    CHECK(recv(fd, buf, sizeof(buf), MSG_DONTWAIT) < 0 && errno == EAGAIN, "queue not empty after the last NLMSG_DONE");
    return 0;
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "route-empty") == 0) route_empty = 1;
        else only = argv[i];
    }
    int probe = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
    if (probe < 0 && errno == EAFNOSUPPORT) {
        // ISH_NETLINK_STUB=0, or a kernel without the stub: nothing to pin.
        printf("SKIP: AF_NETLINK is not available (%s)\n", strerror(errno));
        return 0;
    }
    close(probe);
    run("(a) write() request, blocking recvmsg (busybox ip)", c_write_then_recvmsg, 3000);
    run("(b) send, MSG_PEEK|MSG_TRUNC, then read (iproute2)", c_peek_trunc, 3000);
    run("(c) RTM_GETADDR: 127.0.0.1, family filter", c_getaddr, 3000);
    run("(d) routes/neigh/rules/changes refused at once", c_refused, 3000);
    run("(e) empty queue is EAGAIN when non-blocking", c_nonblock, 3000);
    run("(f) blocking read: signal and SO_RCVTIMEO get through", c_signal, 8000);
    run("(g) Go NetlinkRIB / sendmsg+recvmsg flow", c_go_netlinkrib, 3000);
    run("(h) reply queue full: dump still ends", c_queue_full, 8000);
    printf("%d passed, %d failed\n", pass, fail);
    return fail ? 1 : 0;
}
