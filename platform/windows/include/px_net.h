// Sockets for the Windows build: Linux constants and structure layouts (the
// px layer translates to Winsock). See platform/windows/compat.h.
#ifndef PLATFORM_WINDOWS_PX_NET_H
#define PLATFORM_WINDOWS_PX_NET_H

#define AF_UNSPEC 0
#define AF_UNIX 1
#define AF_LOCAL AF_UNIX
#define AF_INET 2
#define AF_INET6 10
#define AF_NETLINK 16
#define AF_PACKET 17
#define AF_MAX 46
#define PF_UNSPEC AF_UNSPEC
#define PF_UNIX AF_UNIX
#define PF_LOCAL AF_LOCAL
#define PF_INET AF_INET
#define PF_INET6 AF_INET6
#define PF_NETLINK AF_NETLINK
#define PF_ROUTE AF_NETLINK

#define SOCK_STREAM 1
#define SOCK_DGRAM 2
#define SOCK_RAW 3
#define SOCK_SEQPACKET 5
#define SOCK_NONBLOCK 04000
#define SOCK_CLOEXEC 02000000

#define SOL_SOCKET 1
#define SO_DEBUG 1
#define SO_REUSEADDR 2
#define SO_TYPE 3
#define SO_ERROR 4
#define SO_DONTROUTE 5
#define SO_BROADCAST 6
#define SO_SNDBUF 7
#define SO_RCVBUF 8
#define SO_KEEPALIVE 9
#define SO_OOBINLINE 10
#define SO_LINGER 13
#define SO_REUSEPORT 15
#define SO_PASSCRED 16
#define SO_PEERCRED 17
#define SO_RCVLOWAT 18
#define SO_SNDLOWAT 19
#define SO_RCVTIMEO 20
#define SO_SNDTIMEO 21
#define SO_BINDTODEVICE 25
#define SO_TIMESTAMP 29
#define SO_ACCEPTCONN 30
#define SO_PROTOCOL 38
#define SO_DOMAIN 39
#define SOMAXCONN 4096

#define IPPROTO_IP 0
#define IPPROTO_ICMP 1
#define IPPROTO_TCP 6
#define IPPROTO_UDP 17
#define IPPROTO_IPV6 41
#define IPPROTO_ICMPV6 58
#define IPPROTO_RAW 255

#define IP_TOS 1
#define IP_TTL 2
#define IP_HDRINCL 3
#define IP_OPTIONS 4
#define IP_RECVTTL 12
#define IP_RECVTOS 13
#define IP_RETOPTS 7
#define IP_MTU_DISCOVER 10
#define IP_RECVERR 11
// Darwin's: strip the IP header from ICMP datagram sockets (see net.c)
#define IP_STRIPHDR 23
#define IP_MULTICAST_IF 32
#define IP_MULTICAST_TTL 33
#define IP_MULTICAST_LOOP 34
#define IP_ADD_MEMBERSHIP 35
#define IP_DROP_MEMBERSHIP 36
#define IPV6_UNICAST_HOPS 16
#define IPV6_MULTICAST_IF 17
#define IPV6_MULTICAST_HOPS 18
#define IPV6_MULTICAST_LOOP 19
#define IPV6_JOIN_GROUP 20
#define IPV6_LEAVE_GROUP 21
#define IPV6_RECVERR 25
#define IPV6_V6ONLY 26
#define IPV6_TCLASS 67

#define TCP_NODELAY 1
#define TCP_MAXSEG 2
#define TCP_CORK 3
#define TCP_KEEPIDLE 4
#define TCP_KEEPINTVL 5
#define TCP_KEEPCNT 6
#define TCP_INFO 11
#define TCP_CONGESTION 13
#define TCP_USER_TIMEOUT 18
#define TCP_FASTOPEN 23

#define MSG_OOB 1
#define MSG_PEEK 2
#define MSG_DONTROUTE 4
#define MSG_CTRUNC 8
#define MSG_TRUNC 0x20
#define MSG_DONTWAIT 0x40
#define MSG_EOR 0x80
#define MSG_WAITALL 0x100
#define MSG_NOSIGNAL 0x4000
#define MSG_CMSG_CLOEXEC 0x40000000

