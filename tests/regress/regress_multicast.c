// UDP multicast as an mDNS responder uses it, over the loopback so nothing
// leaves the machine.
//
// Linux and Darwin number the multicast options differently and Darwin reads
// some of their arguments differently, and iSH used to map none of
// IP_PKTINFO, IP_ADD_MEMBERSHIP, IP_DROP_MEMBERSHIP, IP_MULTICAST_IF,
// IPV6_ADD_MEMBERSHIP or IPV6_RECVPKTINFO: each was ENOPROTOOPT, so no
// program in the guest could join a group (Rust's mdns-sd, Python's socket).
// Two datagram sockets that set SO_REUSEADDR could not share a port either,
// which Linux allows and an mDNS responder relies on to bind 5353 next to
// the system's. SO_BINDTODEVICE was accepted and ignored, whatever the name.
//
// Every line printed is what arm64 Linux prints as root, except that Linux
// skips the IPv6 datagram: its loopback has no IPv6 multicast route.
//   aarch64-linux-musl-gcc -static -O1 -o mcast regress_multicast.c
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <net/if.h>
#include <poll.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int failed;

static void check(int ok, const char *what) {
    printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok)
        failed = 1;
}

static void check_call(int res, const char *what) {
    if (res < 0) {
        printf("FAIL %s: %s\n", what, strerror(errno));
        failed = 1;
    } else {
        printf("PASS %s\n", what);
    }
}

// Receive one datagram and the IP_PKTINFO / IPV6_PKTINFO that came with it.
static int recv_pktinfo(int fd, int level, int type, void *info, size_t info_len, size_t control_len, int *flags) {
    char buf[64], control[256] __attribute__((aligned(8)));
    struct iovec iov = {buf, sizeof(buf)};
    struct msghdr msg = {.msg_iov = &iov, .msg_iovlen = 1, .msg_control = control, .msg_controllen = control_len};
    struct pollfd pfd = {fd, POLLIN, 0};
    if (poll(&pfd, 1, 2000) != 1)
        return -1;
    int n = recvmsg(fd, &msg, 0);
    if (n < 0)
        return -1;
    *flags = msg.msg_flags;
    int found = 0;
    for (struct cmsghdr *c = CMSG_FIRSTHDR(&msg); c != NULL; c = CMSG_NXTHDR(&msg, c)) {
        if (c->cmsg_level == level && c->cmsg_type == type && c->cmsg_len == CMSG_LEN(info_len)) {
            memcpy(info, CMSG_DATA(c), info_len);
            found = 1;
        }
    }
    return n == 5 && memcmp(buf, "hello", 5) == 0 && found;
}

static void ipv4(unsigned lo) {
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_ANY)};
    socklen_t len = sizeof(addr);
    int one = 1;

    int rx = socket(AF_INET, SOCK_DGRAM, 0);
    int rx2 = socket(AF_INET, SOCK_DGRAM, 0);
    check_call(setsockopt(rx, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)), "SO_REUSEADDR");
    check_call(setsockopt(rx2, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)), "SO_REUSEADDR on a second socket");
    check_call(bind(rx, (void *) &addr, sizeof(addr)), "bind");
    getsockname(rx, (void *) &addr, &len);
    check_call(bind(rx2, (void *) &addr, sizeof(addr)), "a second datagram socket binds the same port");
    close(rx2);
    check_call(setsockopt(rx, IPPROTO_IP, IP_PKTINFO, &one, sizeof(one)), "IP_PKTINFO");

    struct in_addr group, loopback = {htonl(INADDR_LOOPBACK)};
    inet_pton(AF_INET, "239.255.42.99", &group);
    struct ip_mreqn mreqn = {.imr_multiaddr = group, .imr_ifindex = lo};
    check_call(setsockopt(rx, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreqn, sizeof(mreqn)), "IP_ADD_MEMBERSHIP with an interface index");
    struct ip_mreq mreq = {.imr_multiaddr = group, .imr_interface = loopback};
    check_call(setsockopt(rx, IPPROTO_IP, IP_DROP_MEMBERSHIP, &mreq, sizeof(mreq)), "IP_DROP_MEMBERSHIP with an interface address");
    check_call(setsockopt(rx, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)), "IP_ADD_MEMBERSHIP with an interface address");
    int res = setsockopt(rx, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq));
    check(res < 0 && errno == EADDRINUSE, "joining twice is EADDRINUSE");

    int tx = socket(AF_INET, SOCK_DGRAM, 0);
    check_call(setsockopt(tx, IPPROTO_IP, IP_MULTICAST_IF, &loopback, sizeof(loopback)), "IP_MULTICAST_IF with an address");
    struct ip_mreqn ifreq = {.imr_ifindex = lo};
    check_call(setsockopt(tx, IPPROTO_IP, IP_MULTICAST_IF, &ifreq, sizeof(ifreq)), "IP_MULTICAST_IF with an interface index");
    int ttl = 1, loop = 1, value = 0;
    socklen_t value_len = sizeof(value);
    check_call(setsockopt(tx, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl)), "IP_MULTICAST_TTL");
    check_call(setsockopt(tx, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof(loop)), "IP_MULTICAST_LOOP");
    getsockopt(tx, IPPROTO_IP, IP_MULTICAST_TTL, &value, &value_len);
    check(value == 1 && value_len == sizeof(int), "IP_MULTICAST_TTL reads back as an int");

    struct sockaddr_in to = {.sin_family = AF_INET, .sin_port = addr.sin_port, .sin_addr = group};
    check_call(sendto(tx, "hello", 5, 0, (void *) &to, sizeof(to)), "send to the group");
    struct in_pktinfo info = {};
    int flags;
    check(recv_pktinfo(rx, IPPROTO_IP, IP_PKTINFO, &info, sizeof(info), 256, &flags) == 1,
            "the group's datagram arrives with IP_PKTINFO");
    check(info.ipi_ifindex == (int) lo && info.ipi_addr.s_addr == group.s_addr,
            "IP_PKTINFO names the loopback and the group");

    sendto(tx, "hello", 5, 0, (void *) &to, sizeof(to));
    res = recv_pktinfo(rx, IPPROTO_IP, IP_PKTINFO, &info, sizeof(info), 8, &flags);
    check(res == 0 && (flags & MSG_CTRUNC), "a control buffer too small for it is MSG_CTRUNC");
    close(tx);
    close(rx);
}

