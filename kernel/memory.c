#include <fcntl.h>
#include <unistd.h>
#include <sched.h>
#include <sys/mman.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <stdatomic.h>
#include <limits.h>

#define DEFAULT_CHANNEL memory
#include "debug.h"
#include "kernel/errno.h"
#include "kernel/signal.h"
#include "asbestos/frame.h"
#include "kernel/memory.h"
#include "util/verbosetrace.h"
#include "asbestos/asbestos.h"
#include "kernel/vdso.h"
#include "kernel/task.h"
#include "kernel/resource.h"
#include "fs/fd.h"

// increment the change count
static void mem_changed(struct mem *mem);
static struct mmu_ops mem_mmu_ops;

#include "kernel/mm.h"
#include "emu/tlb.h"

#ifdef FASTMEM
// Where the window's time goes (printed with ISH_XJIT_STATS=1)
#include <time.h>
enum { FS_INIT, FS_DESTROY, FS_MAP, FS_UNMAP, FS_FILL, FS_FORK, FS_MIGRATE, FS_N };
static _Atomic uint64_t fs_ns[FS_N], fs_n[FS_N];
static int fs_stats = -1;
static uint64_t fs_now(void) {
    if (fs_stats < 0)
        fs_stats = getenv("ISH_XJIT_STATS") != NULL;
    if (!fs_stats)
        return 0;
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t) t.tv_sec * 1000000000ull + (uint64_t) t.tv_nsec;
}
#define FS_BEGIN uint64_t fs_t0 = fs_now()
#define FS_END(k) (fs_t0 != 0 ? (atomic_fetch_add(&fs_ns[k], fs_now() - fs_t0), atomic_fetch_add(&fs_n[k], 1)) : 0)
static _Atomic uint64_t fm_pages_kept, fm_pages_committed, fm_pages_file, fm_pages_recycled;
void fm_stats_print(FILE *out) {
    fprintf(out, "[fm] pages mapped: %llu reused, %llu committed, %llu of files; %llu kept at munmap\n",
            (unsigned long long) fm_pages_kept, (unsigned long long) fm_pages_committed,
            (unsigned long long) fm_pages_file, (unsigned long long) fm_pages_recycled);
    static const char *names[] = {"init", "destroy", "map", "unmap", "fill", "fork", "migrate"};
    for (int k = 0; k < FS_N; k++)
        fprintf(out, "[fm] %-8s n=%6llu %8.2f ms\n", names[k], (unsigned long long) fs_n[k], fs_ns[k] / 1e6);
}
// The window's guards: a group of accesses checked once, or an access with a
// displacement, a little outside it faults too.
#define FM_GUARD ((size_t) 1 << 20)

// ISH_FASTMEM: 0 none; 1 once a process has run a while (mem_window_migrate()),
// so that the many that don't live long pay nothing; 2 from its start.
static int fm_mode(void) {
    static int mode = -1;
    if (mode < 0) {
        const char *e = getenv("ISH_FASTMEM");
        mode = e != NULL && e[0] >= '0' && e[0] <= '2' ? e[0] - '0' : 0;
#if defined(_WIN32) && defined(GUEST_X86_64)
        // the hypervisor VMs run address spaces that have a window from the
        // start
        extern bool vm_wanted(void);   // asbestos/guest-x86_64/vm.c
        if (e == NULL && vm_wanted())
            mode = 2;
#endif
    }
    return mode;
}

static bool fm_fits(struct mem *mem, page_t start, pages_t pages) {
    return mem->mmu.fm_base != NULL && pages > 0 && start < FM_PAGES && pages <= FM_PAGES - start;
}

// What the window lets through: the checked path sees to the rest (a page
// with compiled code takes its stores there, see P_CODE).
static int fm_prot(unsigned flags) {
    if (!(flags & (P_READ | P_WRITE | P_EXEC)))
        return 0;
    if (P_WRITABLE(flags) && !(flags & P_CODE))
        return PROT_READ | PROT_WRITE;
    return PROT_READ;
}

static void fm_protect_entry(struct pt_entry *entry) {
    // a page not read in yet stays out: reading it in protects it
    if (entry->data->window && !(entry->flags & P_LAZY))
        px_fm_protect((char *) entry->data->data + entry->offset, PAGE_SIZE, fm_prot(entry->flags));
}

// The window protection of n pages from page, whose memory is host on: runs
// of the same protection at once (a system call or two per run, not per
// page). Pages not read in yet are left out.
static void fm_protect_pages(struct mem *mem, char *host, page_t page, pages_t n) {
    pages_t i = 0;
    while (i < n) {
        struct pt_entry *e = mem_pt(mem, page + i);
        if (e == NULL || !e->data->window || (e->flags & P_LAZY)) {
            i++;
            continue;
        }
        int prot = fm_prot(e->flags);
        pages_t j = i + 1;
        while (j < n) {
            struct pt_entry *f = mem_pt(mem, page + j);
            if (f == NULL || !f->data->window || (f->flags & P_LAZY) || fm_prot(f->flags) != prot)
                break;
            j++;
        }
        px_fm_protect(host + ((size_t) i << PAGE_BITS), (size_t) (j - i) << PAGE_BITS, prot);
        i = j;
    }
}

// Reads in the P_LAZY page and those around it of the same mapping, up to a
// chunk. With the write lock: no thread runs guest code meanwhile, so none
// sees a page half read in through the window (in a hypervisor VM, they are
// held: vm_hold). A hypervisor VM leaves the VM
// for each page it misses, which costs more than reading a lot more.
#define FM_CHUNK 16
#define FM_CHUNK_VM 256
static void fm_fill_run(struct mem *mem, page_t page, pages_t most) {
    struct pt_entry *first = mem_pt(mem, page);
    if (first == NULL || !(first->flags & P_LAZY))
        return;
    FS_BEGIN;
    struct data *data = first->data;
    // from up to half a chunk before it
    for (pages_t back = 0; back < most / 2 && page > 0; back++) {
        struct pt_entry *e = mem_pt(mem, page - 1);
        if (e == NULL || e->data != data || !(e->flags & P_LAZY) ||
                e->offset + PAGE_SIZE != first->offset)
            break;
        page--;
        first = e;
    }
    pages_t n = 1;
    while (n < most && page + n < FM_PAGES) {
        struct pt_entry *e = mem_pt(mem, page + n);
        if (e == NULL || e->data != data || !(e->flags & P_LAZY) ||
                e->offset != first->offset + ((size_t) n << PAGE_BITS))
            break;
        n++;
    }
    char *host = (char *) data->data + first->offset;
    size_t len = (size_t) n << PAGE_BITS;
#if defined(_WIN32) && defined(GUEST_X86_64)
    extern void vm_hold(struct mmu *mmu, bool hold);   // asbestos/guest-x86_64/vm.c
    vm_hold(&mem->mmu, true);
#endif
    if (px_fm_commit(host, len, PROT_READ | PROT_WRITE) < 0) {
#if defined(_WIN32) && defined(GUEST_X86_64)
        vm_hold(&mem->mmu, false);
#endif
        return;
    }
    int64_t off = data->lazy_off + (int64_t) first->offset;
    for (size_t done = 0; done < len;) {
        ssize_t got = pread(data->lazy_fd->real_fd, host + done, len - done, off + (int64_t) done);
        if (got <= 0)
            break;   // past the end: zeros
        done += (size_t) got;
    }
    // what a fork copies is what the guest wrote, not this
    px_fm_unwritten(host, len);
    for (pages_t i = 0; i < n; i++)
        mem_pt(mem, page + i)->flags &= ~P_LAZY;
    fm_protect_pages(mem, host, page, n);
#if defined(_WIN32) && defined(GUEST_X86_64)
    vm_hold(&mem->mmu, false);
#endif
    FS_END(FS_FILL);
}

static void fm_fill(struct mem *mem, page_t page) {
    fm_fill_run(mem, page, mem->mmu.vm != NULL ? FM_CHUNK_VM : FM_CHUNK);
}

// The window's copies of pages kept elsewhere (that a hypervisor VM reads
// there) go: translated code would read them instead of the pages.
void mem_window_drop_copies(struct mem *mem) {
    if (mem->mmu.fm_base == NULL)
        return;
    for (page_t page = 0; page < FM_PAGES; mem_next_page(mem, &page)) {
        struct pt_entry *pt = mem_pt(mem, page);
        if (pt != NULL && !pt->data->window)
            px_fm_decommit(mem->mmu.fm_base + ((size_t) page << PAGE_BITS), PAGE_SIZE);
    }
}

// In a hypervisor VM a window page costs a call to the driver to fill each
// time it is committed (px_vm_populate), and unmapping and mapping again is
// what malloc does all the time: anonymous pages unmapped are kept, filled,
// up to FM_RECYCLE_MAX, and the next mapping of just those pages gets them
// zeroed. (Touching them meanwhile doesn't fault, as it should.)
#define FM_RECYCLE_MAX 16384   // pages: 64MB
#define FM_RECYCLE_RUN 256     // pages: longer runs are zeroed by the system
static bool fm_recycle_put(struct mem *mem, page_t page) {
    if (mem->mmu.fm_recycled_pages >= FM_RECYCLE_MAX)
        return false;
    if (mem->mmu.fm_recycled == NULL && (mem->mmu.fm_recycled = calloc(FM_PAGES / 64, 8)) == NULL)
        return false;
    uint64_t bit = 1ull << (page % 64);
    if (!(mem->mmu.fm_recycled[page / 64] & bit)) {
        mem->mmu.fm_recycled[page / 64] |= bit;
        mem->mmu.fm_recycled_pages++;
        fm_pages_recycled++;
    }
    return true;
}

// The pages [start, start + pages) aren't kept any more; true if they all
// were (ready to use, but for zeroing)
static bool fm_recycled(struct mem *mem, page_t page) {
    return mem->mmu.fm_recycled != NULL && (mem->mmu.fm_recycled[page / 64] >> (page % 64)) & 1;
}

static bool fm_recycle_take(struct mem *mem, page_t start, pages_t pages) {
    if (mem->mmu.fm_recycled == NULL || mem->mmu.fm_recycled_pages == 0)
        return false;
    bool all = true;
    for (page_t page = start; page < start + pages; page++) {
        uint64_t bit = 1ull << (page % 64);
        if (mem->mmu.fm_recycled[page / 64] & bit) {
            mem->mmu.fm_recycled[page / 64] &= ~bit;
            mem->mmu.fm_recycled_pages--;
        } else {
            all = false;
        }
    }
    return all;
}

// mem_destroy: unmapping everything before the window goes back emptied
static __thread bool fm_destroying;

// Hypervisor VMs (asbestos/guest-x86_64/vm.c), where a page missed costs a
// VM exit: files mapped are read in at once, up to FM_EAGER
#define FM_EAGER 512
static bool fm_eager(void) {
#if defined(_WIN32) && defined(GUEST_X86_64)
    extern bool vm_wanted(void);   // asbestos/guest-x86_64/vm.c
    return vm_wanted();
#else
    return false;
#endif
}
#endif


#ifdef GUEST_64BIT
// ============================================================
// ARM64: 4-level page table for 48-bit address space
// ============================================================

static void mem_poke_readers(void *arg) {
    struct mmu *mmu = &((struct mem *) arg)->mmu;
    lock(&mmu->tlbs_lock);
    struct tlb *tlb;
    list_for_each_entry(&mmu->tlbs, tlb, tlbs_link) {
        if (tlb->frame != NULL && tlb->frame->cpu.poked_ptr != NULL)
            __atomic_store_n(tlb->frame->cpu.poked_ptr, true, __ATOMIC_SEQ_CST);
    }
    unlock(&mmu->tlbs_lock);
    // (threads in a hypervisor VM don't hold the lock while the guest runs)
}

void mem_init(struct mem *mem) {
    mem->pgdir = calloc(1, sizeof(struct pt_node));
    mem->pgdir_used = 0;
    mem->mmap_hint = 0;
    mem->reservations = NULL;
    mem->mmu.ops = &mem_mmu_ops;
    mem->mmu.asbestos = asbestos_new(&mem->mmu);
    mem->mmu.changes = 0;
#ifdef GUEST_X86_64
    mem->mmu.fm_base = NULL;
    mem->mmu.fm_ticks = 0;
    mem->mmu.fm_want = false;
    mem->mmu.vm = NULL;
    mem->mmu.fm_recycled = NULL;
    mem->mmu.fm_recycled_pages = 0;
#endif
#ifdef FASTMEM
    if (fm_mode() == 2) {
        FS_BEGIN;
        mem->mmu.fm_base = px_fm_reserve((size_t) FM_PAGES << PAGE_BITS, FM_GUARD);
        FS_END(FS_INIT);
    }
#endif
    lock_init(&mem->mmu.tlbs_lock);
    list_init(&mem->mmu.tlbs);
    wrlock_init(&mem->lock);
    lock_init(&mem->cow_lock);    // The threads running guest code hold mem->lock as readers, for as long
    // as they run without a syscall or a tick (a loop of chained blocks: as
    // long as it loops). A writer (mmap, munmap, mprotect) asks them to
    // stop and let go, and they wait for it before they take it again
    // (task_run_current): else it waits until it happens to find no reader,
    // which with a dozen threads can be never.
    mem->lock.writer_waits = mem_poke_readers;
    mem->lock.writer_waits_arg = mem;
}

