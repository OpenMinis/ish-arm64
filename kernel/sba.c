#include "util/macopt.h"
#if ISH_SBA   // util/macopt.h: nothing at all on iOS, nothing calls in
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include "kernel/sba.h"
#include "fs/fd.h"
#include "kernel/memory.h"


static int sba_state = -1;
static _Atomic uint64_t st_calls, st_ok_rw, st_ok_r, st_unmapped, st_foreign, st_unreadable, st_raced;

void sba_stats(void) {
    fprintf(stderr, "📊 SBA populate: calls %llu, rw %llu, r %llu | unmapped %llu, not shadow %llu, "
            "unreadable %llu, raced %llu\n", st_calls, st_ok_rw, st_ok_r, st_unmapped, st_foreign,
            st_unreadable, st_raced);
}

// On by default (macOS only); ISH_SBA=0 turns it off.
bool sba_on(void) {
    if (sba_state < 0) {
        const char *v = getenv("ISH_SBA");
        sba_state = !(v && v[0] == '0');
    }
    return sba_state;
}

void sba_init(struct mem *mem) {
    mem->sba = NULL;
    mem->sba_dying = false;
    mem->sba_keep = false;
    mem->mmu.sba_base = NULL;
    mem->mmu.sba_map = NULL;
    if (!sba_on())
        return;
    // Address space only: the map's pages and the shadow's are committed as
    // they are touched. [map][window, 4GB][one chunk of slack, for accesses
    // the map lets start in the last page]
    size_t size = SBA_MAP_ENTRIES + SBA_SPAN + SBA_CHUNK;
    char *p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED)
        return;
    struct sba_region *r = malloc(sizeof(*r));
    if (r == NULL) {
        munmap(p, size);
        return;
    }
    r->base = p + SBA_MAP_ENTRIES;
    r->refs = 1;
    mem->sba = r;
    mem->mmu.sba_base = r->base;
    mem->mmu.sba_map = (uint8_t *) p;
}

void sba_region_put(struct sba_region *r) {
    if (r == NULL || atomic_fetch_sub(&r->refs, 1) != 1)
        return;
    munmap(r->base - SBA_MAP_ENTRIES, SBA_MAP_ENTRIES + SBA_SPAN + SBA_CHUNK);
    free(r);
}

void sba_destroy(struct mem *mem) {
    // the map goes with the region, which data objects of a forked child
    // may still hold
    mem->mmu.sba_map = NULL;
    mem->mmu.sba_base = NULL;
    sba_region_put(mem->sba);
    mem->sba = NULL;
}

// Fresh zero pages for the whole chunks of [a, b) (shadow offsets), zeroes
// written into the partial ones.
static void zero_range(char *base, uint64_t a, uint64_t b) {
    uint64_t lo = (a + SBA_CHUNK - 1) & ~(SBA_CHUNK - 1), hi = b & ~(SBA_CHUNK - 1);
    if (lo >= hi) {
        memset(base + a, 0, b - a);
        return;
    }
    memset(base + a, 0, lo - a);
    mmap(base + lo, hi - lo, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE | MAP_FIXED, -1, 0);
    memset(base + hi, 0, b - hi);
}

void *sba_anon(struct mem *mem, page_t start, pages_t pages, struct sba_region **region) {
    if (mem->sba == NULL || pages == 0)
        return NULL;
    uint64_t a = (uint64_t) start << PAGE_BITS, b = ((uint64_t) start + pages) << PAGE_BITS;
    if (b > SBA_SPAN || b < a)
        return NULL;
    zero_range(mem->sba->base, a, b);
    atomic_fetch_add(&mem->sba->refs, 1);
    *region = mem->sba;
    return mem->sba->base + a;
}

