#ifndef TLB_H
#define TLB_H

#include <stdatomic.h>
#include <string.h>
#include "emu/mmu.h"
#include "debug.h"

struct tlb_entry {
    page_t page;
    page_t page_if_writable;
    uintptr_t data_minus_addr;
#ifdef GUEST_64BIT
    // Coherence generation: snapshot of mmu->changes when this entry was
    // populated. Used to detect a stale entry whose page was remapped
    // (CoW/mmap/munmap) by another thread while this thread was mid-block.
    // Occupies the former 32-byte padding slot (JIT indexes entries by lsl #5).
    uintptr_t gen;
#endif
};
#define TLB_BITS 13  // 8192 entries
#define TLB_SIZE (1 << TLB_BITS)
struct fiber_block;
struct fiber_frame;

struct tlb {
    struct mmu *mmu;
    page_t dirty_page;
    unsigned mem_changes;
#ifdef GUEST_64BIT
    // TLB_STALE once another thread may have changed the mappings since this
    // TLB was last checked against mmu->changes, else 0. The inline lookups OR
    // it into the page they compare, so a stale TLB misses on every page, and
    // the miss path clears it and flushes. Every valid entry was filled at
    // mem_changes (a fill flushes first when it moved), so this one bit stands
    // for all their generations.
    uint64_t stale;
#endif
    // this is basically one of the return values of tlb_handle_miss, tlb_{read,write}, and __tlb_{read,write}_cross_page
    // yes, this sucks
    addr_t segfault_addr;

    struct tlb_entry entries[TLB_SIZE];

    // Persistent block cache across syscalls (avoids re-lookup after every interrupt)
    // Size defined by FIBER_CACHE_SIZE in asbestos.h
    struct fiber_block *block_cache[1 << 12];  // must match FIBER_CACHE_SIZE
    unsigned block_cache_gen; // tracks asbestos->invalidate_gen for invalidation

    // Persistent fiber_frame (avoids malloc/free + ret_cache zeroing per syscall)
    struct fiber_frame *frame;

#ifdef GUEST_64BIT
    struct mmu *attached;      // whose tlbs list this is on, while running guest code
    struct list tlbs_link;
#endif
#ifdef GUEST_X86_64
    // The x86_64 translator's memory access sites (modulo the size): the
    // page each one found last time in this thread and its data_minus_addr,
    // so that an access to the same page again needs no lookup. Per thread:
    // threads touch different pages from the same code (their stacks).
    // Valid as long as the TLB's entries are: tlb_flush() empties them.
    // ISH_XLAT_SITESTATS: flushes, C misses, site checks, lookups after a site missed
    uint64_t xs_flush, xs_miss, xs_check, xs_lookup, xs_check_sp, xs_member;
    bool xsite_inited;   // every entry was emptied once; later flushes empty the used ones
    // An entry was filled since the last flush. Sites learn pages only from
    // entries, so without one there is nothing to empty: a thread in a
    // hypervisor VM barely uses its TLB, and mappings change all the time.
    bool filled;
    struct { uint64_t page, data_minus_addr; } xsite[1 << 13];
#endif
};

// Hashed: the TLB is direct-mapped, and pages 32 MB apart (the stack and a
// binary's data, say) would otherwise evict each other on every access. The
// x86_64 guest's gadgets (read_prep) and translated code (xlat.c
// e_tlb_index) compute the same.
#define TLB_INDEX(addr) ((((addr >> PAGE_BITS) ^ (addr >> (PAGE_BITS + TLB_BITS))) & (TLB_SIZE - 1)))
#ifdef GUEST_64BIT
#define TLB_PAGE(addr) ((addr) & 0xfffffffffffff000ULL)
#else
#define TLB_PAGE(addr) ((addr) & 0xfffff000)
#endif
#ifdef GUEST_64BIT
// Not page aligned (no equality check matches it) and above any 48-bit
// address, so the JIT's `addr - page` range check never matches it either.
#define TLB_PAGE_EMPTY 0x8000000000000001ull
#else
#define TLB_PAGE_EMPTY 1
#endif
void tlb_refresh(struct tlb *tlb, struct mmu *mmu);
#ifdef GUEST_64BIT
#define TLB_STALE (1ull << 48)   // above every 48-bit page: no entry matches
// Around running guest code (the thread holds the mm and its lock): an
// attached TLB is marked stale by mmu_tlbs_stale() when the mappings change.
void tlb_attach(struct tlb *tlb, struct mmu *mmu);
void tlb_detach(struct tlb *tlb);
void mmu_tlbs_stale(struct mmu *mmu);
#endif
void tlb_free(struct tlb *tlb);
#ifdef GUEST_X86_64
void tlb_stats_exit(void);
extern void (*tlb_stats_hook)(void);
// how many sites the translator handed out (they wrap at the table's size)
extern _Atomic(unsigned) tlb_xsites;
#endif
void tlb_flush(struct tlb *tlb);
void *tlb_handle_miss(struct tlb *tlb, addr_t addr, int type);