int pt_map_lazy(struct mem *mem, page_t start, pages_t pages, unsigned flags) {
    struct mem_reservation *res = malloc(sizeof(struct mem_reservation));
    if (res == NULL)
        return _ENOMEM;
    res->start = start;
    res->pages = pages;
    res->flags = flags | P_ANONYMOUS;
    res->next = mem->reservations;
    mem->reservations = res;
    mem_changed(mem);
    return 0;
}

// Recursively free page table nodes at given level
static void pt_node_free(void *node, int level) {
    if (node == NULL)
        return;
    if (level == 3) {
        // L3: array of pt_entry — just free the array
        free(node);
        return;
    }
    struct pt_node *n = node;
    for (int i = 0; i < PT_ENTRIES; i++) {
        pt_node_free(n->children[i], level + 1);
    }
    free(n);
}

void mem_destroy(struct mem *mem) {
    // [T-ish-mm-diag] Entry state of the lock we are about to take, unlock
    // and destroy; val != 0 here means someone still holds it (see §10).
    if (ish_verbose_trace_enabled)
    printk("[iSH][MEM-DESTROY] pid=%d mem=%p seq=%llu rwlock_val=%d holder_pid=%d\n",
           current ? current->pid : -1, (void *) mem,
           (unsigned long long) MM_OF_MEM(mem)->seq, mem->lock.val, mem->lock.pid);
    write_wrlock(&mem->lock);
    pt_unmap_always(mem, 0, MEM_PAGES);
    while (mem->reservations) {
        struct mem_reservation *r = mem->reservations;
        mem->reservations = r->next;
        free(r);
    }
    // [T-ish-mm-double-destroy-crash] Null out after free so a racing
    // second cleanup path can't dereference a stale pointer.
    asbestos_free(mem->mmu.asbestos);
    mem->mmu.asbestos = NULL;
#ifdef FASTMEM
    FS_BEGIN;
    free(mem->mmu.fm_recycled);
    mem->mmu.fm_recycled = NULL;
    if (mem->mmu.fm_base != NULL)
        px_fm_release(mem->mmu.fm_base, (size_t) FM_PAGES << PAGE_BITS, FM_GUARD);
    FS_END(FS_DESTROY);
    mem->mmu.fm_base = NULL;
#endif
    pt_node_free(mem->pgdir, 0);
    mem->pgdir = NULL;
    write_wrunlock(&mem->lock);
    wrlock_destroy(&mem->lock);
}

// Navigate 4-level page table to find L3 entry, creating intermediate nodes as needed
static struct pt_entry *mem_pt_new(struct mem *mem, page_t page) {
    struct pt_node *l0 = mem->pgdir;
    int i0 = PT_INDEX(page, 0);
    struct pt_node *l1 = l0->children[i0];
    if (l1 == NULL) {
        l1 = l0->children[i0] = calloc(1, sizeof(struct pt_node));
        mem->pgdir_used++;
    }

    int i1 = PT_INDEX(page, 1);
    struct pt_node *l2 = l1->children[i1];
    if (l2 == NULL)
        l2 = l1->children[i1] = calloc(1, sizeof(struct pt_node));

    int i2 = PT_INDEX(page, 2);
    struct pt_entry *l3 = l2->children[i2];
    if (l3 == NULL)
        l3 = l2->children[i2] = calloc(PT_ENTRIES, sizeof(struct pt_entry));

    int i3 = PT_INDEX(page, 3);
    return &l3[i3];
}

struct pt_entry *mem_pt(struct mem *mem, page_t page) {
    struct pt_node *l0 = mem->pgdir;
    if (l0 == NULL) return NULL;

    struct pt_node *l1 = l0->children[PT_INDEX(page, 0)];
    if (l1 == NULL) return NULL;

    struct pt_node *l2 = l1->children[PT_INDEX(page, 1)];
    if (l2 == NULL) return NULL;

    struct pt_entry *l3 = l2->children[PT_INDEX(page, 2)];
    if (l3 == NULL) return NULL;

    struct pt_entry *entry = &l3[PT_INDEX(page, 3)];
    if (entry->data == NULL) return NULL;
    return entry;
}

static void mem_pt_del(struct mem *mem, page_t page) {
    struct pt_entry *entry = mem_pt(mem, page);
    if (entry != NULL)
        entry->data = NULL;
}

// Skip over large unallocated regions efficiently by checking intermediate levels
void mem_next_page(struct mem *mem, page_t *page) {
    (*page)++;
    if (*page >= MEM_PAGES)
        return;

    struct pt_node *l0 = mem->pgdir;
    if (l0 == NULL) { *page = MEM_PAGES; return; }

    while (*page < MEM_PAGES) {
        int i0 = PT_INDEX(*page, 0);
        struct pt_node *l1 = l0->children[i0];
        if (l1 == NULL) {
            // Skip entire L0 region (2^27 pages)
            *page = (((*page >> (PT_BITS * 3)) + 1) << (PT_BITS * 3));
            continue;
        }

        int i1 = PT_INDEX(*page, 1);
        struct pt_node *l2 = l1->children[i1];
        if (l2 == NULL) {
            // Skip entire L1 region (2^18 pages)
            *page = (((*page >> (PT_BITS * 2)) + 1) << (PT_BITS * 2));
            continue;
        }

        int i2 = PT_INDEX(*page, 2);
        struct pt_entry *l3 = l2->children[i2];
        if (l3 == NULL) {
            // Skip entire L2 region (2^9 = 512 pages)
            *page = (((*page >> PT_BITS) + 1) << PT_BITS);
            continue;
        }
        // Found a populated L3 array — page might exist here
        return;
    }
}

// Scan downward from 'start' to MMAP_HOLE_END, skipping unallocated page table
// subtrees for efficiency (L0 covers 2^27 pages, L1 2^18, L2 2^9).
static bool hole_overlaps_reservation(struct mem *mem, page_t start, pages_t size) {
    for (struct mem_reservation *r = mem->reservations; r; r = r->next) {
        if (r->start < start + size && r->start + r->pages > start)
            return true;
    }
    return false;
}

static page_t pt_find_hole_from(struct mem *mem, pages_t size, page_t start) {
    struct pt_node *l0 = mem->pgdir;
    page_t hole_end = 0;
    bool in_hole = false;

    page_t page = start;
    while (page > MMAP_HOLE_END) {
        // Fast-skip unallocated L0 subtrees
        int i0 = PT_INDEX(page, 0);
        struct pt_node *l1 = l0 ? l0->children[i0] : NULL;
        if (l1 == NULL) {
            page_t l0_base = (page_t)i0 << (PT_BITS * 3);
            if (!in_hole) { in_hole = true; hole_end = page + 1; }
            page_t effective_base = (l0_base > MMAP_HOLE_END) ? l0_base : MMAP_HOLE_END + 1;
            if (in_hole && hole_end - effective_base >= size)
                return hole_end - size;
            if (l0_base == 0) break;
            page = l0_base - 1;
            continue;
        }

        // Fast-skip unallocated L1 subtrees
        int i1 = PT_INDEX(page, 1);
        struct pt_node *l2 = l1->children[i1];
        if (l2 == NULL) {
            page_t l1_base = ((page_t)i0 << (PT_BITS * 3)) | ((page_t)i1 << (PT_BITS * 2));
            if (!in_hole) { in_hole = true; hole_end = page + 1; }
            page_t effective_base = (l1_base > MMAP_HOLE_END) ? l1_base : MMAP_HOLE_END + 1;
            if (in_hole && hole_end - effective_base >= size)
                return hole_end - size;
            if (l1_base == 0) break;
            page = l1_base - 1;
            continue;
        }

        // Fast-skip unallocated L2 subtrees
        int i2 = PT_INDEX(page, 2);
        struct pt_entry *l3 = l2->children[i2];
        if (l3 == NULL) {
            page_t l2_base = ((page_t)i0 << (PT_BITS * 3)) | ((page_t)i1 << (PT_BITS * 2)) | ((page_t)i2 << PT_BITS);
            if (!in_hole) { in_hole = true; hole_end = page + 1; }
            page_t effective_base = (l2_base > MMAP_HOLE_END) ? l2_base : MMAP_HOLE_END + 1;
            if (in_hole && hole_end - effective_base >= size)
                return hole_end - size;
            if (l2_base == 0) break;
            page = l2_base - 1;
            continue;
        }

        // L3 exists — check individual pages
        if (mem_pt(mem, page) != NULL) {
            in_hole = false;
        } else {
            if (!in_hole) { in_hole = true; hole_end = page + 1; }
            if (hole_end - page >= size)
                return page;
        }
        if (page == 0) break;
        page--;
    }
    return BAD_PAGE;
}

// Scan upward in the high address space (above 4GB) for large allocations
// that don't fit in the low region. Used for Wasm guard regions etc.
static page_t pt_find_hole_high(struct mem *mem, pages_t size) {
    // Search from 0x100000 (4GB) upward to USER_ADDR_MAX_PAGE
    // Use a simple strategy: scan upward looking for unallocated L0 subtrees.
    // A subtree with no page-table nodes can still be covered by a lazy
    // mem_reservation (e.g. the 512GB JSC gigacage / V8 cage), which does NOT
    // create pt_node children. So we must also treat reservation-covered
    // subtrees as occupied, otherwise we'd hand out a hole that
    // hole_overlaps_reservation() then rejects, and the mmap fails with ENOMEM
    // even though there's plenty of free VA above the reservation.
    page_t page = 0x100000; // Start at 4GB
    page_t hole_start = page;
    pages_t hole_size = 0;

    while (page < USER_ADDR_MAX_PAGE) {
        int i0 = PT_INDEX(page, 0);
        struct pt_node *l0 = mem->pgdir;
        struct pt_node *l1 = l0 ? l0->children[i0] : NULL;
        page_t l0_size = (page_t)1 << (PT_BITS * 3);
        page_t l0_base = (page_t)i0 << (PT_BITS * 3);
        page_t subtree_end = l0_base + l0_size;
        if (subtree_end > USER_ADDR_MAX_PAGE)
            subtree_end = USER_ADDR_MAX_PAGE;
        // Occupied if a page table exists OR a reservation overlaps this subtree.
        bool occupied = (l1 != NULL) ||
            hole_overlaps_reservation(mem, l0_base, l0_size);
        if (!occupied) {
            if (hole_size == 0) hole_start = l0_base < page ? page : l0_base;
            hole_size = subtree_end - hole_start;
            if (hole_size >= size)
                return hole_start;
            page = subtree_end;
            continue;
        }
        // Something is mapped/reserved here — reset the running hole and skip.
        hole_size = 0;
        page = subtree_end;
    }
    return BAD_PAGE;
}