void sba_release(struct mem *mem, page_t start, pages_t pages) {
    if (mem->sba == NULL)
        return;
    uint64_t a = (uint64_t) start << PAGE_BITS, b = ((uint64_t) start + pages) << PAGE_BITS;
    if (a >= SBA_SPAN)
        return;
    if (b > SBA_SPAN || b < a)
        b = SBA_SPAN;
    const unsigned per = SBA_CHUNK >> PAGE_BITS;
    uint64_t run = 0, end = 0;   // chunks [run, end) to give back
    for (uint64_t c = a & ~(SBA_CHUNK - 1); c < b; c += SBA_CHUNK) {
        bool free_chunk = true;
        for (unsigned i = 0; i < per && free_chunk; i++)
            free_chunk = mem_pt(mem, (page_t) (c >> PAGE_BITS) + i) == NULL;
        if (free_chunk && end == c) {
            end = c + SBA_CHUNK;
            continue;
        }
        if (run < end)
            mmap(mem->sba->base + run, end - run, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANON | MAP_NORESERVE | MAP_FIXED, -1, 0);
        run = c;
        end = free_chunk ? c + SBA_CHUNK : c;
    }
    if (run < end)
        mmap(mem->sba->base + run, end - run, PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANON | MAP_NORESERVE | MAP_FIXED, -1, 0);
}

void sba_invalidate(struct mem *mem, page_t start, pages_t pages) {
    uint8_t *map = mem->mmu.sba_map;
    if (map == NULL || mem->sba_dying)
        return;
    uint64_t a = (uint64_t) start << PAGE_BITS;
    if (a >= SBA_SPAN)
        return;
    uint64_t b = ((uint64_t) start + pages) << PAGE_BITS;
    if (b > SBA_SPAN || b < a)
        b = SBA_SPAN;
    for (uint64_t p = a >> SBA_MAP_BITS; p < b >> SBA_MAP_BITS; p++)
        if (map[p])
            __atomic_store_n(&map[p], 0, __ATOMIC_RELEASE);
}

struct sba_fork {
    struct mem *src, *dst;
    // src data object -> dst data object (open addressing)
    unsigned cap, n;
    struct data **from, **to;
    // chunk runs to copy, merged as pages arrive in order
    uint64_t run_lo, run_hi;
};

struct sba_fork *sba_fork_begin(struct mem *src, struct mem *dst) {
    if (src->sba == NULL || dst->sba == NULL)
        return NULL;
    struct sba_fork *f = calloc(1, sizeof(*f));
    if (f == NULL)
        return NULL;
    f->src = src;
    f->dst = dst;
    f->cap = 1024;
    f->from = calloc(f->cap, sizeof(*f->from));
    f->to = calloc(f->cap, sizeof(*f->to));
    if (f->from == NULL || f->to == NULL) {
        free(f->from);
        free(f->to);
        free(f);
        return NULL;
    }
    return f;
}

static void fork_copy(struct sba_fork *f) {
    if (f->run_lo >= f->run_hi)
        return;
    char *s = f->src->sba->base + f->run_lo, *d = f->dst->sba->base + f->run_lo;
    size_t len = f->run_hi - f->run_lo;
    if (mach_vm_copy(mach_task_self(), (mach_vm_address_t) s, len, (mach_vm_address_t) d) != KERN_SUCCESS)
        memcpy(d, s, len);
    f->run_lo = f->run_hi = 0;
}

static bool fork_grow(struct sba_fork *f) {
    unsigned cap = f->cap * 2;
    struct data **from = calloc(cap, sizeof(*from)), **to = calloc(cap, sizeof(*to));
    if (from == NULL || to == NULL) {
        free(from);
        free(to);
        return false;
    }
    for (unsigned i = 0; i < f->cap; i++) {
        if (f->from[i] == NULL)
            continue;
        unsigned h = (unsigned) (((uintptr_t) f->from[i] >> 4) * 2654435761u) & (cap - 1);
        while (from[h] != NULL)
            h = (h + 1) & (cap - 1);
        from[h] = f->from[i];
        to[h] = f->to[i];
    }
    free(f->from);
    free(f->to);
    f->from = from;
    f->to = to;
    f->cap = cap;
    return true;
}

