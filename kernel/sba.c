#include "util/macopt.h"
#if ISH_SBA   // util/macopt.h: nothing at all on iOS, nothing calls in
#include <pthread.h>
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


static int sba_state = -1, fastmem_state = -1;
static _Atomic uint64_t st_calls, st_ok_rw, st_ok_r, st_unmapped, st_foreign, st_unreadable, st_raced, st_faults;

void sba_stats(void) {
    fprintf(stderr, "📊 SBA populate: calls %llu, rw %llu, r %llu | unmapped %llu, not shadow %llu, "
            "unreadable %llu, raced %llu | fastmem opens %llu\n", st_calls, st_ok_rw, st_ok_r, st_unmapped, st_foreign,
            st_unreadable, st_raced, st_faults);
}

// On by default (macOS only); ISH_SBA=0 turns it off.
bool sba_on(void) {
    if (sba_state < 0) {
        const char *v = getenv("ISH_SBA");
        sba_state = !(v && v[0] == '0');
    }
    return sba_state;
}

static bool fastmem_on(void) {
    if (fastmem_state < 0) {
        const char *v = getenv("ISH_FASTMEM");
        fastmem_state = !(v && v[0] == '0');
    }
    return fastmem_state;
}

// Show the alias the window's host pages of [a, b) (shadow offsets, rounded
// out to host pages) again, closed: what it showed may have been replaced.
static void resync(struct sba_region *r, uint64_t a, uint64_t b) {
    if (r->alias == NULL || b <= a)
        return;
    a &= ~(SBA_CHUNK - 1);
    b = (b + SBA_CHUNK - 1) & ~(SBA_CHUNK - 1);
    mach_vm_address_t t = (mach_vm_address_t) (r->alias + a);
    vm_prot_t cur, max;
    if (mach_vm_remap(mach_task_self(), &t, b - a, 0, VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE, mach_task_self(),
                      (mach_vm_address_t) (r->base + a), FALSE, &cur, &max, VM_INHERIT_NONE) != KERN_SUCCESS)
        fprintf(stderr, "SBA: alias remap failed at %#llx\n", (unsigned long long) a);
    // closed either way: an alias page that is not the window's only ever faults
    mach_vm_protect(mach_task_self(), (mach_vm_address_t) (r->alias + a), b - a, FALSE, VM_PROT_NONE);
}

// The alias's host pages of [a, b) let go of what they showed (released
// shadow memory stays resident while anything maps it); whatever maps them
// again resyncs them.
static void unsync(struct sba_region *r, uint64_t a, uint64_t b) {
    if (r->alias == NULL || b <= a)
        return;
    mmap(r->alias + a, b - a, PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE | MAP_FIXED, -1, 0);
}

void sba_resync_host(struct mem *mem, void *host, size_t len) {
    if (mem->sba == NULL)
        return;
    uint64_t a = (uint64_t) ((char *) host - mem->sba->base);
    resync(mem->sba, a, a + len);
}

