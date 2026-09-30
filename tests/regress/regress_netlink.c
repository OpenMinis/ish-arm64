// Linux native oracle and iSH guest test. cc -static -O2 -Wall -o /tmp/netlink this.c
#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <net/if.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

static unsigned checks;
#define CHECK(c) do { checks++; if (!(c)) { fprintf(stderr, "FAIL line %d: %s (errno=%d)\n", __LINE__, #c, errno); return 1; } } while (0)
struct request { struct nlmsghdr h; struct ifinfomsg info; };
static struct request request(unsigned type, unsigned seq) {
    struct request r = { .h = {sizeof(r), type, NLM_F_REQUEST | NLM_F_DUMP, seq, 0} };
    return r;
}
int main(int argc, char **argv) {
    int ish = argc > 1 && !strcmp(argv[1], "ish");
    int fd = socket(AF_NETLINK, SOCK_RAW | SOCK_NONBLOCK | SOCK_CLOEXEC, NETLINK_ROUTE);
    CHECK(fd >= 0);
    CHECK(fcntl(fd, F_GETFL) & O_NONBLOCK);
    CHECK(fcntl(fd, F_GETFD) & FD_CLOEXEC);
    struct sockaddr_nl local = {.nl_family = AF_NETLINK}, kernel = {.nl_family = AF_NETLINK};
    CHECK(bind(fd, (void *) &local, sizeof(local)) == 0);
    socklen_t len = sizeof(local);
    CHECK(getsockname(fd, (void *) &local, &len) == 0);
    CHECK(len == sizeof(local) && local.nl_family == AF_NETLINK && local.nl_pid != 0);
    char buf[8192];
    CHECK(recv(fd, buf, sizeof(buf), 0) == -1 && errno == EAGAIN);
    struct pollfd p = {fd, POLLIN, 0};
    CHECK(poll(&p, 1, 0) == 0);
    int ep = epoll_create1(EPOLL_CLOEXEC);
    CHECK(ep >= 0);
    struct epoll_event event = {.events = EPOLLIN, .data.fd = fd};
    CHECK(epoll_ctl(ep, EPOLL_CTL_ADD, fd, &event) == 0);
    CHECK(epoll_wait(ep, &event, 1, 0) == 0);
    int links = 0, addresses = 0, up = 0;
    for (unsigned type = RTM_GETLINK; type <= RTM_GETADDR; type += 4) {
        struct request req = request(type, type);
        struct iovec siov[] = {{&req, 7}, {(char *) &req + 7, sizeof(req) - 7}};
        struct msghdr sm = {.msg_name = &kernel, .msg_namelen = sizeof(kernel), .msg_iov = siov, .msg_iovlen = 2};
        CHECK(sendmsg(fd, &sm, 0) == sizeof(req));
        CHECK(poll(&p, 1, 1000) == 1 && (p.revents & POLLIN));
        CHECK(epoll_wait(ep, &event, 1, 1000) == 1 && (event.events & EPOLLIN));
        // Peeking a short buffer must not consume the datagram.
        char tiny[5];
        struct sockaddr_nl sender;
        struct iovec piov = {tiny, sizeof(tiny)};
        struct msghdr pm = {.msg_name = &sender, .msg_namelen = sizeof(sender), .msg_iov = &piov, .msg_iovlen = 1};
        int peek = recvmsg(fd, &pm, MSG_PEEK | MSG_TRUNC);
        CHECK(peek > sizeof(tiny) && (pm.msg_flags & MSG_TRUNC));
        CHECK(pm.msg_namelen == sizeof(sender) && sender.nl_family == AF_NETLINK && sender.nl_pid == 0);
        int done = 0;
        while (!done) {
            CHECK(poll(&p, 1, 1000) == 1);
            struct iovec riov[] = {{buf, 11}, {buf + 11, sizeof(buf) - 11}};
            struct msghdr rm = {.msg_name = &sender, .msg_namelen = sizeof(sender), .msg_iov = riov, .msg_iovlen = 2};
            int n = recvmsg(fd, &rm, 0);
            CHECK(n >= NLMSG_HDRLEN && !(rm.msg_flags & MSG_TRUNC));
            CHECK(sender.nl_family == AF_NETLINK && sender.nl_pid == 0);
            CHECK(rm.msg_namelen == sizeof(sender) && rm.msg_controllen == 0);
            for (struct nlmsghdr *h = (void *) buf; NLMSG_OK(h, n); h = NLMSG_NEXT(h, n)) {
                CHECK(h->nlmsg_seq == type && h->nlmsg_pid == local.nl_pid);
                CHECK(h->nlmsg_flags & NLM_F_MULTI);
                if (h->nlmsg_type == NLMSG_DONE) { done = 1; continue; }
                CHECK(h->nlmsg_type == type - 2);
                int payload = NLMSG_PAYLOAD(h, 0);
                if (type == RTM_GETLINK) {
                    struct ifinfomsg *info = NLMSG_DATA(h);
                    CHECK(payload >= sizeof(*info) && info->ifi_index > 0);
                    int alen = IFLA_PAYLOAD(h), named = 0, mtu = 0;
                    for (struct rtattr *a = IFLA_RTA(info); RTA_OK(a, alen); a = RTA_NEXT(a, alen)) {
                        if (a->rta_type == IFLA_IFNAME) { CHECK(memchr(RTA_DATA(a), 0, RTA_PAYLOAD(a))); named = 1; }
                        if (a->rta_type == IFLA_MTU) { CHECK(RTA_PAYLOAD(a) == 4); mtu = 1; }
                    }
                    CHECK(alen == 0 && named && mtu);
                    links++;
                    if ((info->ifi_flags & IFF_UP) && !(info->ifi_flags & IFF_LOOPBACK)) up++;
                } else {
                    struct ifaddrmsg *info = NLMSG_DATA(h);
                    CHECK(payload >= sizeof(*info) && info->ifa_index > 0);
                    CHECK(info->ifa_family == AF_INET || info->ifa_family == AF_INET6);
                    CHECK(info->ifa_prefixlen <= (info->ifa_family == AF_INET ? 32 : 128));
                    int alen = IFA_PAYLOAD(h), address = 0;
                    for (struct rtattr *a = IFA_RTA(info); RTA_OK(a, alen); a = RTA_NEXT(a, alen)) {
                        if (a->rta_type == IFA_ADDRESS) { CHECK(RTA_PAYLOAD(a) == (info->ifa_family == AF_INET ? 4 : 16)); address = 1; }
                    }
                    CHECK(alen == 0 && address);
                    addresses++;
                }
            }
            CHECK(n == 0);
        }
        CHECK(recv(fd, buf, sizeof(buf), 0) == -1 && errno == EAGAIN);
    }
    CHECK(links > 0 && addresses > 0 && up > 0);
    struct request req = request(RTM_GETLINK, 79);
    CHECK(sendto(fd, &req, sizeof(req), 0, (void *) &kernel, sizeof(kernel)) == sizeof(req));
    len = 2;
    int n = recvfrom(fd, buf, sizeof(buf), MSG_PEEK, (void *) &kernel, &len);
    CHECK(n > NLMSG_HDRLEN && len == sizeof(kernel) && kernel.nl_family == AF_NETLINK);
    len = sizeof(kernel);
    CHECK(recvfrom(fd, buf, 5, MSG_TRUNC, (void *) &kernel, &len) == n);
    CHECK(kernel.nl_pid == 0);
    // Native may send DONE as a separate datagram.
    while (recv(fd, buf, sizeof(buf), 0) >= 0) {}
    if (ish) {
        CHECK(send(fd, &req, 3, 0) == -1 && errno == EINVAL);
        req.h.nlmsg_len = sizeof(req) + 100;
        CHECK(send(fd, &req, sizeof(req), 0) == sizeof(req));
        CHECK(recv(fd, buf, sizeof(buf), 0) >= NLMSG_LENGTH(sizeof(struct nlmsgerr)));
        CHECK(((struct nlmsghdr *) buf)->nlmsg_type == NLMSG_ERROR);
        CHECK(((struct nlmsgerr *) NLMSG_DATA((struct nlmsghdr *) buf))->error == -EINVAL);
        req = request(RTM_NEWLINK, 80);
        CHECK(write(fd, &req, sizeof(req)) == sizeof(req));
        CHECK(read(fd, buf, sizeof(buf)) >= NLMSG_LENGTH(sizeof(struct nlmsgerr)));
        CHECK(((struct nlmsghdr *) buf)->nlmsg_type == NLMSG_ERROR);
        CHECK(((struct nlmsgerr *) NLMSG_DATA((struct nlmsghdr *) buf))->error == -EOPNOTSUPP);
        req = request(RTM_GETLINK, 81);
        char big[4097] = {0};
        CHECK(send(fd, big, sizeof(big), 0) == -1 && errno == EMSGSIZE);
        kernel.nl_pid = 42;
        CHECK(sendto(fd, &req, sizeof(req), 0, (void *) &kernel, sizeof(kernel)) == -1 && errno == EPERM);
        // Saturating the reply queue must return EAGAIN, never deadlock.
        int sent = 0;
        while (sent < 10000 && send(fd, &req, sizeof(req), 0) >= 0) sent++;
        CHECK(sent > 0 && sent < 10000 && errno == EAGAIN);
        int received = 0;
        while (recv(fd, buf, sizeof(buf), 0) >= 0) received++;
        CHECK(received == sent && errno == EAGAIN);
    }
    CHECK(poll(&p, 1, 0) == 0);
    CHECK(epoll_wait(ep, &event, 1, 0) == 0);
    CHECK(fcntl(fd, F_SETFL, 0) == 0);
    CHECK(recv(fd, buf, sizeof(buf), MSG_DONTWAIT) == -1 && errno == EAGAIN);
    int other = socket(AF_NETLINK, SOCK_DGRAM | SOCK_NONBLOCK, NETLINK_ROUTE);
    CHECK(other >= 0);
    CHECK(bind(other, (void *) &local, sizeof(local)) == -1 && errno == EADDRINUSE);
    struct sockaddr_nl other_name = {.nl_family = AF_NETLINK};
    CHECK(bind(other, (void *) &other_name, sizeof(other_name)) == 0);
    len = sizeof(other_name);
    CHECK(getsockname(other, (void *) &other_name, &len) == 0);
    CHECK(other_name.nl_pid != local.nl_pid);
    if (ish) {
        struct request r = request(RTM_GETADDR, 90);
        struct iovec iv = {&r, sizeof(r)};
        struct msghdr m = {.msg_iov = &iv, .msg_iovlen = 1025};
        CHECK(sendmsg(other, &m, 0) == -1 && errno == EMSGSIZE);
        CHECK(recvmsg(other, &m, 0) == -1 && errno == EMSGSIZE);
        m.msg_iovlen = 1; iv.iov_base = (void *) 1;
        CHECK(sendmsg(other, &m, 0) == -1 && errno == EFAULT);
        CHECK(socket(AF_NETLINK, SOCK_STREAM, NETLINK_ROUTE) == -1 && errno == ESOCKTNOSUPPORT);
        CHECK(socket(AF_NETLINK, SOCK_RAW, 999) == -1 && errno == EPROTONOSUPPORT);
        CHECK(bind(other, (void *) &local, 1) == -1 && errno == EINVAL);
        CHECK(send(other, &r, sizeof(r), MSG_OOB) == -1 && errno == EOPNOTSUPP);
    }
    CHECK(close(other) == 0);
    int duplicate = dup(fd);
    CHECK(duplicate >= 0);
    CHECK(close(fd) == 0);
    CHECK(recv(duplicate, buf, sizeof(buf), MSG_DONTWAIT) == -1 && errno == EAGAIN);
    CHECK(close(duplicate) == 0 && close(ep) == 0);
    printf("NETLINK_PASS checks=%u links=%d addresses=%d up=%d\n", checks, links, addresses, up);
    return 0;
}
