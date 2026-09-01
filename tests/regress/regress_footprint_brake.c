// [T-ish-footprint-brake] Pins the footprint-governor admission logic:
// mode switching, hysteresis, staleness fail-closed, the sanity gate, and
// the two paths that must keep working under BRAKE (growsdown fault page)
// or start being gated (pt_set_flags' reservation commit — the old
// admission bypass).
//
// Like the sibling suites, this MIRRORS the logic in kernel/mmap.c /
// kernel/memory.c rather than linking them (they drag in the whole kernel).
// The clock is a controllable variable here precisely so staleness is
// testable without sleeping. If you change the real logic, change the
// mirror — the comments on each block name the source function.
//
// Build & run:
//   cc -O1 -pthread -o /tmp/t_brake tests/regress/regress_footprint_brake.c && /tmp/t_brake
#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>

#define PAGE_SIZE 4096
#define P_READ 1
#define P_WRITE 2
#define P_EXEC 4
#define P_ANONYMOUS 8

enum ish_mem_state { ISH_MEM_OK = 0, ISH_MEM_BRAKE = 1 };
#define ISH_MEM_STALE_MS 2000

// ------------------------------------------------ mirrored governor state

static _Atomic long anon_page_count;
static _Atomic long anon_page_limit = 131072;
static _Atomic int mem_state_v = ISH_MEM_OK;
static _Atomic uint64_t mem_limit_v;
static _Atomic uint64_t mem_avail_v;
static _Atomic uint64_t mem_stamp_ms;

// The test's controllable clock (the real code uses CLOCK_MONOTONIC).
static uint64_t fake_now_ms = 100000;
static uint64_t monotonic_ms(void) { return fake_now_ms; }

static bool ish_footprint_mode(void) { return atomic_load(&mem_stamp_ms) != 0; }

// Mirrors ish_set_memory_status().
static void ish_set_memory_status(uint64_t limit, uint64_t avail, bool critical) {
    if (limit == 0) return;
    int prev = atomic_load(&mem_state_v);
    int next;
    if (critical) next = ISH_MEM_BRAKE;
    else if (avail * 10 < limit) next = ISH_MEM_BRAKE;
    else if (prev == ISH_MEM_BRAKE && avail * 100 < limit * 15) next = ISH_MEM_BRAKE;
    else next = ISH_MEM_OK;
    atomic_store(&mem_limit_v, limit);
    atomic_store(&mem_avail_v, avail);
    atomic_store(&mem_state_v, next);
    atomic_store(&mem_stamp_ms, monotonic_ms());
}

// Mirrors ish_mem_commit_ok().
static bool ish_mem_commit_ok(uint64_t bytes) {
    uint64_t stamp = atomic_load(&mem_stamp_ms);
    if (stamp == 0) return true;
    uint64_t limit = atomic_load(&mem_limit_v);
    if (limit != 0 && bytes > limit) return false;
    if (monotonic_ms() - stamp > ISH_MEM_STALE_MS) return false;
    return atomic_load(&mem_state_v) == ISH_MEM_OK;
}

// ------------------------------------- mirrored reserve (mmap.c) + helpers

static __thread long precharged;
static void anon_pages_precharged(long p) { precharged = p; }

// Mirrors anon_pages_reserve() with both regimes.
static bool anon_pages_reserve(long pages) {
    if (ish_footprint_mode()) {
        if (!ish_mem_commit_ok((uint64_t)pages * PAGE_SIZE)) return false;
        atomic_fetch_add(&anon_page_count, pages);
        anon_pages_precharged(pages);
        return true;
    }
    long limit = atomic_load(&anon_page_limit);
    long count = atomic_load(&anon_page_count);
    do {
        if (count + pages > limit) return false;
    } while (!atomic_compare_exchange_weak(&anon_page_count, &count, count + pages));
    anon_pages_precharged(pages);
    return true;
}
static void anon_pages_unreserve(long pages) { atomic_fetch_sub(&anon_page_count, pages); }

// Minimal pt_map_nothing: consumes the precharge like the real one.
static bool map_should_fail;
static int pt_map_nothing(long pages) {
    if (map_should_fail) { anon_pages_precharged(0); return -12; }
    long credit = precharged; precharged = 0;
    if (credit < pages) atomic_fetch_add(&anon_page_count, pages - credit);
    return 0;
}

// Mirrors pt_set_flags' gated reservation-commit (memory.c).
static int mprotect_commit(long run_len) {
    if (!anon_pages_reserve(run_len)) return -12;   // _ENOMEM
    int merr = pt_map_nothing(run_len);
    if (merr < 0) { anon_pages_unreserve(run_len); return merr; }
    return 0;
}

// Mirrors the growsdown rule (memory.c:1125): the fault page itself is never
// refused — on a failed window reservation the caller shrinks to one page
// and maps it unconditionally.
static long growsdown_fault(long window) {
    long got = window;
    if (!anon_pages_reserve(window)) got = 1;   // shrink, then map regardless
    pt_map_nothing(got);
    return got;
}

// ------------------------------------------------------------------ harness

