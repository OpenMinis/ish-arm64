// AF_NETLINK stub: request parsing and reply synthesis. See fs/netlink.h.
//
// Host-only code: nothing here touches guest memory or kernel locks, so it
// can be called from any syscall path. getifaddrs() is a local, synchronous
// query (no network request is made, nothing leaves the device).

#include <errno.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <net/if_dl.h>
#include <net/if_types.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>

#include "fs/netlink.h"

// ---- Linux wire format (same on every Linux arch) -----------------------

struct nlmsghdr_l {
    uint32_t nlmsg_len;
    uint16_t nlmsg_type;
    uint16_t nlmsg_flags;
    uint32_t nlmsg_seq;
    uint32_t nlmsg_pid;
};
struct ifinfomsg_l {
    uint8_t ifi_family;
    uint8_t ifi_pad;
    uint16_t ifi_type;
    int32_t ifi_index;
    uint32_t ifi_flags;
    uint32_t ifi_change;
};
struct ifaddrmsg_l {
    uint8_t ifa_family;
    uint8_t ifa_prefixlen;
    uint8_t ifa_flags;
    uint8_t ifa_scope;
    uint32_t ifa_index;
};
struct rtattr_l {
    uint16_t rta_len;
    uint16_t rta_type;
};

#define NLMSG_ERROR_L 2
#define NLMSG_DONE_L 3
#define NLMSG_MIN_TYPE_L 0x10

#define NLM_F_REQUEST_L 0x01
#define NLM_F_MULTI_L 0x02
#define NLM_F_ACK_L 0x04
#define NLM_F_DUMP_L 0x300

#define RTM_NEWLINK_L 16
#define RTM_GETLINK_L 18
#define RTM_NEWADDR_L 20
#define RTM_GETADDR_L 22
#define RTM_GETROUTE_L 26

#define IFLA_ADDRESS_L 1
#define IFLA_BROADCAST_L 2
#define IFLA_IFNAME_L 3
#define IFLA_MTU_L 4
#define IFLA_QDISC_L 6
#define IFLA_TXQLEN_L 13
#define IFLA_OPERSTATE_L 16
#define IFLA_LINKMODE_L 17

#define IFA_ADDRESS_L 1
#define IFA_LOCAL_L 2
#define IFA_LABEL_L 3
#define IFA_BROADCAST_L 4
#define IFA_FLAGS_L 8
#define IFA_F_PERMANENT_L 0x80

#define AF_INET_L 2
#define AF_INET6_L 10

#define IFF_UP_L 0x1
#define IFF_BROADCAST_L 0x2
#define IFF_LOOPBACK_L 0x8
#define IFF_POINTOPOINT_L 0x10
#define IFF_RUNNING_L 0x40
#define IFF_NOARP_L 0x80
#define IFF_PROMISC_L 0x100
#define IFF_ALLMULTI_L 0x200
#define IFF_MULTICAST_L 0x1000
#define IFF_LOWER_UP_L 0x10000

#define ARPHRD_ETHER_L 1
#define ARPHRD_LOOPBACK_L 772
#define ARPHRD_NONE_L 0xFFFE

#define IF_OPER_UNKNOWN_L 0
#define IF_OPER_DOWN_L 2
#define IF_OPER_UP_L 6

#define RT_SCOPE_UNIVERSE_L 0
#define RT_SCOPE_LINK_L 253
#define RT_SCOPE_HOST_L 254

#define ENODEV_L 19
#define EINVAL_L 22
#define EOPNOTSUPP_L 95

#define ALIGN4(n) (((n) + 3u) & ~3u)

// ---- the host's interfaces ------------------------------------------------

static void guest_ifname(char out[16], const char *host) {
    // The guest expects the loopback to be called "lo"; every other
    // interface keeps the name the host gives it.
    if (strcmp(host, "lo0") == 0)
        host = "lo";
    strncpy(out, host, 15);
    out[15] = '\0';
}

