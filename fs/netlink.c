// Read-only Linux route-netlink snapshots backed by host getifaddrs().
// This deliberately does not forward guest route changes or synthesize events.
#include <errno.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>
#ifdef __APPLE__
#include <net/if_dl.h>
#else
#include <netpacket/packet.h>
#endif
#include "fs/netlink.h"
#include "kernel/errno.h"

struct nl_header {
    uint32_t len;
    uint16_t type, flags;
    uint32_t seq, pid;
};
struct nl_link {
    uint8_t family, pad;
    uint16_t type;
    int32_t index;
    uint32_t flags, change;
};
struct nl_addr {
    uint8_t family, prefix, flags, scope;
    uint32_t index;
};
struct nl_attr { uint16_t len, type; };
#define ALIGN4(n) (((n) + 3u) & ~3u)
#define NL_ERROR 2
#define NL_DONE 3
#define NL_REQUEST 1
#define NL_MULTI 2
#define NL_DUMP 0x300
#define RTM_GETLINK_ 18
#define RTM_GETADDR_ 22
#define RTM_GETROUTE_ 26

struct builder {
    char *data;
    size_t used, capacity, start;
    uint32_t seq, port;
    int error;
};
static void append(struct builder *b, const void *data, size_t size) {
    if (b->error) return;
    size_t aligned = ALIGN4(size);
    if (aligned > b->capacity - b->used) {
        b->error = _ENOBUFS;
        return;
    }
    memcpy(b->data + b->used, data, size);
    memset(b->data + b->used + size, 0, aligned - size);
    b->used += aligned;
}
static void begin(struct builder *b, uint16_t type) {
    b->start = b->used;
    struct nl_header h = {sizeof(h), type, NL_MULTI, b->seq, b->port};
    append(b, &h, sizeof(h));
}
static void end(struct builder *b) {
    if (!b->error) {
        uint32_t size = b->used - b->start;
        memcpy(b->data + b->start, &size, sizeof(size));
    }
}
static void attr(struct builder *b, uint16_t type, const void *data, size_t size) {
    struct nl_attr a = {sizeof(a) + size, type};
    append(b, &a, sizeof(a));
    append(b, data, size);
}
static uint32_t linux_flags(unsigned flags) {
    uint32_t out = 0;
    if (flags & IFF_UP) out |= 1;
    if (flags & IFF_BROADCAST) out |= 2;
    if (flags & IFF_LOOPBACK) out |= 8;
    if (flags & IFF_POINTOPOINT) out |= 16;
    if (flags & IFF_RUNNING) out |= 64;
    if (flags & IFF_MULTICAST) out |= 4096;
    return out;
}
static void link_message(struct builder *b, struct ifaddrs *ifa, struct ifaddrs *all, int ctl) {
    unsigned index = if_nametoindex(ifa->ifa_name);
    if (!index) return; // Interface disappeared during the snapshot.
    struct nl_link link = {
        .type = (ifa->ifa_flags & IFF_LOOPBACK) ? 772 : 1,
        .index = index, .flags = linux_flags(ifa->ifa_flags),
    };
    if (ifa->ifa_flags & IFF_POINTOPOINT) link.type = 65534; // ARPHRD_NONE
    const void *hardware = NULL;
    size_t hardware_size = 0;
    for (struct ifaddrs *p = all; p; p = p->ifa_next) {
        if (!p->ifa_addr || !p->ifa_name || strcmp(p->ifa_name, ifa->ifa_name)) continue;
#ifdef __APPLE__
        if (p->ifa_addr->sa_family == AF_LINK) {
            const struct sockaddr_dl *dl = (const void *) p->ifa_addr;
            hardware = LLADDR(dl);
            hardware_size = dl->sdl_alen;
            if ((const char *) hardware + hardware_size > (const char *) dl + dl->sdl_len)
                hardware_size = 0;
        }
#else
        if (p->ifa_addr->sa_family == AF_PACKET) {
            const struct sockaddr_ll *ll = (const void *) p->ifa_addr;
            link.type = ll->sll_hatype;
            hardware = ll->sll_addr;
            if (ll->sll_halen <= sizeof(ll->sll_addr)) hardware_size = ll->sll_halen;
        }
#endif
    }
    begin(b, RTM_GETLINK_ - 2);
    append(b, &link, sizeof(link));
    attr(b, 3, ifa->ifa_name, strlen(ifa->ifa_name) + 1); // IFLA_IFNAME
    if (hardware_size) attr(b, 1, hardware, hardware_size); // IFLA_ADDRESS
    struct ifreq req = {0};
    strncpy(req.ifr_name, ifa->ifa_name, sizeof(req.ifr_name) - 1);
    if (ctl >= 0 && ioctl(ctl, SIOCGIFMTU, &req) == 0) {
        uint32_t mtu = req.ifr_mtu;
        attr(b, 4, &mtu, sizeof(mtu));
    }
    end(b);
}
static const void *ip_bytes(const struct sockaddr *sa) {
    if (!sa) return NULL;
    if (sa->sa_family == AF_INET) return &((const struct sockaddr_in *) sa)->sin_addr;
    if (sa->sa_family == AF_INET6) return &((const struct sockaddr_in6 *) sa)->sin6_addr;
    return NULL;
}
static unsigned prefix_length(const struct sockaddr *mask, size_t size) {
    const uint8_t *bytes = ip_bytes(mask);
    if (!bytes) return 0;
    unsigned prefix = 0;
    for (size_t i = 0; i < size; i++) {
        for (unsigned bit = 128; bit; bit >>= 1) {
            if (!(bytes[i] & bit)) return prefix;
            prefix++;
        }
    }
    return prefix;
}
static void addr_message(struct builder *b, struct ifaddrs *ifa, uint8_t family) {
    if (!ifa->ifa_addr) return;
    int af = ifa->ifa_addr->sa_family;
    if (af != AF_INET && af != AF_INET6) return;
    uint8_t guest_af = af == AF_INET ? 2 : 10;
    if (family && family != guest_af) return;
    unsigned index = if_nametoindex(ifa->ifa_name);
    if (!index) return;
    size_t size = af == AF_INET ? 4 : 16;
    union { struct in6_addr aligned; uint8_t bytes[16]; } storage;
    uint8_t *local = storage.bytes;
    memcpy(local, ip_bytes(ifa->ifa_addr), size);
#ifdef __APPLE__
    // Darwin embeds the interface scope in link-local IPv6 bytes 2..3.
    if (af == AF_INET6 && local[0] == 0xfe && (local[1] & 0xc0) == 0x80)
        local[2] = local[3] = 0;
#endif
    struct nl_addr addr = {
        .family = guest_af, .prefix = prefix_length(ifa->ifa_netmask, size),
        .flags = 0x80, .index = index, // IFA_F_PERMANENT
    };
    if ((af == AF_INET && local[0] == 127) ||
            (af == AF_INET6 && IN6_IS_ADDR_LOOPBACK((const struct in6_addr *) local)))
        addr.scope = 254; // RT_SCOPE_HOST
    else if ((af == AF_INET && local[0] == 169 && local[1] == 254) ||
            (af == AF_INET6 && local[0] == 0xfe && (local[1] & 0xc0) == 0x80))
        addr.scope = 253; // RT_SCOPE_LINK
    begin(b, RTM_GETADDR_ - 2);
    append(b, &addr, sizeof(addr));
    const void *peer = NULL;
    if (af == AF_INET && (ifa->ifa_flags & IFF_POINTOPOINT) && ifa->ifa_dstaddr &&
            ifa->ifa_dstaddr->sa_family == af)
        peer = ip_bytes(ifa->ifa_dstaddr);
    attr(b, 1, peer ? peer : local, size); // IFA_ADDRESS
    if (af == AF_INET) attr(b, 2, local, size); // IFA_LOCAL
    attr(b, 3, ifa->ifa_name, strlen(ifa->ifa_name) + 1); // IFA_LABEL
    end(b);
}

