#include <stddef.h>
#include <stdbool.h>
#include "fs/proc.h"
#include "fs/netlink.h"

// /proc/net/dev - one line per interface the host has, with the host's own
// counters. The same snapshot answers RTM_GETLINK (fs/netlink.c), so `ip
// link`, getifaddrs() and this file name the same interfaces.
static int proc_show_net_dev(struct proc_entry *UNUSED(entry), struct proc_data *buf) {
    proc_printf(buf, "Inter-|   Receive                                                |  Transmit\n");
    proc_printf(buf, " face |bytes    packets errs drop fifo frame compressed multicast|bytes    packets errs drop fifo colls carrier compressed\n");

    struct netlink_host_link *links;
    struct netlink_host_addr *addrs;
    size_t n_links, n_addrs;
    netlink_host_snapshot(&links, &n_links, &addrs, &n_addrs);
    for (size_t i = 0; i < n_links; i++) {
        const struct netlink_host_link *l = &links[i];
        proc_printf(buf, "%6s: %7llu %7llu %4llu %4llu    0     0          0 %9llu",
                    l->name, (unsigned long long) l->rx_bytes, (unsigned long long) l->rx_packets,
                    (unsigned long long) l->rx_errs, (unsigned long long) l->rx_drop,
                    (unsigned long long) l->rx_multicast);
        proc_printf(buf, " %8llu %7llu %4llu    0    0 %5llu       0          0\n",
                    (unsigned long long) l->tx_bytes, (unsigned long long) l->tx_packets,
                    (unsigned long long) l->tx_errs, (unsigned long long) l->collisions);
    }
    netlink_host_free(links, addrs);
    return 0;
}

// /proc/net/if_inet6 - the host's IPv6 addresses, in Linux's format:
// address ifindex prefixlen scope flags name
static int proc_show_net_if_inet6(struct proc_entry *UNUSED(entry), struct proc_data *buf) {
    struct netlink_host_link *links;
    struct netlink_host_addr *addrs;
    size_t n_links, n_addrs;
    netlink_host_snapshot(&links, &n_links, &addrs, &n_addrs);
    for (size_t i = 0; i < n_addrs; i++) {
        const struct netlink_host_addr *a = &addrs[i];
        if (a->family != 10)
            continue;
        for (int b = 0; b < 16; b++)
            proc_printf(buf, "%02x", a->local[b]);
        // Linux's scope byte here: 0x10 host, 0x20 link, 0x00 global.
        unsigned scope = a->scope == 254 ? 0x10 : a->scope == 253 ? 0x20 : 0x00;
        proc_printf(buf, " %02x %02x %02x 80 %8s\n", a->index & 0xff, a->prefixlen, scope, a->name);
    }
    netlink_host_free(links, addrs);
    return 0;
}

// There is deliberately no /proc/net/route: the host's routing table is not
// available to us, and a made-up one would be read as the truth.

// /proc/net directory entries
struct proc_dir_entry proc_net_entries[] = {
    {"dev", .show = proc_show_net_dev},
    {"if_inet6", .show = proc_show_net_if_inet6},
};
#define PROC_NET_LEN (sizeof(proc_net_entries)/sizeof(proc_net_entries[0]))

static bool proc_net_readdir(struct proc_entry *UNUSED(entry), unsigned long *index, struct proc_entry *next_entry) {
    if (*index < PROC_NET_LEN) {
        *next_entry = (struct proc_entry) {&proc_net_entries[*index], *index, NULL, NULL, 0, 0};
        (*index)++;
        return true;
    }
    return false;
}

// Exported /proc/net directory entry
struct proc_children proc_net_children = PROC_CHILDREN({
    {"dev", .show = proc_show_net_dev},
    {"if_inet6", .show = proc_show_net_if_inet6},
});