// Find a hole for a mmap allocation.
//  * `prefer_high` asks for addresses above 4GB when possible. This is
//    used for large V8 heap-cage reservations (PROT_NONE chunks ≥32MB)
//    so their contents cannot be confused with legitimate low-address
//    guest pointers. Node V8 without pointer compression stores full
//    64-bit tagged pointers into heap slots: when iSH places the heap
//    in 0xc0000000..0xd0000000 (low 4GB), a slot containing garbage
//    24-bit value 0x00c39b1b looks indistinguishable from a real heap
//    pointer and V8's later deref lands on an unmapped page. Placing
//    the heap above 4GB makes such values obviously non-canonical so
//    V8's own null-checks catch them.
static page_t pt_find_hole_impl(struct mem *mem, pages_t size, bool prefer_high) {
    if (prefer_high) {
        page_t result = pt_find_hole_high(mem, size);
        if (result != BAD_PAGE && !hole_overlaps_reservation(mem, result, size))
            return result;
        // fall through to low search as a last resort
    }

    page_t start = mem->mmap_hint;
    if (start == 0 || start > MMAP_HOLE_START || start <= MMAP_HOLE_END + size)
        start = MMAP_HOLE_START;

    // Try low address space with reservation awareness.
    // If a candidate overlaps a reservation, retry below the reservation.
    for (int attempt = 0; attempt < 10; attempt++) {
        page_t result = pt_find_hole_from(mem, size, start);
        if (result == BAD_PAGE)
            break;
        if (!hole_overlaps_reservation(mem, result, size)) {
            mem->mmap_hint = (result > 0) ? result - 1 : 0;
            return result;
        }
        // Skip below the overlapping reservation
        for (struct mem_reservation *r = mem->reservations; r; r = r->next) {
            if (r->start < result + size && r->start + r->pages > result) {
                start = (r->start > 0) ? r->start - 1 : 0;
                break;
            }
        }
    }

    // Wrap around from top
    if (start < MMAP_HOLE_START) {
        start = MMAP_HOLE_START;
        for (int attempt = 0; attempt < 10; attempt++) {
            page_t result = pt_find_hole_from(mem, size, start);
            if (result == BAD_PAGE)
                break;
            if (!hole_overlaps_reservation(mem, result, size)) {
                mem->mmap_hint = (result > 0) ? result - 1 : 0;
                return result;
            }
            for (struct mem_reservation *r = mem->reservations; r; r = r->next) {
                if (r->start < result + size && r->start + r->pages > result) {
                    start = (r->start > 0) ? r->start - 1 : 0;
                    break;
                }
            }
        }
    }

    // Low 4GB exhausted — try high address space (above 4GB).
    if (!prefer_high) {
        page_t result = pt_find_hole_high(mem, size);
        if (result != BAD_PAGE && !hole_overlaps_reservation(mem, result, size))
            return result;
    }

    return BAD_PAGE;
}

page_t pt_find_hole(struct mem *mem, pages_t size) {
    return pt_find_hole_impl(mem, size, /*prefer_high=*/false);
}

page_t pt_find_hole_for_reservation(struct mem *mem, pages_t size) {
    return pt_find_hole_impl(mem, size, /*prefer_high=*/true);
}

#else
// ============================================================
// x86: 2-level flat page table for 32-bit address space
// ============================================================

void mem_init(struct mem *mem) {
    mem->pgdir = calloc(MEM_PGDIR_SIZE, sizeof(struct pt_entry *));
    mem->pgdir_used = 0;
    mem->mmap_hint = 0;
    mem->reservations = NULL;
    mem->mmu.ops = &mem_mmu_ops;
    mem->mmu.asbestos = asbestos_new(&mem->mmu);
    mem->mmu.changes = 0;
    wrlock_init(&mem->lock);
    lock_init(&mem->cow_lock);
}

void mem_destroy(struct mem *mem) {
    // [T-ish-mm-diag] See the ARM64 variant above.
    if (ish_verbose_trace_enabled)
    printk("[iSH][MEM-DESTROY] pid=%d mem=%p seq=%llu rwlock_val=%d holder_pid=%d\n",
           current ? current->pid : -1, (void *) mem,
           (unsigned long long) MM_OF_MEM(mem)->seq, mem->lock.val, mem->lock.pid);
    write_wrlock(&mem->lock);
#ifdef FASTMEM
    fm_destroying = true;   // px_fm_release empties the window at once
#endif
    pt_unmap_always(mem, 0, MEM_PAGES);
#ifdef FASTMEM
    fm_destroying = false;
#endif
    // [T-ish-mm-double-destroy-crash] Freed asbestos MUST also be nulled out.
    // Under CLONE_VM exit_group races (multi-threaded python3 during MCP
    // teardown), the same struct mm can be reached by a second cleanup path
    // once refcount has already hit zero — the pt_unmap_always above would
    // then dereference a freed asbestos and crash in asbestos_invalidate_range
    // with EXC_BAD_ACCESS. Nulling here + NULL-guarding the invalidate calls
    // turns the race into a safe no-op instead of a segfault.
    asbestos_free(mem->mmu.asbestos);
    mem->mmu.asbestos = NULL;
    while (mem->reservations) {
        struct mem_reservation *r = mem->reservations;
        mem->reservations = r->next;
        free(r);
    }
    for (int i = 0; i < MEM_PGDIR_SIZE; i++) {
        if (mem->pgdir[i] != NULL)
            free(mem->pgdir[i]);
    }
    free(mem->pgdir);
    write_wrunlock(&mem->lock);
    wrlock_destroy(&mem->lock);
}

#define PGDIR_TOP(page) ((page) >> 10)
#define PGDIR_BOTTOM(page) ((page) & (MEM_PGDIR_SIZE - 1))

static struct pt_entry *mem_pt_new(struct mem *mem, page_t page) {
    struct pt_entry *pgdir = mem->pgdir[PGDIR_TOP(page)];
    if (pgdir == NULL) {
        pgdir = mem->pgdir[PGDIR_TOP(page)] = calloc(MEM_PGDIR_SIZE, sizeof(struct pt_entry));
        mem->pgdir_used++;
    }
    return &pgdir[PGDIR_BOTTOM(page)];
}

struct pt_entry *mem_pt(struct mem *mem, page_t page) {
    struct pt_entry *pgdir = mem->pgdir[PGDIR_TOP(page)];
    if (pgdir == NULL)
        return NULL;
    struct pt_entry *entry = &pgdir[PGDIR_BOTTOM(page)];
    if (entry->data == NULL)
        return NULL;
    return entry;
}

static void mem_pt_del(struct mem *mem, page_t page) {
    struct pt_entry *entry = mem_pt(mem, page);
    if (entry != NULL)
        entry->data = NULL;
}

void mem_next_page(struct mem *mem, page_t *page) {
    (*page)++;
    if (*page >= MEM_PAGES)
        return;
    while (*page < MEM_PAGES && mem->pgdir[PGDIR_TOP(*page)] == NULL)
        *page = (*page - PGDIR_BOTTOM(*page)) + MEM_PGDIR_SIZE;
}

static page_t x86_find_hole_from(struct mem *mem, pages_t size, page_t start) {
    page_t hole_end = 0;
    bool in_hole = false;
    for (page_t page = start; page > 0x40000; page--) {
        if (!in_hole && mem_pt(mem, page) == NULL) {
            in_hole = true;
            hole_end = page + 1;
        }
        if (mem_pt(mem, page) != NULL)
            in_hole = false;
        else if (hole_end - page == size)
            return page;
    }
    return BAD_PAGE;
}

page_t pt_find_hole(struct mem *mem, pages_t size) {
    page_t start = mem->mmap_hint;
    if (start == 0 || start > 0xefffd || start <= 0x40000 + size)
        start = 0xefffd;

    page_t result = x86_find_hole_from(mem, size, start);
    if (result != BAD_PAGE) {
        mem->mmap_hint = (result > 0) ? result - 1 : 0;
        return result;
    }

    if (start < 0xefffd) {
        result = x86_find_hole_from(mem, size, 0xefffd);
        if (result != BAD_PAGE) {
            mem->mmap_hint = (result > 0) ? result - 1 : 0;
            return result;
        }
    }

    return BAD_PAGE;
}

#endif // GUEST_64BIT

// ============================================================
// Reservation API — shared by x86 and ARM64.
// Only the ARM64 path actually creates reservations today (via pt_map_lazy),
// but the lookup/overlap/remove helpers are called unconditionally from
// pt_is_hole / pt_unmap / pt_set_flags / pt_copy_on_demand, so they must
// exist for both architectures.
// ============================================================

struct mem_reservation *mem_find_reservation(struct mem *mem, page_t page) {
    for (struct mem_reservation *r = mem->reservations; r; r = r->next) {
        if (page >= r->start && page < r->start + r->pages)
            return r;
    }
    return NULL;
}

static bool reservations_overlap(struct mem *mem, page_t start, pages_t pages) {
    for (struct mem_reservation *r = mem->reservations; r; r = r->next) {
        if (r->start < start + pages && r->start + r->pages > start)
            return true;
    }
    return false;
}

void mem_remove_reservations(struct mem *mem, page_t start, pages_t pages) {
    struct mem_reservation **pp = &mem->reservations;
    while (*pp) {
        struct mem_reservation *r = *pp;
        page_t r_end = r->start + r->pages;
        page_t u_end = start + pages;
        if (r->start >= u_end || r_end <= start) {
            pp = &r->next;
            continue;
        }
        if (r->start >= start && r_end <= u_end) {
            *pp = r->next;
            free(r);
            continue;
        }
        if (r->start < start && r_end > u_end) {
            struct mem_reservation *tail = malloc(sizeof(struct mem_reservation));
            if (tail) {
                tail->start = u_end;
                tail->pages = r_end - u_end;
                tail->flags = r->flags;
                tail->next = r->next;
                r->pages = start - r->start;
                r->next = tail;
            }
            pp = &(tail ? tail : r)->next;
            continue;
        }
        if (r->start < start) {
            r->pages = start - r->start;
        } else {
            r->start = u_end;
            r->pages = r_end - u_end;
        }
        pp = &r->next;
    }
}

bool pt_is_hole(struct mem *mem, page_t start, pages_t pages) {
    for (page_t page = start; page < start + pages; page++) {
        if (mem_pt(mem, page) != NULL)
            return false;
    }
    if (reservations_overlap(mem, start, pages))
        return false;
    return true;
}

int pt_map(struct mem *mem, page_t start, pages_t pages, void *memory, size_t offset, unsigned flags) {
    if (memory == MAP_FAILED)
        return errno_map();

    // If this fails, the munmap in pt_unmap would probably fail.
    assert((uintptr_t) memory % real_page_size == 0 || memory == vdso_data);

    struct data *data = malloc(sizeof(struct data));
    if (data == NULL)
        return _ENOMEM;
    *data = (struct data) {
        .data = memory,
        .size = pages * PAGE_SIZE + offset,

#if LEAK_DEBUG
        .pid = current ? current->pid : 0,
        .dest = start << PAGE_BITS,
#endif
    };

    for (page_t page = start; page < start + pages; page++) {
        if (mem_pt(mem, page) != NULL)
            pt_unmap(mem, page, 1);
#ifdef FASTMEM
        // the page unmapped just now (or before) may have been kept to use
        // again (fm_recycle_put): it isn't, now that something else is there
        if (page < FM_PAGES)
            fm_recycle_take(mem, page, 1);
#endif
        data->refcount++;
        struct pt_entry *pt = mem_pt_new(mem, page);
        pt->data = data;
        pt->offset = ((page - start) << PAGE_BITS) + offset;
        pt->flags = flags;

    }
    return 0;
}

int pt_unmap(struct mem *mem, page_t start, pages_t pages) {
    for (page_t page = start; page < start + pages; page++)
        if (mem_pt(mem, page) == NULL)
            return -1;
    return pt_unmap_always(mem, start, pages);
}

