#ifndef KERNEL_DIAG_H
#define KERNEL_DIAG_H

// [FEAT-261027-15] Always-on diagnostic probes for the BoringSSL teardown
// crash: counters, a small event ring, and per-thread ownership, written out
// by the app's crash handler. Observation only - nothing here changes what
// the kernel does. Never called from the TLB lookup, generated code or block
// dispatch: every probe sits on a cold path (exec, fork, exit, munmap,
// translation-time AOT lookup, jetsam, the exit safety valve).

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

enum ish_diag_counter {
    // memory and thread lifecycle
    DIAG_UNMAP_CALLS,           // pt_unmap_always calls
    DIAG_UNMAP_PAGES,           // pages it unmapped
    DIAG_UNMAP_HOST_FREES,      // host munmaps of guest backing (refcount reached 0)
    DIAG_UNMAP_SHARED_MM,       // pt_unmap_always calls while another task shares the mm
    DIAG_MEM_CHANGED,           // mmu generation bumps
    DIAG_WX_CLEARS,             // W^X: a code page written under the mem read lock
    DIAG_JETSAM_ADDED,          // translated blocks invalidated (put on a jetsam list)
    DIAG_JETSAM_FREED,          // blocks freed from it
    DIAG_VALVE_LEAKED,          // threads the exit safety valve gave up on
    DIAG_DEFERRED_RELEASED,     // ...whose mm was later released by the thread itself
    DIAG_MM_DESTROYED,
    DIAG_EXIT_GROUPS,
    // exec / threads
    DIAG_EXECS,
    DIAG_EXEC_NODE_DEFAULT,     // node started in the default (AOT / hybrid) mode
    DIAG_EXEC_NODE_JITLESS,     // node started with --jitless
    DIAG_OWN_THREADS,           // guest threads iSH created
    DIAG_BORROWS,               // app threads lent to the kernel (begin calls)
    DIAG_BORROW_RENAMES,        // app threads iSH renamed (never restored)
    // AOT, at translation time
    DIAG_AOT_FIX_B,             // moved hits whose b / bl immediate differs from the image
    DIAG_AOT_FIX_BCOND,
    DIAG_AOT_FIX_CBZ,
    DIAG_AOT_FIX_TBZ,
    DIAG_AOT_FIX_ADR,
    DIAG_AOT_FIX_ADRP,
    DIAG_AOT_FIX_OTHER,
    DIAG_AOT_GIVEUP_DATA,       // moved candidate refused: a non-instruction word differs
    DIAG_AOT_GIVEUP_LOOP,       // ...a self-loop translation whose words differ
    DIAG_AOT_GIVEUP_INSN,       // ...an instruction differs beyond its pc-relative immediate
    DIAG_AOT_GIVEUP_LDRLIT,     // ...of which: an ldr (literal) whose offset moved
    DIAG_AOT_GIVEUP_ADRSHIFT,   // ...adr / adrp targets moved by different distances
    DIAG_AOT_REJECTED_IMAGES,
    DIAG_COUNTER_COUNT
};

extern _Atomic uint64_t ish_diag_counters[DIAG_COUNTER_COUNT];

static inline void ish_diag_add(enum ish_diag_counter c, uint64_t n) {
    atomic_fetch_add_explicit(&ish_diag_counters[c], n, memory_order_relaxed);
}

// Peaks (set with a relaxed compare-exchange loop on cold paths).
extern _Atomic int64_t ish_diag_live_node, ish_diag_peak_procs, ish_diag_peak_node;
void ish_diag_note_procs(int64_t live);

enum ish_diag_event {
    DIAG_EV_EXEC = 1,           // node started; a: 1 default mode, 2 jitless
    DIAG_EV_MM_DESTROY,         // a: mm seq
    DIAG_EV_EXIT_GROUP,         // a: threads in the group
    DIAG_EV_BIG_MUNMAP,         // a: pages, b: other tasks on the mm
    DIAG_EV_VALVE_LEAK,         // a: threads leaked
    DIAG_EV_DEFERRED_RELEASE,   // a: refcount before
    DIAG_EV_AOT_REJECT,         // a: image abi, b: this ish's abi
    DIAG_EV_BORROW_RENAME,      // a: pid the app thread was renamed for
    DIAG_EV_BORROW_BEGIN,       // a: borrow depth after
};
// One event: time, thread, pid, two numbers. 64 slots, the oldest overwritten.
void ish_diag_event(enum ish_diag_event type, int64_t a, int64_t b);

// Thread ownership. iSH's own guest threads are marked once at start; an
// app thread (GCD worker) running kernel code is marked for the span the app
// lends it (between become_new_init_child and restoring current).
void ish_diag_thread_own(int pid);
void ish_diag_thread_renamed(int pid);
void ish_diag_borrow_begin(void);
void ish_diag_borrow_end(void);

// exec of node: the mode its V8 runs in.
void ish_diag_node_mode(int jitless);
// exec done (`current` runs the new program): counts it and keeps the live /
// peak node count of its process.
void ish_diag_exec(int is_node);
// A process's group is torn down (or forked: the copy starts as not node).
struct tgroup;
void ish_diag_group_gone(struct tgroup *group);
void ish_diag_group_forked(struct tgroup *group);

// Set by the JIT engine (asbestos/guest-arm64/jit.c) when it is built in.
extern void (*ish_diag_jit_counts)(uint64_t out[6]);

// Text for /proc/ish/diag and the app's snapshots: one "key=value" list.
size_t ish_diag_describe(char *buf, size_t size);

// Crash time, async-signal-safe: no malloc, no locks, only write(2). Writes
// the calling (crashing) thread's ownership, the counters and the event ring.
void ish_diag_write_crash(int fd);

#endif
