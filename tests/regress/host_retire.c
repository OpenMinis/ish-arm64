// [FIX-261114-33] kernel/retire.h: a reaped struct stays allocated for
// RETIRE_SLOTS pushes, and a write that arrives meanwhile is reported.
//
// Build and run on the host: cc -I. -o /tmp/host_retire tests/regress/host_retire.c && /tmp/host_retire
// With -fsanitize=address it also shows that nothing is freed early, twice or leaked
// (apart from the entries still in the ring at exit).
#include <stdio.h>
#include "kernel/retire.h"

static int failures;
#define CHECK(cond, what) do { \
    if (cond) printf("  ok    %s\n", what); else { printf("  FAIL  %s\n", what); failures++; } \
} while (0)

struct thing { char pad[1400]; };

int main(void) {
    static struct retire_ring ring;
    struct retire_report rep;
    struct thing *first[RETIRE_SLOTS];

    // Fill the ring: nothing leaves.
    bool none_left = true;
    for (int i = 0; i < RETIRE_SLOTS; i++) {
        first[i] = malloc(sizeof(struct thing));
        memset(first[i], 0x11 + i, sizeof(struct thing));
        retire_push(&ring, first[i], sizeof(struct thing), 1000 + i, &rep);
        if (rep.evicted) none_left = false;
    }
    CHECK(none_left, "nothing is freed while the ring fills");

    // While retired, the memory is still ours: a late user can read what it left...
    CHECK(((unsigned char *) first[0])[700] == 0x11 && ((unsigned char *) first[RETIRE_SLOTS - 1])[0] == 0x11 + RETIRE_SLOTS - 1,
          "a retired struct keeps its contents");
    // ...and write. Entry 1 gets a late write of 8 bytes at offset 96, and one byte at 1399.
    memset((char *) first[1] + 96, 0xEE, 8);
    first[1]->pad[1399] ^= 1;

    // The next push displaces entry 0, which nobody touched.
    struct thing *extra = malloc(sizeof(struct thing));
    memset(extra, 0x77, sizeof(struct thing));
    retire_push(&ring, extra, sizeof(struct thing), 2000, &rep);
    CHECK(rep.evicted && !rep.changed && rep.tag == 1000 && rep.ptr == first[0] && rep.size == sizeof(struct thing),
          "the oldest entry leaves first, reported clean");

    // The one after displaces entry 1: the late write is found, with its extent.
    extra = malloc(sizeof(struct thing));
    memset(extra, 0x78, sizeof(struct thing));
    retire_push(&ring, extra, sizeof(struct thing), 2001, &rep);
    CHECK(rep.evicted && rep.changed && rep.tag == 1001, "a late write is reported, with the tag of the struct it hit");
    CHECK(rep.first == 96 && rep.last == 1399 && rep.bytes == 9, "with the first and last changed byte and the count");

    // A write that puts back the same bytes is not a change.
    memset(first[2], 0x13, sizeof(struct thing));
    extra = malloc(sizeof(struct thing));
    retire_push(&ring, extra, sizeof(struct thing), 2002, &rep);
    CHECK(rep.evicted && !rep.changed && rep.tag == 1002, "identical bytes are not a change");

    // Many more than the ring holds: each entry leaves exactly once, in order.
    bool order = true;
    int expect = 1003;
    for (int i = 0; i < 5 * RETIRE_SLOTS; i++) {
        struct thing *t = malloc(sizeof(struct thing));
        memset(t, i, sizeof(struct thing));
        retire_push(&ring, t, sizeof(struct thing), 3000 + i, &rep);
        if (!rep.evicted || rep.changed) order = false;
        if (expect < 1000 + RETIRE_SLOTS) { if (rep.tag != expect) order = false; expect++; }
    }
    CHECK(order, "every entry leaves once, oldest first, over many rounds");

    // Different sizes share a ring; the report carries each one's own size.
    static struct retire_ring mixed;
    char *small = malloc(16), *big = malloc(4096);
    memset(small, 1, 16); memset(big, 2, 4096);
    retire_push(&mixed, small, 16, 1, &rep);
    retire_push(&mixed, big, 4096, 2, &rep);
    big[4095] = 9;
    bool sizes = true;
    for (int i = 0; i < RETIRE_SLOTS; i++) {
        char *t = malloc(32);
        memset(t, 3, 32);
        retire_push(&mixed, t, 32, 10 + i, &rep);
        if (i == RETIRE_SLOTS - 2 && !(rep.tag == 1 && rep.size == 16 && !rep.changed)) sizes = false;
        if (i == RETIRE_SLOTS - 1 && !(rep.tag == 2 && rep.size == 4096 && rep.changed && rep.first == 4095 && rep.bytes == 1)) sizes = false;
    }
    CHECK(sizes, "entries of different sizes are compared over their own size");

    printf(failures == 0 ? "ALL PASSED\n" : "%d FAILED\n", failures);
    return failures != 0;
}