int pt_unmap_always(struct mem *mem, page_t start, pages_t pages) {
#if defined(_WIN32) && defined(GUEST_X86_64)
    // a hypervisor VM's page tables above the window (asbestos/guest-x86_64/vm.c)
    extern void vm_pages_changed(struct mmu *mmu, page_t start, pages_t pages);
    if (mem->mmu.vm != NULL)
        vm_pages_changed(&mem->mmu, start, pages);
#endif
    // Reset mmap_hint if freed region is above it, so pt_find_hole can
    // reuse the freed space on the next mmap(addr=0).
    if (start + pages - 1 > mem->mmap_hint)
        mem->mmap_hint = start + pages - 1;

    mem_remove_reservations(mem, start, pages);
#ifdef FASTMEM
    // window pages go back as they are unmapped, a run at a time (unless the
    // whole window is about to)
    char *fm_run = NULL;
    size_t fm_len = 0;
#endif

    for (page_t page = start; page < start + pages; mem_next_page(mem, &page)) {
        struct pt_entry *pt = mem_pt(mem, page);
        if (pt == NULL)
            continue;

        asbestos_invalidate_page(mem->mmu.asbestos, page);
        struct data *data = pt->data;
#ifdef FASTMEM
        // (a page kept elsewhere may have a read-only copy in the window, for
        // a hypervisor VM: asbestos/guest-x86_64/vm.c)
        if (data->window && !fm_destroying && pages <= FM_RECYCLE_RUN && fm_eager() &&
                (pt->flags & P_ANONYMOUS) && !(pt->flags & (P_SHARED | P_COW)) &&
                fm_prot(pt->flags) == (PROT_READ | PROT_WRITE) && fm_recycle_put(mem, page)) {
            // kept (fm_recycle_put)
        } else if ((data->window || (mem->mmu.vm != NULL && page < FM_PAGES)) && !fm_destroying) {
            // decommitted: not to be used again as it is
            if (page < FM_PAGES)
                fm_recycle_take(mem, page, 1);
            char *host = data->window ? (char *) data->data + pt->offset
                                      : mem->mmu.fm_base + ((size_t) page << PAGE_BITS);
            if (fm_run != NULL && fm_run + fm_len != host) {
                px_fm_decommit(fm_run, fm_len);
                fm_run = NULL;
            }
            if (fm_run == NULL) {
                fm_run = host;
                fm_len = 0;
            }
            fm_len += PAGE_SIZE;
        }
#endif
#if ANON_MMAP_LIMIT_PAGES > 0
        // Decrement per-page for anonymous mappings. This correctly handles
        // partial unmaps (munmap of subset of original mmap region) where the
        // data object's refcount doesn't reach 0 but the guest page is gone.
        //
        // [T-ish-anon-count-negative] Must mirror pt_map_nothing's charge
        // condition EXACTLY. It skips PROT_NONE mappings (a reservation costs
        // no physical memory), so unmapping one must not decrement either —
        // otherwise every PROT_NONE region the guest maps and frees walks the
        // counter down, which is the second half of how it reached -1359 MB.
        if ((pt->flags & P_ANONYMOUS) && anon_page_is_charged(pt->flags))
            atomic_fetch_sub(&anon_page_count, 1);
#endif
        mem_pt_del(mem, page);
        if (--data->refcount == 0) {
            // vdso wasn't allocated with mmap, it's just in our data segment
#ifdef FASTMEM
            if (data->data != vdso_data && !data->window) {
                px_vm_forget_host(data->data, data->size);   // a VM may have it mapped
#else
            if (data->data != vdso_data) {
#endif
                munmap(data->data, data->size);
            }
            if (data->fd != NULL) {
                fd_close(data->fd);
            }
#ifdef FASTMEM
            if (data->lazy_fd != NULL)
                fd_close(data->lazy_fd);
#endif
            free(data);
        }
    }
#ifdef FASTMEM
    if (fm_run != NULL) {
        FS_BEGIN;
        px_fm_decommit(fm_run, fm_len);
        FS_END(FS_UNMAP);
    }
#endif
    mem_changed(mem);
    return 0;
}

// [T-ish-cluster-hitrate] Every anonymous host allocation, and the guest pages
// it backs, regardless of which path asked for it. Without this the cluster
// hit rate is unanchored: a high hit rate means nothing if most memory is
// actually arriving through mmap/brk instead.
_Atomic uint64_t st_anon_mmaps;        // host mmap calls for anonymous memory
_Atomic uint64_t st_anon_pages_mapped; // guest pages they backed

#ifdef FASTMEM
// Moves the pages of one mapping from page on (n of them, the same data at
// consecutive offsets) into the window: a read-only view of a file as P_LAZY
// pages (read in again as used), anything else copied.
static void fm_migrate_run(struct mem *mem, page_t page, pages_t n) {
    struct pt_entry *first = mem_pt(mem, page);
    struct data *od = first->data;
    char *from = (char *) od->data + first->offset;
    char *to = mem->mmu.fm_base + ((size_t) page << PAGE_BITS);
    size_t len = (size_t) n << PAGE_BITS;
    bool lazy = od->fd != NULL && od->fd->real_fd >= 0 && !(first->flags & P_ANONYMOUS) &&
        px_fm_file_view(from);
    unsigned *flags = malloc(n * sizeof(*flags));
    if (flags == NULL)
        return;
    for (pages_t i = 0; i < n; i++)
        flags[i] = mem_pt(mem, page + i)->flags & ~(P_CODE | P_COW);
    struct fd *fd = od->fd != NULL ? fd_retain(od->fd) : NULL;
    int64_t file_off = (int64_t) od->file_offset + (int64_t) first->offset;
    const char *name = od->name;
    px_fm_decommit(to, len);
    // a view's pages already in memory are copied (cheaper than reading them
    // in again as they are used), the others left for the file
    uint64_t *copy = NULL;
    if (lazy) {
        copy = malloc((n + 63) / 64 * 8);
        if (copy != NULL && px_fm_resident(from, n, copy) < 0)
            memset(copy, 0, (n + 63) / 64 * 8);
    }
    for (pages_t i = 0; i < n;) {
        bool c = !lazy || (copy != NULL && ((copy[i / 64] >> (i % 64)) & 1));
        pages_t j = i + 1;
        while (j < n && (!lazy || (copy != NULL && ((copy[j / 64] >> (j % 64)) & 1))) == c)
            j++;
        if (c) {
            char *t = to + ((size_t) i << PAGE_BITS);
            px_fm_commit(t, (size_t) (j - i) << PAGE_BITS, PROT_READ | PROT_WRITE);
            for (pages_t k = i; k < j; k++) {
                char *f = from + ((size_t) k << PAGE_BITS);
                if (!px_fm_committed(f))
                    continue;   // never touched: zeros
                bool none = fm_prot(flags[k]) == 0;
                if (none)
                    mprotect(f, PAGE_SIZE, PROT_READ);
                memcpy(to + ((size_t) k << PAGE_BITS), f, PAGE_SIZE);
                if (none)
                    mprotect(f, PAGE_SIZE, PROT_NONE);
            }
            if (lazy)   // the file's own pages: not written, for a fork
                px_fm_unwritten(t, (size_t) (j - i) << PAGE_BITS);
        }
        i = j;
    }
#if ANON_MMAP_LIMIT_PAGES > 0
    // pt_map lets go of the old pages (uncharging the anonymous ones): the
    // new ones are the same pages, charged again
    long charged = 0;
    for (pages_t i = 0; i < n; i++)
        if ((flags[i] & P_ANONYMOUS) && anon_page_is_charged(flags[i]))
            charged++;
#endif
    if (pt_map(mem, page, n, to, 0, flags[0]) < 0) {
        px_fm_decommit(to, len);
        if (fd != NULL)
            fd_close(fd);
        free(copy);
        free(flags);
        return;
    }
#if ANON_MMAP_LIMIT_PAGES > 0
    atomic_fetch_add(&anon_page_count, charged);
#endif
    struct data *nd = mem_pt(mem, page)->data;
    nd->window = true;
    nd->name = name;
    nd->fd = fd;
    nd->file_offset = (size_t) file_off;
    if (lazy) {
        nd->lazy_fd = fd_retain(fd);
        nd->lazy_off = file_off;
    }
    for (pages_t i = 0; i < n; i++) {
        bool copied = !lazy || (copy != NULL && ((copy[i / 64] >> (i % 64)) & 1));
        mem_pt(mem, page + i)->flags = flags[i] | (copied ? 0 : P_LAZY);
    }
    fm_protect_pages(mem, to, page, n);
    free(copy);
    free(flags);
}

#define FM_HIGH_MAX ((pages_t) 16384)   // 64 MB above 4 GB: no window
void mem_window_migrate(struct mem *mem) {
    write_wrlock(&mem->lock);
    if (mem->mmu.fm_base != NULL || !mem->mmu.fm_want) {
        write_wrunlock(&mem->lock);
        return;
    }
    // Memory above 4 GB stays out of the window: a program that keeps much of
    // its data there (V8 puts its heap cage there) would pay for the move and
    // gain little. Not now, nor later.
    pages_t high = 0;
    for (struct mem_reservation *r = mem->reservations; r != NULL; r = r->next)
        if (r->start >= FM_PAGES)
            high += r->pages;
    for (page_t page = FM_PAGES; page < MEM_PAGES && high < FM_HIGH_MAX; mem_next_page(mem, &page))
        if (mem_pt(mem, page) != NULL)
            high++;
    if (high >= FM_HIGH_MAX) {
        mem->mmu.fm_want = false;
        mem->mmu.fm_ticks = UINT_MAX;
        write_wrunlock(&mem->lock);
        return;
    }
    FS_BEGIN;
    mem->mmu.fm_base = px_fm_reserve((size_t) FM_PAGES << PAGE_BITS, FM_GUARD);
    if (mem->mmu.fm_base == NULL) {
        mem->mmu.fm_want = false;
        write_wrunlock(&mem->lock);
        return;
    }
    for (page_t page = 0; page < FM_PAGES; mem_next_page(mem, &page)) {
        struct pt_entry *e = mem_pt(mem, page);
        if (e == NULL || e->data->window || (e->flags & P_SHARED) || e->data->data == vdso_data)
            continue;
        pages_t n = 1;
        while (page + n < FM_PAGES) {
            struct pt_entry *f = mem_pt(mem, page + n);
            if (f == NULL || f->data != e->data || (f->flags & P_SHARED) ||
                    f->offset != e->offset + ((size_t) n << PAGE_BITS))
                break;
            n++;
        }
        fm_migrate_run(mem, page, n);
        page += n - 1;   // mem_next_page adds the last one
    }
    // what was translated without the window goes; blocks come back with it
    asbestos_invalidate_all(mem->mmu.asbestos);
    mem_changed(mem);
    FS_END(FS_MIGRATE);
    write_wrunlock(&mem->lock);
}
#endif

#ifdef FASTMEM
int pt_map_window(struct mem *mem, page_t start, pages_t pages, unsigned flags,
                  struct fd *file, int64_t offset) {
    if (!fm_fits(mem, start, pages))
        return 1;
    if (flags & P_SHARED) {
        // kept elsewhere: nothing of the window may be left there for an
        // access to land on (pages kept to use again, a VM's copies)
        if (fm_recycle_take(mem, start, pages) || mem->mmu.vm != NULL)
            px_fm_decommit(mem->mmu.fm_base + ((size_t) start << PAGE_BITS), (size_t) pages << PAGE_BITS);
        return 1;
    }
    FS_BEGIN;
    for (page_t page = start; page < start + pages; page++)
        if (mem_pt(mem, page) != NULL)
            pt_unmap(mem, page, 1);
    char *memory = mem->mmu.fm_base + ((size_t) start << PAGE_BITS);
    size_t len = (size_t) pages << PAGE_BITS;
    if (file != NULL) {
        atomic_fetch_add_explicit(&fm_pages_file, pages, memory_order_relaxed);
        fm_recycle_take(mem, start, pages);
        px_fm_decommit(memory, len);
        flags |= P_LAZY;
    } else if (fm_prot(flags) == (PROT_READ | PROT_WRITE) && mem->mmu.fm_recycled_pages > 0) {
        // the pages kept zeroed (committed and filled already), the rest
        // committed, a run of each at a time
        for (page_t page = start; page < start + pages;) {
            bool kept = fm_recycled(mem, page);
            page_t end = page + 1;
            while (end < start + pages && fm_recycled(mem, end) == kept)
                end++;
            if (kept)
                fm_recycle_take(mem, page, end - page);
            char *at = mem->mmu.fm_base + ((size_t) page << PAGE_BITS);
            size_t run = (size_t) (end - page) << PAGE_BITS;
            if (kept)
                memset(at, 0, run);
            else if (px_fm_commit(at, run, fm_prot(flags)) < 0)
                return _ENOMEM;
            atomic_fetch_add_explicit(kept ? &fm_pages_kept : &fm_pages_committed, end - page, memory_order_relaxed);
            page = end;
        }
    } else {
        atomic_fetch_add_explicit(&fm_pages_committed, pages, memory_order_relaxed);
        fm_recycle_take(mem, start, pages);
        if (px_fm_commit(memory, len, fm_prot(flags)) < 0)
            return _ENOMEM;
    }
    int err = pt_map(mem, start, pages, memory, 0, flags);
    if (err < 0) {
        px_fm_decommit(memory, len);
        return err;
    }
    struct data *data = mem_pt(mem, start)->data;
    data->window = true;
    if (file != NULL) {
        data->lazy_fd = fd_retain(file);
        data->lazy_off = offset;
    }
    FS_END(FS_MAP);
    if (file != NULL && pages <= FM_EAGER && fm_eager()) {
        for (page_t page = start; page < start + pages; page++)
            if (mem_pt(mem, page)->flags & P_LAZY)
                fm_fill_run(mem, page, FM_CHUNK_VM);
    }
    return 0;
}
#endif

int pt_map_nothing(struct mem *mem, page_t start, pages_t pages, unsigned flags) {
    if (pages == 0) return 0;
    int err;
#ifdef FASTMEM
    err = pt_map_window(mem, start, pages, flags | P_ANONYMOUS, NULL, 0);
    if (err <= 0)
        goto mapped;
#endif
    // Use host PROT_NONE for guest PROT_NONE mappings. Go runtime reserves
    // ~1.1GB of PROT_NONE address space for page summaries; allocating real
    // memory for these wastes physical RAM and causes iOS jetsam kills.
    int host_prot = PROT_READ | PROT_WRITE;
    if (!(flags & P_READ) && !(flags & P_WRITE) && !(flags & P_EXEC))
        host_prot = PROT_NONE;
    size_t map_size = (size_t)pages * PAGE_SIZE;
    void *memory = mmap(NULL, map_size, host_prot, MAP_PRIVATE | MAP_ANONYMOUS, 0, 0);
    if (memory == MAP_FAILED)
        return _ENOMEM;
    if (host_prot != PROT_NONE) {
        // PROT_NONE reservations cost no physical memory, so they would only
        // dilute the ratio we are trying to measure.
        atomic_fetch_add_explicit(&st_anon_mmaps, 1, memory_order_relaxed);
        atomic_fetch_add_explicit(&st_anon_pages_mapped, (uint64_t)pages,
                                  memory_order_relaxed);
    }
    err = pt_map(mem, start, pages, memory, 0, flags | P_ANONYMOUS);
#ifdef FASTMEM
mapped:
#endif
#if ANON_MMAP_LIMIT_PAGES > 0
    // [T-ish-anon-count-negative] Charge HERE, not at the call sites.
    //
    // pt_unmap decrements once for EVERY P_ANONYMOUS page it removes, so every
    // page mapped here must be charged exactly once or the counter drifts
    // negative — and a negative count silently hands the guest extra headroom
    // on top of the cap. Observed on device 2026-08-25: `in use -86998 pages
    // (-1359 MB host)`, i.e. the cap was granting 1.3GB of free credit while
    // the app climbed to a 3338MB jetsam kill.
    //
    // The old design charged at the call sites, and an audit found 9 of 15
    // sites mapping anonymous pages without charging — including
    // pt_set_flags' mprotect-commit, which materialises whole 32k-page runs
    // (the V8/malloc bomb path that produced the number above). Charging in
    // the one place that creates the mapping makes the asymmetry structurally
    // impossible rather than a thing every new call site must remember.
    //
    // Sites that pre-charge to enforce the cap (sys_mmap, sys_brk, the
    // reservation lazy-commit) hand their reservation over via
    // anon_pages_precharged() so this does not double-count them.
    if (err >= 0 && anon_page_is_charged(flags))
        anon_pages_charge_mapped(pages);
    else
        anon_pages_precharged(0);   // drop any unconsumed reservation credit
#endif
    return err;
}

// [T-ish-cluster-commit] Commit a single faulting guest page as part of a
// host-page-sized CLUSTER, to stop 4KB guest pages each burning a whole 16KB
// host page.
//
// Every committed page gets its own host mmap, and Darwin on Apple Silicon
// cannot hand back less than 16KB, so a one-page commit wastes 12KB. Measured
// on the 2026-08-25 jsonnet compile: 116,836 VM_ALLOCATE regions, every one
// exactly 16KB, holding 456MB of guest pages while consuming 1826MB of host
// memory — a 4x amplification.
//
// The cluster machinery already exists: pt_map() points N guest pages at one
// host allocation and refcounts it, and pt_unmap() only munmaps when the last
// page of that allocation goes away. That IS the per-cluster occupancy map the
// naive "one mmap per page" callers were bypassing by passing pages=1.
//
// Correctness over packing (never fabricate a cluster):
//   * The cluster is the aligned run of `real_page_size / PAGE_SIZE` guest
//     pages containing `page` — always contiguous and aligned by construction,
//     never assembled from scattered requests.
//   * Every page in it must currently be unmapped. A mapped neighbour means
//     the region is already backed; silently re-mapping it would strand the
//     old allocation and corrupt the address space.
//   * `same_flags` lets the caller require that neighbours belong to the same
//     logical region (e.g. one reservation) before they are committed early.
//   * Anything unmet degrades to the plain single-page commit. Wasting 12KB is
//     always preferable to mapping a page the guest did not ask for.
//
// On a 4KB-page host (x86 sim, Linux) cluster_pages == 1 and this is exactly
// the old behaviour.
//
// Caller must hold mem->lock for writing — same as the pt_map_nothing call it
// replaces. Anonymous-page accounting deliberately stays in GUEST pages: the
// cap must stay correct in the worst case where every commit degrades to a
// single page, so it is charged per guest page here just as before.
// [T-ish-cluster-hitrate] Diagnostic counters. A workload whose real benefit
// is far below the 4x the design predicts must be measurable, not guessed at:
// these separate "did the cluster fire" from the specific reason it did not,
// because a mapped neighbour (the guest is filling a region we already backed
// — benign, the memory is shared anyway) and a foreign/absent reservation (the
// cluster is genuinely unavailable) call for completely different responses.
_Atomic uint64_t st_cluster_full;        // committed the whole host page
_Atomic uint64_t st_cluster_single;      // degraded to one guest page
_Atomic uint64_t st_cluster_why_mapped;  // ...because a neighbour was mapped
_Atomic uint64_t st_cluster_why_flags;   // ...because same_flags refused
_Atomic uint64_t st_cluster_why_nocluster; // ...host page == guest page
_Atomic uint64_t st_cluster_why_enomem;  // ...cluster mmap failed, retried
_Atomic uint64_t st_cluster_pages_committed; // guest pages actually committed
_Atomic uint64_t st_cluster_calls;       // total calls

int pt_map_cluster(struct mem *mem, page_t page, unsigned flags,
                   bool (*same_flags)(struct mem *mem, page_t page, void *ctx),
                   void *ctx, pages_t *committed_out) {
    pages_t cluster_pages = (pages_t)(real_page_size >> PAGE_BITS);
    if (cluster_pages < 1)
        cluster_pages = 1;

    atomic_fetch_add_explicit(&st_cluster_calls, 1, memory_order_relaxed);

    page_t base = page & ~(page_t)(cluster_pages - 1);
    bool whole_cluster = cluster_pages > 1;
    if (!whole_cluster)
        atomic_fetch_add_explicit(&st_cluster_why_nocluster, 1, memory_order_relaxed);

    if (whole_cluster) {
        for (page_t p = base; p < base + cluster_pages; p++) {
            if (mem_pt(mem, p) != NULL) {
                atomic_fetch_add_explicit(&st_cluster_why_mapped, 1, memory_order_relaxed);
                whole_cluster = false; break;
            }
            if (same_flags != NULL && !same_flags(mem, p, ctx)) {
                atomic_fetch_add_explicit(&st_cluster_why_flags, 1, memory_order_relaxed);
                whole_cluster = false; break;
            }
        }
    }

    page_t start = whole_cluster ? base : page;
    pages_t count = whole_cluster ? cluster_pages : 1;
#if ANON_MMAP_LIMIT_PAGES > 0
    // [T-ish-anon-count-negative] The caller may have parked a precharge for
    // this commit. pt_map_nothing consumes it, so on the retry below we must
    // re-park what is still owed — otherwise the failed attempt eats the
    // credit and the retry charges the same page a second time.
    long precharge_held = anon_pages_precharge_peek();
#endif
    int err = pt_map_nothing(mem, start, count, flags);
    if (err < 0) {
        // Retry as a single page: the cluster may have failed only because the
        // larger host allocation didn't fit.
        if (whole_cluster) {
            atomic_fetch_add_explicit(&st_cluster_why_enomem, 1, memory_order_relaxed);
            whole_cluster = false;
#if ANON_MMAP_LIMIT_PAGES > 0
            anon_pages_precharged(precharge_held);
#endif
            err = pt_map_nothing(mem, page, 1, flags);
            count = 1;
        }
        if (err < 0) {
            if (committed_out) *committed_out = 0;
            return err;
        }
    }
    if (count > 1)
        atomic_fetch_add_explicit(&st_cluster_full, 1, memory_order_relaxed);
    else
        atomic_fetch_add_explicit(&st_cluster_single, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&st_cluster_pages_committed, (uint64_t)count,
                              memory_order_relaxed);
    if (committed_out) *committed_out = count;
    return 0;
}

// Snapshot the cluster-commit counters for host-side diagnostics.
void ish_anon_mmap_stats(uint64_t *mmaps, uint64_t *pages) {
    if (mmaps) *mmaps = atomic_load(&st_anon_mmaps);
    if (pages) *pages = atomic_load(&st_anon_pages_mapped);
}

void ish_cluster_stats(uint64_t *calls, uint64_t *full, uint64_t *single,
                       uint64_t *why_mapped, uint64_t *why_flags,
                       uint64_t *why_nocluster, uint64_t *why_enomem,
                       uint64_t *pages_committed) {
    if (calls) *calls = atomic_load(&st_cluster_calls);
    if (full) *full = atomic_load(&st_cluster_full);
    if (single) *single = atomic_load(&st_cluster_single);
    if (why_mapped) *why_mapped = atomic_load(&st_cluster_why_mapped);
    if (why_flags) *why_flags = atomic_load(&st_cluster_why_flags);
    if (why_nocluster) *why_nocluster = atomic_load(&st_cluster_why_nocluster);
    if (why_enomem) *why_enomem = atomic_load(&st_cluster_why_enomem);
    if (pages_committed) *pages_committed = atomic_load(&st_cluster_pages_committed);
}

// Cluster predicate for the reservation lazy-commit path: a neighbour may be
// pulled in only if it belongs to the SAME reservation. Committing a page from
// a different reservation (or none) would hand the guest memory it never
// reserved, with flags taken from the wrong region.
static bool reservation_cluster_ok(struct mem *mem, page_t page, void *ctx) {
    return mem_find_reservation(mem, page) == (struct mem_reservation *) ctx;
}

// W^X stat: stores that hit a P_CODE page and unprotected it (see mem_ptr).
_Atomic uint64_t st_wx_clears = 0;

// Metadata flags that must be preserved across mprotect — they track
// allocation type and state, not user-visible protection bits. P_CODE must
// survive mprotect: making a compiled code page writable doesn't remove the
// need to invalidate its blocks on the first store.
#define P_META_FLAGS (P_ANONYMOUS | P_GROWSDOWN | P_COW | P_SHARED | P_CODE | P_LAZY)

int pt_set_flags(struct mem *mem, page_t start, pages_t pages, int flags) {
#if defined(_WIN32) && defined(GUEST_X86_64)
    // a hypervisor VM's page tables above the window (asbestos/guest-x86_64/vm.c)
    extern void vm_pages_changed(struct mmu *mmu, page_t start, pages_t pages);
    if (mem->mmu.vm != NULL)
        vm_pages_changed(&mem->mmu, start, pages);
#endif
    for (page_t page = start; page < start + pages; page++) {
        if (mem_pt(mem, page) == NULL) {
            if (mem_find_reservation(mem, page) != NULL)
                continue;
            return _ENOMEM;
        }
    }
    for (page_t page = start; page < start + pages; page++) {
        struct pt_entry *entry = mem_pt(mem, page);
        if (entry == NULL) {
#ifdef GUEST_64BIT
            // Page is reservation-only (V8 heap cage chunk reserved
            // PROT_NONE, guest now mprotect'ing RW). Materialise it
            // via pt_map_nothing with the requested flags — i.e. treat
            // mprotect as "commit this page with the new protection".
            //
            // Coalesce runs of contiguous reservation-only pages into a
            // single pt_map_nothing call so the underlying host mmap
            // allocates one VM region instead of one per page. V8
            // typically mprotect'es a full 128MB cage (32k pages) in
            // one syscall; per-page mmap exhausts the iOS vm_map entry
            // limit (~64k entries) and ENOMEM's out well before
            // physical memory is tight — this is the primary npm
            // segfault cause on iPhone 8/iOS 16.
            unsigned want = flags & (P_READ | P_WRITE | P_EXEC);
            if (want) {
                page_t run_start = page;
                pages_t run_len = 1;
                while (page + 1 < start + pages &&
                       mem_pt(mem, page + 1) == NULL) {
                    page++;
                    run_len++;
                }
#if ANON_MMAP_LIMIT_PAGES > 0
                // [T-ish-footprint-brake] This commit path used to be the ONE
                // admission bypass: it charged the counter but never checked
                // any limit, which is how V8's cage commits pushed the count
                // to 160% of the cap and got every later tiny malloc refused
                // instead of this one big ask. Route it through the same
                // reserve gate as mmap/brk. Partial success (earlier runs in
                // this same mprotect already committed) is acceptable —
                // POSIX leaves mprotect's partial-failure state unspecified,
                // and Linux itself can fail an mprotect midway.
                if (!anon_pages_reserve((long)run_len))
                    return _ENOMEM;
                int merr = pt_map_nothing(mem, run_start, run_len, flags | P_ANONYMOUS);
                if (merr < 0) {
                    anon_pages_unreserve((long)run_len);
                    return merr;
                }
#else
                pt_map_nothing(mem, run_start, run_len, flags | P_ANONYMOUS);
#endif
            }
#endif
            continue;
        }
        int old_flags = entry->flags;
        entry->flags = flags | (old_flags & P_META_FLAGS);
#if ANON_MMAP_LIMIT_PAGES > 0
        // [T-ish-anon-count-mprotect] mprotect can flip the very predicate the
        // charge/uncharge pair is keyed on, so the count has to follow it.
        //
        // pt_map_nothing charges only when anon_page_is_charged(flags), and
        // pt_unmap decrements only when it still holds at unmap time. Rewriting
        // flags here without adjusting the count broke that pairing in BOTH
        // directions: RW -> PROT_NONE was charged at map and then skipped at
        // unmap (a permanent leak), and PROT_NONE -> RW was never charged yet
        // decremented at unmap (the counter goes negative). The negative
        // direction is demonstrated by case [12] in
        // regress_anon_count_balance.c.
        //
        // That second direction is a plausible contributor to the -1359 MB
        // count [T-ish-anon-count-negative] added its clamp for — an
        // unaccounted mprotect is one way to reach a negative count — but this
        // was not traced back to that incident, so treat the connection as a
        // hypothesis rather than the established cause. Either way the clamp
        // remains worth keeping as a backstop.
        //
        // Only anonymous pages are counted, and only the charged-state
        // TRANSITION matters: an RW -> RX change moves no memory and must not
        // move the count. The reservation branch above `continue`s before
        // reaching here, so pages it commits via pt_map_nothing are charged
        // exactly once, by pt_map_nothing.
        if (old_flags & P_ANONYMOUS) {
            bool was_charged = anon_page_is_charged(old_flags);
            bool now_charged = anon_page_is_charged(entry->flags);
            if (was_charged != now_charged)
                atomic_fetch_add(&anon_page_count, now_charged ? 1 : -1);
        }
#endif

#ifdef FASTMEM
        // the window carries the protection, whichever way it goes (below,
        // for the whole range)
        if (entry->data->window)
            continue;
#endif
        // check if protection is increasing
        if ((flags & ~old_flags) & (P_READ|P_WRITE)) {
            void *data = (char *) entry->data->data + entry->offset;
            // force to be page aligned
            data = (void *) ((uintptr_t) data & ~(real_page_size - 1));
            int prot = PROT_READ;
            if (flags & P_WRITE) prot |= PROT_WRITE;
            if (mprotect(data, real_page_size, prot) < 0) {
                // A private file page the host maps read-only (a read view on
                // Windows) cannot be made writable in place; copy it on the
                // first write, as a private mapping should.
                if (!(flags & P_WRITE) || (old_flags & P_ANONYMOUS))
                    return errno_map();
                entry->flags |= P_COW;
            }
        }
    }
#ifdef FASTMEM
    if (fm_fits(mem, start, pages))
        fm_protect_pages(mem, mem->mmu.fm_base + ((size_t) start << PAGE_BITS), start, pages);
#endif
    mem_changed(mem);
    return 0;
}

#ifdef FASTMEM
// A fork's copy of the window pages from page on that belong to the same
// mapping, in a row: they are never shared (a copy on write would have to
// put another page where the shared one is), so the child gets what the
// parent wrote, copied, and the rest as it was made: zeros, or the file
// again (P_LAZY). Returns how many pages it took, 0 if it can't.
static pages_t fm_fork_copy_(struct mem *src, struct mem *dst, page_t page);
static pages_t fm_fork_copy(struct mem *src, struct mem *dst, page_t page) {
    FS_BEGIN;
    pages_t n = fm_fork_copy_(src, dst, page);
    FS_END(FS_FORK);
    return n;
}
static pages_t fm_fork_copy_(struct mem *src, struct mem *dst, page_t page) {
    struct pt_entry *first = mem_pt(src, page);
    struct data *sd = first->data;
    pages_t n = 0;
    while (page + n < FM_PAGES) {
        struct pt_entry *e = mem_pt(src, page + n);
        if (e == NULL || e->data != sd || e->offset != first->offset + ((size_t) n << PAGE_BITS))
            break;
        n++;
    }
    if (!fm_fits(dst, page, n))
        return 0;
    size_t len = (size_t) n << PAGE_BITS;
    char *from = (char *) sd->data + first->offset;
    char *to = dst->mmu.fm_base + ((size_t) page << PAGE_BITS);
    uint64_t *written = malloc((n + 63) / 64 * 8);
    if (written == NULL)
        return 0;
    if (px_fm_written(from, n, written) < 0)
        memset(written, 0xff, (n + 63) / 64 * 8);
    px_fm_decommit(to, len);
    if (pt_map(dst, page, n, to, 0, first->flags & ~P_CODE) < 0) {
        free(written);
        return 0;
    }
    struct data *dd = mem_pt(dst, page)->data;
    dd->window = true;
    dd->name = sd->name;
    if (sd->fd != NULL) {
        dd->fd = fd_retain(sd->fd);
        dd->file_offset = sd->file_offset + first->offset;
    }
    if (sd->lazy_fd != NULL) {
        dd->lazy_fd = fd_retain(sd->lazy_fd);
        dd->lazy_off = sd->lazy_off + (int64_t) first->offset;
    }
    // which pages the file has again, the rest committed a run at a time.
    // (In a hypervisor VM a page missed costs an exit: the parent's copy of
    // what it read in is copied too.)
    bool copy_read = fm_eager();
    for (pages_t i = 0; i < n; i++) {
        unsigned flags = mem_pt(src, page + i)->flags & ~P_CODE;
        bool w = (written[i / 64] >> (i % 64)) & 1;
        if ((flags & P_LAZY) || (sd->lazy_fd != NULL && !w && !copy_read))
            flags |= P_LAZY;   // the file has it
        mem_pt(dst, page + i)->flags = flags;
    }
    for (pages_t i = 0; i < n;) {
        if (mem_pt(dst, page + i)->flags & P_LAZY) {
            i++;
            continue;
        }
        pages_t j = i + 1;
        while (j < n && !(mem_pt(dst, page + j)->flags & P_LAZY))
            j++;
        // filled once, by fm_protect_pages below: twice cost a fork 1.4ms in a VM
        px_fm_commit_unfilled(to + ((size_t) i << PAGE_BITS), (size_t) (j - i) << PAGE_BITS, PROT_READ | PROT_WRITE);
        i = j;
    }
    // what the parent wrote (or read in); the rest is zeros
    char *region_end = NULL;
    bool committed = false;
    for (pages_t i = 0; i < n; i++) {
        bool w = (written[i / 64] >> (i % 64)) & 1;
        if ((!w && !(copy_read && sd->lazy_fd != NULL)) || (mem_pt(dst, page + i)->flags & P_LAZY))
            continue;
        char *f = from + ((size_t) i << PAGE_BITS);
        int prot = fm_prot(mem_pt(src, page + i)->flags);
        if (f >= region_end)
            committed = px_fm_committed_until(f, &region_end);
        if (!committed)
            continue;
        if (prot == 0)
            px_fm_protect(f, PAGE_SIZE, PROT_READ);
        memcpy(to + ((size_t) i << PAGE_BITS), f, PAGE_SIZE);
        if (prot == 0)
            px_fm_protect(f, PAGE_SIZE, 0);
    }
    fm_protect_pages(dst, to, page, n);
    free(written);
    return n;
}
#endif

#ifdef FASTMEM
// A fork's copy of window pages for a child without the window (it gets its
// own once it has run a while): those the parent wrote, copied; the rest of
// a file mapping as a view of the file again; zeros otherwise. The same
// mapping in a row from page on, as fm_fork_copy(); in pieces of one kind.
static pages_t fm_fork_plain(struct mem *src, struct mem *dst, page_t page) {
    struct pt_entry *first = mem_pt(src, page);
    struct data *sd = first->data;
    pages_t n = 0;
    while (page + n < FM_PAGES) {
        struct pt_entry *e = mem_pt(src, page + n);
        if (e == NULL || e->data != sd || e->offset != first->offset + ((size_t) n << PAGE_BITS))
            break;
        n++;
    }
    char *from = (char *) sd->data + first->offset;
    uint64_t *written = malloc((n + 63) / 64 * 8);
    if (written == NULL)
        return 0;
    if (px_fm_written(from, n, written) < 0)
        memset(written, 0xff, (n + 63) / 64 * 8);
    // a page the file has: never written here (or not read in at all)
    #define FROM_FILE(i) (sd->lazy_fd != NULL && ((mem_pt(src, page + (i))->flags & P_LAZY) || \
                          !((written[(i) / 64] >> ((i) % 64)) & 1)))
    for (pages_t i = 0; i < n;) {
        bool file = FROM_FILE(i);
        pages_t j = i + 1;
        while (j < n && FROM_FILE(j) == file)
            j++;
        size_t len = (size_t) (j - i) << PAGE_BITS;
        unsigned flags = mem_pt(src, page + i)->flags & ~(P_CODE | P_LAZY);
        int64_t off = sd->lazy_off + (int64_t) mem_pt(src, page + i)->offset;
        char *memory;
        if (file) {
            memory = mmap(NULL, len, PROT_READ | ((flags & P_WRITE) ? PROT_WRITE : 0), MAP_PRIVATE,
                          sd->lazy_fd->real_fd, off);
        } else {
            memory = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            if (memory != MAP_FAILED) {
                for (pages_t k = i; k < j; k++) {
                    char *f = from + ((size_t) k << PAGE_BITS);
                    if (!((written[k / 64] >> (k % 64)) & 1) || !px_fm_committed(f))
                        continue;
                    bool none = fm_prot(mem_pt(src, page + k)->flags) == 0;
                    if (none)
                        px_fm_protect(f, PAGE_SIZE, PROT_READ);
                    memcpy(memory + ((size_t) (k - i) << PAGE_BITS), f, PAGE_SIZE);
                    if (none)
                        px_fm_protect(f, PAGE_SIZE, 0);
                }
            }
        }
        if (memory == MAP_FAILED || pt_map(dst, page + i, j - i, memory, 0, flags) < 0) {
            free(written);
            return 0;
        }
        struct data *dd = mem_pt(dst, page + i)->data;
        dd->name = sd->name;
        if (sd->fd != NULL) {
            dd->fd = fd_retain(sd->fd);
            dd->file_offset = sd->file_offset + mem_pt(src, page + i)->offset;
        }
        for (pages_t k = i; k < j; k++)
            mem_pt(dst, page + k)->flags = mem_pt(src, page + k)->flags & ~(P_CODE | P_LAZY);
        i = j;
    }
    #undef FROM_FILE
    free(written);
    return n;
}
#endif