static unsigned linux_ifflags(unsigned bsd) {
    unsigned f = 0;
    if (bsd & IFF_UP) f |= IFF_UP_L;
    if (bsd & IFF_BROADCAST) f |= IFF_BROADCAST_L;
    if (bsd & IFF_LOOPBACK) f |= IFF_LOOPBACK_L;
    if (bsd & IFF_POINTOPOINT) f |= IFF_POINTOPOINT_L;
    if (bsd & IFF_RUNNING) f |= IFF_RUNNING_L | IFF_LOWER_UP_L;
    if (bsd & IFF_NOARP) f |= IFF_NOARP_L;
    if (bsd & IFF_PROMISC) f |= IFF_PROMISC_L;
    if (bsd & IFF_ALLMULTI) f |= IFF_ALLMULTI_L;
    if (bsd & IFF_MULTICAST) f |= IFF_MULTICAST_L;
    return f;
}

static uint8_t prefix_len(const uint8_t *mask, size_t n) {
    unsigned bits = 0;
    for (size_t i = 0; i < n; i++) {
        uint8_t b = mask[i];
        if (b == 0xff) { bits += 8; continue; }
        while (b & 0x80) { bits++; b = (uint8_t) (b << 1); }
        break;
    }
    return (uint8_t) bits;
}

static struct netlink_host_link *link_for(struct netlink_host_link **links, size_t *n,
                                          const char *host_name, unsigned bsd_flags) {
    char name[16];
    guest_ifname(name, host_name);
    for (size_t i = 0; i < *n; i++)
        if (strcmp((*links)[i].name, name) == 0)
            return &(*links)[i];
    struct netlink_host_link *grown = realloc(*links, (*n + 1) * sizeof(**links));
    if (grown == NULL)
        return NULL;
    *links = grown;
    struct netlink_host_link *l = &grown[(*n)++];
    memset(l, 0, sizeof(*l));
    memcpy(l->name, name, sizeof(name));
    l->index = if_nametoindex(host_name);
    l->flags = linux_ifflags(bsd_flags);
    bool loopback = (bsd_flags & IFF_LOOPBACK) != 0;
    l->arphrd = loopback ? ARPHRD_LOOPBACK_L : ARPHRD_NONE_L;
    l->mtu = loopback ? 16384 : 1500;
    // Linux reports the loopback as UNKNOWN; anything else is up when it
    // is both administratively up and running.
    if (loopback)
        l->operstate = IF_OPER_UNKNOWN_L;
    else
        l->operstate = ((bsd_flags & IFF_UP) && (bsd_flags & IFF_RUNNING)) ? IF_OPER_UP_L : IF_OPER_DOWN_L;
    return l;
}

static int by_index_link(const void *a, const void *b) {
    const struct netlink_host_link *x = a, *y = b;
    return x->index < y->index ? -1 : x->index > y->index;
}
static int by_index_addr(const void *a, const void *b) {
    const struct netlink_host_addr *x = a, *y = b;
    if (x->index != y->index)
        return x->index < y->index ? -1 : 1;
    return (int) x->family - (int) y->family;
}

static void loopback_only(struct netlink_host_link **links, size_t *n_links,
                          struct netlink_host_addr **addrs, size_t *n_addrs) {
    // What is true of every device even when the host cannot be asked.
    *links = calloc(1, sizeof(**links));
    *addrs = calloc(1, sizeof(**addrs));
    *n_links = *n_addrs = 0;
    if (*links != NULL) {
        struct netlink_host_link *l = &(*links)[0];
        strcpy(l->name, "lo");
        l->index = 1;
        l->flags = IFF_UP_L | IFF_LOOPBACK_L | IFF_RUNNING_L | IFF_LOWER_UP_L;
        l->mtu = 16384;
        l->arphrd = ARPHRD_LOOPBACK_L;
        l->operstate = IF_OPER_UNKNOWN_L;
        *n_links = 1;
    }
    if (*addrs != NULL) {
        struct netlink_host_addr *a = &(*addrs)[0];
        strcpy(a->name, "lo");
        a->index = 1;
        a->family = AF_INET_L;
        a->prefixlen = 8;
        a->scope = RT_SCOPE_HOST_L;
        a->local[0] = 127;
        a->local[3] = 1;
        *n_addrs = 1;
    }
}