#define SHUT_RD 0
#define SHUT_WR 1
#define SHUT_RDWR 2

#define SCM_RIGHTS 1
#define SCM_CREDENTIALS 2

#define INADDR_ANY ((in_addr_t) 0x00000000)
#define INADDR_BROADCAST ((in_addr_t) 0xffffffff)
#define INADDR_NONE ((in_addr_t) 0xffffffff)
#define INADDR_LOOPBACK ((in_addr_t) 0x7f000001)
#define INET_ADDRSTRLEN 16
#define INET6_ADDRSTRLEN 46

struct sockaddr {
    sa_family_t sa_family;
    char sa_data[14];
};
struct sockaddr_storage {
    sa_family_t ss_family;
    char __ss_padding[118];
    unsigned long long __ss_align;
};
struct in_addr {
    in_addr_t s_addr;
};
struct sockaddr_in {
    sa_family_t sin_family;
    in_port_t sin_port;
    struct in_addr sin_addr;
    unsigned char sin_zero[8];
};
struct in6_addr {
    union {
        uint8_t __u6_addr8[16];
        uint16_t __u6_addr16[8];
        uint32_t __u6_addr32[4];
    } __in6_u;
};
#define s6_addr __in6_u.__u6_addr8
#define s6_addr16 __in6_u.__u6_addr16
#define s6_addr32 __in6_u.__u6_addr32
struct sockaddr_in6 {
    sa_family_t sin6_family;
    in_port_t sin6_port;
    uint32_t sin6_flowinfo;
    struct in6_addr sin6_addr;
    uint32_t sin6_scope_id;
};
struct sockaddr_un {
    sa_family_t sun_family;
    char sun_path[108];
};
struct linger {
    int l_onoff;
    int l_linger;
};
struct ucred {
    int pid;
    uid_t uid;
    gid_t gid;
};
struct ip_mreq {
    struct in_addr imr_multiaddr;
    struct in_addr imr_interface;
};
struct ipv6_mreq {
    struct in6_addr ipv6mr_multiaddr;
    unsigned ipv6mr_interface;
};

struct cmsghdr {
    size_t cmsg_len;
    int cmsg_level;
    int cmsg_type;
};
#define CMSG_ALIGN(len) (((len) + sizeof(size_t) - 1) & ~(sizeof(size_t) - 1))
#define CMSG_SPACE(len) (CMSG_ALIGN(len) + CMSG_ALIGN(sizeof(struct cmsghdr)))
#define CMSG_LEN(len) (CMSG_ALIGN(sizeof(struct cmsghdr)) + (len))
#define CMSG_DATA(cmsg) ((unsigned char *) (cmsg) + CMSG_ALIGN(sizeof(struct cmsghdr)))
#define CMSG_FIRSTHDR(mhdr) ((size_t) (mhdr)->msg_controllen >= sizeof(struct cmsghdr) \
        ? (struct cmsghdr *) (mhdr)->msg_control : (struct cmsghdr *) 0)
#define CMSG_NXTHDR(mhdr, cmsg) px_cmsg_nxthdr(mhdr, cmsg)
static inline struct cmsghdr *px_cmsg_nxthdr(struct px_msghdr *m, struct cmsghdr *c) {
    if (c->cmsg_len < sizeof(struct cmsghdr))
        return 0;
    unsigned char *next = (unsigned char *) c + CMSG_ALIGN(c->cmsg_len);
    unsigned char *end = (unsigned char *) m->msg_control + m->msg_controllen;
    if (next + sizeof(struct cmsghdr) > end || next + CMSG_ALIGN(((struct cmsghdr *) next)->cmsg_len) > end)
        return 0;
    return (struct cmsghdr *) next;
}

static inline uint16_t htons(uint16_t x) { return __builtin_bswap16(x); }
static inline uint16_t ntohs(uint16_t x) { return __builtin_bswap16(x); }
static inline uint32_t htonl(uint32_t x) { return __builtin_bswap32(x); }
static inline uint32_t ntohl(uint32_t x) { return __builtin_bswap32(x); }

const char *px_inet_ntop(int af, const void *src, char *dst, socklen_t size);
int px_inet_pton(int af, const char *src, void *dst);
#define inet_ntop px_inet_ntop
#define inet_pton px_inet_pton

#endif
