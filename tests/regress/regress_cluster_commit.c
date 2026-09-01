// [T-ish-cluster-commit] Cluster-commit regression tests.
//
// Models the pt_map/pt_unmap ownership contract and the pt_map_cluster
// selection rule, with INSTRUMENTED host mmap/munmap so the tests can assert
// call counts directly rather than inferring them from RSS.
//
// The model mirrors the real code:
//   * pt_map(start, pages, memory) points `pages` guest pages at one host
//     allocation and sets refcount = pages.
//   * pt_unmap decrements per page and munmaps only when refcount hits 0.
//   * pt_map_cluster commits the aligned host-page-sized run containing the
//     faulting page, but only when every page in it is unmapped and the
//     same_flags predicate accepts it; otherwise it commits a single page.
//
// Build: cc -O1 -pthread -o regress_cluster_commit regress_cluster_commit.c
//
// Kept independent of the kernel headers on purpose: those need the whole
// emu/ tree and a guest target, which the regress suite does not build.

#include <assert.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PAGE_BITS 12
#define PAGE_SIZE (1 << PAGE_BITS)
#define MAX_PAGES 4096

static size_t real_page_size = 16384; // Apple Silicon default; overridden in tests

// ---------------------------------------------------------------- host model

static long host_mmap_calls, host_munmap_calls, host_bytes_live;
static pthread_mutex_t host_lock = PTHREAD_MUTEX_INITIALIZER;

struct data {
    void *base;
    size_t size;
    int refcount;
};

static void *host_mmap(size_t size) {
    pthread_mutex_lock(&host_lock);
    host_mmap_calls++;
    // Host rounds every allocation up to its page size — this is the 4x waste.
    size_t rounded = (size + real_page_size - 1) & ~(real_page_size - 1);
    host_bytes_live += (long)rounded;
    pthread_mutex_unlock(&host_lock);
    void *p = malloc(size ? size : 1);
    assert(p != NULL);
    return p;
}

static void host_munmap(void *p, size_t size) {
    pthread_mutex_lock(&host_lock);
    host_munmap_calls++;
    size_t rounded = (size + real_page_size - 1) & ~(real_page_size - 1);
    host_bytes_live -= (long)rounded;
    pthread_mutex_unlock(&host_lock);
    free(p);
}

// ------------------------------------------------------------- page-table model

struct pt_entry {
    struct data *data;
    unsigned flags;
    bool present;
};

struct mem {
    struct pt_entry pt[MAX_PAGES];
    pthread_mutex_t lock;
    int reservation_of[MAX_PAGES]; // -1 = none; models mem_find_reservation
};

static void mem_init(struct mem *m) {
    memset(m->pt, 0, sizeof(m->pt));
    for (int i = 0; i < MAX_PAGES; i++) m->reservation_of[i] = -1;
    pthread_mutex_init(&m->lock, NULL);
}

static long anon_page_count, anon_page_limit = 1L << 30;

static bool anon_pages_reserve(long pages) {
    long c;
    pthread_mutex_lock(&host_lock);
    if (anon_page_count + pages > anon_page_limit) { pthread_mutex_unlock(&host_lock); return false; }
    anon_page_count += pages; c = anon_page_count;
    pthread_mutex_unlock(&host_lock);
    (void)c;
    return true;
}
static void anon_pages_unreserve(long pages) {
    pthread_mutex_lock(&host_lock);
    anon_page_count -= pages;
    pthread_mutex_unlock(&host_lock);
}

// pt_map: one host allocation shared by `pages` guest pages (refcounted).
static int pt_map(struct mem *m, long start, long pages, unsigned flags) {
    if (start < 0 || pages <= 0 || start + pages > MAX_PAGES) return -1;
    struct data *d = calloc(1, sizeof(*d));
    assert(d != NULL);
    d->size = (size_t)pages * PAGE_SIZE;
    d->base = host_mmap(d->size);
    d->refcount = 0;
    for (long p = start; p < start + pages; p++) {
        assert(!m->pt[p].present && "pt_map over a live page");
        m->pt[p].data = d;
        m->pt[p].flags = flags;
        m->pt[p].present = true;
        d->refcount++;
    }
    return 0;
}

