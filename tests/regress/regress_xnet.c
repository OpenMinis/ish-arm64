// Transparent TCP forwarding (fs/xnet.h), against tests/regress/
// xnet_fake_entry.py. run.sh starts the fake entry and runs iSH with
// ISH_XNET_CONFIG pointing at:
//   hook=198.18.0.0/15,fd00:1::/64  exempt=198.19.0.0/16
//   self=192.0.2.10,fd00:1::10      conn_id=1
// The fake entry answers CONNECT by the destination port (see its header).
//
//   regress_xnet hook         forwarding, every path a guest can take
//   regress_xnet entry-down   the entry is not there: connections go direct
//   regress_xnet fail-closed  the entry is not there, fail_closed=1
//   regress_xnet direct       prints what plain sockets do, for run.sh to
//                             compare between builds and configurations
//
// Expected values are what arm64 Linux does for a real connection with the
// same outcome.
//   aarch64-linux-musl-gcc -static -O1 -o regress_xnet regress_xnet.c -lpthread
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

static int failed;

static void check(int ok, const char *what) {
    printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok)
        failed = 1;
}

static void check_errno(int res, int want, const char *what) {
    int got = res < 0 ? errno : 0;
    if (got == want) {
        printf("PASS %s\n", what);
    } else {
        printf("FAIL %s: got %s, want %s\n", what, got ? strerror(got) : "success",
                want ? strerror(want) : "success");
        failed = 1;
    }
}

static long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000;
}

// A counter from /proc/ish/xnet.
static long xstat(const char *name) {
    FILE *f = fopen("/proc/ish/xnet", "r");
    if (f == NULL)
        return -1;
    char key[64];
    long value, found = -1;
    while (fscanf(f, "%63s %ld", key, &value) == 2)
        if (strcmp(key, name) == 0)
            found = value;
    fclose(f);
    return found;
}

static struct sockaddr_in v4(const char *ip, int port) {
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(port)};
    inet_pton(AF_INET, ip, &a.sin_addr);
    return a;
}

static struct sockaddr_in6 v6(const char *ip, int port) {
    struct sockaddr_in6 a = {.sin6_family = AF_INET6, .sin6_port = htons(port)};
    inet_pton(AF_INET6, ip, &a.sin6_addr);
    return a;
}

static int tcp(int nonblock) {
    return socket(AF_INET, SOCK_STREAM | (nonblock ? SOCK_NONBLOCK : 0), 0);
}

static int connect4(int fd, const char *ip, int port) {
    struct sockaddr_in a = v4(ip, port);
    return connect(fd, (void *) &a, sizeof(a));
}

// Write a line and read it back.
static int echoes(int fd, const char *text) {
    char buf[128];
    size_t len = strlen(text);
    if (write(fd, text, len) != (ssize_t) len)
        return 0;
    size_t got = 0;
    while (got < len) {
        struct pollfd p = {fd, POLLIN, 0};
        if (poll(&p, 1, 3000) != 1)
            return 0;
        ssize_t n = read(fd, buf + got, sizeof(buf) - got);
        if (n <= 0)
            return 0;
        got += n;
    }
    return got == len && memcmp(buf, text, len) == 0;
}

static int so_error(int fd) {
    int err = -1;
    socklen_t len = sizeof(err);
    getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len);
    return err;
}

static int wait_out(int fd, int ms) {
    struct pollfd p = {fd, POLLOUT, 0};
    if (poll(&p, 1, ms) != 1)
        return 0;
    return p.revents;
}

// --- hook ------------------------------------------------------------------