static void ipv6(unsigned lo) {
    struct sockaddr_in6 addr = {.sin6_family = AF_INET6, .sin6_addr = in6addr_any};
    socklen_t len = sizeof(addr);
    int one = 1;

    int rx = socket(AF_INET6, SOCK_DGRAM, 0);
    check_call(setsockopt(rx, IPPROTO_IPV6, IPV6_RECVPKTINFO, &one, sizeof(one)), "IPV6_RECVPKTINFO");
    check_call(bind(rx, (void *) &addr, sizeof(addr)), "bind");
    getsockname(rx, (void *) &addr, &len);
    struct ipv6_mreq mreq = {.ipv6mr_interface = lo};
    inet_pton(AF_INET6, "ff12::4242", &mreq.ipv6mr_multiaddr);
    check_call(setsockopt(rx, IPPROTO_IPV6, IPV6_ADD_MEMBERSHIP, &mreq, sizeof(mreq)), "IPV6_ADD_MEMBERSHIP");

    int tx = socket(AF_INET6, SOCK_DGRAM, 0);
    int loop = 1, hops = 1;
    check_call(setsockopt(tx, IPPROTO_IPV6, IPV6_MULTICAST_IF, &lo, sizeof(lo)), "IPV6_MULTICAST_IF");
    check_call(setsockopt(tx, IPPROTO_IPV6, IPV6_MULTICAST_LOOP, &loop, sizeof(loop)), "IPV6_MULTICAST_LOOP");
    check_call(setsockopt(tx, IPPROTO_IPV6, IPV6_MULTICAST_HOPS, &hops, sizeof(hops)), "IPV6_MULTICAST_HOPS");
    struct sockaddr_in6 to = {.sin6_family = AF_INET6, .sin6_port = addr.sin6_port,
            .sin6_addr = mreq.ipv6mr_multiaddr, .sin6_scope_id = lo};
    // Linux has no IPv6 multicast route on its loopback; Darwin does.
    if (sendto(tx, "hello", 5, 0, (void *) &to, sizeof(to)) < 0 && errno == ENETUNREACH) {
        printf("SKIP no IPv6 multicast over the loopback here\n");
    } else {
        struct in6_pktinfo info = {};
        int flags;
        check(recv_pktinfo(rx, IPPROTO_IPV6, IPV6_PKTINFO, &info, sizeof(info), 256, &flags) == 1,
                "the group's datagram arrives with IPV6_PKTINFO");
        check(info.ipi6_ifindex == lo && memcmp(&info.ipi6_addr, &mreq.ipv6mr_multiaddr, 16) == 0,
                "IPV6_PKTINFO names the loopback and the group");
    }
    check_call(setsockopt(rx, IPPROTO_IPV6, IPV6_DROP_MEMBERSHIP, &mreq, sizeof(mreq)), "IPV6_DROP_MEMBERSHIP");
    close(tx);
    close(rx);
}

static void bind_to_device(void) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    check_call(setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, "lo", 3), "SO_BINDTODEVICE lo");
    int res = setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, "nosuchif9", 10);
    check(res < 0 && errno == ENODEV, "SO_BINDTODEVICE to a missing interface is ENODEV");
    check_call(setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, "", 0), "SO_BINDTODEVICE with no name unbinds");
    close(fd);
}

int main(void) {
    unsigned lo = if_nametoindex("lo");
    check(lo != 0, "the loopback has an index");
    ipv4(lo);
    ipv6(lo);
    bind_to_device();
    return failed;
}