int pt_copy_on_write(struct mem *src, struct mem *dst, page_t start, page_t pages) {
#if defined(_WIN32) && defined(GUEST_X86_64)
    // the source's pages turn copy on write: its VM's entries for them go
    extern void vm_pages_changed(struct mmu *mmu, page_t start, pages_t pages);
    if (src->mmu.vm != NULL)
        vm_pages_changed(&src->mmu, start, pages);
#endif
#if ANON_MMAP_LIMIT_PAGES > 0
    long anon_copied = 0;
#endif
    for (page_t page = start; page < start + pages; mem_next_page(src, &page)) {
        struct pt_entry *entry = mem_pt(src, page);
        if (entry == NULL)
            continue;
        if (pt_unmap_always(dst, page, 1) < 0)
            return -1;
#ifdef FASTMEM
        if (entry->data->window) {
            for (page_t p = page; p < FM_PAGES && mem_pt(src, p) != NULL && mem_pt(src, p)->data == entry->data; p++)
                pt_unmap_always(dst, p, 1);
            // the child gets the window from its start only if everything does
            pages_t n = fm_mode() == 2 ? fm_fork_copy(src, dst, page) : fm_fork_plain(src, dst, page);
            if (n == 0)
                return -1;
#if ANON_MMAP_LIMIT_PAGES > 0
            for (pages_t i = 0; i < n; i++) {
                unsigned f = mem_pt(src, page + i)->flags;
                if ((f & P_ANONYMOUS) && anon_page_is_charged(f))
                    anon_copied++;
            }
#endif
            page += n - 1;   // mem_next_page adds the last one
            continue;
        }
#endif
        if (!(entry->flags & P_SHARED))
            entry->flags |= P_COW;
        entry->data->refcount++;
        struct pt_entry *dst_entry = mem_pt_new(dst, page);
        dst_entry->data = entry->data;
        dst_entry->offset = entry->offset;
        // Don't inherit P_CODE: the child's asbestos has no blocks for this
        // page yet; it re-marks on its own first compile.
        dst_entry->flags = entry->flags & ~P_CODE;
#if ANON_MMAP_LIMIT_PAGES > 0
        // [T-ish-anon-count-cow-predicate] Must mirror pt_unmap's decrement
        // condition EXACTLY, which is `P_ANONYMOUS && anon_page_is_charged`.
        // Counting on P_ANONYMOUS alone charged the child for PROT_NONE pages
        // that pt_unmap then refuses to give back — so every fork of a
        // PROT_NONE region leaked its full page count, permanently. A single
        // 128MB V8 cage chunk is 32768 pages, so a handful of forks pinned the
        // counter above the cap and every later allocation, even 1 page, was
        // refused while `free -m` showed single-digit MB in use.
        if ((entry->flags & P_ANONYMOUS) && anon_page_is_charged(entry->flags))
            anon_copied++;
#endif
    }
#if ANON_MMAP_LIMIT_PAGES > 0
    // The child process now has its own set of anonymous pages.
    // These will be decremented per-page when the child's mm is freed.
    atomic_fetch_add(&anon_page_count, anon_copied);
#endif
    for (struct mem_reservation *r = src->reservations; r; r = r->next) {
        struct mem_reservation *copy = malloc(sizeof(struct mem_reservation));
        if (copy) {
            *copy = *r;
            copy->next = dst->reservations;
            dst->reservations = copy;
        }
    }
    mem_changed(src);
    mem_changed(dst);
    return 0;
}