static void blocking_connect(void) {
    long hooked = xstat("hooked");
    int fd = tcp(0);
    check_errno(connect4(fd, "198.18.0.1", 22), 0, "blocking connect through the entry");
    check(xstat("hooked") == hooked + 1, "it was forwarded");
    char banner[32] = {};
    ssize_t n = read(fd, banner, 14);
    check(n == 14 && memcmp(banner, "SSH-2.0-fake\r\n", 14) == 0,
            "bytes that came with the entry's answer reach the guest");
    check(echoes(fd, "hello\n"), "data flows both ways");

    struct sockaddr_in peer = {}, self = {};
    socklen_t len = sizeof(peer);
    check_errno(getpeername(fd, (void *) &peer, &len), 0, "getpeername");
    char ip[64];
    inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof(ip));
    check(len == sizeof(peer) && peer.sin_family == AF_INET && strcmp(ip, "198.18.0.1") == 0 &&
            ntohs(peer.sin_port) == 22, "getpeername is the destination");
    len = sizeof(self);
    check_errno(getsockname(fd, (void *) &self, &len), 0, "getsockname");
    inet_ntop(AF_INET, &self.sin_addr, ip, sizeof(ip));
    check(self.sin_family == AF_INET && strcmp(ip, "192.0.2.10") == 0 && self.sin_port != 0,
            "getsockname is this node's tailnet address");
    struct sockaddr_in small;
    len = 4;
    check(getpeername(fd, (void *) &small, &len) == 0 && len == sizeof(struct sockaddr_in),
            "a short buffer gets the true length");
    check_errno(connect4(fd, "198.18.0.1", 22), EISCONN, "connect again is EISCONN");
    check(so_error(fd) == 0, "SO_ERROR is 0");
    close(fd);
}

static void nonblocking_connect(void) {
    int fd = tcp(1);
    long start = now_ms();
    check_errno(connect4(fd, "198.18.0.1", 5), EINPROGRESS, "non-blocking connect is EINPROGRESS");
    check_errno(connect4(fd, "198.18.0.1", 5), EALREADY, "connect while in progress is EALREADY");
    char c;
    check_errno(read(fd, &c, 1), EAGAIN, "read before the answer is EAGAIN");
    check_errno(write(fd, "x", 1), EAGAIN, "write before the answer is EAGAIN");
    check(so_error(fd) == 0, "SO_ERROR while in progress is 0");
    struct sockaddr_in peer;
    socklen_t len = sizeof(peer);
    check_errno(getpeername(fd, (void *) &peer, &len), ENOTCONN, "getpeername while in progress is ENOTCONN");
    check(wait_out(fd, 0) == 0, "not writable while the entry has not answered");
    int revents = wait_out(fd, 4000);
    long took = now_ms() - start;
    check(revents == POLLOUT, "writable once it answered");
    check(took >= 800, "and not before");
    check(so_error(fd) == 0, "SO_ERROR is 0");
    check_errno(connect4(fd, "198.18.0.1", 5), 0, "connect after completion is 0, as on Linux");
    check_errno(connect4(fd, "198.18.0.1", 5), EISCONN, "and then EISCONN");
    check(echoes(fd, "nonblocking\n"), "data flows");
    check_errno(read(fd, &c, 1), EAGAIN, "still non-blocking after the swap");
    check(fcntl(fd, F_GETFL) & O_NONBLOCK, "O_NONBLOCK still reads back");
    close(fd);
}

static const struct { int port, err; const char *what; } failures[] = {
    {1, ECONNREFUSED, "0x05 is ECONNREFUSED"},
    {2, EHOSTUNREACH, "0x04 is EHOSTUNREACH"},
    {3, ENETUNREACH, "0x03 is ENETUNREACH"},
    {6, ETIMEDOUT, "0x06 is ETIMEDOUT"},
    {8, EACCES, "0x02 is EACCES"},
    {9, ECONNREFUSED, "another code is ECONNREFUSED"},
    {10, ECONNREFUSED, "an entry that hangs up is ECONNREFUSED"},
};

static void failing_connects(void) {
    char what[96];
    for (size_t i = 0; i < sizeof(failures) / sizeof(failures[0]); i++) {
        int fd = tcp(0);
        snprintf(what, sizeof(what), "blocking: %s", failures[i].what);
        check_errno(connect4(fd, "198.18.0.1", failures[i].port), failures[i].err, what);
        close(fd);

        fd = tcp(1);
        connect4(fd, "198.18.0.1", failures[i].port);
        int revents = wait_out(fd, 3000);
        snprintf(what, sizeof(what), "non-blocking: %s", failures[i].what);
        check((revents & POLLOUT) && (revents & (POLLERR | POLLHUP)) && so_error(fd) == failures[i].err, what);
        close(fd);
    }

    int fd = tcp(1);
    connect4(fd, "198.18.0.1", 1);
    wait_out(fd, 3000);
    check(so_error(fd) == ECONNREFUSED && so_error(fd) == 0, "SO_ERROR reports the failure once");
    check_errno(connect4(fd, "198.18.0.1", 1), ECONNABORTED, "connect after that is ECONNABORTED");
    check_errno(connect4(fd, "198.18.0.1", 7), EINPROGRESS, "and the next one starts over");
    check(wait_out(fd, 3000) == POLLOUT && so_error(fd) == 0 && echoes(fd, "again\n"),
            "and connects");
    close(fd);

    fd = tcp(1);
    connect4(fd, "198.18.0.1", 1);
    wait_out(fd, 3000);
    char c;
    check_errno(read(fd, &c, 1), ECONNREFUSED, "read on a failed connection reports the error");
    check(so_error(fd) == 0, "and SO_ERROR is then clear");
    close(fd);

    fd = tcp(0);
    check_errno(connect4(fd, "198.18.0.1", 2), EHOSTUNREACH, "blocking failure");
    check_errno(connect4(fd, "198.18.0.1", 7), 0, "a blocking socket can connect again after it");
    check(echoes(fd, "retry\n"), "and it works");
    close(fd);
}

