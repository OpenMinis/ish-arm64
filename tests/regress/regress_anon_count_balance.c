// [T-ish-anon-count-negative] Charge/uncharge balance for anon_page_count.
//
// Device evidence 2026-08-25 (iPhone18,1): the cap logged
//   "in use -86998 pages (-1359 MB host)"
// i.e. the counter had gone NEGATIVE, so the cap was granting 1.3GB of free
// headroom on top of its 2048MB limit while the app climbed to a 3338MB
// jetsam kill. Root cause: pt_unmap decremented for EVERY P_ANONYMOUS page,
// but only 6 of 15 mapping sites charged — the rest (notably pt_set_flags'
// mprotect-commit, which materialises whole 32k-page runs) mapped for free.
//
// These tests model the FIXED contract:
//   * pt_map_nothing charges every page it maps, minus any precharge parked
//     by a caller that reserved first.
//   * PROT_NONE mappings are neither charged nor decremented — both sides
//     gate on the same predicate.
//   * pt_unmap decrements exactly what was charged.
// Every case asserts the counter never goes negative and returns to zero.
//
// Build: cc -O1 -pthread -o regress_anon_count_balance regress_anon_count_balance.c

#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PAGE_BITS 12
#define MAX_PAGES 8192
#define P_READ  (1 << 0)
#define P_WRITE (1 << 1)
#define P_EXEC  (1 << 2)
#define P_ANONYMOUS (1 << 6)

static size_t real_page_size = 16384;
static _Atomic long anon_page_count;
static _Atomic long anon_page_limit = 1L << 30;
static long negative_observations;   // how often the guard would have fired

static bool anon_page_is_charged(unsigned flags) {
    return (flags & (P_READ | P_WRITE | P_EXEC)) != 0;
}

static __thread long anon_precharged_pages;
static void anon_pages_precharged(long p) { anon_precharged_pages = p; }
static long anon_pages_precharge_peek(void) { return anon_precharged_pages; }

static void anon_count_check(long count) {
    if (count < 0) negative_observations++;
}

static bool anon_pages_reserve(long pages) {
    long limit = atomic_load(&anon_page_limit);
    long count = atomic_load(&anon_page_count);
    anon_count_check(count);
    do {
        if (count + pages > limit) return false;
    } while (!atomic_compare_exchange_weak(&anon_page_count, &count, count + pages));
    anon_pages_precharged(pages);
    return true;
}
static void anon_pages_unreserve(long pages) { atomic_fetch_sub(&anon_page_count, pages); }

static void anon_pages_charge_mapped(long pages) {
    long credit = anon_precharged_pages;
    anon_precharged_pages = 0;
    if (credit >= pages) return;
    atomic_fetch_add(&anon_page_count, pages - credit);
}

// ------------------------------------------------------------- page table

struct pt { bool present; unsigned flags; int owner; };
static struct pt table[MAX_PAGES];
static pthread_mutex_t memlock = PTHREAD_MUTEX_INITIALIZER;
static int next_owner = 1;

static int pt_map_nothing(long start, long pages, unsigned flags) {
    if (pages <= 0 || start < 0 || start + pages > MAX_PAGES) {
        anon_pages_precharged(0);
        return -1;
    }
    int owner = next_owner++;
    for (long p = start; p < start + pages; p++) {
        if (table[p].present) { anon_pages_precharged(0); return -1; }
    }
    for (long p = start; p < start + pages; p++) {
        table[p].present = true;
        table[p].flags = flags | P_ANONYMOUS;
        table[p].owner = owner;
    }
    if (anon_page_is_charged(flags)) anon_pages_charge_mapped(pages);
    else anon_pages_precharged(0);
    return 0;
}

static void pt_unmap(long start, long pages) {
    for (long p = start; p < start + pages; p++) {
        if (p < 0 || p >= MAX_PAGES || !table[p].present) continue;
        if ((table[p].flags & P_ANONYMOUS) && anon_page_is_charged(table[p].flags))
            atomic_fetch_sub(&anon_page_count, 1);
        table[p].present = false;
        table[p].flags = 0;
    }
    anon_count_check(atomic_load(&anon_page_count));
}

static int pt_map_cluster(long page, unsigned flags, long *out) {
    long cp = (long)(real_page_size >> PAGE_BITS); if (cp < 1) cp = 1;
    long base = page & ~(cp - 1);
    bool whole = cp > 1 && base >= 0 && base + cp <= MAX_PAGES;
    if (whole)
        for (long p = base; p < base + cp; p++)
            if (table[p].present) { whole = false; break; }
    long start = whole ? base : page, count = whole ? cp : 1;
    long held = anon_pages_precharge_peek();
    int err = pt_map_nothing(start, count, flags);
    if (err < 0 && whole) {
        anon_pages_precharged(held);        // re-park for the retry
        err = pt_map_nothing(page, 1, flags);
        count = 1;
    }
    if (err < 0) { if (out) *out = 0; return err; }
    if (out) *out = count;
    return 0;
}