forceinline __no_instrument void *__tlb_read_ptr(struct tlb *tlb, addr_t addr) {
    struct tlb_entry entry = tlb->entries[TLB_INDEX(addr)];
    if (entry.page == TLB_PAGE(addr)
#ifdef GUEST_64BIT
        // Coherence: reject a hit whose page was remapped by another thread
        // since this entry was cached (CoW/mmap advanced mmu->changes). Falling
        // through to tlb_handle_miss re-translates to the current backing.
        && entry.gen == (uintptr_t) __atomic_load_n(&tlb->mmu->changes, __ATOMIC_ACQUIRE)
#endif
    ) {
        void *address = (void *) (entry.data_minus_addr + addr);
        return address;
    }
    return tlb_handle_miss(tlb, addr, MEM_READ);
}
bool __tlb_read_cross_page(struct tlb *tlb, addr_t addr, char *out, unsigned size);
forceinline __no_instrument bool tlb_read(struct tlb *tlb, addr_t addr, void *out, unsigned size) {
    if (PGOFFSET(addr) > PAGE_SIZE - size)
        return __tlb_read_cross_page(tlb, addr, out, size);
    void *ptr = __tlb_read_ptr(tlb, addr);
    if (ptr == NULL)
        return false;
    memcpy(out, ptr, size);
    return true;
}

// C-level write watchpoint: detect stores that bypass assembly write_prep.
// DIAGNOSTIC, OFF BY DEFAULT — when enabled it adds a volatile-global load +
// branch to __tlb_write_ptr, the hottest path in the emulator (every guest
// store). Leaving it compiled in makes store-heavy programs (bun/JSC) pay that
// cost on every write, which is a real slowdown on-device. Uncomment only when
// actively debugging a stray-store bug (mirrors ENABLE_WRITE_WATCHPOINT, the
// assembly-side counterpart in gadgets.h, which is likewise off by default).
// #define ENABLE_C_WRITE_WATCHPOINT 1
extern volatile addr_t g_watch_page_val;
extern volatile addr_t g_watch_pages[2];
void c_watch_write_hit(addr_t addr, const char *caller);

forceinline __no_instrument void *__tlb_write_ptr(struct tlb *tlb, addr_t addr) {
#ifdef ENABLE_C_WRITE_WATCHPOINT
    if (g_watch_pages[0] && ((addr & ~0xfffULL) == g_watch_pages[0] ||
                             (addr & ~0xfffULL) == g_watch_pages[1])) {
        c_watch_write_hit(addr, __func__);
    }
#endif
    struct tlb_entry entry = tlb->entries[TLB_INDEX(addr)];
    if (entry.page_if_writable == TLB_PAGE(addr)
#ifdef GUEST_64BIT
        && entry.gen == (uintptr_t) __atomic_load_n(&tlb->mmu->changes, __ATOMIC_ACQUIRE)
#endif
    ) {
        tlb->dirty_page = TLB_PAGE(addr);
        void *address = (void *) (entry.data_minus_addr + addr);
        return address;
    }
    return tlb_handle_miss(tlb, addr, MEM_WRITE);
}
bool __tlb_write_cross_page(struct tlb *tlb, addr_t addr, const char *value, unsigned size);
forceinline __no_instrument bool tlb_write(struct tlb *tlb, addr_t addr, const void *value, unsigned size) {
    if (PGOFFSET(addr) > PAGE_SIZE - size)
        return __tlb_write_cross_page(tlb, addr, value, size);
    void *ptr = __tlb_write_ptr(tlb, addr);
    if (ptr == NULL)
        return false;
    memcpy(ptr, value, size);
    return true;
}

#endif
