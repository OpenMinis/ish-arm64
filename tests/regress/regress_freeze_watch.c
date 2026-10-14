// [T-ish-freeze-watch] Pins the iSH freeze detector (kernel/freeze_watch.h).
//
// Field case 2026-10-13 (Minis 1.16 (1), iPhone18,2): from 18:32 the guest
// made no fork and no syscall for 30 minutes while shell commands waited for
// a spawn that never returned. Every host log kept running and none said the
// guest had stopped. The detector is a pure function over successive samples,
// so this test includes the real header and feeds it sequences - no kernel.
//
// Build & run:
//   cc -O1 -I. -o /tmp/t_freeze tests/regress/regress_freeze_watch.c && /tmp/t_freeze
#include <stdio.h>
#include <string.h>
#include "kernel/freeze_watch.h"

static int failures = 0;
#define CHECK(label, cond) do { \
    if (cond) printf("  ok   %s\n", label); \
    else { printf("  FAIL %s\n", label); failures++; } } while (0)

// One sample every 10 s, as the [CPUTop] sampler takes them.
#define T(s) ((uint64_t)(s) * 1000000000ULL)

static struct ish_freeze_sample busy(uint64_t t, uint64_t forks, uint64_t sys) {
    return (struct ish_freeze_sample) { .now_ns = T(t), .forks = forks, .syscalls = sys,
        .live_procs = 5, .spawn_pending_since_ns = 0 };
}
static struct ish_freeze_sample stuck(uint64_t t, uint64_t forks, uint64_t sys, uint64_t pending_since) {
    return (struct ish_freeze_sample) { .now_ns = T(t), .forks = forks, .syscalls = sys,
        .live_procs = 5, .spawn_pending_since_ns = T(pending_since) };
}

