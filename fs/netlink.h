#ifndef ISH_NETLINK_H
#define ISH_NETLINK_H

#include <stddef.h>
#include <stdint.h>

#define AF_NETLINK_ 16
#define NETLINK_ROUTE_ 0
#define NETLINK_BUFFER_SIZE 4096

struct sockaddr_nl_ {
    uint16_t family, pad;
    uint32_t pid, groups;
};

// Construct one bounded, complete multipart snapshot (including NLMSG_DONE),
// or NLMSG_ERROR. Returns its size, or a negative guest errno. No host changes.
int netlink_snapshot(const void *request, size_t size, uint32_t port,
                     void *response, size_t capacity);

#endif