int netlink_host_snapshot(struct netlink_host_link **links, size_t *n_links,
                          struct netlink_host_addr **addrs, size_t *n_addrs) {
    *links = NULL;
    *addrs = NULL;
    *n_links = *n_addrs = 0;
    struct ifaddrs *list = NULL;
    if (getifaddrs(&list) != 0 || list == NULL) {
        if (list != NULL)
            freeifaddrs(list);
        loopback_only(links, n_links, addrs, n_addrs);
        return 0;
    }
    for (struct ifaddrs *ifa = list; ifa != NULL; ifa = ifa->ifa_next) {
        if (ifa->ifa_name == NULL)
            continue;
        struct netlink_host_link *l = link_for(links, n_links, ifa->ifa_name, ifa->ifa_flags);
        if (l == NULL || ifa->ifa_addr == NULL)
            continue;
        if (ifa->ifa_addr->sa_family == AF_LINK) {
            const struct sockaddr_dl *dl = (const struct sockaddr_dl *) (void *) ifa->ifa_addr;
            if (dl->sdl_type == IFT_ETHER)
                l->arphrd = ARPHRD_ETHER_L;
            else if (dl->sdl_type == IFT_LOOP)
                l->arphrd = ARPHRD_LOOPBACK_L;
            // The hardware address exactly as the host hands it out: iOS
            // gives apps a fixed placeholder (02:00:00:00:00:00), and
            // that is what is passed on. Nothing is made up.
            if (dl->sdl_alen == 6) {
                memcpy(l->lladdr, LLADDR(dl), 6);
                l->has_lladdr = true;
            }
            if (ifa->ifa_data != NULL) {
                const struct if_data *d = ifa->ifa_data;
                if (d->ifi_mtu > 0)
                    l->mtu = d->ifi_mtu;
                l->rx_bytes = d->ifi_ibytes;
                l->rx_packets = d->ifi_ipackets;
                l->rx_errs = d->ifi_ierrors;
                l->rx_drop = d->ifi_iqdrops;
                l->rx_multicast = d->ifi_imcasts;
                l->tx_bytes = d->ifi_obytes;
                l->tx_packets = d->ifi_opackets;
                l->tx_errs = d->ifi_oerrors;
                l->collisions = d->ifi_collisions;
            }
            continue;
        }
        int fam = ifa->ifa_addr->sa_family;
        if (fam != AF_INET && fam != AF_INET6)
            continue;
        struct netlink_host_addr *grown = realloc(*addrs, (*n_addrs + 1) * sizeof(**addrs));
        if (grown == NULL)
            continue;
        *addrs = grown;
        struct netlink_host_addr *a = &grown[*n_addrs];
        memset(a, 0, sizeof(*a));
        memcpy(a->name, l->name, sizeof(a->name));
        a->index = l->index;
        bool loopback = (ifa->ifa_flags & IFF_LOOPBACK) != 0;
        if (fam == AF_INET) {
            const struct sockaddr_in *in = (const struct sockaddr_in *) (void *) ifa->ifa_addr;
            a->family = AF_INET_L;
            memcpy(a->local, &in->sin_addr, 4);
            a->prefixlen = 32;
            if (ifa->ifa_netmask != NULL)
                a->prefixlen = prefix_len((const uint8_t *) &((const struct sockaddr_in *) (void *) ifa->ifa_netmask)->sin_addr, 4);
            if ((ifa->ifa_flags & IFF_POINTOPOINT) && ifa->ifa_dstaddr != NULL && ifa->ifa_dstaddr->sa_family == AF_INET) {
                memcpy(a->peer, &((const struct sockaddr_in *) (void *) ifa->ifa_dstaddr)->sin_addr, 4);
                a->has_peer = true;
            } else if ((ifa->ifa_flags & IFF_BROADCAST) && ifa->ifa_broadaddr != NULL && ifa->ifa_broadaddr->sa_family == AF_INET) {
                memcpy(a->brd, &((const struct sockaddr_in *) (void *) ifa->ifa_broadaddr)->sin_addr, 4);
                a->has_brd = true;
            }
            if (loopback || a->local[0] == 127)
                a->scope = RT_SCOPE_HOST_L;
            else if (a->local[0] == 169 && a->local[1] == 254)
                a->scope = RT_SCOPE_LINK_L;
            else
                a->scope = RT_SCOPE_UNIVERSE_L;
        } else {
            const struct sockaddr_in6 *in6 = (const struct sockaddr_in6 *) (void *) ifa->ifa_addr;
            a->family = AF_INET6_L;
            memcpy(a->local, &in6->sin6_addr, 16);
            bool link_local = a->local[0] == 0xfe && (a->local[1] & 0xc0) == 0x80;
            // BSD keeps the interface index inside a link-local address
            // (bytes 2-3); Linux does not.
            if (link_local)
                a->local[2] = a->local[3] = 0;
            a->prefixlen = 128;
            if (ifa->ifa_netmask != NULL) {
                const struct sockaddr_in6 *m = (const struct sockaddr_in6 *) (void *) ifa->ifa_netmask;
                size_t have = 16;
                // A netmask sockaddr may be shorter than a full one.
                size_t off = offsetof(struct sockaddr_in6, sin6_addr);
                if (m->sin6_len < off + 16)
                    have = m->sin6_len > off ? m->sin6_len - off : 0;
                a->prefixlen = prefix_len((const uint8_t *) &m->sin6_addr, have);
            }
            static const uint8_t v6_loopback[16] = {[15] = 1};
            if (memcmp(a->local, v6_loopback, 16) == 0)
                a->scope = RT_SCOPE_HOST_L;
            else if (link_local)
                a->scope = RT_SCOPE_LINK_L;
            else
                a->scope = RT_SCOPE_UNIVERSE_L;
        }
        (*n_addrs)++;
    }
    freeifaddrs(list);
    if (*n_links == 0) {
        free(*links);
        free(*addrs);
        loopback_only(links, n_links, addrs, n_addrs);
        return 0;
    }
    qsort(*links, *n_links, sizeof(**links), by_index_link);
    if (*n_addrs > 0)
        qsort(*addrs, *n_addrs, sizeof(**addrs), by_index_addr);
    return 0;
}

