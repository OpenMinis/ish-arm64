// Transparent UDP and DNS forwarding (fs/xnet.h, stage 2), against
// tests/regress/xnet_fake_entry.py. run.sh runs iSH with ISH_XNET_CONFIG:
//   hook=198.18.0.0/15,fd00:1::/64,10.99.0.0/16
//   udp_hook=198.18.0.0/15,fd00:1::/64     (10.99/16: TCP only, a proxy route)
//   exempt=198.19.0.0/16
//   resolvers=198.18.0.53,10.250.0.53      dns_intercept=1
// The fake entry's relay answers by the destination port (see its header).
//
//   regress_xnet_udp udp          forwarding, every path a guest can take
//   regress_xnet_udp entry-down   the entry is not there: datagrams go direct
//   regress_xnet_udp fail-closed  the entry is not there, fail_closed=1
//   regress_xnet_udp noudp        the entry refuses UDP ASSOCIATE
//   regress_xnet_udp direct       prints what plain datagram sockets do, for
//                                 run.sh to compare between configurations
//
//   aarch64-linux-musl-gcc -static -O1 -o regress_xnet_udp regress_xnet_udp.c
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

static int failed;

static void check(int ok, const char *what) {
    printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok)
        failed = 1;
}

static void check_errno(long res, int want, const char *what) {
    int got = res < 0 ? errno : 0;
    if (got == want) {
        printf("PASS %s\n", what);
    } else {
        printf("FAIL %s: got %s, want %s\n", what, got ? strerror(got) : "success",
                want ? strerror(want) : "success");
        failed = 1;
    }
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

static int udp(int nonblock) {
    return socket(AF_INET, SOCK_DGRAM | (nonblock ? SOCK_NONBLOCK : 0), 0);
}

static int readable(int fd, int ms) {
    struct pollfd p = {fd, POLLIN, 0};
    return poll(&p, 1, ms) == 1 && (p.revents & POLLIN);
}

static long send4(int fd, const char *ip, int port, const void *data, size_t len) {
    struct sockaddr_in a = v4(ip, port);
    return sendto(fd, data, len, 0, (void *) &a, sizeof(a));
}

// recvfrom with a timeout; the source in *from.
static long recv_from(int fd, void *buf, size_t len, struct sockaddr_in *from) {
    if (!readable(fd, 3000))
        return -2;
    socklen_t flen = sizeof(*from);
    memset(from, 0, sizeof(*from));
    return recvfrom(fd, buf, len, 0, (void *) from, &flen);
}

static int from_is(const struct sockaddr_in *a, const char *ip, int port) {
    struct sockaddr_in want = v4(ip, port);
    return a->sin_family == AF_INET && a->sin_port == want.sin_port && a->sin_addr.s_addr == want.sin_addr.s_addr;
}

// --- udp ---------------------------------------------------------------------

static void round_trips(void) {
    long sent = xstat("udp_sent"), received = xstat("udp_received"), opened = xstat("udp_assoc_opened");
    int fd = udp(0);
    char buf[256];
    struct sockaddr_in from;
    check(send4(fd, "198.18.0.1", 7, "hello", 5) == 5, "sendto a hooked destination returns the payload length");
    long n = recv_from(fd, buf, sizeof(buf), &from);
    check(n == 5 && memcmp(buf, "hello", 5) == 0, "the echo comes back without the SOCKS header");
    check(from_is(&from, "198.18.0.1", 7), "recvfrom reports the real source, not the relay");
    check(send4(fd, "198.18.0.2", 7, "second", 6) == 6 && recv_from(fd, buf, sizeof(buf), &from) == 6 &&
            from_is(&from, "198.18.0.2", 7), "a second destination on the same socket");
    check(xstat("udp_assoc_opened") == opened + 1, "one association for both destinations");
    check(xstat("udp_sent") == sent + 2 && xstat("udp_received") == received + 2, "counted: 2 sent, 2 received");

    // Several datagrams in a row keep their boundaries.
    send4(fd, "198.18.0.3", 19, "x", 1);
    int ok = 1;
    for (int i = 1; i <= 3; i++) {
        n = recv_from(fd, buf, sizeof(buf), &from);
        ok &= n == 1 && buf[0] == '0' + i && from_is(&from, "198.18.0.3", 19);
    }
    check(ok, "three datagrams arrive as three, in order");

    // A bigger one (Darwin's loopback takes 9216 bytes by default).
    static char big[4000], back[4100];
    for (size_t i = 0; i < sizeof(big); i++)
        big[i] = (char) i;
    check(send4(fd, "198.18.0.1", 7, big, sizeof(big)) == sizeof(big) &&
            recv_from(fd, back, sizeof(back), &from) == sizeof(big) && memcmp(big, back, sizeof(big)) == 0,
            "a 4000-byte datagram round-trips intact");
    static char huge[65500];
    errno = 0;
    check_errno(send4(fd, "198.18.0.1", 7, huge, sizeof(huge)), EMSGSIZE,
            "a datagram with no room left for the header: EMSGSIZE");
    close(fd);
}

static void msg_calls(void) {
    int fd = udp(0);
    struct sockaddr_in to = v4("198.18.0.1", 7), from;
    char a[] = "two ", b[] = "parts";
    struct iovec out[2] = {{a, 4}, {b, 5}};
    struct msghdr m = {.msg_name = &to, .msg_namelen = sizeof(to), .msg_iov = out, .msg_iovlen = 2};
    check(sendmsg(fd, &m, 0) == 9, "sendmsg with two iovecs");
    char x[3], y[16];
    struct iovec in[2] = {{x, 3}, {y, sizeof(y)}};
    struct msghdr r = {.msg_name = &from, .msg_namelen = sizeof(from), .msg_iov = in, .msg_iovlen = 2};
    readable(fd, 3000);
    long n = recvmsg(fd, &r, 0);
    check(n == 9 && memcmp(x, "two", 3) == 0 && memcmp(y, " parts", 6) == 0, "recvmsg scatters the payload");
    check(r.msg_namelen == sizeof(from) && from_is(&from, "198.18.0.1", 7), "recvmsg names the real source");

    // MSG_PEEK, then the same datagram again; MSG_TRUNC both ways.
    send4(fd, "198.18.0.1", 7, "0123456789", 10);
    readable(fd, 3000);
    char buf[16];
    socklen_t flen = sizeof(from);
    n = recvfrom(fd, buf, 4, MSG_PEEK, (void *) &from, &flen);
    check(n == 4 && memcmp(buf, "0123", 4) == 0 && from_is(&from, "198.18.0.1", 7), "MSG_PEEK sees the payload and source");
    struct iovec small = {buf, 4};
    struct msghdr t = {.msg_iov = &small, .msg_iovlen = 1};
    n = recvmsg(fd, &t, MSG_PEEK);
    check(n == 4 && (t.msg_flags & MSG_TRUNC), "a short buffer: 4 bytes and MSG_TRUNC in msg_flags");
    n = recv(fd, buf, 4, MSG_TRUNC);
    check(n == 10, "recv(MSG_TRUNC) returns the whole payload length, not the header's");
    check(!readable(fd, 200), "and the datagram is gone");

    // sendmmsg / recvmmsg
    char p1[] = "m1", p2[] = "m2", p3[] = "m3";
    struct iovec iv[3] = {{p1, 2}, {p2, 2}, {p3, 2}};
    struct mmsghdr mm[3];
    memset(mm, 0, sizeof(mm));
    for (int i = 0; i < 3; i++) {
        mm[i].msg_hdr.msg_name = &to;
        mm[i].msg_hdr.msg_namelen = sizeof(to);
        mm[i].msg_hdr.msg_iov = &iv[i];
        mm[i].msg_hdr.msg_iovlen = 1;
    }
    check(sendmmsg(fd, mm, 3, 0) == 3, "sendmmsg sends three");
    char rb[3][8];
    struct iovec ri[3] = {{rb[0], 8}, {rb[1], 8}, {rb[2], 8}};
    struct sockaddr_in rf[3];
    struct mmsghdr rm[3];
    memset(rm, 0, sizeof(rm));
    for (int i = 0; i < 3; i++) {
        rm[i].msg_hdr.msg_name = &rf[i];
        rm[i].msg_hdr.msg_namelen = sizeof(rf[i]);
        rm[i].msg_hdr.msg_iov = &ri[i];
        rm[i].msg_hdr.msg_iovlen = 1;
    }
    usleep(200 * 1000);
    int got = recvmmsg(fd, rm, 3, MSG_DONTWAIT, NULL);
    int ok = got == 3;
    for (int i = 0; ok && i < 3; i++)
        ok = rm[i].msg_len == 2 && rb[i][0] == 'm' && rb[i][1] == '1' + i && from_is(&rf[i], "198.18.0.1", 7);
    check(ok, "recvmmsg takes three, each with its source");
    close(fd);
}

static void waiting(void) {
    int fd = udp(1);
    char buf[16];
    errno = 0;
    check_errno(recv(fd, buf, sizeof(buf), 0), EAGAIN, "non-blocking recv with nothing there: EAGAIN");
    int ep = epoll_create1(0);
    struct epoll_event ev = {.events = EPOLLIN, .data.fd = fd}, got;
    epoll_ctl(ep, EPOLL_CTL_ADD, fd, &ev);
    send4(fd, "198.18.0.1", 7, "ep", 2);
    check(epoll_wait(ep, &got, 1, 3000) == 1 && got.data.fd == fd, "epoll sees the relayed reply");
    check(recv(fd, buf, sizeof(buf), 0) == 2, "and it reads");
    check(epoll_wait(ep, &got, 1, 200) == 0, "then nothing more");
    close(ep);
    send4(fd, "198.18.0.1", 9, "void", 4);
    check(!readable(fd, 300), "a destination that does not answer: poll keeps waiting");
    close(fd);
}

static void connected(void) {
    int fd = udp(0);
    struct sockaddr_in to = v4("198.18.0.4", 13), peer, from;
    check(connect(fd, (void *) &to, sizeof(to)) == 0, "connect() to a hooked destination");
    socklen_t len = sizeof(peer);
    check(getpeername(fd, (void *) &peer, &len) == 0 && from_is(&peer, "198.18.0.4", 13),
            "getpeername() is the destination");
    check(send(fd, "c1", 2, 0) == 2, "send() without an address");
    char buf[16];
    // Port 13 answers from port 14 first: a connected socket never sees it.
    long n = recv_from(fd, buf, sizeof(buf), &from);
    check(n == 2 && memcmp(buf, "c1", 2) == 0 && from_is(&from, "198.18.0.4", 13),
            "only the peer's datagram is delivered, the stray one is filtered");
    check(write(fd, "w2", 2) == 2, "write()");
    check(readable(fd, 3000) && read(fd, buf, sizeof(buf)) == 2 && memcmp(buf, "w2", 2) == 0, "read()");
    check(xstat("udp_dropped") >= 2, "the filtered datagrams are counted");
    struct sockaddr unspec = {.sa_family = AF_UNSPEC};
    check(connect(fd, &unspec, sizeof(unspec)) == 0, "connect(AF_UNSPEC) dissolves it");
    check(xstat("udp_assoc_active") == 1, "the association stays for later sendto()s");
    len = sizeof(peer);
    errno = 0;
    check_errno(getpeername(fd, (void *) &peer, &len), ENOTCONN, "then getpeername(): ENOTCONN");
    errno = 0;
    check_errno(send(fd, "x", 1, 0), EDESTADDRREQ, "and send() without an address: EDESTADDRREQ");
    close(fd);
}

static void renewal(void) {
    long renewed = xstat("udp_assoc_renewed");
    int fd = udp(0);
    char buf[16];
    struct sockaddr_in from;
    check(send4(fd, "198.18.0.1", 99, "bye", 3) == 3 && recv_from(fd, buf, sizeof(buf), &from) == 3,
            "the last echo before the entry ends the association");
    usleep(200 * 1000);
    check(send4(fd, "198.18.0.1", 7, "again", 5) == 5 && recv_from(fd, buf, sizeof(buf), &from) == 5 &&
            from_is(&from, "198.18.0.1", 7), "the next datagram opens a new association and gets through");
    check(xstat("udp_assoc_renewed") == renewed + 1, "counted as renewed");
    close(fd);
}

static void ipv6(void) {
    int fd = socket(AF_INET6, SOCK_DGRAM, 0);
    struct sockaddr_in6 to = v6("fd00:1::5", 7), from;
    char buf[16];
    check(sendto(fd, "six", 3, 0, (void *) &to, sizeof(to)) == 3, "IPv6 socket, IPv6 destination");
    socklen_t len = sizeof(from);
    check(readable(fd, 3000) && recvfrom(fd, buf, sizeof(buf), 0, (void *) &from, &len) == 3 &&
            from.sin6_family == AF_INET6 && memcmp(&from.sin6_addr, &to.sin6_addr, 16) == 0 &&
            from.sin6_port == to.sin6_port, "the reply names the IPv6 source");
    struct sockaddr_in6 mapped = v6("::ffff:198.18.0.1", 7);
    check(sendto(fd, "map", 3, 0, (void *) &mapped, sizeof(mapped)) == 3, "IPv6 socket, v4-mapped destination");
    len = sizeof(from);
    check(readable(fd, 3000) && recvfrom(fd, buf, sizeof(buf), 0, (void *) &from, &len) == 3 &&
            memcmp(&from.sin6_addr, &mapped.sin6_addr, 16) == 0, "the reply comes from the v4-mapped source");
    close(fd);

    long v6only = xstat("udp_pass_v6only");
    fd = socket(AF_INET6, SOCK_DGRAM, 0);
    int one = 1;
    setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &one, sizeof(one));
    sendto(fd, "x", 1, 0, (void *) &to, sizeof(to));
    check(xstat("udp_pass_v6only") == v6only + 1, "an IPV6_V6ONLY socket goes direct (it cannot reach the relay)");
    close(fd);
}