/// The mprotect-commit path (pt_set_flags) — the one that leaked in production.
static void pt_set_flags_commit(long start, long pages, unsigned flags) {
    for (long p = start; p < start + pages; p++) {
        if (table[p].present) continue;
        long run_start = p, run_len = 1;
        while (p + 1 < start + pages && !table[p + 1].present) { p++; run_len++; }
        pt_map_nothing(run_start, run_len, flags);   // charges internally now
    }
}

/// [T-ish-anon-count-mprotect] The flag-CHANGE half of pt_set_flags, on pages
/// that are already present. Rewriting flags moves the very predicate the
/// charge/uncharge pair keys on, so the count must follow the transition.
static void pt_set_flags_change(long start, long pages, unsigned flags) {
    for (long p = start; p < start + pages; p++) {
        if (!table[p].present) continue;
        unsigned old_flags = table[p].flags;
        table[p].flags = flags | (old_flags & P_ANONYMOUS);
        if (old_flags & P_ANONYMOUS) {
            bool was = anon_page_is_charged(old_flags);
            bool now = anon_page_is_charged(table[p].flags);
            if (was != now)
                atomic_fetch_add(&anon_page_count, now ? 1 : -1);
        }
    }
    anon_count_check(atomic_load(&anon_page_count));
}

/// [T-ish-anon-count-cow-predicate] fork(): the child gets its own set of
/// anonymous pages. The counting predicate MUST match pt_unmap's decrement
/// predicate, or the child is charged for pages that are never given back.
static long pt_copy_on_write(long start, long pages, int child_owner) {
    long anon_copied = 0;
    for (long p = start; p < start + pages; p++) {
        if (!table[p].present) continue;
        if ((table[p].flags & P_ANONYMOUS) && anon_page_is_charged(table[p].flags))
            anon_copied++;
    }
    atomic_fetch_add(&anon_page_count, anon_copied);
    (void)child_owner;
    return anon_copied;
}

// ------------------------------------------------------------------ harness

static int checks, failures;
static void check(const char *n, bool ok) {
    checks++;
    if (!ok) { failures++; printf("  \033[31mFAIL\033[0m %s\n", n); }
    else printf("  \033[32mok\033[0m   %s\n", n);
}

/// Counted but not printed — for assertions inside a long loop, where one line
/// per iteration would bury the result. A failure still lands in the total, and
/// the loop's own summary check reports the aggregate.
static void check_silent(bool ok) {
    checks++;
    if (!ok) failures++;
}
static void reset(void) {
    memset(table, 0, sizeof(table));
    atomic_store(&anon_page_count, 0);
    atomic_store(&anon_page_limit, 1L << 30);
    anon_precharged_pages = 0; negative_observations = 0;
    real_page_size = 16384; next_owner = 1;
}

struct warg { int base; };
static void *worker(void *vp) {
    struct warg *a = vp;
    unsigned seed = (unsigned)(size_t)a;
    for (int i = 0; i < 20000; i++) {
        long page = a->base + (rand_r(&seed) % 32);
        unsigned flags = (rand_r(&seed) % 4 == 0) ? 0 : (P_READ | P_WRITE);
        pthread_mutex_lock(&memlock);
        if (!table[page].present) {
            if (anon_page_is_charged(flags)) {
                if (anon_pages_reserve(4)) {
                    long got = 0;
                    pt_map_cluster(page, flags, &got);
                    if (got < 4) anon_pages_unreserve(4 - got);
                }
            } else {
                pt_map_cluster(page, flags, NULL);
            }
        } else {
            pt_unmap(page, 1);
        }
        pthread_mutex_unlock(&memlock);
    }
    return NULL;
}