// pt_unmap: decrement per page; munmap only when the last page goes away.
static int pt_unmap(struct mem *m, long start, long pages) {
    for (long p = start; p < start + pages; p++) {
        if (p < 0 || p >= MAX_PAGES || !m->pt[p].present) continue;
        struct data *d = m->pt[p].data;
        anon_pages_unreserve(1);
        m->pt[p].present = false;
        m->pt[p].data = NULL;
        if (--d->refcount == 0) {
            host_munmap(d->base, d->size);
            free(d);
        }
    }
    return 0;
}

typedef bool (*same_flags_fn)(struct mem *m, long page, void *ctx);

static bool reservation_cluster_ok(struct mem *m, long page, void *ctx) {
    return m->reservation_of[page] == *(int *)ctx;
}

// The function under test.
static int pt_map_cluster(struct mem *m, long page, unsigned flags,
                          same_flags_fn same, void *ctx, long *committed_out) {
    long cluster_pages = (long)(real_page_size >> PAGE_BITS);
    if (cluster_pages < 1) cluster_pages = 1;
    long base = page & ~(cluster_pages - 1);
    bool whole = cluster_pages > 1;

    if (base < 0 || base + cluster_pages > MAX_PAGES) whole = false;
    if (whole) {
        for (long p = base; p < base + cluster_pages; p++) {
            if (m->pt[p].present) { whole = false; break; }
            if (same != NULL && !same(m, p, ctx)) { whole = false; break; }
        }
    }
    long start = whole ? base : page;
    long count = whole ? cluster_pages : 1;
    int err = pt_map(m, start, count, flags);
    if (err < 0) {
        if (whole) { err = pt_map(m, page, 1, flags); count = 1; }
        if (err < 0) { if (committed_out) *committed_out = 0; return err; }
    }
    if (committed_out) *committed_out = count;
    return 0;
}

// ------------------------------------------------------- concurrency worker

struct arg { struct mem *m; int base; };

static void *cluster_worker(void *vp) {
    struct arg *a = vp;
    unsigned seed = (unsigned)(size_t)a;
    for (int i = 0; i < 20000; i++) {
        long page = a->base + (rand_r(&seed) % 16);
        pthread_mutex_lock(&a->m->lock);
        if (!a->m->pt[page].present) {
            long got = 0;
            if (anon_pages_reserve(4)) {
                pt_map_cluster(a->m, page, 3, NULL, NULL, &got);
                if (got < 4) anon_pages_unreserve(4 - got);
            }
        } else {
            pt_unmap(a->m, page, 1);
        }
        pthread_mutex_unlock(&a->m->lock);
    }
    return NULL;
}

// ------------------------------------------------------------------ harness

static int checks, failures;
static void check(const char *name, bool ok) {
    checks++;
    if (!ok) { failures++; printf("  \033[31mFAIL\033[0m %s\n", name); }
    else printf("  \033[32mok\033[0m   %s\n", name);
}
static void reset(struct mem *m) {
    mem_init(m);
    host_mmap_calls = host_munmap_calls = host_bytes_live = 0;
    anon_page_count = 0; anon_page_limit = 1L << 30;
    real_page_size = 16384;
}

