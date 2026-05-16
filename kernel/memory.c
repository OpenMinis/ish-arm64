#include <fcntl.h>
#include <unistd.h>
#include <sched.h>
#include <sys/mman.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <stdatomic.h>

#define DEFAULT_CHANNEL memory
#include "debug.h"
#include "kernel/errno.h"
#include "kernel/signal.h"
#include "kernel/memory.h"
#include "asbestos/asbestos.h"
#include "kernel/vdso.h"
#include "kernel/task.h"
#include "kernel/resource.h"
#include "fs/fd.h"

// increment the change count
static void mem_changed(struct mem *mem);
static struct mmu_ops mem_mmu_ops;

#include "kernel/mm.h"


#ifdef GUEST_ARM64
// ============================================================
// ARM64: 4-level page table for 48-bit address space
// ============================================================

void mem_init(struct mem *mem) {
    mem->pgdir = calloc(1, sizeof(struct pt_node));
    mem->pgdir_used = 0;
    mem->mmap_hint = 0;
    mem->reservations = NULL;
    mem->mmu.ops = &mem_mmu_ops;
    mem->mmu.asbestos = asbestos_new(&mem->mmu);
    mem->mmu.changes = 0;
    wrlock_init(&mem->lock);
    lock_init(&mem->cow_lock);
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
    write_wrlock(&mem->lock);
    pt_unmap_always(mem, 0, MEM_PAGES);
    while (mem->reservations) {
        struct mem_reservation *r = mem->reservations;
        mem->reservations = r->next;
        free(r);
    }
    asbestos_free(mem->mmu.asbestos);
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
    // Use a simple strategy: scan upward looking for unallocated L0 subtrees
    page_t page = 0x100000; // Start at 4GB
    page_t hole_start = page;
    pages_t hole_size = 0;

    while (page < USER_ADDR_MAX_PAGE) {
        int i0 = PT_INDEX(page, 0);
        struct pt_node *l0 = mem->pgdir;
        struct pt_node *l1 = l0 ? l0->children[i0] : NULL;
        if (l1 == NULL) {
            // Entire L0 subtree is empty (2^27 pages = 512GB)
            page_t l0_size = (page_t)1 << (PT_BITS * 3);
            page_t l0_base = (page_t)i0 << (PT_BITS * 3);
            if (hole_size == 0) hole_start = l0_base < page ? page : l0_base;
            page_t subtree_end = l0_base + l0_size;
            if (subtree_end > USER_ADDR_MAX_PAGE)
                subtree_end = USER_ADDR_MAX_PAGE;
            hole_size = subtree_end - hole_start;
            if (hole_size >= size)
                return hole_start;
            page = subtree_end;
            continue;
        }
        // L1 exists — something is mapped here, skip
        hole_size = 0;
        page_t l0_size = (page_t)1 << (PT_BITS * 3);
        page_t l0_base = (page_t)i0 << (PT_BITS * 3);
        page = l0_base + l0_size;
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
    write_wrlock(&mem->lock);
    pt_unmap_always(mem, 0, MEM_PAGES);
    asbestos_free(mem->mmu.asbestos);
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

#endif // GUEST_ARM64

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
    // Reset mmap_hint if freed region is above it, so pt_find_hole can
    // reuse the freed space on the next mmap(addr=0).
    if (start + pages - 1 > mem->mmap_hint)
        mem->mmap_hint = start + pages - 1;

    mem_remove_reservations(mem, start, pages);

    for (page_t page = start; page < start + pages; mem_next_page(mem, &page)) {
        struct pt_entry *pt = mem_pt(mem, page);
        if (pt == NULL)
            continue;

        asbestos_invalidate_page(mem->mmu.asbestos, page);
        struct data *data = pt->data;
#if ANON_MMAP_LIMIT_PAGES > 0
        // Decrement per-page for anonymous mappings. This correctly handles
        // partial unmaps (munmap of subset of original mmap region) where the
        // data object's refcount doesn't reach 0 but the guest page is gone.
        if (pt->flags & P_ANONYMOUS)
            atomic_fetch_sub(&anon_page_count, 1);
#endif
        mem_pt_del(mem, page);
        if (--data->refcount == 0) {
            // vdso wasn't allocated with mmap, it's just in our data segment
            if (data->data != vdso_data) {
                munmap(data->data, data->size);
            }
            if (data->fd != NULL) {
                fd_close(data->fd);
            }
            free(data);
        }
    }
    mem_changed(mem);
    return 0;
}

int pt_map_nothing(struct mem *mem, page_t start, pages_t pages, unsigned flags) {
    if (pages == 0) return 0;
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
    return pt_map(mem, start, pages, memory, 0, flags | P_ANONYMOUS);
}

// Metadata flags that must be preserved across mprotect — they track
// allocation type and state, not user-visible protection bits.
#define P_META_FLAGS (P_ANONYMOUS | P_GROWSDOWN | P_COW | P_SHARED)

int pt_set_flags(struct mem *mem, page_t start, pages_t pages, int flags) {
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
#ifdef GUEST_ARM64
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
                pt_map_nothing(mem, run_start, run_len, flags | P_ANONYMOUS);
            }
#endif
            continue;
        }
        int old_flags = entry->flags;
        entry->flags = flags | (old_flags & P_META_FLAGS);

        // check if protection is increasing
        if ((flags & ~old_flags) & (P_READ|P_WRITE)) {
            void *data = (char *) entry->data->data + entry->offset;
            // force to be page aligned
            data = (void *) ((uintptr_t) data & ~(real_page_size - 1));
            int prot = PROT_READ;
            if (flags & P_WRITE) prot |= PROT_WRITE;
            if (mprotect(data, real_page_size, prot) < 0)
                return errno_map();
        }
    }
    mem_changed(mem);
    return 0;
}