int main(void) {
    struct ish_freeze_watch w;

    printf("a. a healthy guest never trips\n");
    ish_freeze_watch_init(&w);
    int events = 0;
    for (int i = 0; i < 100; i++) {
        if (ish_freeze_watch_step(&w, busy(i * 10, i * 3, i * 1000)) != ISH_FREEZE_NONE) events++;
    }
    CHECK("no event over 1000 s of progress", events == 0);
    CHECK("frozen_for is 0", ish_freeze_watch_frozen_for_s(&w, T(1000)) == 0);

    printf("b. quiet guest with no spawn waiting (e.g. `sleep 600`) is not a freeze\n");
    ish_freeze_watch_init(&w);
    events = 0;
    for (int i = 0; i < 100; i++) {
        if (ish_freeze_watch_step(&w, busy(i * 10, 7, 500)) != ISH_FREEZE_NONE) events++;
    }
    CHECK("no event without a pending spawn", events == 0);

    printf("c. the 10-13 shape: no progress + a spawn waiting\n");
    ish_freeze_watch_init(&w);
    ish_freeze_watch_step(&w, busy(0, 100, 5000));
    ish_freeze_watch_step(&w, busy(10, 110, 6000));        // last progress at t=10
    int entered_at = -1, reminders = 0;
    for (int t = 20; t <= 1800; t += 10) {
        enum ish_freeze_event e = ish_freeze_watch_step(&w, stuck(t, 110, 6000, 15));
        if (e == ISH_FREEZE_ENTERED && entered_at < 0) entered_at = t;
        if (e == ISH_FREEZE_ENTERED && entered_at >= 0 && t != entered_at) reminders = -100;
        if (e == ISH_FREEZE_STILL) reminders++;
    }
    CHECK("entered once, about 3 min after the last progress",
          entered_at >= 10 + ISH_FREEZE_ENTER_S && entered_at <= 10 + ISH_FREEZE_ENTER_S + 10);
    CHECK("then a reminder every 5 min, not every sample",
          reminders == (1800 - entered_at) / ISH_FREEZE_REMIND_S);
    CHECK("frozen_for counts from the last progress",
          ish_freeze_watch_frozen_for_s(&w, T(1800)) == 1790);

    printf("d. recovery\n");
    enum ish_freeze_event r = ish_freeze_watch_step(&w, busy(1810, 111, 6001));
    CHECK("progress after a freeze reports RECOVERED once", r == ISH_FREEZE_RECOVERED);
    CHECK("…and frozen_for drops to 0", ish_freeze_watch_frozen_for_s(&w, T(1810)) == 0);
    CHECK("next healthy sample is quiet", ish_freeze_watch_step(&w, busy(1820, 112, 6002)) == ISH_FREEZE_NONE);

    printf("e. any one signal of progress resets the clock\n");
    ish_freeze_watch_init(&w);
    ish_freeze_watch_step(&w, busy(0, 1, 1));
    int tripped = 0;
    for (int t = 10; t <= 600; t += 10) {
        // forks frozen, but syscalls still move: not frozen
        if (ish_freeze_watch_step(&w, stuck(t, 1, 1 + t, 5)) != ISH_FREEZE_NONE) tripped = 1;
    }
    CHECK("syscalls still counting -> not frozen", !tripped);
    ish_freeze_watch_init(&w);
    ish_freeze_watch_step(&w, busy(0, 1, 1));
    tripped = 0;
    for (int t = 10; t <= 600; t += 10) {
        if (ish_freeze_watch_step(&w, stuck(t, 1 + t, 1, 5)) != ISH_FREEZE_NONE) tripped = 1;
    }
    CHECK("forks still counting -> not frozen", !tripped);

    printf("f. a spawn that just started does not count yet\n");
    ish_freeze_watch_init(&w);
    ish_freeze_watch_step(&w, busy(0, 1, 1));
    tripped = 0;
    for (int t = 10; t <= 600; t += 10) {
        // a fresh spawn request every sample, each pending for < 1 s
        if (ish_freeze_watch_step(&w, stuck(t, 1, 1, t)) != ISH_FREEZE_NONE) tripped = 1;
    }
    CHECK("a spawn younger than ISH_FREEZE_SPAWN_MIN_S is not evidence", !tripped);

    printf("g. no guest at all\n");
    ish_freeze_watch_init(&w);
    struct ish_freeze_sample empty = stuck(0, 0, 0, 0);
    empty.live_procs = 0;
    tripped = 0;
    for (int t = 0; t <= 600; t += 10) {
        empty.now_ns = T(t);
        if (ish_freeze_watch_step(&w, empty) != ISH_FREEZE_NONE) tripped = 1;
    }
    CHECK("zero live guest processes never trips", !tripped);

    printf("h. counter wrap / restart is progress, not a freeze\n");
    ish_freeze_watch_init(&w);
    ish_freeze_watch_step(&w, busy(0, 1000, 1000));
    CHECK("counters going down count as change",
          ish_freeze_watch_step(&w, stuck(10, 5, 5, 1)) == ISH_FREEZE_NONE
          && ish_freeze_watch_frozen_for_s(&w, T(10)) == 0);

    printf("i. one sample after a long gap (app suspended) is not a freeze\n");
    ish_freeze_watch_init(&w);
    ish_freeze_watch_step(&w, busy(0, 10, 10));
    // The app was suspended for 20 minutes with a spawn already waiting; the
    // first sample after resuming sees no change yet.
    CHECK("a single quiet sample after a 1200 s gap does not enter",
          ish_freeze_watch_step(&w, stuck(1200, 10, 10, 5)) == ISH_FREEZE_NONE);
    CHECK("…and progress on the next sample keeps it quiet",
          ish_freeze_watch_step(&w, busy(1210, 11, 12)) == ISH_FREEZE_NONE);
    // A real freeze after the gap still needs ISH_FREEZE_MIN_QUIET_SAMPLES samples.
    ish_freeze_watch_init(&w);
    ish_freeze_watch_step(&w, busy(0, 10, 10));
    int at = -1;
    for (int t = 1200, n = 0; n < 20; t += 10, n++) {
        if (ish_freeze_watch_step(&w, stuck(t, 10, 10, 5)) == ISH_FREEZE_ENTERED) { at = n + 1; break; }
    }
    CHECK("after a gap it enters only once enough quiet samples were seen",
          at == ISH_FREEZE_MIN_QUIET_SAMPLES);

    printf(failures == 0 ? "ALL PASSED\n" : "%d FAILED\n", failures);
    return failures == 0 ? 0 : 1;
}