static void epoll_modes(void) {
    static const struct { unsigned events; const char *what; } modes[] = {
        {EPOLLOUT, "level-triggered"},
        {EPOLLOUT | EPOLLET, "edge-triggered"},
        {EPOLLOUT | EPOLLONESHOT, "oneshot"},
    };
    char what[96];
    for (int before = 0; before < 2; before++) {
        for (size_t i = 0; i < 3; i++) {
            int ep = epoll_create1(0);
            int fd = tcp(1);
            struct epoll_event ev = {.events = modes[i].events, .data.fd = fd}, out;
            if (before)
                epoll_ctl(ep, EPOLL_CTL_ADD, fd, &ev);
            connect4(fd, "198.18.0.1", 5);
            if (!before)
                epoll_ctl(ep, EPOLL_CTL_ADD, fd, &ev);
            const char *when = before ? "registered before connect" : "registered after connect";
            check(epoll_wait(ep, &out, 1, 0) == 0, "epoll: nothing while in progress");
            int n = epoll_wait(ep, &out, 1, 4000);
            snprintf(what, sizeof(what), "epoll %s, %s: wakes when connected", modes[i].what, when);
            check(n == 1 && out.data.fd == fd && (out.events & EPOLLOUT) && !(out.events & (EPOLLERR | EPOLLHUP)), what);
            n = epoll_wait(ep, &out, 1, 100);
            snprintf(what, sizeof(what), "epoll %s, %s: %s", modes[i].what, when,
                    i == 0 ? "reports it again" : "does not report it again");
            check(i == 0 ? n == 1 : n == 0, what);
            if (i == 2) {
                epoll_ctl(ep, EPOLL_CTL_MOD, fd, &ev);
                check(epoll_wait(ep, &out, 1, 100) == 1, "epoll oneshot: re-armed by EPOLL_CTL_MOD");
            }
            close(fd);
            close(ep);
        }
    }

    // A failure wakes an epoll set too, with EPOLLERR.
    int ep = epoll_create1(0);
    int fd = tcp(1);
    struct epoll_event ev = {.events = EPOLLOUT | EPOLLET, .data.fd = fd}, out;
    epoll_ctl(ep, EPOLL_CTL_ADD, fd, &ev);
    connect4(fd, "198.18.0.1", 2);
    int n = epoll_wait(ep, &out, 1, 3000);
    check(n == 1 && (out.events & (EPOLLERR | EPOLLHUP)) && so_error(fd) == EHOSTUNREACH,
            "epoll: a failed connection wakes the set and SO_ERROR says why");
    close(fd);
    close(ep);
}

static void select_wait(void) {
    int fd = tcp(1);
    connect4(fd, "198.18.0.1", 5);
    fd_set w;
    FD_ZERO(&w);
    FD_SET(fd, &w);
    struct timeval zero = {0, 0};
    check(select(fd + 1, NULL, &w, NULL, &zero) == 0, "select: not writable while in progress");
    FD_ZERO(&w);
    FD_SET(fd, &w);
    struct timeval tv = {4, 0};
    check(select(fd + 1, NULL, &w, NULL, &tv) == 1 && FD_ISSET(fd, &w) && so_error(fd) == 0,
            "select: writable once connected");
    close(fd);
}

static int shared_fd;
static void *poll_waiter(void *arg) {
    (void) arg;
    return (void *) (long) wait_out(shared_fd, 5000);
}
static void *epoll_waiter(void *arg) {
    (void) arg;
    int ep = epoll_create1(0);
    struct epoll_event ev = {.events = EPOLLOUT, .data.fd = shared_fd}, out;
    epoll_ctl(ep, EPOLL_CTL_ADD, shared_fd, &ev);
    long n = epoll_wait(ep, &out, 1, 5000);
    close(ep);
    return (void *) n;
}