static void mem_changed(struct mem *mem) {
    __atomic_add_fetch(&mem->mmu.changes, 1, __ATOMIC_SEQ_CST);
#ifdef GUEST_64BIT
    mmu_tlbs_stale(&mem->mmu);
#endif
}

// Public wrapper: bump the memory-change generation so other threads' TLB
// coherence checks force a re-translate. Used by paths that mutate page
// backing without going through pt_map/pt_unmap (e.g. madvise MADV_DONTNEED
// zeroing a page in place).
void mem_changed_pub(struct mem *mem) {
    mem_changed(mem);
}

// This version will return NULL instead of making necessary pagetable changes.
// Used by the emulator to avoid deadlocks.
static void *mem_ptr_nofault(struct mem *mem, addr_t addr, int type) {
    struct pt_entry *entry = mem_pt(mem, PAGE(addr));
    if (entry == NULL)
        return NULL;
    if (entry->flags & P_LAZY)
        return NULL;   // not read in yet: mem_ptr does it
    if (type == MEM_WRITE && !P_WRITABLE(entry->flags))
        return NULL;
    return entry->data->data + entry->offset + PGOFFSET(addr);
}

void *mem_ptr(struct mem *mem, addr_t addr, int type) {
#ifndef NDEBUG
    void *old_ptr = mem_ptr_nofault(mem, addr, type); // just for an assert
    // Set once we drop mem->lock below. While the lock is released the page
    // table is fair game for other threads, and the CoW path deliberately
    // installs a freshly mmap'd page, so `old_ptr` stops being a valid
    // prediction of the final pointer. Only compare when we never let go.
    //
    // Set at each of the three read_wrunlock() sites (growsdown, reservation,
    // CoW). The later write_wrunlock()/read_wrlock() downgrades are lock-drop
    // windows too, but every one of them is downstream of one of those three,
    // so `remapped` is already true by the time they run. If you ever add a
    // path that reaches a downgrade without passing a read_wrunlock() above,
    // set the flag there as well or this assert can abort again.
    bool remapped = false;
#endif

    page_t page = PAGE(addr);
    struct pt_entry *entry = mem_pt(mem, page);
    extern __thread volatile sig_atomic_t in_jit;

    if (entry == NULL) {
        // page does not exist
        // look to see if the next VM region is willing to grow down
        page_t p = page;
        mem_next_page(mem, &p);
        while (p < MEM_PAGES && mem_pt(mem, p) == NULL)
            mem_next_page(mem, &p);
        if (p >= MEM_PAGES)
            goto check_reservation;
        if (!(mem_pt(mem, p)->flags & P_GROWSDOWN))
            goto check_reservation;

        // Enforce RLIMIT_STACK: don't grow stack beyond the limit.
#ifdef GUEST_64BIT
        // Stack top is at STACK_TOP_PAGE (guard page), stack grows down from STACK_INIT_PAGE.
        pages_t guard_page = STACK_TOP_PAGE;
#else
        // Stack top is at page 0xffffe (guard page), stack grows down from 0xffffd.
        pages_t guard_page = 0xffffe;
#endif
        rlim_t_ stack_limit = rlimit(RLIMIT_STACK_);
        if (stack_limit != RLIM_INFINITY_) {
            pages_t stack_pages = guard_page - page;
            // Allow the same headroom native Linux effectively gives. Userspace
            // (JSC/bun, glibc) derives its stack bound as SP_at_entry -
            // RLIMIT_STACK, while we count from the stack VMA top (guard page),
            // which sits args/env/auxv-sized bytes ABOVE that SP. Linux also
            // grants the initial stack VMA 128KB of expansion headroom
            // (setup_arg_pages: stack_expand = 131072). Without the slack, JS
            // recursion that runs right up to JSC's believed bound faults one
            // page BELOW our enforced bottom before JSC can raise RangeError —
            // deterministic SIGSEGV at stack_top - RLIMIT_STACK (bun/claude).
            uint64_t slack = 131072;
            if ((uint64_t)stack_pages * PAGE_SIZE > stack_limit + slack)
                return NULL;
        }

        // Lock upgrade: release read, acquire write.
        // In JIT context (inside fiber_enter), other threads hold
        // mem->lock READ from their task_run_current. Our own read lock
        // is released just below; write_wrlock would then block on
        // OTHER threads' read locks, but those threads also eventually
        // release their read lock (each cycle of cpu_run_to_interrupt).
        //
        // CRITICAL: must NOT return NULL → INT_GPF on trylock contention
        // in the growsdown path. INT_GPF → JIT block retry → re-execute
        // `sub sp, sp, #N` → sp is decremented AGAIN, compounding the
        // allocation on each retry. Observed with musl's printf_core
        // where `sub sp, sp, #0x1dd0` after 3 retries leaves sp 3x
        // lower than intended; later ldp/ret read zeros from a fresh
        // page and jump to PC=0.
        //
        // Busy-wait spin on trylock with a cap (~32 iterations) to
        // avoid unbounded spin in a pathological case. Each reader
        // releases its lock on the next JIT block boundary, so 32
        // iterations give other threads time to finish their current
        // block without our thread needing to return and risk the
        // retry SP compounding.
        read_wrunlock(&mem->lock);
#ifndef NDEBUG
        // Lock dropped — see `remapped` at the top of the function.
        remapped = true;
#endif
        // write_wrlock here uses a NON-BLOCKING trylock-spin (see
        // __write_wrlock in util/sync.h; the blocking fallback was removed in
        // 26c5d9d5 because it deadlocked JSC's GC). We released our own read
        // lock just above, and other threads release theirs on each JIT block
        // boundary, so a reader-free window normally appears quickly — but the
        // spin is unbounded in time, so nothing read before it still holds.
        //
        // Returning NULL → INT_GPF from this path is catastrophic for
        // growsdown: INT_GPF retry re-enters the JIT block from the
        // start, re-executing the `sub sp, sp, #N` prologue and
        // decrementing guest SP a second time. After 3 retries, the
        // caller's saved x29/x30 location is 3× the intended offset
        // below the original SP; the caller's epilogue ldp reads 0/0
        // from a fresh growsdown page and ret jumps to PC=0.
        write_wrlock(&mem->lock);
        // Re-check after acquiring write lock (another thread may have grown it)
        entry = mem_pt(mem, page);
        if (entry != NULL) {
            // Already mapped by another thread
            write_wrunlock(&mem->lock);
            read_wrlock(&mem->lock);
            goto have_entry;
        }
        // Grow stack aggressively: allocate from `page` up to but not
        // including the next mapped page. This matches native Linux
        // expand_stack behaviour where a large `sub sp, sp, #N` followed
        // by stores anywhere within the new frame works without
        // faulting on every intermediate page. Musl's deeper fs helpers
        // use ~2-page frames (sub sp, #0x1dd0) — growing one page at a
        // time on first access misses pages above the fault that
        // haven't been touched yet, which later store instructions then
        // fault on. The SIGSEGV handler runs on the same shallow stack
        // and recursively faults, corrupting PC to 0 (infinite loop).
        //
        // CRITICAL: the fault page `page` MUST always be mapped, otherwise
        // mem_pt(page) below returns NULL and the faulting store lands on
        // unmapped memory — silently corrupting the frame. The old cap
        // moved grow_start UP toward the stack, so a fault more than
        // max_grow pages below the mapped stack (e.g. a single
        // `sub sp, sp, #0x30000; str [sp]` — a >256KB frame faulting ~48
        // pages down) left the fault page itself unmapped. Cap instead how
        // far ABOVE the fault page we pre-map, to bound accidental
        // expansion when a wild pointer falls into a growsdown region; the
        // gap between the fault page and the stack, if any, grows later on
        // demand. The fault page itself is never dropped.
        {
            const pages_t max_grow = 256;   // pre-map window (1 MB)
            pages_t grow_end = p; // first already-mapped page
            pages_t grow_start = page;
            if ((pages_t)(grow_end - grow_start) > max_grow)
                grow_end = grow_start + max_grow; // keep fault page, cap upward
            pages_t grow_count = grow_end - grow_start;
#if ANON_MMAP_LIMIT_PAGES > 0
            // [T-ish-anon-cap-dynamic] Respect the runtime cap for the
            // 1MB pre-map window, but NEVER refuse the fault page itself —
            // the block comment above explains that an unmapped fault page
            // silently corrupts the frame. When the window doesn't fit,
            // shrink to just the fault page and count it unconditionally:
            // a 4KB overrun of the cap is noise, a corrupted stack is not.
            // [T-ish-anon-count-negative] Reserve only to decide the size;
            // pt_map_nothing does the charging (consuming this reservation).
            // The fault page itself is never refused, so when the window does
            // not fit we shrink to 1 page and let pt_map_nothing charge it
            // even if that nudges the cap — a 4KB overrun is noise, a
            // corrupted stack is not.
            if (!anon_pages_reserve((long)grow_count))
                grow_count = 1;
#endif
            // [T-ish-cluster-commit] A multi-page window is already one host
            // allocation shared by refcount, so it needs no clustering. Only
            // the degraded single-page case (cap-shrunk, or a fault adjacent
            // to mapped stack) would burn a whole host page for 4KB — route
            // that one through the cluster helper. No same_flags predicate:
            // every page here is stack in the same growsdown region, and the
            // helper still refuses to touch already-mapped neighbours.
            if (grow_count == 1) {
                // Charging happens inside pt_map_nothing for whatever the
                // cluster ends up committing — nothing to do here.
                pt_map_cluster(mem, grow_start, P_WRITE | P_GROWSDOWN,
                               NULL, NULL, NULL);
            } else {
                pt_map_nothing(mem, grow_start, grow_count, P_WRITE | P_GROWSDOWN);
            }
        }
        write_wrunlock(&mem->lock);
        read_wrlock(&mem->lock);

        entry = mem_pt(mem, page);
        goto have_entry;

check_reservation: ;
        struct mem_reservation *res = mem_find_reservation(mem, page);
        if (res == NULL)
            return NULL;
        read_wrunlock(&mem->lock);
#ifndef NDEBUG
        // Lock dropped — see `remapped` at the top of the function.
        remapped = true;
#endif
        // Acquire rather than return NULL → INT_GPF retry (which re-runs the
        // JIT block's prologue and can compound sub-sp or other side effects).
        // See growsdown branch above for the full rationale, including why the
        // acquire spins instead of blocking.
        write_wrlock(&mem->lock);
        entry = mem_pt(mem, page);
        if (entry == NULL) {
            // [T-ish-anon-cap-dynamic] This lazy commit-on-fault is the path
            // large NORESERVE reservations actually materialize through, one
            // page per TLB miss — during the 2026-08-25 jsonnet compile it
            // committed >116k pages while the old code only ever *counted*
            // them, so the anon cap never applied and the footprint sailed
            // past every limit. Enforce here too: on refusal leave the entry
            // NULL, so mem_ptr returns NULL and the guest takes SIGSEGV —
            // the same thing Linux does when overcommitted memory can't be
            // backed. Roll the reservation back if the host mmap itself
            // fails, so a transient failure doesn't leak count forever.
            // [T-ish-cluster-commit] Commit the whole host page at once when
            // the neighbours are free and belong to this same reservation.
            // This is the path that produced the 116k single-page regions.
            unsigned commit_flags = res->flags & ~P_GROWSDOWN;
            pages_t want = (pages_t)(real_page_size >> PAGE_BITS);
            if (want < 1) want = 1;
#if ANON_MMAP_LIMIT_PAGES > 0
            static _Atomic long reservation_denied_reported;
            // Charge the cluster up front, then refund whatever wasn't
            // committed. Charging after the fact could let two threads both
            // pass a nearly-full cap and overshoot it.
            pages_t charged = want;
            if (!anon_pages_reserve(charged)) {
                charged = 1;
                if (!anon_pages_reserve(charged))
                    charged = 0;
            }
            if (charged == 0) {
                long prev = atomic_fetch_add(&reservation_denied_reported, 1);
                if (prev % 4096 == 0)
                    printk("mem: anon page cap refused lazy commit for pid=%d "
                           "(page %#x, %ld denials so far) — guest gets SIGSEGV\n",
                           current != NULL ? current->pid : -1, page, prev + 1);
            } else {
                // [T-ish-anon-count-negative] The reservation above is handed
                // to pt_map_nothing (via the thread-local precharge) and
                // consumed by whatever it maps; it charges the remainder
                // itself. Only the UNUSED part of the reservation has to come
                // back here — when the cluster degrades, or the map fails.
                pages_t got = 0;
                int cerr = (charged == want)
                    ? pt_map_cluster(mem, page, commit_flags,
                                     reservation_cluster_ok, res, &got)
                    : pt_map_nothing(mem, page, 1, commit_flags);
                if (charged != want)
                    got = (cerr < 0) ? 0 : 1;
                if (got < charged)
                    anon_pages_unreserve((long)(charged - got));
            }
#else
            pt_map_cluster(mem, page, commit_flags,
                           reservation_cluster_ok, res, NULL);
#endif
        }
        write_wrunlock(&mem->lock);
        read_wrlock(&mem->lock);
        entry = mem_pt(mem, page);
    }

have_entry:
#ifdef FASTMEM
    if (entry != NULL && (entry->flags & P_LAZY)) {
        if (mem_no_fill)
            return NULL;
        read_wrunlock(&mem->lock);
#ifndef NDEBUG
        remapped = true;
#endif
        write_wrlock(&mem->lock);
        fm_fill(mem, page);
        write_wrunlock(&mem->lock);
        read_wrlock(&mem->lock);
        entry = mem_pt(mem, page);
    }
#endif
#ifdef GUEST_X86_64
    // A page with no access must fault on a read as well. The JVM puts its
    // heap after a PROT_NONE page and loads from it as an implicit null
    // check, expecting the SIGSEGV; given the host pointer instead, the read
    // hit the host's no-access page and killed ish on Windows (and read zeros
    // elsewhere). x86 has no write- or execute-only pages, so either of those
    // makes the page readable.
    if (entry != NULL && type == MEM_READ && !(entry->flags & (P_READ | P_WRITE | P_EXEC)))
        return NULL;
#endif
    // Diagnostic write watchpoint: host-side write acquisitions (user_write,
    // madvise zeroing, CoW) bypass the guest store gadgets — record them too.
    if (type == MEM_WRITE || type == MEM_WRITE_PTRACE) {
        extern volatile addr_t g_watch_pages[2];
        if (g_watch_pages[0] && ((addr & ~0xfffULL) == g_watch_pages[0] ||
                                 (addr & ~0xfffULL) == g_watch_pages[1])) {
            extern void watch_record_memptr(uint64_t addr);
            watch_record_memptr(addr);
        }
    }
    if (entry != NULL && (type == MEM_WRITE || type == MEM_WRITE_PTRACE)) {
        // if page is unwritable, well tough luck
        if (type != MEM_WRITE_PTRACE && !(entry->flags & P_WRITE))
            return NULL;
        if (type == MEM_WRITE_PTRACE) {
            // TODO: Is P_WRITE really correct? The page shouldn't be writable without ptrace.
            entry->flags |= P_WRITE | P_COW;
        }
        // get rid of any compiled blocks in this page
        asbestos_invalidate_page(mem->mmu.asbestos, page);
        // W^X: the page holds compiled guest code. Unprotect it and bump
        // mmu->changes so every thread's cached writable TLB entry for the
        // page goes stale — later stores keep coming back through here while
        // blocks exist. The invalidate above happened first, so a compile
        // racing with this store either finished inserting (its block was
        // just invalidated) or is still reading bytes (it will see the bump /
        // cleared flag and discard its possibly-torn block).
        if (__atomic_load_n(&entry->flags, __ATOMIC_RELAXED) & P_CODE) {
            __atomic_fetch_and(&entry->flags, ~P_CODE, __ATOMIC_ACQ_REL);
#ifdef FASTMEM
            fm_protect_entry(entry);
#endif
            atomic_fetch_add_explicit(&st_wx_clears, 1, memory_order_relaxed);
            mem_changed(mem);
            // Order the bump before the caller's store: a compiling thread
            // that observes the (torn) store bytes must also observe the
            // bump when it verifies.
            __atomic_thread_fence(__ATOMIC_SEQ_CST);
        }
        // if page is cow, ~~milk~~ copy it
        if (entry->flags & P_COW) {
            void *copy = mmap(NULL, PAGE_SIZE, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS, 0, 0);

            read_wrunlock(&mem->lock);
#ifndef NDEBUG
            // Lock dropped — see `remapped` at the top of the function.
            remapped = true;
#endif
            // write_wrlock uses a NON-BLOCKING trylock-spin (util/sync.h): it
            // retries with a backoff that ramps to a steady 100us sleep and
            // never queues as a blocking waiter, because blocking here is
            // writer-preferring on Darwin and deadlocks JSC's GC against JIT
            // reader threads (26c5d9d5). We must get the write lock rather
            // than return NULL → INT_GPF: retry would re-run the entire JIT
            // block from the start, and a prologue like `sub sp, sp, #N`
            // before the faulting store would decrement SP again each retry,
            // corrupting the frame layout. Note the spin can last for many
            // milliseconds while readers stay live, which is precisely why
            // nothing observed before this call may be assumed afterwards.
            write_wrlock(&mem->lock);
            // Re-fetch entry after lock upgrade — another thread may have
            // already resolved this CoW while we were waiting for the lock.
            entry = mem_pt(mem, page);
            if (entry != NULL && (entry->flags & P_COW)) {
                void *data = (char *) entry->data->data + entry->offset;
                memcpy(copy, data, PAGE_SIZE);
#if ANON_MMAP_LIMIT_PAGES > 0
                // pt_map will unmap the old page (decrementing anon_page_count),
                // so pre-increment for the new CoW copy to keep balance.
                if (entry->flags & P_ANONYMOUS)
                    atomic_fetch_add(&anon_page_count, 1);
#endif
                pt_map(mem, page, 1, copy, 0, entry->flags &~ P_COW);
                mem_changed(mem);
            } else {
                munmap(copy, PAGE_SIZE);
            }
            write_wrunlock(&mem->lock);
            read_wrlock(&mem->lock);
        }
    }

    void *ptr = mem_ptr_nofault(mem, addr, type);
#ifndef NDEBUG
    // `remapped` covers all three paths that drop mem->lock (growsdown,
    // reservation, CoW); each installs new page-table state on purpose, so a
    // changed pointer there is correct behavior, not a bug.
    assert(remapped || old_ptr == NULL || old_ptr == ptr || type == MEM_WRITE_PTRACE);
#endif
    return ptr;
}

