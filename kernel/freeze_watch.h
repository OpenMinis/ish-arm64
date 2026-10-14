#ifndef KERNEL_FREEZE_WATCH_H
#define KERNEL_FREEZE_WATCH_H
// [T-ish-freeze-watch] Detects a guest that has stopped making progress.
//
// Field case 2026-10-13 (Minis 1.16 (1), iPhone18,2): from 18:32 the guest
// made no fork and no syscall for 30 minutes. Shell commands waited for a
// spawn that never returned, and the user tapped Stop eleven times before
// swiping the app away. Every host log kept running; none said the guest had
// stopped.
//
// Pure and header-only: the host sampler ([CPUTop], every 10 s) feeds it
// lock-free counters, and tests/regress/regress_freeze_watch.c includes this
// same header. Nothing here takes a lock or touches kernel state - a frozen
// guest may be frozen precisely because a kernel lock is held.
//
// A quiet guest is not a frozen one: `sleep 600` makes no fork, no syscall
// and uses no CPU for ten minutes. What a quiet but healthy kernel never does
// is leave a host spawn request unanswered - creating a process takes
// milliseconds - so "frozen" needs both: no progress for ISH_FREEZE_ENTER_S,
// and a spawn the host has been waiting on for at least
// ISH_FREEZE_SPAWN_MIN_S.
#include <stdint.h>

/// No fork and no syscall for this long (with a spawn waiting) = frozen.
/// About 3 minutes: well past any normal spawn, short enough to be in the
/// log before a user gives up (the 10-13 user swiped after ~30 min).
#define ISH_FREEZE_ENTER_S 180
/// While frozen, one reminder line this often instead of one per sample.
#define ISH_FREEZE_REMIND_S 300
/// A spawn pending for less than this is not yet evidence.
#define ISH_FREEZE_SPAWN_MIN_S 30
/// Consecutive samples with no progress needed as well as the time. A
/// suspended app takes no samples, so the first one after it resumes can
/// see minutes of "no progress" that the guest simply was not allowed to
/// make; requiring several observed samples (about a minute at the 10 s
/// cadence) keeps that from reading as a freeze.
#define ISH_FREEZE_MIN_QUIET_SAMPLES 6

struct ish_freeze_sample {
    uint64_t now_ns;                 // monotonic
    uint64_t forks;                  // ish_guest_forks
    uint64_t syscalls;               // ish_guest_syscalls
    int64_t live_procs;              // ish_live_guest_procs
    uint64_t spawn_pending_since_ns; // 0 = no host spawn waiting
};

enum ish_freeze_event {
    ISH_FREEZE_NONE = 0,
    ISH_FREEZE_ENTERED,              // log once
    ISH_FREEZE_STILL,                // reminder (every ISH_FREEZE_REMIND_S)
    ISH_FREEZE_RECOVERED,            // progress resumed after a freeze
};

struct ish_freeze_watch {
    int have_prev;
    uint64_t prev_forks, prev_syscalls;
    uint64_t last_progress_ns;
    unsigned quiet_samples;
    int frozen;
    uint64_t last_report_ns;
};

static inline void ish_freeze_watch_init(struct ish_freeze_watch *w) {
    *w = (struct ish_freeze_watch) {0};
}

static inline enum ish_freeze_event ish_freeze_watch_step(struct ish_freeze_watch *w,
                                                          struct ish_freeze_sample s) {
    if (!w->have_prev) {
        w->have_prev = 1;
        w->prev_forks = s.forks;
        w->prev_syscalls = s.syscalls;
        w->last_progress_ns = s.now_ns;
        return ISH_FREEZE_NONE;
    }
    // Any change counts, including a counter going backwards (a reset).
    int progressed = s.forks != w->prev_forks || s.syscalls != w->prev_syscalls || s.live_procs <= 0;
    w->prev_forks = s.forks;
    w->prev_syscalls = s.syscalls;
    if (progressed) {
        w->last_progress_ns = s.now_ns;
        w->quiet_samples = 0;
        if (w->frozen) {
            w->frozen = 0;
            return ISH_FREEZE_RECOVERED;
        }
        return ISH_FREEZE_NONE;
    }
    w->quiet_samples++;
    if (w->frozen) {
        if (s.now_ns - w->last_report_ns >= (uint64_t)ISH_FREEZE_REMIND_S * 1000000000ULL) {
            w->last_report_ns = s.now_ns;
            return ISH_FREEZE_STILL;
        }
        return ISH_FREEZE_NONE;
    }
    uint64_t quiet_ns = s.now_ns - w->last_progress_ns;
    int spawn_waiting = s.spawn_pending_since_ns != 0 && s.now_ns > s.spawn_pending_since_ns
        && s.now_ns - s.spawn_pending_since_ns >= (uint64_t)ISH_FREEZE_SPAWN_MIN_S * 1000000000ULL;
    if (quiet_ns >= (uint64_t)ISH_FREEZE_ENTER_S * 1000000000ULL && spawn_waiting
        && w->quiet_samples >= ISH_FREEZE_MIN_QUIET_SAMPLES) {
        w->frozen = 1;
        w->last_report_ns = s.now_ns;
        return ISH_FREEZE_ENTERED;
    }
    return ISH_FREEZE_NONE;
}

/// Seconds since the guest last made progress, while frozen; 0 otherwise.
static inline uint64_t ish_freeze_watch_frozen_for_s(const struct ish_freeze_watch *w, uint64_t now_ns) {
    if (!w->frozen || now_ns < w->last_progress_ns) return 0;
    return (now_ns - w->last_progress_ns) / 1000000000ULL;
}

#endif