static void threads_waiting(void) {
    shared_fd = tcp(1);
    connect4(shared_fd, "198.18.0.1", 5);
    pthread_t a, b, c;
    pthread_create(&a, NULL, poll_waiter, NULL);
    pthread_create(&b, NULL, epoll_waiter, NULL);
    pthread_create(&c, NULL, poll_waiter, NULL);
    void *ra, *rb, *rc;
    pthread_join(a, &ra);
    pthread_join(b, &rb);
    pthread_join(c, &rc);
    check((long) ra == POLLOUT && (long) rb == 1 && (long) rc == POLLOUT,
            "three threads waiting on one connecting socket all wake");
    close(shared_fd);

    // A blocking connect in one thread, a read waiting in another.
    shared_fd = tcp(0);
    pthread_t r;
    pthread_create(&r, NULL, poll_waiter, NULL);
    check_errno(connect4(shared_fd, "198.18.0.1", 5), 0, "blocking connect while another thread polls");
    pthread_join(r, &ra);
    check((long) ra == POLLOUT, "the polling thread wakes too");
    close(shared_fd);
}

static void on_alarm(int sig) {
    (void) sig;
}

static void interrupted_connect(void) {
    struct sigaction sa = {.sa_handler = on_alarm};
    sigaction(SIGALRM, &sa, NULL);
    struct itimerval it = {.it_value = {0, 300000}};
    setitimer(ITIMER_REAL, &it, NULL);
    int fd = tcp(0);
    long start = now_ms();
    check_errno(connect4(fd, "198.18.0.1", 4), EINTR, "a signal interrupts a blocking connect");
    check(now_ms() - start < 2000, "promptly");
    check_errno(connect4(fd, "198.18.0.1", 4), 0, "connect again waits for it, as on Linux");
    check(echoes(fd, "after eintr\n"), "and data flows");
    close(fd);

    // iSH maps only the 64-bit-time number of SO_SNDTIMEO (67), not the 21
    // a 64-bit libc uses; the layout is the same.
    fd = tcp(0);
    struct timeval tv = {0, 300000};
    setsockopt(fd, SOL_SOCKET, 67, &tv, sizeof(tv));
    check_errno(connect4(fd, "198.18.0.1", 4), EINPROGRESS, "SO_SNDTIMEO bounds a blocking connect");
    close(fd);
}

static void passes(void) {
    long local = xstat("pass_local"), exempt = xstat("pass_exempt"), nomatch = xstat("pass_no_match"),
            bound = xstat("pass_bound"), hooked = xstat("hooked");

    int l = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = v4("127.0.0.1", 0);
    socklen_t len = sizeof(a);
    bind(l, (void *) &a, sizeof(a));
    listen(l, 4);
    getsockname(l, (void *) &a, &len);
    int fd = tcp(0);
    check_errno(connect(fd, (void *) &a, sizeof(a)), 0, "loopback connects directly");
    close(fd);
    close(l);
    check(xstat("pass_local") == local + 1, "loopback is never forwarded");

    fd = tcp(1);
    connect4(fd, "198.19.5.5", 80);
    close(fd);
    check(xstat("pass_exempt") == exempt + 1, "an exempt prefix wins over the hook prefix");

    fd = tcp(1);
    connect4(fd, "203.0.113.9", 80);
    close(fd);
    check(xstat("pass_no_match") == nomatch + 1, "a destination no hook covers goes direct");

    fd = tcp(1);
    struct sockaddr_in me = v4("127.0.0.1", 0);
    bind(fd, (void *) &me, sizeof(me));
    connect4(fd, "198.18.0.1", 7);
    close(fd);
    check(xstat("pass_bound") == bound + 1, "a socket bound to an address is left alone");

    fd = tcp(1);
    setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, "lo", 3);
    connect4(fd, "198.18.0.1", 7);
    close(fd);
    check(xstat("pass_bound") == bound + 2, "a socket bound to a device is left alone");

    fd = socket(AF_INET, SOCK_DGRAM, 0);
    connect4(fd, "198.18.0.1", 53);
    close(fd);
    check(xstat("hooked") == hooked, "UDP is not forwarded (yet)");
}