static void dns(void) {
    // A query for a tailnet name, to a resolver inside the hook range and
    // to one outside it: both are intercepted as DNS.
    static const unsigned char q[] = {
        0x12, 0x34, 0x01, 0x00, 0, 1, 0, 0, 0, 0, 0, 0,
        8, 't', 'a', 'i', 'l', 'n', 'o', 'd', 'e', 7, 'e', 'x', 'a', 'm', 'p', 'l', 'e',
        2, 't', 's', 3, 'n', 'e', 't', 0, 0, 1, 0, 1};
    const char *resolvers[] = {"198.18.0.53", "10.250.0.53"};
    long dns_sent = xstat("udp_dns_sent");
    for (int i = 0; i < 2; i++) {
        int fd = udp(1);
        unsigned char buf[512];
        struct sockaddr_in from;
        send4(fd, resolvers[i], 53, q, sizeof(q));
        long n = recv_from(fd, buf, sizeof(buf), &from);
        char what[128];
        snprintf(what, sizeof(what), "UDP DNS via %s: the answer, from the resolver, id kept", resolvers[i]);
        check(n > (long) sizeof(q) && buf[0] == 0x12 && buf[1] == 0x34 && from_is(&from, resolvers[i], 53), what);
        check(n >= 4 && memcmp(buf + n - 4, "\x64\x40\x00\x07", 4) == 0, "the answer is the tailnet address");
        close(fd);
    }
    check(xstat("udp_dns_sent") == dns_sent + 2, "counted as DNS datagrams");

    // DNS over TCP to a resolver.
    long tcp_dns = xstat("dns_tcp_hooked");
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in r = v4("10.250.0.53", 53);
    check(connect(fd, (void *) &r, sizeof(r)) == 0, "TCP to a resolver's port 53 connects through the entry");
    unsigned char frame[2 + sizeof(q)], ans[512];
    frame[0] = 0;
    frame[1] = sizeof(q);
    memcpy(frame + 2, q, sizeof(q));
    long n = -1;
    if (write(fd, frame, sizeof(frame)) == sizeof(frame)) {
        n = 0;
        while (n < 2 + (long) sizeof(q) + 16) {
            struct pollfd p = {fd, POLLIN, 0};
            if (poll(&p, 1, 3000) != 1)
                break;
            long k = read(fd, ans + n, sizeof(ans) - n);
            if (k <= 0)
                break;
            n += k;
        }
    }
    check(n > 2 + (long) sizeof(q) && ans[2] == 0x12 && ans[3] == 0x34, "a DNS answer over TCP");
    check(xstat("dns_tcp_hooked") == tcp_dns + 1, "counted as a DNS connection");
    close(fd);
}