static int error_message(void *out, size_t capacity, struct nl_header req,
                         uint32_t port, int error) {
    // NLM_F_CAPPED: original request header included, payload omitted.
    struct { struct nl_header h; int32_t error; struct nl_header request; } reply = {
        .h = {sizeof(reply), NL_ERROR, 0x100, req.seq, port}, .error = error, .request = req,
    };
    if (capacity < sizeof(reply)) return _ENOBUFS;
    memcpy(out, &reply, sizeof(reply));
    return sizeof(reply);
}
int netlink_snapshot(const void *request, size_t size, uint32_t port,
                     void *response, size_t capacity) {
    struct nl_header req;
    if (size < sizeof(req)) return _EINVAL;
    memcpy(&req, request, sizeof(req));
    int error = _EINVAL;
    if (req.len < sizeof(req) + 1 || req.len > size || ALIGN4(req.len) < size ||
            !(req.flags & NL_REQUEST)) goto fail;
    error = _EOPNOTSUPP;
    if ((req.flags & NL_DUMP) != NL_DUMP ||
            (req.type != RTM_GETLINK_ && req.type != RTM_GETADDR_ && req.type != RTM_GETROUTE_))
        goto fail;
    uint8_t family = ((const uint8_t *) request)[sizeof(req)];
    error = _EAFNOSUPPORT;
    if (family != 0 && family != 2 && family != 10) goto fail;
    struct builder b = {response, 0, capacity, 0, req.seq, port, 0};
    if (req.type != RTM_GETROUTE_) {
        struct ifaddrs *all;
        if (getifaddrs(&all) < 0) { error = errno_map(); goto fail; }
        int ctl = req.type == RTM_GETLINK_ ? socket(AF_INET, SOCK_DGRAM, 0) : -1;
        for (struct ifaddrs *ifa = all; ifa && !b.error; ifa = ifa->ifa_next) {
            if (!ifa->ifa_name) continue;
            if (req.type == RTM_GETADDR_) {
                addr_message(&b, ifa, family);
            } else {
                struct ifaddrs *prev;
                for (prev = all; prev != ifa; prev = prev->ifa_next)
                    if (prev->ifa_name && !strcmp(prev->ifa_name, ifa->ifa_name)) break;
                if (prev == ifa) link_message(&b, ifa, all, ctl);
            }
        }
        if (ctl >= 0) close(ctl);
        freeifaddrs(all);
    }
    // Routes remain an empty dump: host routing is not guest-configurable.
    begin(&b, NL_DONE);
    int32_t status = 0;
    append(&b, &status, sizeof(status));
    end(&b);
    if (b.error) { error = b.error; goto fail; }
    return b.used;
fail:
    return error_message(response, capacity, req, port, error);
}
