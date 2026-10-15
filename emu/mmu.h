#ifndef EMU_CPU_MEM_H
#define EMU_CPU_MEM_H

#include "misc.h"
#ifdef GUEST_ARM64
#include "util/list.h"
#include "util/sync.h"
#endif

// Page number type: bits of address above PAGE_BITS
#ifdef GUEST_ARM64
// ARM64: 48-bit address space → 36-bit page numbers
typedef uint64_t page_t;
typedef uint64_t pages_t;
#define BAD_PAGE 0x1000000000ULL  // Invalid page marker (beyond 36-bit page range)
#define MEM_PAGES BAD_PAGE        // Upper bound for iteration (not allocated as array)
#else
// x86: 32-bit address space → 20-bit page numbers
typedef dword_t page_t;
typedef dword_t pages_t;
#define BAD_PAGE 0x100000       // Invalid page marker (beyond 20-bit page range)
#define MEM_PAGES (1 << 20)     // 4GB address space (page numbers 0 to 0xFFFFF)
#endif

#ifndef __KERNEL__
#define PAGE_BITS 12
#undef PAGE_SIZE // defined in system headers somewhere
#define PAGE_SIZE (1 << PAGE_BITS)
#define PAGE(addr) ((addr) >> PAGE_BITS)
#define PGOFFSET(addr) ((addr) & (PAGE_SIZE - 1))
// bytes MUST be unsigned if you would like this to overflow to zero
#define PAGE_ROUND_UP(bytes) (PAGE((bytes) + PAGE_SIZE - 1))
#endif

struct mmu {
    struct mmu_ops *ops;
    struct asbestos *asbestos;
    uint64_t changes;
#ifdef GUEST_ARM64
    // The TLBs of the threads running guest code in this address space
    // (tlb_attach()); a change to the mappings marks them stale.
    lock_t tlbs_lock;
    struct list tlbs;
#endif
};

#define MEM_READ 0
#define MEM_WRITE 1
#define MEM_WRITE_PTRACE 2

struct mmu_ops {
    // type is MEM_READ or MEM_WRITE
    void *(*translate)(struct mmu *mmu, addr_t addr, int type);
    // Like translate(MEM_WRITE) but returns NULL instead of triggering CoW/GROWSDOWN.
    // Used by TLB to speculatively populate write permission on read miss.
    void *(*translate_write_nofault)(struct mmu *mmu, addr_t addr);
};

static inline void *mmu_translate(struct mmu *mmu, addr_t addr, int type) {
    return mmu->ops->translate(mmu, addr, type);
}

#endif