int main(void) {
    printf("\n[1] the production leak: mprotect-commit maps whole runs\n");
    {
        reset();
        // 32k-page run, exactly the V8/malloc-bomb shape.
        pt_set_flags_commit(0, 4096, P_READ | P_WRITE);
        long after_map = atomic_load(&anon_page_count);
        check("run of 4096 pages charged in full", after_map == 4096);
        pt_unmap(0, 4096);
        check("counter back to zero after unmap", atomic_load(&anon_page_count) == 0);
        check("never went negative", negative_observations == 0);
    }

    printf("\n[2] PROT_NONE: neither charged nor decremented\n");
    {
        reset();
        pt_map_nothing(100, 64, 0);                 // PROT_NONE reservation
        check("PROT_NONE mapping charges nothing", atomic_load(&anon_page_count) == 0);
        pt_unmap(100, 64);
        check("PROT_NONE unmap does not decrement", atomic_load(&anon_page_count) == 0);
        check("never went negative", negative_observations == 0);
    }

    printf("\n[3] mixed PROT_NONE + committed, interleaved (the -1359MB shape)\n");
    {
        reset();
        for (int round = 0; round < 200; round++) {
            pt_map_nothing(round * 8, 4, 0);                       // reservation
            pt_map_nothing(round * 8 + 4, 4, P_READ | P_WRITE);    // committed
        }
        check("only committed pages counted", atomic_load(&anon_page_count) == 200 * 4);
        for (int round = 0; round < 200; round++) pt_unmap(round * 8, 8);
        check("counter back to zero", atomic_load(&anon_page_count) == 0);
        check("never went negative", negative_observations == 0);
    }

    printf("\n[4] reserve-then-map is charged ONCE, not twice\n");
    {
        reset();
        bool ok = anon_pages_reserve(4);
        check("reserve succeeded", ok && atomic_load(&anon_page_count) == 4);
        pt_map_nothing(500, 4, P_READ | P_WRITE);
        check("map consumed the precharge (still 4, not 8)",
              atomic_load(&anon_page_count) == 4);
        pt_unmap(500, 4);
        check("back to zero", atomic_load(&anon_page_count) == 0);
    }

    printf("\n[5] cluster retry does not double-charge\n");
    {
        reset();
        // Block the cluster so the first map fails and the retry runs.
        table[601].present = true; table[601].flags = P_READ | P_WRITE | P_ANONYMOUS;
        long before = atomic_load(&anon_page_count);
        anon_pages_reserve(4);
        long got = 0;
        pt_map_cluster(600, P_READ | P_WRITE, &got);
        check("degraded to a single page", got == 1);
        if (got < 4) anon_pages_unreserve(4 - got);
        check("charged exactly 1 page for it",
              atomic_load(&anon_page_count) == before + 1);
        table[601].present = false;
        pt_unmap(600, 1);
        check("back to zero", atomic_load(&anon_page_count) == 0);
        check("never went negative", negative_observations == 0);
    }

    printf("\n[6] failed map leaves no charge and no stale precharge\n");
    {
        reset();
        anon_pages_reserve(4);
        int err = pt_map_nothing(MAX_PAGES - 1, 8, P_READ | P_WRITE);  // overflows
        check("oversized map rejected", err < 0);
        anon_pages_unreserve(4);
        check("counter back to zero", atomic_load(&anon_page_count) == 0);
        // A later unrelated map must not silently consume the dead credit.
        pt_map_nothing(700, 2, P_READ | P_WRITE);
        check("next map charged in full (no stale credit)",
              atomic_load(&anon_page_count) == 2);
        pt_unmap(700, 2);
        check("back to zero", atomic_load(&anon_page_count) == 0);
    }

    printf("\n[7] double unmap cannot drive the counter below zero\n");
    {
        reset();
        pt_map_nothing(800, 4, P_READ | P_WRITE);
        pt_unmap(800, 4);
        pt_unmap(800, 4);                 // repeat — pages already gone
        check("second unmap is a no-op", atomic_load(&anon_page_count) == 0);
        check("never went negative", negative_observations == 0);
    }

    printf("\n[8] stress: mixed flags, random map/unmap, 200k ops\n");
    {
        reset();
        unsigned seed = 999;
        long min_seen = 0;
        for (int i = 0; i < 200000; i++) {
            long page = rand_r(&seed) % 2000;
            unsigned flags = (rand_r(&seed) % 3 == 0) ? 0 : (P_READ | P_WRITE);
            if (!table[page].present) pt_map_cluster(page, flags, NULL);
            else pt_unmap(page, 1);
            long c = atomic_load(&anon_page_count);
            if (c < min_seen) min_seen = c;
        }
        for (long p = 0; p < MAX_PAGES; p++) if (table[p].present) pt_unmap(p, 1);
        check("counter never dipped below zero", min_seen == 0);
        check("all pages released, counter zero", atomic_load(&anon_page_count) == 0);
        check("guard never fired", negative_observations == 0);
    }

    printf("\n[9] concurrency: 6 threads, mixed PROT_NONE and committed\n");
    {
        reset();
        pthread_t t[6]; struct warg a[6];
        for (int i = 0; i < 6; i++) { a[i].base = 100 + i * 32; pthread_create(&t[i], NULL, worker, &a[i]); }
        for (int i = 0; i < 6; i++) pthread_join(t[i], NULL);
        for (long p = 0; p < MAX_PAGES; p++) if (table[p].present) pt_unmap(p, 1);
        check("counter back to zero under contention", atomic_load(&anon_page_count) == 0);
        check("guard never fired", negative_observations == 0);
    }

    printf("\n[10] the guard itself: a negative count IS detected\n");
    {
        reset();
        atomic_store(&anon_page_count, -5);   // simulate the production bug
        anon_pages_reserve(1);
        check("guard observed the negative count", negative_observations == 1);
    }

    // [T-ish-anon-count-mprotect] The pairing breaks in BOTH directions when
    // mprotect rewrites flags without adjusting the count. Each case below
    // FAILED before the fix: [11] leaked, [12] went negative.
    printf("\n[11] mprotect RW -> PROT_NONE -> munmap must not leak\n");
    {
        reset();
        pt_map_nothing(0, 64, P_READ | P_WRITE);
        check("charged while RW", atomic_load(&anon_page_count) == 64);
        pt_set_flags_change(0, 64, 0);                  // drop to PROT_NONE
        check("uncharged on losing RW", atomic_load(&anon_page_count) == 0);
        pt_unmap(0, 64);
        check("counter back to zero after unmap", atomic_load(&anon_page_count) == 0);
        check("guard never fired", negative_observations == 0);
    }

    printf("\n[12] mprotect PROT_NONE -> RW -> munmap must not go negative\n");
    {
        reset();
        pt_map_nothing(0, 64, 0);                       // PROT_NONE: uncharged
        check("PROT_NONE reservation costs nothing", atomic_load(&anon_page_count) == 0);
        pt_set_flags_change(0, 64, P_READ | P_WRITE);   // commit it
        check("charged on gaining RW", atomic_load(&anon_page_count) == 64);
        pt_unmap(0, 64);
        check("counter back to zero after unmap", atomic_load(&anon_page_count) == 0);
        check("guard never fired (no negative dip)", negative_observations == 0);
    }

    printf("\n[13] a protection change that moves no memory must move no count\n");
    {
        reset();
        pt_map_nothing(0, 32, P_READ | P_WRITE);
        long before = atomic_load(&anon_page_count);
        pt_set_flags_change(0, 32, P_READ | P_EXEC);    // RW -> RX: still charged
        check("RW -> RX leaves the count alone", atomic_load(&anon_page_count) == before);
        pt_unmap(0, 32);
        check("counter back to zero", atomic_load(&anon_page_count) == 0);
    }

    // [T-ish-anon-count-cow-predicate] The production failure: a fork of a
    // PROT_NONE region charged the child for pages pt_unmap refuses to return.
    printf("\n[14] fork of a PROT_NONE cage must not leak (the Node bug)\n");
    {
        reset();
        const long cage = 512;                          // stands in for a V8 chunk
        pt_map_nothing(0, cage, 0);                     // PROT_NONE reservation
        check("cage costs nothing while reserved", atomic_load(&anon_page_count) == 0);
        long copied = pt_copy_on_write(0, cage, 2);     // fork
        check("fork charges nothing for PROT_NONE pages", copied == 0);
        check("count still zero after fork", atomic_load(&anon_page_count) == 0);
        pt_unmap(0, cage);                              // child exits
        check("count still zero after child exit", atomic_load(&anon_page_count) == 0);
    }

    printf("\n[15] repeated fork/exit cycles must not drift upward\n");
    {
        reset();
        // Mixed region: a PROT_NONE cage plus genuinely committed pages, which
        // is what a real process image looks like.
        pt_map_nothing(0, 256, 0);                      // cage
        pt_map_nothing(256, 64, P_READ | P_WRITE);      // real anonymous memory
        long baseline = atomic_load(&anon_page_count);
        check("baseline counts only the committed pages", baseline == 64);
        for (int i = 0; i < 200; i++) {
            long copied = pt_copy_on_write(0, 320, 100 + i);   // fork
            check_silent(copied == 64);                        // only committed pages
            atomic_fetch_sub(&anon_page_count, copied);        // child exits, returns them
        }
        check("200 fork/exit cycles left no drift",
              atomic_load(&anon_page_count) == baseline);
        pt_unmap(0, 320);
        check("counter back to zero", atomic_load(&anon_page_count) == 0);
        check("guard never fired", negative_observations == 0);
    }

    printf("\n%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