void netlink_host_free(struct netlink_host_link *links, struct netlink_host_addr *addrs) {
    free(links);
    free(addrs);
}

// ---- building messages ----------------------------------------------------

struct msg {
    uint8_t buf[NETLINK_STUB_DGRAM_MAX];
    size_t len;
};

static void msg_begin(struct msg *m, uint16_t type, uint16_t flags, uint32_t seq) {
    memset(m->buf, 0, sizeof(m->buf));
    struct nlmsghdr_l *h = (struct nlmsghdr_l *) m->buf;
    h->nlmsg_type = type;
    h->nlmsg_flags = flags;
    h->nlmsg_seq = seq;
    // The reply's port id is the socket's, and getsockname() reports 0 for
    // a stub socket: busybox, iproute2 and Go all compare the two.
    h->nlmsg_pid = 0;
    m->len = sizeof(*h);
}
static void *msg_reserve(struct msg *m, size_t n) {
    size_t at = ALIGN4(m->len);
    if (at + n > sizeof(m->buf))
        return NULL;
    m->len = at + n;
    return m->buf + at;
}
static void msg_attr(struct msg *m, uint16_t type, const void *data, size_t n) {
    struct rtattr_l *a = msg_reserve(m, sizeof(*a) + n);
    if (a == NULL)
        return;
    a->rta_len = (uint16_t) (sizeof(*a) + n);
    a->rta_type = type;
    if (n > 0)
        memcpy(a + 1, data, n);
}
static int msg_end(struct msg *m, netlink_stub_emit_t emit, void *ctx) {
    m->len = ALIGN4(m->len);
    ((struct nlmsghdr_l *) m->buf)->nlmsg_len = (uint32_t) m->len;
    return emit(ctx, m->buf, m->len);
}

