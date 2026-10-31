#ifndef KERNEL_DIAG_H
#define KERNEL_DIAG_H

// [FIX-261123-ISHMAC-CRASH] No-op stand-in for the iOS line's kernel/diag.h.
//
// The iOS line (compat/suite-more) has always-on diagnostic probes (counters,
// event rings, /proc/ish/diag, the crash-dump block; FEAT-261027-15 and later)
// that its crash and race fixes call into. This Mac line does not carry that
// module (no kernel/diag.c, no /proc/ish/diag). The fixes ported from there
// are kept byte-for-byte; their probe calls land here and compile to nothing.
// Everything in this header is observation only on the iOS line too - no
// value here changes what the kernel does - except DIAG_TIMER_MAGIC, which
// posix_timer_callback compares against to drop a fire on a dead slot (so it
// keeps the iOS value), and the slot marking in ish_diag_timer_leak below.
//
// If the diag module is ever ported here, replace this file with the real one.

#include <stdint.h>

enum ish_diag_counter {
    // [FIX-261031-04] POSIX timers and signal numbers
    DIAG_TIMER_CREATE,
    DIAG_TIMER_DELETE,
    DIAG_TIMER_DELETE_OOB,
    DIAG_TIMER_SETTIME_OOB,
    DIAG_TIMER_SIGNO_REJECTED,
    DIAG_SEND_SIGNAL_OOB,
    DIAG_TIMER_LEAKED_AT_EXIT,
    DIAG_TIMER_THREADS_LIVE,
    // [INV-261109-03]
    DIAG_TIMER_STALE_SKIPPED,
    DIAG_TIMER_STRUCT_BAD,
    DIAG_TIMER_INHERITED_OPS,
    // [FIX-261114-33]
    DIAG_GROUP_LATE_WRITE,
};

enum ish_diag_event {
    DIAG_EV_SEND_SIGNAL_OOB,
    DIAG_EV_TIMER_OOB,
    DIAG_EV_TIMER_LEAK_EXIT,
    DIAG_EV_TIMER_STALE,
    DIAG_EV_TIMER_FIRE,
};

enum ish_diag_timer_call {
    DIAG_TIMER_CALL_DELETE = 1,
    DIAG_TIMER_CALL_SETTIME = 2,
    DIAG_TIMER_CALL_GETTIME = 3,
    DIAG_TIMER_CALL_GETOVERRUN = 4,
};
#define DIAG_TIMER_SLOT_EMPTY 16

// Functional: set in a posix_timer by timer_create, cleared by timer_delete,
// fork and group death; posix_timer_callback drops a fire without it.
#define DIAG_TIMER_MAGIC 0x54494d52u    // "TIMR"

enum ish_diag_timer_life {
    DIAG_TL_CREATE = 1,
    DIAG_TL_DELETE,
    DIAG_TL_GROUP_DEAD,
    DIAG_TL_GROUP_FREE,
    DIAG_TL_INHERITED_OP,
    DIAG_TL_STALE_SKIP,
    DIAG_TL_STRUCT_BAD,
};

struct tgroup;
struct posix_timer;
struct timer;

static inline void ish_diag_add(enum ish_diag_counter c, uint64_t n) { (void) c; (void) n; }
static inline void ish_diag_event(enum ish_diag_event type, int64_t a, int64_t b) { (void) type; (void) a; (void) b; }
static inline uint32_t ish_diag_timer_seq_next(void) { return 0; }
static inline void ish_diag_timer_fire(const struct posix_timer *timer) { (void) timer; }
// Not observation only: besides counting, the iOS version marks every slot
// still holding a timer as gone (magic 0), which is what makes
// posix_timer_callback drop a later fire into the dead group (INV-261109-03).
// That part is kept. Needs struct tgroup, so it exists only where
// kernel/task.h came first (its one caller, kernel/exit.c).
#ifdef TASK_H
static inline void ish_diag_timer_leak(struct tgroup *group) {
    for (int i = 0; i < TIMERS_MAX; i++) {
        struct posix_timer *t = &group->posix_timers[i];
        if (t->timer == NULL)
            continue;
        __atomic_store_n(&t->magic, 0, __ATOMIC_RELAXED);
    }
}
#endif
static inline void ish_diag_timer_life(enum ish_diag_timer_life kind, const void *slot, const struct timer *timer,
                                       const struct tgroup *group, int id, int64_t x) {
    (void) kind; (void) slot; (void) timer; (void) group; (void) id; (void) x;
}
static inline void ish_diag_timer_struct_bad(const struct timer *timer, enum ish_diag_timer_call call, uint32_t magic) {
    (void) timer; (void) call; (void) magic;
}
static inline void ish_diag_timer_stale_skip(const struct posix_timer *slot, const struct timer *firing) {
    (void) slot; (void) firing;
}

#endif