struct data *sba_fork_page(struct sba_fork *f, struct data *sd, page_t page) {
    if (f == NULL || sd->sba != f->src->sba)
        return NULL;
    if (2 * (f->n + 1) > f->cap && !fork_grow(f))
        return NULL;
    unsigned h = (unsigned) (((uintptr_t) sd >> 4) * 2654435761u) & (f->cap - 1);
    while (f->from[h] != NULL && f->from[h] != sd)
        h = (h + 1) & (f->cap - 1);
    struct data *dd = f->to[h];
    if (dd == NULL) {
        dd = malloc(sizeof(*dd));
        if (dd == NULL)
            return NULL;
        *dd = *sd;
        dd->data = f->dst->sba->base + ((char *) sd->data - f->src->sba->base);
        dd->refcount = 0;
        dd->sba = f->dst->sba;
        atomic_fetch_add(&dd->sba->refs, 1);
        if (dd->fd != NULL)
            dd->fd = fd_retain(dd->fd);
        f->from[h] = sd;
        f->to[h] = dd;
        f->n++;
    }
    // the page's host page, merged with the run before it
    uint64_t lo = ((uint64_t) page << PAGE_BITS) & ~(SBA_CHUNK - 1), hi = lo + SBA_CHUNK;
    if (lo <= f->run_hi && hi >= f->run_lo && f->run_lo < f->run_hi) {
        if (hi > f->run_hi)
            f->run_hi = hi;
    } else {
        fork_copy(f);
        f->run_lo = lo;
        f->run_hi = hi;
    }
    return dd;
}

void sba_fork_finish(struct sba_fork *f) {
    if (f == NULL)
        return;
    fork_copy(f);
    free(f->from);
    free(f->to);
    free(f);
}

// What native code may do at page p through the window (SBA_R | SBA_W), mem
// read-locked.
static uint8_t page_rights(struct mem *mem, page_t p) {
    struct pt_entry *e = mem_pt(mem, p);
    if (e == NULL) {
        st_unmapped++;
        return 0;
    }
    if (e->data->sba != mem->sba ||
            (char *) e->data->data + e->offset != mem->mmu.sba_base + ((uint64_t) p << PAGE_BITS)) {
        st_foreign++;
        return 0;
    }
    // Readable as the TLB path has it: any mapped page but PROT_NONE ones
    // (stack pages are P_WRITE | P_GROWSDOWN, without P_READ).
    unsigned flags = __atomic_load_n(&e->flags, __ATOMIC_ACQUIRE);
    if (!(flags & (P_READ | P_WRITE | P_EXEC))) {
        st_unreadable++;
        return 0;
    }
    if ((flags & P_WRITE) && !(flags & (P_COW | P_CODE)))
        return SBA_R | SBA_W;
    return SBA_R;
}

// TLB miss on addr: let native code at the page if it is ours.
void sba_populate(struct mmu *mmu, addr_t addr) {
    uint8_t *map = mmu->sba_map;
    if (map == NULL || addr >= SBA_SPAN)
        return;
    uint64_t idx = addr >> SBA_MAP_BITS;
    if ((map[idx] & (SBA_R | SBA_W)) == (SBA_R | SBA_W))
        return;
    struct mem *mem = container_of(mmu, struct mem, mmu);
    st_calls++;
    read_wrlock(&mem->lock);
    uint64_t gen = __atomic_load_n(&mmu->changes, __ATOMIC_ACQUIRE);
    uint8_t v = page_rights(mem, (page_t) idx);
    if (v) {
        __atomic_store_n(&map[idx], v, __ATOMIC_RELEASE);
        // A change while we looked (code-page marking runs under the read
        // lock) may have made this stale.
        if (__atomic_load_n(&mmu->changes, __ATOMIC_ACQUIRE) != gen) {
            __atomic_store_n(&map[idx], 0, __ATOMIC_RELEASE);
            st_raced++;
        } else if (v & SBA_W) {
            st_ok_rw++;
        } else {
            st_ok_r++;
        }
    }
    read_wrunlock(&mem->lock);
}

#endif