size_t netlink_stub_done(void *out, uint32_t seq) {
    struct {
        struct nlmsghdr_l h;
        int32_t status;
    } done = {
        .h = {.nlmsg_len = sizeof(done), .nlmsg_type = NLMSG_DONE_L, .nlmsg_flags = NLM_F_MULTI_L, .nlmsg_seq = seq},
        .status = 0,
    };
    memcpy(out, &done, sizeof(done));
    return sizeof(done);
}

static int emit_done(uint32_t seq, netlink_stub_emit_t emit, void *ctx) {
    uint8_t buf[32];
    size_t n = netlink_stub_done(buf, seq);
    return emit(ctx, buf, n);
}

// NLMSG_ERROR carrying a Linux errno (0 = an ACK) and the request's header.
static int emit_error(const struct nlmsghdr_l *req, int linux_errno, netlink_stub_emit_t emit, void *ctx) {
    struct {
        struct nlmsghdr_l h;
        int32_t error;
        struct nlmsghdr_l echo;
    } e = {
        .h = {.nlmsg_len = sizeof(e), .nlmsg_type = NLMSG_ERROR_L, .nlmsg_seq = req->nlmsg_seq},
        .error = -linux_errno,
        .echo = *req,
    };
    return emit(ctx, &e, sizeof(e));
}

