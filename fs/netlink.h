#ifndef FS_NETLINK_H
#define FS_NETLINK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// AF_NETLINK stub: what a guest's rtnetlink requests are answered with.
//
// There is no Linux network stack under the guest and Darwin has no
// AF_NETLINK, so nothing is forwarded. What the host can tell us truthfully
// is synthesized from getifaddrs(): the link list (RTM_GETLINK) and the
// address list (RTM_GETADDR). Everything the host cannot tell us (routes,
// rules, neighbours, qdiscs, any change request, every netlink protocol
// other than NETLINK_ROUTE) is refused at once with NLMSG_ERROR -- never
// left unanswered, because an unanswered request is a reader blocked for
// good. See fs/sock.c for the socket half.

// The largest datagram netlink_stub_answer() emits. One message per
// datagram, so any reader's buffer (Go: one page; musl/busybox: 8K) fits it.
#define NETLINK_STUB_DGRAM_MAX 1024

// What the last request on a socket was: enough to finish a dump whose
// replies could not all be queued.
struct netlink_stub_req {
    bool valid;
    uint32_t seq;
    uint16_t type;
    uint16_t flags;
};

// Called once per reply datagram. A negative return stops the answer.
typedef int (*netlink_stub_emit_t)(void *ctx, const void *dgram, size_t len);

// Answer every request in `req` (one sendmsg may carry several messages).
// `protocol` is the socket's netlink protocol (NETLINK_ROUTE is 0).
// Returns 0, or the first negative value `emit` returned. `last` receives
// the last request seen.
int netlink_stub_answer(int protocol, const void *req, size_t req_len,
                        netlink_stub_emit_t emit, void *ctx,
                        struct netlink_stub_req *last);

// An NLMSG_DONE for `seq`, written to `out` (at least 20 bytes). Returns
// its length.
size_t netlink_stub_done(void *out, uint32_t seq);

// How RTM_GETROUTE is answered: false (default) = NLMSG_ERROR EOPNOTSUPP,
// true = an empty dump. Set with ISH_NETLINK_ROUTE=error|empty.
bool netlink_stub_route_empty(void);

// --- the host's interfaces, as the guest is shown them -------------------

struct netlink_host_link {
    char name[16];       // guest name: the host's, except lo0 is "lo"
    unsigned index;      // host ifindex (lo is 1)
    unsigned flags;      // Linux IFF_* bits
    unsigned mtu;
    uint16_t arphrd;     // Linux ARPHRD_*
    uint8_t operstate;   // Linux IF_OPER_*
    bool has_lladdr;
    uint8_t lladdr[6];
    // The host's counters for the interface (zero when it gives none).
    uint64_t rx_bytes, rx_packets, rx_errs, rx_drop, rx_multicast;
    uint64_t tx_bytes, tx_packets, tx_errs, collisions;
};

struct netlink_host_addr {
    char name[16];
    unsigned index;
    uint8_t family;      // Linux AF_INET (2) / AF_INET6 (10)
    uint8_t prefixlen;
    uint8_t scope;       // Linux RT_SCOPE_*
    bool has_peer, has_brd;
    uint8_t local[16], peer[16], brd[16];
};

// Snapshot of the host's interfaces. Both arrays are malloc'd (free with
// netlink_host_free) and ordered by ifindex. When the host cannot be asked
// the list is the loopback alone, which is true of every device. Returns 0.
int netlink_host_snapshot(struct netlink_host_link **links, size_t *n_links,
                          struct netlink_host_addr **addrs, size_t *n_addrs);
void netlink_host_free(struct netlink_host_link *links, struct netlink_host_addr *addrs);

#endif
