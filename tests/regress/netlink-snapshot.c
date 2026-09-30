// Standalone Linux encoder test: link with fs/netlink.c.
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "fs/netlink.h"
#include "kernel/errno.h"
// The isolated Linux harness has no guest task/signal runtime.
int errno_map(void) { return -errno; }
struct header { uint32_t len; uint16_t type, flags; uint32_t seq, pid; };
struct request { struct header h; uint8_t family; uint8_t padding[3]; };
int main(void) {
    struct request req = {{17, 18, 0x301, 123, 999}, 0, {0}};
    _Alignas(8) unsigned char out[4096 + 16];
    memset(out, 0xa5, sizeof(out));
    assert(netlink_snapshot(&req, 3, 44, out, 4096) == _EINVAL);
    int n = netlink_snapshot(&req, sizeof(req), 44, out, 4096);
    assert(n > 20 && n <= 4096);
    struct header *h = (void *) out;
    assert(h->type == 16 && h->seq == 123 && h->pid == 44);
    // A full snapshot that cannot fit is replaced by one error, never partial DONE.
    n = netlink_snapshot(&req, sizeof(req), 44, out, 36);
    assert(n == 36 && h->type == 2 && (h->flags & 0x100));
    int32_t error;
    memcpy(&error, out + 16, 4);
    assert(error == _ENOBUFS);
    assert(netlink_snapshot(&req, sizeof(req), 44, out, 16) == _ENOBUFS);
    req.h.type = 26;
    n = netlink_snapshot(&req, sizeof(req), 44, out, 4096);
    assert(n == 20 && h->type == 3 && h->flags == 2);
    for (unsigned family = 2; family <= 10; family += 8) {
        req.h.type = 22; req.family = family;
        n = netlink_snapshot(&req, sizeof(req), 44, out, 4096);
        size_t off = 0;
        while (off < (size_t) n) {
            struct header current;
            memcpy(&current, out + off, sizeof(current));
            assert(current.len >= 16 && off + current.len <= (size_t) n);
            if (current.type == 20) assert(out[off + 16] == family);
            else assert(current.type == 3);
            off += (current.len + 3) & ~3u;
        }
        assert(off == (size_t) n);
    }
    // Deterministic malformed-header/capacity sweep under ASan/UBSan.
    for (unsigned i = 0; i < 10000; i++) {
        req.h.len = i % 40;
        req.h.type = i % 30;
        req.h.flags = i & 1 ? 0x301 : 1;
        req.family = i % 16;
        size_t cap = i % 4097;
        memset(out, 0xa5, sizeof(out));
        n = netlink_snapshot(&req, i % 21, 44, out, cap);
        assert(n < 0 || (size_t) n <= cap);
        for (size_t j = cap; j < sizeof(out); j++) assert(out[j] == 0xa5);
    }
    puts("NETLINK_SNAPSHOT_PASS 10000 malformed/capacity cases");
}
