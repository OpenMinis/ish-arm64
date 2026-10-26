#ifndef KERNEL_SBA_H
#define KERNEL_SBA_H

// Shadow-backed address space (SBA): the private memory of an address space
// below SBA_SPAN lives in one host reservation, guest page A at host address
// base + A, so native code reaches it with an add instead of the software
// TLB (asbestos/guest-arm64/jit.c: tlb_lookup()). Anonymous mappings are
// carved out of the shadow, private file mappings are host mappings of the
// file placed there (or copies, where offsets do not line up), pages that
// mprotect makes accessible move in, and fork gives the child a host
// copy-on-write copy of the parent's shadow.
//
// A byte map right below the window, one byte per guest page, says which
// pages native code may read (SBA_R) or write (SBA_W) that way: mapped from
// the shadow with those permissions, not copy-on-write, not holding compiled
// code. Everything else -- shared mappings, PROT_NONE, the vdso, addresses
// above the window -- takes the TLB path as before, whose misses also fill
// the map (sba_populate()); every page table change clears it first.
// On by default on macOS, ISH_SBA=0 turns it off.

#include <stdbool.h>
#include <stdint.h>
#include "emu/mmu.h"

#define SBA_BITS 35          // the window: guest addresses below 32GB
#define SBA_SPAN (1ull << SBA_BITS)
#define SBA_CHUNK_BITS 14    // host page: what sba_release() can give back
#define SBA_CHUNK (1ull << SBA_CHUNK_BITS)
#define SBA_MAP_BITS 12      // map granularity: a guest page
#define SBA_MAP_ENTRIES (SBA_SPAN >> SBA_MAP_BITS)   // the map sits right below the window
#define SBA_R 1
#define SBA_W 2

struct mem;

// One per address space, referenced by the data objects that live in it,
// which can outlive the address space for a moment (a racing munmap).
struct sba_region {
    char *base;
    _Atomic int refs;
};

bool sba_on(void);
void sba_init(struct mem *mem);
void sba_destroy(struct mem *mem);
// Zeroed shadow memory for [start, start + pages), or NULL if the range is
// not eligible. *region gets a reference for the data object.
void *sba_anon(struct mem *mem, page_t start, pages_t pages, struct sba_region **region);
void sba_region_put(struct sba_region *r);
// [start, start + pages) was unmapped: give back the host pages of the
// shadow that hold no mapped page any more. mem write-locked.
void sba_release(struct mem *mem, page_t start, pages_t pages);
// Native code must not use the window for these pages any more.
void sba_invalidate(struct mem *mem, page_t start, pages_t pages);
// fork: the child gets its own copy of each shadow page (host copy-on-write,
// mach_vm_copy) instead of sharing it copy-on-write in the page table, so
// neither side loses the window. sba_fork_page() hands out the child's data
// object for a page of src_data (NULL: not shadow memory, share it as
// before); sba_fork_finish() copies what was handed out.
struct sba_fork;
struct data;
struct sba_fork *sba_fork_begin(struct mem *src, struct mem *dst);
struct data *sba_fork_page(struct sba_fork *f, struct data *src_data, page_t page);
void sba_fork_finish(struct sba_fork *f);
void sba_stats(void);
// A hole of `pages` in the window above 4GB (BAD_PAGE if none): where large
// reservations and NORESERVE mappings hinted at far addresses go (V8's heap
// pages), instead of anywhere in the 48-bit space. mem write-locked.
page_t sba_find_hole(struct mem *mem, pages_t pages);

#endif