// Diagnostic: racy unlocked peek of a guest u32 for the write watchpoint.
uint32_t mem_watch_peek32_mem(struct mem *mem, uint64_t addr) {
    if (mem == NULL)
        return 0xdead0001;
    struct pt_entry *entry = mem_pt(mem, PAGE(addr));
    if (entry == NULL || entry->data == NULL)
        return 0xdead0002;
    return *(uint32_t *)((char *)entry->data->data + entry->offset + PGOFFSET(addr));
}
uint32_t mem_watch_peek32(uint64_t addr) {
    if (current == NULL)
        return 0xdead0001;
    return mem_watch_peek32_mem(current->mem, addr);
}

// W^X code-page protection: mark `page` as containing compiled guest code.
// Caller must hold the mem read lock (JIT context). Returns 1 if the page
// transitioned to code — mmu->changes is bumped so every thread's cached
// writable TLB entry for the page goes stale and the next store faults into
// mem_ptr (which invalidates the page's blocks and unprotects it). Returns 0
// if already marked, -1 if the page isn't mapped.
int mmu_mark_code_page(struct mmu *mmu, page_t page) {
    struct mem *mem = container_of(mmu, struct mem, mmu);
    struct pt_entry *entry = mem_pt(mem, page);
    if (entry == NULL)
        return -1;
#ifdef FASTMEM
    // read-only in the window first: once P_CODE is there, every store to
    // the page must come through mem_ptr
    if (entry->data->window && !(entry->flags & (P_CODE | P_LAZY)))
        px_fm_protect((char *) entry->data->data + entry->offset, PAGE_SIZE,
                      fm_prot(entry->flags | P_CODE));
#endif
    unsigned old = __atomic_fetch_or(&entry->flags, P_CODE, __ATOMIC_ACQ_REL);
    if (old & P_CODE)
        return 0;
    mem_changed(mem);
    return 1;
}