int main(void) {
    struct mem m;

    printf("\n[1] basic: 4 aligned pages -> ONE host mmap; munmap only on last release\n");
    {
        reset(&m);
        long got = 0;
        pt_map_cluster(&m, 100, 3, NULL, NULL, &got);
        check("cluster committed 4 guest pages", got == 4);
        check("exactly 1 host mmap", host_mmap_calls == 1);
        // Pages 100..103 all resolve without further host calls.
        for (long p = 100; p < 104; p++)
            check("page present without extra mmap", m.pt[p].present && host_mmap_calls == 1);
        pt_unmap(&m, 100, 1); check("release 1/4: no munmap", host_munmap_calls == 0);
        pt_unmap(&m, 101, 1); check("release 2/4: no munmap", host_munmap_calls == 0);
        pt_unmap(&m, 102, 1); check("release 3/4: no munmap", host_munmap_calls == 0);
        pt_unmap(&m, 103, 1); check("release 4/4: exactly 1 munmap", host_munmap_calls == 1);
        check("no host bytes leaked", host_bytes_live == 0);
    }

    printf("\n[2] partial release keeps the cluster alive\n");
    {
        reset(&m);
        pt_map_cluster(&m, 200, 3, NULL, NULL, NULL);
        pt_unmap(&m, 201, 2); // release the middle two
        check("2 of 4 released: still no munmap", host_munmap_calls == 0);
        check("survivors still mapped", m.pt[200].present && m.pt[203].present);
        pt_unmap(&m, 200, 1);
        check("3 of 4 released: still no munmap", host_munmap_calls == 0);
        pt_unmap(&m, 203, 1);
        check("all released: munmap fires once", host_munmap_calls == 1);
        check("no host bytes leaked", host_bytes_live == 0);
    }

    printf("\n[3] degraded: neighbour already mapped -> single page, no remap\n");
    {
        reset(&m);
        pt_map(&m, 301, 1, 3);               // pre-existing page inside the cluster
        long before = host_mmap_calls, got = 0;
        pt_map_cluster(&m, 300, 3, NULL, NULL, &got);
        check("degraded to a single page", got == 1);
        check("one extra host mmap only", host_mmap_calls == before + 1);
        check("pre-existing neighbour untouched", m.pt[301].present);
        check("uninvolved pages stay unmapped", !m.pt[302].present && !m.pt[303].present);
    }

    printf("\n[4] degraded: same_flags rejects a foreign reservation\n");
    {
        reset(&m);
        int res_a = 7;
        for (long p = 400; p < 403; p++) m.reservation_of[p] = 7;
        m.reservation_of[403] = 9;           // different reservation
        long got = 0;
        pt_map_cluster(&m, 400, 3, reservation_cluster_ok, &res_a, &got);
        check("mixed reservations -> single page", got == 1);
        check("foreign page not committed", !m.pt[403].present);
        // All-same reservation clusters fully.
        for (long p = 404; p < 408; p++) m.reservation_of[p] = 7;
        got = 0;
        pt_map_cluster(&m, 404, 3, reservation_cluster_ok, &res_a, &got);
        check("uniform reservation -> full cluster", got == 4);
    }

    printf("\n[5] unaligned fault normalises to its cluster base\n");
    {
        reset(&m);
        long got = 0;
        pt_map_cluster(&m, 503, 3, NULL, NULL, &got);  // last page of cluster 500..503
        check("still commits 4 pages", got == 4);
        check("normalised to base 500", m.pt[500].present && m.pt[503].present);
        check("did NOT spill into the next cluster", !m.pt[504].present);
        check("exactly 1 host mmap", host_mmap_calls == 1);
    }

    printf("\n[6] adjacent clusters are independent\n");
    {
        reset(&m);
        pt_map_cluster(&m, 600, 3, NULL, NULL, NULL);
        pt_map_cluster(&m, 604, 3, NULL, NULL, NULL);
        check("two separate host mmaps", host_mmap_calls == 2);
        pt_unmap(&m, 600, 4);
        check("first cluster freed", host_munmap_calls == 1);
        check("second cluster still mapped", m.pt[604].present && m.pt[607].present);
        pt_unmap(&m, 604, 4);
        check("second cluster freed", host_munmap_calls == 2);
        check("no host bytes leaked", host_bytes_live == 0);
    }

    printf("\n[7] cap: charge the cluster, refund the unused, refuse at the limit\n");
    {
        reset(&m);
        anon_page_limit = 6;                 // room for one cluster + 2 pages
        long got = 0;
        bool ok = anon_pages_reserve(4);
        check("charge 4 for the cluster", ok && anon_page_count == 4);
        pt_map_cluster(&m, 700, 3, NULL, NULL, &got);
        check("committed 4", got == 4);
        // Degraded commit must refund the difference.
        pt_map(&m, 705, 1, 3);               // block the next cluster
        ok = anon_pages_reserve(4);
        check("charge 4 again (total 8 > limit 6) refused", !ok && anon_page_count == 4);
        ok = anon_pages_reserve(1);
        check("single-page charge fits", ok && anon_page_count == 5);
        got = 0;
        pt_map_cluster(&m, 704, 3, NULL, NULL, &got);
        check("degraded commit is 1 page", got == 1);
        check("count reflects real pages (5)", anon_page_count == 5);
        // Cap refusal leaves nothing charged.
        long before = anon_page_count;
        ok = anon_pages_reserve(100);
        check("over-limit reserve adds nothing", !ok && anon_page_count == before);
    }

    printf("\n[8] host page == guest page: cluster degenerates to 1 (x86/Linux)\n");
    {
        reset(&m);
        real_page_size = 4096;
        long got = 0;
        pt_map_cluster(&m, 800, 3, NULL, NULL, &got);
        check("commits exactly 1 page", got == 1);
        check("neighbours untouched", !m.pt[801].present);
        real_page_size = 16384;
    }

    printf("\n[9] boundary inputs\n");
    {
        reset(&m);
        long got = 0;
        int err = pt_map_cluster(&m, MAX_PAGES - 1, 3, NULL, NULL, &got);
        check("page at the very top still commits", err == 0 && got >= 1);
        check("did not write past the table", m.pt[MAX_PAGES - 1].present);
        reset(&m);
        err = pt_map(&m, MAX_PAGES - 1, 8, 3);   // would overflow
        check("oversized map rejected", err < 0);
        reset(&m);
        err = pt_map(&m, 10, 0, 3);
        check("zero-page map rejected", err < 0);
        check("no host allocation for rejected maps", host_mmap_calls == 0);
    }

    printf("\n[10] stress: random commit/release, no leak, no double free\n");
    {
        reset(&m);
        unsigned seed = 12345;
        for (int i = 0; i < 200000; i++) {
            long page = 1000 + (rand_r(&seed) % 1000);
            if (rand_r(&seed) & 1) {
                if (!m.pt[page].present) {
                    long got = 0;
                    if (anon_pages_reserve(4)) {
                        pt_map_cluster(&m, page, 3, NULL, NULL, &got);
                        if (got < 4) anon_pages_unreserve(4 - got);
                    }
                }
            } else {
                if (m.pt[page].present) pt_unmap(&m, page, 1);
            }
        }
        for (long p = 0; p < MAX_PAGES; p++)
            if (m.pt[p].present) pt_unmap(&m, p, 1);
        check("all host allocations released", host_mmap_calls == host_munmap_calls);
        check("no host bytes leaked", host_bytes_live == 0);
        check("anon count back to zero", anon_page_count == 0);
    }

    printf("\n[11] concurrency: threads on shared and adjacent clusters\n");
    {
        reset(&m);
        // Serialised through mem->lock exactly as the kernel does; the test is
        // that the refcount/accounting invariants survive interleaving.
        pthread_t t[6];
        struct arg args[6];
        for (int i = 0; i < 6; i++) {
            args[i] = (struct arg){ &m, 2000 + (i / 2) * 16 }; // pairs share a region
            pthread_create(&t[i], NULL, cluster_worker, &args[i]);
        }
        for (int i = 0; i < 6; i++) pthread_join(t[i], NULL);
        for (long p = 0; p < MAX_PAGES; p++)
            if (m.pt[p].present) pt_unmap(&m, p, 1);
        check("no double free / leak under contention", host_mmap_calls == host_munmap_calls);
        check("no host bytes leaked", host_bytes_live == 0);
        check("anon count back to zero", anon_page_count == 0);
    }

    printf("\n[12] amplification: the actual point of the change\n");
    {
        // Touch 4096 consecutive guest pages (16MB of guest memory) one fault
        // at a time, the way the jsonnet compile did.
        reset(&m);
        for (long p = 0; p < 4096; p++)
            if (!m.pt[p].present) pt_map_cluster(&m, p, 3, NULL, NULL, NULL);
        long clustered_bytes = host_bytes_live, clustered_calls = host_mmap_calls;

        reset(&m);
        for (long p = 0; p < 4096; p++) pt_map(&m, p, 1, 3);   // old behaviour
        long naive_bytes = host_bytes_live, naive_calls = host_mmap_calls;

        printf("     naive:     %ld mmaps, %ld MB host\n", naive_calls, naive_bytes >> 20);
        printf("     clustered: %ld mmaps, %ld MB host\n", clustered_calls, clustered_bytes >> 20);
        check("host mmap calls cut 4x", clustered_calls * 4 == naive_calls);
        check("host bytes cut 4x", clustered_bytes * 4 == naive_bytes);
        check("clustered host bytes == guest bytes (no waste)",
              clustered_bytes == 4096L * PAGE_SIZE);
    }

    printf("\n%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
