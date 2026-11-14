#ifndef KERNEL_RETIRE_H
#define KERNEL_RETIRE_H
// [FIX-261114-33] A short quarantine for structs that late users may still touch.
//
// A struct pushed here is not freed at once: it stays allocated, untouched,
// until RETIRE_SLOTS more have been pushed after it. A thread that still
// holds a pointer to it (a leaked or exiting guest thread, a timer thread)
// then reads and writes memory that is still ours instead of a block the
// allocator has taken back - a write into a freed block lands on the
// allocator's free list and aborts the process in some unrelated free().
//
// A copy is taken on the way in and compared on the way out, so a write that
// arrived in between is reported with its offsets instead of going unseen.
//
// Not thread safe: the caller serialises (pids_lock for thread groups).
// Header-only so the host tests can compile it on its own.
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#define RETIRE_SLOTS 32

struct retire_slot {
    void *ptr;
    void *copy;   // NULL when the copy could not be allocated: no check then
    size_t size;
    int tag;
};

struct retire_ring {
    struct retire_slot slots[RETIRE_SLOTS];
    unsigned next;
};

// What was found in the entry that left the ring.
struct retire_report {
    bool evicted;   // an entry left (and was freed)
    bool changed;   // it was written after it was retired
    int tag;        // the tag it was pushed with
    void *ptr;      // its address (already freed: for logging only)
    size_t size;
    size_t first;   // first and last changed byte
    size_t last;
    size_t bytes;   // how many bytes differ
};

// Retire `ptr` (malloc'd, `size` bytes). Frees the entry it displaces, if
// any, and describes it in `report`.
static inline void retire_push(struct retire_ring *ring, void *ptr, size_t size, int tag,
                               struct retire_report *report) {
    struct retire_slot *slot = &ring->slots[ring->next];
    memset(report, 0, sizeof(*report));
    if (slot->ptr != NULL) {
        report->evicted = true;
        report->tag = slot->tag;
        report->ptr = slot->ptr;
        report->size = slot->size;
        if (slot->copy != NULL) {
            const unsigned char *now = slot->ptr, *then = slot->copy;
            for (size_t i = 0; i < slot->size; i++) {
                if (now[i] == then[i])
                    continue;
                if (report->bytes == 0)
                    report->first = i;
                report->last = i;
                report->bytes++;
            }
            report->changed = report->bytes > 0;
            free(slot->copy);
        }
        free(slot->ptr);
    }
    slot->ptr = ptr;
    slot->size = size;
    slot->tag = tag;
    slot->copy = malloc(size);
    if (slot->copy != NULL)
        memcpy(slot->copy, ptr, size);
    ring->next = (ring->next + 1) % RETIRE_SLOTS;
}

#endif