// fastmem without an alias (no window, or the remap failed): one that only
// faults, so native code takes the TLB path for everything.
static char *dead_alias(void) {
    static char *_Atomic dead;
    char *d = atomic_load(&dead);
    if (d == NULL) {
        void *p = mmap(NULL, SBA_SPAN + SBA_CHUNK, PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
        if (p == MAP_FAILED)
            abort();   // native code would reach host memory below 32GB
        if (atomic_compare_exchange_strong(&dead, &d, (char *) p))
            d = p;
        else
            munmap(p, SBA_SPAN + SBA_CHUNK);
    }
    return d;
}

void sba_init(struct mem *mem) {
    mem->sba = NULL;
    mem->sba_dying = false;
    mem->sba_keep = false;
    mem->mmu.sba_base = NULL;
    mem->mmu.sba_map = NULL;
    mem->mmu.sba_xbase = fastmem_on() && sba_on() ? dead_alias() : NULL;
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
    r->alias = NULL;
    r->refs = 1;
    if (fastmem_on()) {
        // Closed address space to begin with: whatever puts memory in the
        // window shows it to the alias (resync()), closed, and pt_map()
        // opens it (sba_open()).
        void *a = mmap(NULL, SBA_SPAN + SBA_CHUNK, PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
        if (a != MAP_FAILED)
            r->alias = a;
    }
    mem->sba = r;
    mem->mmu.sba_base = r->base;
    mem->mmu.sba_map = (uint8_t *) p;
    if (r->alias)
        mem->mmu.sba_xbase = r->alias;
}

static void region_free(struct sba_region *r) {
    munmap(r->base - SBA_MAP_ENTRIES, SBA_MAP_ENTRIES + SBA_SPAN + SBA_CHUNK);
    if (r->alias)
        munmap(r->alias, SBA_SPAN + SBA_CHUNK);
    free(r);
}

// Unmapping a region's 64GB of address space takes long enough (more so
// with the alias, protected a host page at a time) to show in every exit
// and exec: a thread of its own does it.
static pthread_mutex_t reap_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t reap_cond = PTHREAD_COND_INITIALIZER;
static struct sba_region *reap_list[64];
static unsigned reap_n;
static bool reaper_started;

static void *reaper(void *arg) {
    (void) arg;
    pthread_mutex_lock(&reap_lock);
    for (;;) {
        while (reap_n == 0)
            pthread_cond_wait(&reap_cond, &reap_lock);
        struct sba_region *r = reap_list[--reap_n];
        pthread_mutex_unlock(&reap_lock);
        region_free(r);
        pthread_mutex_lock(&reap_lock);
    }
    return NULL;
}

void sba_region_put(struct sba_region *r) {
    if (r == NULL || atomic_fetch_sub(&r->refs, 1) != 1)
        return;
    pthread_mutex_lock(&reap_lock);
    if (!reaper_started) {
        pthread_t t;
        reaper_started = pthread_create(&t, NULL, reaper, NULL) == 0;
        if (reaper_started)
            pthread_detach(t);
    }
    if (reaper_started && reap_n < sizeof(reap_list) / sizeof(reap_list[0])) {
        reap_list[reap_n++] = r;
        pthread_cond_signal(&reap_cond);
        r = NULL;
    }
    pthread_mutex_unlock(&reap_lock);
    if (r != NULL)   // the reaper is behind: do it here
        region_free(r);
}

void sba_destroy(struct mem *mem) {
    // the map goes with the region, which data objects of a forked child
    // may still hold
    mem->mmu.sba_map = NULL;
    mem->mmu.sba_base = NULL;
    mem->mmu.sba_xbase = fastmem_on() ? dead_alias() : NULL;
    sba_region_put(mem->sba);
    mem->sba = NULL;
}

// Fresh zero pages for the whole chunks of [a, b) (shadow offsets), zeroes
// written into the partial ones.
static void zero_range(struct sba_region *r, uint64_t a, uint64_t b) {
    char *base = r->base;
    uint64_t lo = (a + SBA_CHUNK - 1) & ~(SBA_CHUNK - 1), hi = b & ~(SBA_CHUNK - 1);
    if (lo >= hi) {
        memset(base + a, 0, b - a);
    } else {
        memset(base + a, 0, lo - a);
        mmap(base + lo, hi - lo, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE | MAP_FIXED, -1, 0);
        memset(base + hi, 0, b - hi);
    }
    // the partial host pages too: released before, the alias let go of them
    resync(r, a, b);
}

void *sba_anon(struct mem *mem, page_t start, pages_t pages, struct sba_region **region) {
    if (mem->sba == NULL || pages == 0)
        return NULL;
    uint64_t a = (uint64_t) start << PAGE_BITS, b = ((uint64_t) start + pages) << PAGE_BITS;
    if (b > SBA_SPAN || b < a)
        return NULL;
    zero_range(mem->sba, a, b);
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
        if (run < end) {
            mmap(mem->sba->base + run, end - run, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANON | MAP_NORESERVE | MAP_FIXED, -1, 0);
            unsync(mem->sba, run, end);
        }
        run = c;
        end = free_chunk ? c + SBA_CHUNK : c;
    }
    if (run < end) {
        mmap(mem->sba->base + run, end - run, PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANON | MAP_NORESERVE | MAP_FIXED, -1, 0);
        unsync(mem->sba, run, end);
    }
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
    // fastmem: close the alias's host pages that hold any of them
    if (mem->sba->alias) {
        a &= ~(SBA_CHUNK - 1);
        b = (b + SBA_CHUNK - 1) & ~(SBA_CHUNK - 1);
        mach_vm_protect(mach_task_self(), (mach_vm_address_t) (mem->sba->alias + a), b - a, FALSE, VM_PROT_NONE);
    }
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
    // A copy-on-write mapping of the parent's pages. mach_vm_copy() copies
    // them outright when the alias (fastmem) shares them.
    mach_vm_address_t t = (mach_vm_address_t) d;
    vm_prot_t cur, max;
    if (mach_vm_remap(mach_task_self(), &t, len, 0, VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE, mach_task_self(),
                      (mach_vm_address_t) s, TRUE, &cur, &max, VM_INHERIT_NONE) != KERN_SUCCESS &&
            mach_vm_copy(mach_task_self(), (mach_vm_address_t) s, len, (mach_vm_address_t) d) != KERN_SUCCESS)
        memcpy(d, s, len);
    resync(f->dst->sba, f->run_lo, f->run_hi);
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

// fastmem: what the alias page of the host page at shadow offset c may
// allow: the rights all its mapped guest pages have. A page with nothing
// mapped does not count -- the end of a mapping is rarely a host page's, so
// counting it would close the last host page of most mappings, a thread's
// stack top and its TLS among them -- unless something maps it on a fault:
// a reservation (committed on first touch) or the page below a stack (the
// stack grows a whole host page at a time, see mem_ptr()). An access there
// then reads the window's zeroes instead of faulting, as it would on a host
// with 16KB pages; whatever maps the page later zeroes it first.
static uint8_t chunk_rights(struct mem *mem, uint64_t c) {
    uint8_t v = SBA_R | SBA_W;
    bool any = false, hole = false;
    page_t first = (page_t) (c >> PAGE_BITS), end = first + (page_t) (SBA_CHUNK >> PAGE_BITS);
    for (page_t p = first; p < end && v; p++) {
        struct pt_entry *e = mem_pt(mem, p);
        if (e == NULL) {
            hole = true;
            if (mem_find_reservation(mem, p) != NULL)
                v = 0;
            continue;
        }
        // above a hole, a stack would grow into it
        if (hole && (e->flags & P_GROWSDOWN))
            v = 0;
        v &= page_rights(mem, p);
        any = true;
    }
    if (hole && v) {
        struct pt_entry *e = mem_pt(mem, end);
        if (e != NULL && (e->flags & P_GROWSDOWN) && mem_pt(mem, end - 1) == NULL)
            v = 0;
    }
    return any ? v : 0;
}

void sba_open(struct mem *mem, page_t start, pages_t pages) {
    struct sba_region *r = mem->sba;
    if (r == NULL || r->alias == NULL || mem->sba_dying)
        return;
    uint64_t a = (uint64_t) start << PAGE_BITS, b = ((uint64_t) start + pages) << PAGE_BITS;
    if (a >= SBA_SPAN)
        return;
    if (b > SBA_SPAN || b < a)
        b = SBA_SPAN;
    a &= ~(SBA_CHUNK - 1);
    b = (b + SBA_CHUNK - 1) & ~(SBA_CHUNK - 1);
    // runs of host pages with the same rights, one protect each
    uint64_t run = a;
    uint8_t rv = chunk_rights(mem, a);
    for (uint64_t c = a + SBA_CHUNK; c <= b; c += SBA_CHUNK) {
        uint8_t v = c < b ? chunk_rights(mem, c) : 0xff;
        if (v == rv)
            continue;
        if (rv)
            mach_vm_protect(mach_task_self(), (mach_vm_address_t) (r->alias + run), c - run, FALSE,
                            rv & SBA_W ? VM_PROT_READ | VM_PROT_WRITE : VM_PROT_READ);
        run = c;
        rv = v;
    }
}

#define FASTMEM_RUN 16   // host pages opened at most on each side of a fault

int sba_fault(struct mem *mem, uint64_t addr, bool write) {
    struct sba_region *r = mem->sba;
    if (r == NULL || r->alias == NULL || addr >= SBA_SPAN + SBA_CHUNK)
        return 0;
    uint8_t need = write ? SBA_R | SBA_W : SBA_R;
    uint64_t c = addr & ~(SBA_CHUNK - 1);
    if (c >= SBA_SPAN)
        return 0;
    uint8_t v = chunk_rights(mem, c);
    if ((v & need) != need)
        return 0;   // not allowed, or a host page shared with less accessible guest pages
    // open the run of host pages around it with the same rights in one go
    uint64_t lo = c, hi = c + SBA_CHUNK;
    for (int i = 0; i < FASTMEM_RUN && lo > 0 && chunk_rights(mem, lo - SBA_CHUNK) == v; i++)
        lo -= SBA_CHUNK;
    for (int i = 0; i < FASTMEM_RUN && hi < SBA_SPAN && chunk_rights(mem, hi) == v; i++)
        hi += SBA_CHUNK;
    vm_prot_t prot = v & SBA_W ? VM_PROT_READ | VM_PROT_WRITE : VM_PROT_READ;
    if (mach_vm_protect(mach_task_self(), (mach_vm_address_t) (r->alias + lo), hi - lo, FALSE, prot) != KERN_SUCCESS)
        return 0;
    // Marking a page as code runs under the read lock too: it sets P_CODE,
    // then closes the alias. Seen now, close again; not seen yet, its close
    // comes after ours.
    for (uint64_t x = lo; x < hi; x += SBA_CHUNK) {
        if (chunk_rights(mem, x) != v) {
            mach_vm_protect(mach_task_self(), (mach_vm_address_t) (r->alias + lo), hi - lo, FALSE, VM_PROT_NONE);
            return 0;
        }
    }
    st_faults++;
    return 1;
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