// Darwin reads boolean options back as their flag bit (TCP_NODELAY is 4,
// SO_KEEPALIVE 8), and iSH passes that through; what matters here is that a
// forwarded socket reads back what an ordinary one does.
static void read_options(int fd, int *nodelay, int *keepalive) {
    socklen_t len = sizeof(int);
    *nodelay = *keepalive = -1;
    getsockopt(fd, IPPROTO_TCP, TCP_NODELAY, nodelay, &len);
    len = sizeof(int);
    getsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, keepalive, &len);
}

static void options_survive(void) {
    int one = 1, nodelay, keepalive, want_nodelay, want_keepalive;
    int l = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = v4("127.0.0.1", 0);
    socklen_t len = sizeof(a);
    bind(l, (void *) &a, sizeof(a));
    listen(l, 1);
    getsockname(l, (void *) &a, &len);
    int plain = tcp(0);
    setsockopt(plain, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    setsockopt(plain, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
    connect(plain, (void *) &a, sizeof(a));
    read_options(plain, &want_nodelay, &want_keepalive);
    close(plain);
    close(l);

    int fd = tcp(0);
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
    connect4(fd, "198.18.0.1", 7);
    read_options(fd, &nodelay, &keepalive);
    check(nodelay != 0 && nodelay == want_nodelay, "TCP_NODELAY set before connect survives");
    check(keepalive != 0 && keepalive == want_keepalive, "SO_KEEPALIVE set before connect survives");
    int type = 0;
    len = sizeof(type);
    getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &len);
    check(type == SOCK_STREAM, "SO_TYPE");
    close(fd);
}

// The entry is given up within a second of the close (the waiting thread
// looks once a second).
static int settle(void) {
    long until = now_ms() + 3000;
    while (xstat("pending") != 0 && now_ms() < until)
        usleep(50000);
    return xstat("pending") == 0;
}

static void closed_while_pending(void) {
    settle(); // the SO_SNDTIMEO case above left one behind
    long cancelled = xstat("cancelled");
    int fd = tcp(1);
    connect4(fd, "198.18.0.1", 4);
    close(fd);
    check(settle(), "closing a connecting socket gives the entry up");
    check(xstat("cancelled") == cancelled + 1, "and counts it as cancelled");
}

static void ipv6(void) {
    int fd = socket(AF_INET6, SOCK_STREAM, 0);
    struct sockaddr_in6 a = v6("fd00:1::1", 7), peer = {}, self = {};
    check_errno(connect(fd, (void *) &a, sizeof(a)), 0, "IPv6 connect through the entry");
    check(echoes(fd, "six\n"), "data flows");
    socklen_t len = sizeof(peer);
    getpeername(fd, (void *) &peer, &len);
    check(len == sizeof(peer) && peer.sin6_family == AF_INET6 &&
            memcmp(&peer.sin6_addr, &a.sin6_addr, 16) == 0 && peer.sin6_port == a.sin6_port,
            "IPv6 getpeername is the destination");
    len = sizeof(self);
    getsockname(fd, (void *) &self, &len);
    struct sockaddr_in6 want = v6("fd00:1::10", 0);
    check(self.sin6_family == AF_INET6 && memcmp(&self.sin6_addr, &want.sin6_addr, 16) == 0,
            "IPv6 getsockname is this node's tailnet address");
    close(fd);

    fd = socket(AF_INET6, SOCK_STREAM, 0);
    a = v6("::ffff:198.18.0.1", 7);
    check_errno(connect(fd, (void *) &a, sizeof(a)), 0, "a v4-mapped destination is forwarded");
    check(echoes(fd, "mapped\n"), "data flows");
    len = sizeof(self);
    getsockname(fd, (void *) &self, &len);
    want = v6("::ffff:192.0.2.10", 0);
    check(memcmp(&self.sin6_addr, &want.sin6_addr, 16) == 0, "its getsockname is v4-mapped");
    int v6only = 1;
    check_errno(setsockopt(fd, IPPROTO_IPV6, IPV6_TCLASS, &v6only, sizeof(v6only)), 0,
            "IPv6 options still apply to the socket");
    close(fd);
}

static void many_at_once(void) {
    enum { N = 24 };
    int fds[N];
    for (int i = 0; i < N; i++) {
        fds[i] = tcp(1);
        connect4(fds[i], "198.18.0.1", i % 2 ? 5 : 7);
    }
    int ok = 0;
    for (int i = 0; i < N; i++) {
        if (wait_out(fds[i], 5000) == POLLOUT && so_error(fds[i]) == 0 && echoes(fds[i], "many\n"))
            ok++;
        close(fds[i]);
    }
    check(ok == N, "24 connections at once");
}

// --- entry down ------------------------------------------------------------

static void entry_down(int closed) {
    long down = xstat("pass_entry_down"), refused = xstat("refused_closed");
    for (int i = 0; i < 2; i++) {
        int fd = tcp(1);
        int res = connect4(fd, "198.18.0.1", 7);
        if (closed)
            check_errno(res, ENETUNREACH, "entry down, fail-closed: ENETUNREACH");
        else
            check(res == 0 || errno != ENETUNREACH, "entry down: the connection went out directly");
        close(fd);
    }
    if (closed)
        check(xstat("refused_closed") == refused + 2, "counted as refused");
    else
        check(xstat("pass_entry_down") == down + 2, "counted as entry-down passes");
    check(xstat("entry_down_events") == 1, "the entry was tried once, then left alone");
    check(xstat("hooked") == 0, "nothing forwarded");
}

// --- direct ----------------------------------------------------------------

#define SHOW(expr) do { errno = 0; long r_ = (long) (expr); printf("%-60s = %ld errno=%d\n", #expr, r_, r_ < 0 ? errno : 0); } while (0)

static void direct(void) {
    int l = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = v4("127.0.0.1", 0), p;
    socklen_t len = sizeof(a);
    SHOW(bind(l, (void *) &a, sizeof(a)));
    SHOW(listen(l, 4));
    getsockname(l, (void *) &a, &len);
    int fd = tcp(0);
    SHOW(connect(fd, (void *) &a, sizeof(a)));
    SHOW(connect(fd, (void *) &a, sizeof(a)));
    int c = accept(l, NULL, NULL);
    SHOW(write(fd, "abc", 3));
    char buf[8];
    SHOW(read(c, buf, sizeof(buf)));
    len = sizeof(p);
    SHOW(getpeername(fd, (void *) &p, &len));
    printf("peer family=%d len=%d same=%d\n", p.sin_family, len, p.sin_port == a.sin_port);
    len = 4;
    SHOW(getsockname(fd, (void *) &p, &len));
    printf("short getsockname len=%d\n", len);
    SHOW(so_error(fd));
    close(c);
    close(fd);

    // Refused, non-blocking: the listener is gone.
    close(l);
    fd = tcp(1);
    SHOW(connect(fd, (void *) &a, sizeof(a)));
    int revents = wait_out(fd, 2000);
    printf("refused revents has POLLOUT=%d POLLERR|POLLHUP=%d\n", !!(revents & POLLOUT), !!(revents & (POLLERR | POLLHUP)));
    SHOW(so_error(fd));
    SHOW(so_error(fd));
    close(fd);
    fd = tcp(0);
    SHOW(connect(fd, (void *) &a, sizeof(a)));
    close(fd);

    fd = tcp(1);
    SHOW(read(fd, buf, 1));
    SHOW(getpeername(fd, (void *) &p, &len));
    close(fd);

    int u = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in ua = v4("127.0.0.1", 0);
    len = sizeof(ua);
    bind(u, (void *) &ua, sizeof(ua));
    getsockname(u, (void *) &ua, &len);
    SHOW(sendto(u, "u", 1, 0, (void *) &ua, sizeof(ua)));
    SHOW(recv(u, buf, sizeof(buf), 0));
    close(u);
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    const char *mode = argc > 1 ? argv[1] : "hook";
    if (strcmp(mode, "direct") == 0) {
        direct();
        return 0;
    }
    if (strcmp(mode, "entry-down") == 0 || strcmp(mode, "fail-closed") == 0) {
        entry_down(strcmp(mode, "fail-closed") == 0);
        return failed;
    }
    check(xstat("enabled") == 1 && xstat("configured") == 1, "/proc/ish/xnet: on and configured");
    blocking_connect();
    nonblocking_connect();
    failing_connects();
    epoll_modes();
    select_wait();
    threads_waiting();
    interrupted_connect();
    passes();
    options_survive();
    closed_while_pending();
    ipv6();
    many_at_once();
    check(xstat("pending") == 0, "nothing left waiting at the end");
    return failed;
}