static void passes(void) {
    int fd = udp(1);
    long local = xstat("udp_pass_local"), exempt = xstat("udp_pass_exempt"),
         proxy = xstat("udp_pass_proxy_route"), nomatch = xstat("udp_pass_no_match"), sent = xstat("udp_sent");
    send4(fd, "198.19.0.1", 7, "e", 1);
    send4(fd, "10.99.0.1", 7, "p", 1);
    send4(fd, "192.0.2.1", 7, "n", 1);
    check(xstat("udp_pass_exempt") == exempt + 1, "exempt destination: direct");
    check(xstat("udp_pass_proxy_route") == proxy + 1, "TCP-only hook prefix: udp-direct(proxy-route)");
    check(xstat("udp_pass_no_match") == nomatch + 1, "no hook prefix: direct");
    check(xstat("udp_sent") == sent, "none of them went to the relay");
    close(fd);

    // Loopback: a local echo works exactly as before, and a datagram from a
    // local port that is not the relay is never unwrapped, even when it
    // looks like a SOCKS UDP header.
    int a = udp(0), b = udp(0);
    struct sockaddr_in la = v4("127.0.0.1", 0), lb = v4("127.0.0.1", 0);
    socklen_t len = sizeof(la);
    bind(b, (void *) &lb, sizeof(lb));
    len = sizeof(lb);
    getsockname(b, (void *) &lb, &len);
    send4(a, "198.18.0.1", 7, "open", 4); // a now has an association
    char buf[64];
    struct sockaddr_in from;
    recv_from(a, buf, sizeof(buf), &from);
    len = sizeof(la);
    getsockname(a, (void *) &la, &len);
    la.sin_addr.s_addr = htonl(INADDR_LOOPBACK); // xnet bound it to the wildcard
    static const unsigned char fake[] = {0, 0, 0, 1, 198, 18, 0, 9, 0, 7, 'f', 'a', 'k', 'e'};
    sendto(b, fake, sizeof(fake), 0, (void *) &la, sizeof(la));
    long n = recv_from(a, buf, sizeof(buf), &from);
    check(n == sizeof(fake) && memcmp(buf, fake, sizeof(fake)) == 0 && from.sin_port == lb.sin_port,
            "a header-shaped datagram from another local port is delivered as is, from that port");
    check(xstat("udp_pass_local") >= local, "loopback is never forwarded");
    close(a);
    close(b);

    long bound = xstat("udp_pass_bound");
    fd = udp(1);
    struct sockaddr_in me = v4("127.0.0.1", 0);
    bind(fd, (void *) &me, sizeof(me));
    send4(fd, "198.18.0.1", 7, "b", 1);
    check(xstat("udp_pass_bound") == bound + 1, "a socket bound to an address goes its own way");
    close(fd);
}