int pt_copy_on_write(struct mem *src, struct mem *dst, page_t start, page_t pages) {
#if ANON_MMAP_LIMIT_PAGES > 0
    long anon_copied = 0;
#endif
    for (page_t page = start; page < start + pages; mem_next_page(src, &page)) {
        struct pt_entry *entry = mem_pt(src, page);
        if (entry == NULL)
            continue;
        if (pt_unmap_always(dst, page, 1) < 0)
            return -1;
        if (!(entry->flags & P_SHARED))
            entry->flags |= P_COW;
        entry->data->refcount++;
        struct pt_entry *dst_entry = mem_pt_new(dst, page);
        dst_entry->data = entry->data;
        dst_entry->offset = entry->offset;
        dst_entry->flags = entry->flags;
#if ANON_MMAP_LIMIT_PAGES > 0
        if (entry->flags & P_ANONYMOUS)
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
    __atomic_add_fetch(&mem->mmu.changes, 1, __ATOMIC_RELEASE);
}

// This version will return NULL instead of making necessary pagetable changes.
// Used by the emulator to avoid deadlocks.
static void *mem_ptr_nofault(struct mem *mem, addr_t addr, int type) {
    struct pt_entry *entry = mem_pt(mem, PAGE(addr));
    if (entry == NULL)
        return NULL;
    if (type == MEM_WRITE && !P_WRITABLE(entry->flags))
        return NULL;
    return entry->data->data + entry->offset + PGOFFSET(addr);
}

void *mem_ptr(struct mem *mem, addr_t addr, int type) {
#ifndef NDEBUG
    void *old_ptr = mem_ptr_nofault(mem, addr, type); // just for an assert
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
#ifdef GUEST_ARM64
        // Stack top is at STACK_TOP_PAGE (guard page), stack grows down from STACK_INIT_PAGE.
        pages_t guard_page = STACK_TOP_PAGE;
#else
        // Stack top is at page 0xffffe (guard page), stack grows down from 0xffffd.
        pages_t guard_page = 0xffffe;
#endif
        rlim_t_ stack_limit = rlimit(RLIMIT_STACK_);
        if (stack_limit != RLIM_INFINITY_) {
            pages_t stack_pages = guard_page - page;
            if ((uint64_t)stack_pages * PAGE_SIZE > stack_limit)
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
        // BLOCKING write_wrlock (not trylock) in BOTH JIT and non-JIT
        // contexts. The earlier concern was "JIT context other threads
        // hold mem->lock READ — blocking would deadlock"; actually the
        // JIT-context thread released its own read lock just above and
        // other threads release their read locks on each JIT block
        // boundary. Blocking here waits at most one JIT cycle.
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
        // Cap expansion at 16 pages (64KB) to bound accidental
        // expansion when a wild pointer just happens to fall into a
        // growsdown region.
        {
            const pages_t max_grow = 16;
            pages_t grow_end = p; // first already-mapped page
            pages_t grow_start = page;
            if ((pages_t)(grow_end - grow_start) > max_grow)
                grow_start = grow_end - max_grow;
            pages_t grow_count = grow_end - grow_start;
#if ANON_MMAP_LIMIT_PAGES > 0
            atomic_fetch_add(&anon_page_count, grow_count);
#endif
            pt_map_nothing(mem, grow_start, grow_count, P_WRITE | P_GROWSDOWN);
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
        // BLOCKING: avoid NULL → INT_GPF retry (which re-runs the JIT
        // block's prologue and can compound sub-sp or other side
        // effects). See growsdown branch above for the full rationale.
        write_wrlock(&mem->lock);
        entry = mem_pt(mem, page);
        if (entry == NULL) {
#if ANON_MMAP_LIMIT_PAGES > 0
            atomic_fetch_add(&anon_page_count, 1);
#endif
            pt_map_nothing(mem, page, 1, res->flags & ~P_GROWSDOWN);
        }
        write_wrunlock(&mem->lock);
        read_wrlock(&mem->lock);
        entry = mem_pt(mem, page);
    }

have_entry:
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
        // if page is cow, ~~milk~~ copy it
        if (entry->flags & P_COW) {
            void *copy = mmap(NULL, PAGE_SIZE, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS, 0, 0);

            read_wrunlock(&mem->lock);
            // BLOCKING write_wrlock (not trylock) in both JIT and
            // non-JIT contexts. Returning NULL → INT_GPF retry re-runs
            // the entire JIT block from the start; if the block
            // contains a prologue like `sub sp, sp, #N` before the
            // faulting store, each retry decrements SP again,
            // corrupting the frame layout. Blocking briefly waits for
            // other readers to release their per-block read locks,
            // which is bounded (~one JIT cycle).
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
    assert(old_ptr == NULL || old_ptr == ptr || type == MEM_WRITE_PTRACE);
#endif
    return ptr;
}

static void *mem_mmu_translate(struct mmu *mmu, addr_t addr, int type) {
    // Use mem_ptr instead of mem_ptr_nofault to properly handle:
    // 1. Copy-on-write (COW) pages - need to copy before write
    // 2. Growing stack pages (P_GROWSDOWN)
    return mem_ptr(container_of(mmu, struct mem, mmu), addr, type);
}

static void *mem_mmu_translate_write_nofault(struct mmu *mmu, addr_t addr) {
    return mem_ptr_nofault(container_of(mmu, struct mem, mmu), addr, MEM_WRITE);
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