static int emit_link(const struct netlink_host_link *l, uint16_t flags, uint32_t seq,
                     netlink_stub_emit_t emit, void *ctx) {
    struct msg m;
    msg_begin(&m, RTM_NEWLINK_L, flags, seq);
    struct ifinfomsg_l *ifi = msg_reserve(&m, sizeof(*ifi));
    ifi->ifi_type = l->arphrd;
    ifi->ifi_index = (int32_t) l->index;
    ifi->ifi_flags = l->flags;
    msg_attr(&m, IFLA_IFNAME_L, l->name, strlen(l->name) + 1);
    uint32_t mtu = l->mtu, txqlen = (l->flags & IFF_LOOPBACK_L) ? 1000 : 0;
    msg_attr(&m, IFLA_MTU_L, &mtu, sizeof(mtu));
    msg_attr(&m, IFLA_TXQLEN_L, &txqlen, sizeof(txqlen));
    msg_attr(&m, IFLA_OPERSTATE_L, &l->operstate, 1);
    uint8_t linkmode = 0;
    msg_attr(&m, IFLA_LINKMODE_L, &linkmode, 1);
    msg_attr(&m, IFLA_QDISC_L, "noqueue", sizeof("noqueue"));
    if (l->flags & IFF_LOOPBACK_L) {
        // Linux shows the loopback with an all-zero address and broadcast.
        static const uint8_t zero[6];
        msg_attr(&m, IFLA_ADDRESS_L, zero, 6);
        msg_attr(&m, IFLA_BROADCAST_L, zero, 6);
    } else if (l->has_lladdr) {
        msg_attr(&m, IFLA_ADDRESS_L, l->lladdr, 6);
        if (l->arphrd == ARPHRD_ETHER_L) {
            static const uint8_t ones[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
            msg_attr(&m, IFLA_BROADCAST_L, ones, 6);
        }
    }
    return msg_end(&m, emit, ctx);
}

static int emit_addr(const struct netlink_host_addr *a, uint32_t seq, netlink_stub_emit_t emit, void *ctx) {
    struct msg m;
    msg_begin(&m, RTM_NEWADDR_L, NLM_F_MULTI_L, seq);
    struct ifaddrmsg_l *ifa = msg_reserve(&m, sizeof(*ifa));
    ifa->ifa_family = a->family;
    ifa->ifa_prefixlen = a->prefixlen;
    ifa->ifa_flags = IFA_F_PERMANENT_L;
    ifa->ifa_scope = a->scope;
    ifa->ifa_index = a->index;
    size_t n = a->family == AF_INET_L ? 4 : 16;
    if (a->family == AF_INET_L) {
        // IFA_LOCAL is this end; IFA_ADDRESS the peer on a point-to-point
        // link and the same address otherwise.
        msg_attr(&m, IFA_ADDRESS_L, a->has_peer ? a->peer : a->local, n);
        msg_attr(&m, IFA_LOCAL_L, a->local, n);
        if (a->has_brd)
            msg_attr(&m, IFA_BROADCAST_L, a->brd, n);
        msg_attr(&m, IFA_LABEL_L, a->name, strlen(a->name) + 1);
    } else {
        msg_attr(&m, IFA_ADDRESS_L, a->local, n);
    }
    uint32_t flags = IFA_F_PERMANENT_L;
    msg_attr(&m, IFA_FLAGS_L, &flags, sizeof(flags));
    return msg_end(&m, emit, ctx);
}

// ---- answering ------------------------------------------------------------

bool netlink_stub_route_empty(void) {
    static int cached = -1;
    if (cached == -1) {
        // The host's routing table is not available to us, so a route dump
        // is refused by default rather than answered with "no routes",
        // which a caller would take for the truth. ISH_NETLINK_ROUTE=empty
        // brings back the empty dump.
        const char *v = getenv("ISH_NETLINK_ROUTE");
        cached = (v != NULL && strcmp(v, "empty") == 0) ? 1 : 0;
    }
    return cached == 1;
}

// The name a non-dump RTM_GETLINK asks for (IFLA_IFNAME), or NULL.
static const char *wanted_ifname(const struct nlmsghdr_l *h) {
    size_t off = ALIGN4(sizeof(*h) + sizeof(struct ifinfomsg_l));
    const uint8_t *base = (const uint8_t *) h;
    while (off + sizeof(struct rtattr_l) <= h->nlmsg_len) {
        const struct rtattr_l *a = (const struct rtattr_l *) (base + off);
        if (a->rta_len < sizeof(*a) || off + a->rta_len > h->nlmsg_len)
            break;
        if ((a->rta_type & 0x3fff) == IFLA_IFNAME_L) {
            const char *name = (const char *) (a + 1);
            size_t n = a->rta_len - sizeof(*a);
            if (n > 0 && memchr(name, '\0', n) != NULL)
                return name;
            return NULL;
        }
        off += ALIGN4(a->rta_len);
    }
    return NULL;
}

static int answer_getlink(const struct nlmsghdr_l *h, netlink_stub_emit_t emit, void *ctx) {
    struct netlink_host_link *links;
    struct netlink_host_addr *addrs;
    size_t n_links, n_addrs;
    netlink_host_snapshot(&links, &n_links, &addrs, &n_addrs);
    int err = 0;
    bool has_body = h->nlmsg_len >= sizeof(*h) + sizeof(struct ifinfomsg_l);
    int32_t want_index = has_body ? ((const struct ifinfomsg_l *) (h + 1))->ifi_index : 0;
    const char *want_name = has_body ? wanted_ifname(h) : NULL;
    bool dump = (h->nlmsg_flags & NLM_F_DUMP_L) != 0 || (want_index == 0 && want_name == NULL);
    if (dump) {
        for (size_t i = 0; i < n_links && err >= 0; i++)
            err = emit_link(&links[i], NLM_F_MULTI_L, h->nlmsg_seq, emit, ctx);
        if (err >= 0)
            err = emit_done(h->nlmsg_seq, emit, ctx);
    } else {
        const struct netlink_host_link *found = NULL;
        for (size_t i = 0; i < n_links && found == NULL; i++) {
            if (want_index != 0 ? (int32_t) links[i].index == want_index
                                : strcmp(links[i].name, want_name) == 0)
                found = &links[i];
        }
        if (found == NULL) {
            err = emit_error(h, ENODEV_L, emit, ctx);
        } else {
            err = emit_link(found, 0, h->nlmsg_seq, emit, ctx);
            if (err >= 0 && (h->nlmsg_flags & NLM_F_ACK_L))
                err = emit_error(h, 0, emit, ctx);
        }
    }
    netlink_host_free(links, addrs);
    return err;
}

static int answer_getaddr(const struct nlmsghdr_l *h, netlink_stub_emit_t emit, void *ctx) {
    struct netlink_host_link *links;
    struct netlink_host_addr *addrs;
    size_t n_links, n_addrs;
    netlink_host_snapshot(&links, &n_links, &addrs, &n_addrs);
    // The first byte after the header is the family in every request
    // shape (ifaddrmsg, rtgenmsg, ifinfomsg); 0 asks for both.
    uint8_t want = h->nlmsg_len > sizeof(*h) ? *(const uint8_t *) (h + 1) : 0;
    int err = 0;
    for (size_t i = 0; i < n_addrs && err >= 0; i++) {
        if (want != 0 && addrs[i].family != want)
            continue;
        err = emit_addr(&addrs[i], h->nlmsg_seq, emit, ctx);
    }
    if (err >= 0)
        err = emit_done(h->nlmsg_seq, emit, ctx);
    netlink_host_free(links, addrs);
    return err;
}

int netlink_stub_answer(int protocol, const void *req, size_t req_len,
                        netlink_stub_emit_t emit, void *ctx,
                        struct netlink_stub_req *last) {
    const uint8_t *p = req;
    size_t left = req_len;
    int err = 0;
    if (last != NULL)
        last->valid = false;
    while (err >= 0 && left >= sizeof(struct nlmsghdr_l)) {
        struct nlmsghdr_l h;
        memcpy(&h, p, sizeof(h));
        if (h.nlmsg_len < sizeof(h) || h.nlmsg_len > left) {
            // A header that lies about its length: Linux drops the rest
            // of the buffer. Answer it so the sender is not left waiting.
            h.nlmsg_len = sizeof(h);
            err = emit_error(&h, EINVAL_L, emit, ctx);
            break;
        }
        if (last != NULL) {
            last->valid = true;
            last->seq = h.nlmsg_seq;
            last->type = h.nlmsg_type;
            last->flags = h.nlmsg_flags;
        }
        // Copy the message out so it can be read with its own alignment.
        union {
            struct nlmsghdr_l h;
            uint8_t raw[NETLINK_STUB_DGRAM_MAX];
        } one;
        size_t take = h.nlmsg_len < sizeof(one.raw) ? h.nlmsg_len : sizeof(one.raw);
        memset(&one, 0, sizeof(one));
        memcpy(one.raw, p, take);
        one.h.nlmsg_len = (uint32_t) take;

        if (!(h.nlmsg_flags & NLM_F_REQUEST_L) || h.nlmsg_type < NLMSG_MIN_TYPE_L) {
            // Not a request (control messages, stray replies): Linux
            // ignores these too, and nothing is owed for them.
            if (last != NULL)
                last->valid = false;
        } else if (protocol != 0) {
            // Only NETLINK_ROUTE has anything behind it.
            err = emit_error(&one.h, EOPNOTSUPP_L, emit, ctx);
        } else if (h.nlmsg_type == RTM_GETLINK_L) {
            err = answer_getlink(&one.h, emit, ctx);
        } else if (h.nlmsg_type == RTM_GETADDR_L) {
            err = answer_getaddr(&one.h, emit, ctx);
        } else if (h.nlmsg_type == RTM_GETROUTE_L && netlink_stub_route_empty() &&
                   (h.nlmsg_flags & NLM_F_DUMP_L)) {
            err = emit_done(h.nlmsg_seq, emit, ctx);
        } else {
            // Routes, rules, neighbours, qdiscs, every change request: the
            // host has no answer, so say so at once.
            err = emit_error(&one.h, EOPNOTSUPP_L, emit, ctx);
        }
        size_t step = ALIGN4(h.nlmsg_len);
        if (step > left)
            break;
        p += step;
        left -= step;
    }
    return err < 0 ? err : 0;
}