// --- entry down / refusing UDP --------------------------------------------------

static void entry_down(int closed) {
    long down = xstat("udp_pass_entry_down"), refused = xstat("udp_refused_closed");
    int fd = udp(1);
    for (int i = 0; i < 2; i++) {
        long res = send4(fd, "198.18.0.1", 7, "d", 1);
        if (closed)
            check_errno(res, ENETUNREACH, "entry down, fail-closed: ENETUNREACH");
        else
            check(res == 1, "entry down: the datagram went out directly");
    }
    close(fd);
    if (closed)
        check(xstat("udp_refused_closed") == refused + 2, "counted as refused");
    else
        check(xstat("udp_pass_entry_down") == down + 2, "counted as entry-down passes");
    check(xstat("udp_sent") == 0 && xstat("udp_assoc_active") == 0, "nothing relayed, nothing left open");
}

static void no_udp(void) {
    int fd = udp(1);
    check(send4(fd, "198.18.0.1", 7, "a", 1) == 1, "the first datagram goes out directly");
    send4(fd, "198.18.0.1", 7, "b", 1);
    check(xstat("udp_assoc_failed") == 1, "the refused association is tried once, then left alone");
    check(xstat("udp_pass_entry_down") == 2 && xstat("udp_sent") == 0, "both datagrams went out directly");
    check(send4(fd, "198.18.0.1", 7, "c", 1) == 1, "and sendto() succeeds");
    close(fd);
    int t = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = v4("198.18.0.1", 7);
    check(connect(t, (void *) &a, sizeof(a)) == 0 && xstat("hooked") == 1,
            "TCP still goes through the entry (only UDP was refused)");
    close(t);
}