__thread int mem_no_fill;

void mmu_fill_lazy(struct mmu *mmu, addr_t addr) {
#ifdef FASTMEM
    struct mem *mem = container_of(mmu, struct mem, mmu);
    for (page_t page = PAGE(addr); page <= PAGE(addr) + 1; page++) {
        struct pt_entry *entry = mem_pt(mem, page);
        if (entry == NULL || !(entry->flags & P_LAZY))
            continue;
        read_wrunlock(&mem->lock);
        write_wrlock(&mem->lock);
        fm_fill(mem, page);
        write_wrunlock(&mem->lock);
        read_wrlock(&mem->lock);
    }
#else
    (void) mmu; (void) addr;
#endif
}

// Whether the P_CODE mark placed by mmu_mark_code_page is still there. A
// store that raced with compilation went through mem_ptr and cleared it.
bool mmu_code_page_intact(struct mmu *mmu, page_t page) {
    struct mem *mem = container_of(mmu, struct mem, mmu);
    struct pt_entry *entry = mem_pt(mem, page);
    return entry != NULL && (__atomic_load_n(&entry->flags, __ATOMIC_ACQUIRE) & P_CODE);
}

static void *mem_mmu_translate(struct mmu *mmu, addr_t addr, int type) {
    // Use mem_ptr instead of mem_ptr_nofault to properly handle:
    // 1. Copy-on-write (COW) pages - need to copy before write
    // 2. Growing stack pages (P_GROWSDOWN)
    return mem_ptr(container_of(mmu, struct mem, mmu), addr, type);
}

static void *mem_mmu_translate_write_nofault(struct mmu *mmu, addr_t addr) {
    struct mem *mem = container_of(mmu, struct mem, mmu);
    // W^X: never stamp a writable TLB entry for a page holding compiled code.
    // Stores must take the write-miss path (mem_ptr), which invalidates the
    // page's blocks and clears P_CODE first.
    struct pt_entry *entry = mem_pt(mem, PAGE(addr));
    if (entry != NULL && (__atomic_load_n(&entry->flags, __ATOMIC_ACQUIRE) & P_CODE))
        return NULL;
    return mem_ptr_nofault(mem, addr, MEM_WRITE);
}

static struct mmu_ops mem_mmu_ops = {
    .translate = mem_mmu_translate,
    .translate_write_nofault = mem_mmu_translate_write_nofault,
};

int mem_segv_reason(struct mem *mem, addr_t addr) {
    struct pt_entry *pt = mem_pt(mem, PAGE(addr));
    if (pt == NULL)
        return SEGV_MAPERR_;
    return SEGV_ACCERR_;
}

size_t real_page_size;
__attribute__((constructor)) static void get_real_page_size() {
    real_page_size = sysconf(_SC_PAGESIZE);
}

void mem_coredump(struct mem *mem, const char *file) {
    int fd = open(file, O_CREAT | O_RDWR | O_TRUNC, 0666);
    if (fd < 0) {
        perror("open");
        return;
    }
    if (ftruncate(fd, 0xffffffff) < 0) {
        perror("ftruncate");
        return;
    }

    int pages = 0;
    for (page_t page = 0; page < MEM_PAGES; mem_next_page(mem, &page)) {
        struct pt_entry *entry = mem_pt(mem, page);
        if (entry == NULL)
            continue;
        pages++;
        if (lseek(fd, (off_t)(page << PAGE_BITS), SEEK_SET) < 0) {
            perror("lseek");
            return;
        }
        if (write(fd, entry->data->data, PAGE_SIZE) < 0) {
            perror("write");
            return;
        }
    }
    printk("dumped %d pages\n", pages);
    close(fd);
}