static int checks, failures;
static void check(const char *n, bool ok) {
    checks++;
    if (!ok) { failures++; printf("  \033[31mFAIL\033[0m %s\n", n); }
    else printf("  \033[32mok\033[0m   %s\n", n);
}
static void reset(void) {
    atomic_store(&anon_page_count, 0);
    atomic_store(&anon_page_limit, 131072);
    atomic_store(&mem_state_v, ISH_MEM_OK);
    atomic_store(&mem_limit_v, 0);
    atomic_store(&mem_avail_v, 0);
    atomic_store(&mem_stamp_ms, 0);
    fake_now_ms = 100000;
    map_should_fail = false;
    precharged = 0;
}

#define MB (1024ULL * 1024)

int main(void) {
    printf("[1] legacy mode: no feed installed, ledger enforced as before\n");
    {
        reset();
        check("small reserve passes", anon_pages_reserve(100));
        check("counter charged", atomic_load(&anon_page_count) == 100);
        check("reserve past ledger limit refused", !anon_pages_reserve(131073));
        check("refusal added nothing", atomic_load(&anon_page_count) == 100);
    }

    printf("\n[2] footprint OK: commitment is admitted PAST the ledger limit\n");
    {
        reset();
        ish_set_memory_status(2048 * MB, 1024 * MB, false);   // 50%% headroom
        check("mode is footprint", ish_footprint_mode());
        // 160k pages = 640MB guest — the exact V8 cage commit the ledger
        // used to refuse. Commitment is cheap; only dirty pages kill.
        check("V8-cage-sized reserve admitted", anon_pages_reserve(163840));
        check("counter still counts (accounting)", atomic_load(&anon_page_count) == 163840);
        check("counter exceeding old ledger limit is fine", atomic_load(&anon_page_count) > 131072);
    }

    printf("\n[3] BRAKE below 10%% headroom, refusal adds nothing\n");
    {
        reset();
        ish_set_memory_status(2000 * MB, 150 * MB, false);    // 7.5%%
        check("state is BRAKE", atomic_load(&mem_state_v) == ISH_MEM_BRAKE);
        check("tiny reserve refused", !anon_pages_reserve(2));
        check("counter unchanged", atomic_load(&anon_page_count) == 0);
    }

    printf("\n[4] hysteresis: 12%% stays braked, 16%% releases\n");
    {
        reset();
        ish_set_memory_status(2000 * MB, 150 * MB, false);    // enter BRAKE
        ish_set_memory_status(2000 * MB, 240 * MB, false);    // 12%% — inside band
        check("12%% keeps the brake on", atomic_load(&mem_state_v) == ISH_MEM_BRAKE);
        check("still refusing", !anon_pages_reserve(2));
        ish_set_memory_status(2000 * MB, 320 * MB, false);    // 16%% — released
        check("16%% releases the brake", atomic_load(&mem_state_v) == ISH_MEM_OK);
        check("admitting again", anon_pages_reserve(2));
    }

    printf("\n[5] critical pressure event forces BRAKE despite headroom\n");
    {
        reset();
        ish_set_memory_status(2000 * MB, 1000 * MB, true);
        check("critical -> BRAKE at 50%% headroom", atomic_load(&mem_state_v) == ISH_MEM_BRAKE);
        check("refusing", !anon_pages_reserve(2));
        ish_set_memory_status(2000 * MB, 1000 * MB, false);   // pressure cleared
        check("released once pressure clears", anon_pages_reserve(2));
    }

    printf("\n[6] sanity gate: one ask larger than the whole limit\n");
    {
        reset();
        ish_set_memory_status(2048 * MB, 1500 * MB, false);   // plenty of room
        long four_gb_pages = (long)(4096 * MB / PAGE_SIZE);
        check("4GB single request refused upfront", !anon_pages_reserve(four_gb_pages));
        check("normal request still admitted", anon_pages_reserve(100));
    }

    printf("\n[7] stale feed fails closed; fresh feed reopens\n");
    {
        reset();
        ish_set_memory_status(2048 * MB, 1024 * MB, false);
        check("fresh feed admits", anon_pages_reserve(10));
        fake_now_ms += ISH_MEM_STALE_MS + 1;                  // sampler died
        check("stale feed refuses", !anon_pages_reserve(10));
        ish_set_memory_status(2048 * MB, 1024 * MB, false);   // sampler back
        check("fresh feed admits again", anon_pages_reserve(10));
    }

    printf("\n[8] growsdown fault page survives the brake\n");
    {
        reset();
        ish_set_memory_status(2000 * MB, 100 * MB, false);    // braked
        long got = growsdown_fault(256);
        check("window shrank instead of refusing", got == 1);
        check("the fault page WAS mapped", atomic_load(&anon_page_count) == 1);
    }

    printf("\n[9] mprotect-commit is gated (the old admission bypass)\n");
    {
        reset();
        ish_set_memory_status(2000 * MB, 100 * MB, false);    // braked
        check("cage commit refused under BRAKE", mprotect_commit(32768) == -12);
        check("counter untouched by the refusal", atomic_load(&anon_page_count) == 0);
        ish_set_memory_status(2000 * MB, 1000 * MB, false);   // released
        check("cage commit admitted when OK", mprotect_commit(32768) == 0);
        check("commit charged the counter", atomic_load(&anon_page_count) == 32768);
        // Host mmap failure inside the commit must roll the reservation back.
        map_should_fail = true;
        check("host-mmap failure surfaces", mprotect_commit(64) == -12);
        check("failed commit left the counter balanced", atomic_load(&anon_page_count) == 32768);
    }

    printf("\n%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