// --- direct ----------------------------------------------------------------------

#define SHOW(expr) do { errno = 0; long r_ = (long) (expr); printf("%-64s = %ld errno=%d\n", #expr, r_, r_ < 0 ? errno : 0); } while (0)

static void direct(void) {
    int a = udp(0), b = udp(1);
    struct sockaddr_in la = v4("127.0.0.1", 0), lb = v4("127.0.0.1", 0), from;
    socklen_t len = sizeof(la);
    SHOW(bind(a, (void *) &la, sizeof(la)));
    getsockname(a, (void *) &la, &len);
    char buf[16];
    SHOW(recv(b, buf, sizeof(buf), 0));
    SHOW(sendto(b, "0123456789", 10, 0, (void *) &la, sizeof(la)));
    readable(a, 2000);
    SHOW(recv(a, buf, 4, MSG_PEEK));
    SHOW(recv(a, buf, 4, MSG_TRUNC));
    SHOW(connect(a, (void *) &lb, sizeof(lb)));
    len = sizeof(from);
    SHOW(getpeername(a, (void *) &from, &len));
    struct sockaddr unspec = {.sa_family = AF_UNSPEC};
    SHOW(connect(a, &unspec, sizeof(unspec)));
    len = sizeof(from);
    SHOW(getpeername(a, (void *) &from, &len));
    SHOW(send(a, "x", 1, 0));
    struct iovec iov = {buf, sizeof(buf)};
    struct msghdr m = {.msg_iov = &iov, .msg_iovlen = 1};
    SHOW(recvmsg(b, &m, MSG_DONTWAIT));
    // Resolver and hooked addresses, from a socket the switch never touches
    // while off: the same results on and off are what run.sh compares, so
    // only addresses that answer the same way in both are used here.
    SHOW(sendto(b, "y", 1, 0, (void *) &la, sizeof(la)));
    readable(a, 2000);
    len = sizeof(from);
    SHOW(recvfrom(a, buf, sizeof(buf), 0, (void *) &from, &len));
    printf("from loopback=%d\n", from.sin_addr.s_addr == htonl(INADDR_LOOPBACK));
    close(a);
    close(b);
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    const char *mode = argc > 1 ? argv[1] : "udp";
    if (strcmp(mode, "direct") == 0) {
        direct();
        return 0;
    }
    if (strcmp(mode, "entry-down") == 0 || strcmp(mode, "fail-closed") == 0) {
        entry_down(strcmp(mode, "fail-closed") == 0);
        return failed;
    }
    if (strcmp(mode, "noudp") == 0) {
        no_udp();
        return failed;
    }
    check(xstat("dns_intercept") == 1 && xstat("udp_hook_prefixes") == 2 && xstat("resolvers") == 2,
            "/proc/ish/xnet: the UDP snapshot is in");
    round_trips();
    msg_calls();
    waiting();
    connected();
    renewal();
    ipv6();
    dns();
    passes();
    usleep(100 * 1000);
    check(xstat("udp_assoc_active") == 0, "every association closed with its socket");
    return failed;
}
