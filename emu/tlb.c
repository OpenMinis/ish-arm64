#include "emu/cpu.h"
#include "emu/tlb.h"
#include "kernel/task.h"
#include "kernel/memory.h"
#include "kernel/fs.h"
#include "util/sync.h"

void tlb_refresh(struct tlb *tlb, struct mmu *mmu) {
    if (tlb->mmu == mmu && tlb->mem_changes == mmu->changes)
        return;
    if (tlb->mmu != mmu) {
        // Address space changed (execve); block cache and ret_cache are invalid
        tlb_reset_cache(tlb);
    }
    tlb->mmu = mmu;
    tlb->dirty_page = TLB_PAGE_EMPTY;
    tlb->mem_changes = mmu->changes;
    tlb_flush(tlb);
}

void tlb_reset_cache(struct tlb *tlb) {
    memset(tlb->block_cache, 0, sizeof(tlb->block_cache));
    tlb->block_cache_gen = 0;
    if (tlb->frame != NULL) {
        free(tlb->frame);
        tlb->frame = NULL;
    }
}

void tlb_flush(struct tlb *tlb) {
    tlb->mem_changes = tlb->mmu->changes;
    for (unsigned i = 0; i < TLB_SIZE; i++)
        tlb->entries[i] = (struct tlb_entry) {.page = 1, .page_if_writable = 1};
}

void tlb_free(struct tlb *tlb) {
    if (tlb->frame != NULL)
        free(tlb->frame);
    free(tlb);
}

bool __tlb_read_cross_page(struct tlb *tlb, addr_t addr, char *value, unsigned size) {
    char *ptr1 = __tlb_read_ptr(tlb, addr);
    if (ptr1 == NULL)
        return false;
    char *ptr2 = __tlb_read_ptr(tlb, (PAGE(addr) + 1) << PAGE_BITS);
    if (ptr2 == NULL)
        return false;
    size_t part1 = PAGE_SIZE - PGOFFSET(addr);
    assert(part1 < size);
    memcpy(value, ptr1, part1);
    memcpy(value + part1, ptr2, size - part1);
    return true;
}

bool __tlb_write_cross_page(struct tlb *tlb, addr_t addr, const char *value, unsigned size) {
    char *ptr1 = __tlb_write_ptr(tlb, addr);
    if (ptr1 == NULL)
        return false;
    char *ptr2 = __tlb_write_ptr(tlb, (PAGE(addr) + 1) << PAGE_BITS);
    if (ptr2 == NULL)
        return false;
    size_t part1 = PAGE_SIZE - PGOFFSET(addr);
    assert(part1 < size);
    memcpy(ptr1, value, part1);
    memcpy(ptr2, value + part1, size - part1);
    return true;
}

__no_instrument void *tlb_handle_miss(struct tlb *tlb, addr_t addr, int type) {
    char *ptr = mmu_translate(tlb->mmu, TLB_PAGE(addr), type);
    if (tlb->mmu->changes != tlb->mem_changes) {
        tlb_flush(tlb);
        // Re-translate after flush. The ptr we got may be stale if another
        // thread did mmap/munmap concurrently. When a multi-page data object
        // is partially unmapped, the old host memory stays readable (refcount
        // > 0 means no PROT_NONE), so a stale ptr silently reads wrong data.
        // Re-translating ensures we get a pointer to the CURRENT mapping.
        ptr = mmu_translate(tlb->mmu, TLB_PAGE(addr), type);
    }
    if (ptr == NULL) {
        tlb->segfault_addr = addr;
        return NULL;
    }

    // Snapshot changes BEFORE populating entry. If another thread modifies
    // the page table between here and the next mem_changes check, the
    // mismatch will be detected and the TLB will be flushed.
    tlb->mem_changes = __atomic_load_n(&tlb->mmu->changes, __ATOMIC_ACQUIRE);

    tlb->dirty_page = TLB_PAGE(addr);

    struct tlb_entry *tlb_ent = &tlb->entries[TLB_INDEX(addr)];
    tlb_ent->page = TLB_PAGE(addr);
    tlb_ent->data_minus_addr = (uintptr_t) ptr - TLB_PAGE(addr);

    if (type == MEM_WRITE) {
        tlb_ent->page_if_writable = TLB_PAGE(addr);
    } else {
        // On read miss, speculatively check if the page is also writable.
        if (tlb->mmu->ops->translate_write_nofault) {
            char *wptr = tlb->mmu->ops->translate_write_nofault(tlb->mmu, TLB_PAGE(addr));
            tlb_ent->page_if_writable = wptr ? TLB_PAGE(addr) : TLB_PAGE_EMPTY;
        } else {
            tlb_ent->page_if_writable = TLB_PAGE_EMPTY;
        }
    }

    return (void *) (tlb_ent->data_minus_addr + addr);
}

#if defined(GUEST_ARM64)
/*
 * C-based memory access functions for ARM64 guest gadgets.
 * These provide reliable memory access by using the proven C-based TLB code.
 *
 * Return value convention for loads:
 * - Returns the loaded value through the 'out' pointer
 * - Returns 0 on success, -1 on segfault (tlb->segfault_addr is set)
 *
 * Return value convention for stores:
 * - Returns 0 on success, -1 on segfault (tlb->segfault_addr is set)
 */



// Load functions - return 0 on success, -1 on segfault
__no_instrument int c_load64(struct tlb *tlb, addr_t addr, uint64_t *out) {
    // Handle unaligned access by reading byte-by-byte
    if ((addr & 7) != 0) {
        // Unaligned - read each byte separately (little-endian)
        uint64_t result = 0;
        for (int i = 0; i < 8; i++) {
            void *ptr = __tlb_read_ptr(tlb, addr + i);
            if (ptr == NULL) {
                return -1;
            }
            result |= ((uint64_t)*(uint8_t *)ptr) << (i * 8);
        }
        *out = result;
        return 0;
    }

    void *ptr = __tlb_read_ptr(tlb, addr);
    if (ptr == NULL) {
        return -1;  // segfault_addr already set by tlb_handle_miss
    }
    *out = *(uint64_t *)ptr;
    return 0;
}

__no_instrument int c_load32(struct tlb *tlb, addr_t addr, uint32_t *out) {
    void *ptr = __tlb_read_ptr(tlb, addr);
    if (ptr == NULL) {
        return -1;
    }
    *out = *(uint32_t *)ptr;
    return 0;
}

__no_instrument int c_load16(struct tlb *tlb, addr_t addr, uint16_t *out) {
    void *ptr = __tlb_read_ptr(tlb, addr);
    if (ptr == NULL) {
        return -1;
    }
    *out = *(uint16_t *)ptr;
    return 0;
}

__no_instrument int c_load8(struct tlb *tlb, addr_t addr, uint8_t *out) {
    void *ptr = __tlb_read_ptr(tlb, addr);
    if (ptr == NULL) {
        return -1;
    }
    *out = *(uint8_t *)ptr;
    return 0;
}

// Sign-extending load functions
__no_instrument int c_load32_sx(struct tlb *tlb, addr_t addr, int64_t *out) {
    void *ptr = __tlb_read_ptr(tlb, addr);
    if (ptr == NULL) {
        return -1;
    }
    *out = (int64_t)(int32_t)*(uint32_t *)ptr;
    return 0;
}

__no_instrument int c_load16_sx64(struct tlb *tlb, addr_t addr, int64_t *out) {
    void *ptr = __tlb_read_ptr(tlb, addr);
    if (ptr == NULL) {
        return -1;
    }
    *out = (int64_t)(int16_t)*(uint16_t *)ptr;
    return 0;
}

__no_instrument int c_load16_sx32(struct tlb *tlb, addr_t addr, int32_t *out) {
    void *ptr = __tlb_read_ptr(tlb, addr);
    if (ptr == NULL) {
        return -1;
    }
    *out = (int32_t)(int16_t)*(uint16_t *)ptr;
    return 0;
}

__no_instrument int c_load8_sx64(struct tlb *tlb, addr_t addr, int64_t *out) {
    void *ptr = __tlb_read_ptr(tlb, addr);
    if (ptr == NULL) {
        return -1;
    }
    *out = (int64_t)(int8_t)*(uint8_t *)ptr;
    return 0;
}

__no_instrument int c_load8_sx32(struct tlb *tlb, addr_t addr, int32_t *out) {
    void *ptr = __tlb_read_ptr(tlb, addr);
    if (ptr == NULL) {
        return -1;
    }
    *out = (int32_t)(int8_t)*(uint8_t *)ptr;
    return 0;
}

// Store functions - return 0 on success, -1 on segfault
__no_instrument int c_store64(struct tlb *tlb, addr_t addr, uint64_t value) {
    // Handle unaligned access by writing byte-by-byte
    if ((addr & 7) != 0) {
        // Unaligned - write each byte separately (little-endian)
        for (int i = 0; i < 8; i++) {
            void *ptr = __tlb_write_ptr(tlb, addr + i);
            if (ptr == NULL) {
                return -1;
            }
            *(uint8_t *)ptr = (value >> (i * 8)) & 0xFF;
        }
        return 0;
    }

    void *ptr = __tlb_write_ptr(tlb, addr);
    if (ptr == NULL) {
        return -1;  // segfault_addr already set
    }
    *(uint64_t *)ptr = value;
    return 0;
}

__no_instrument int c_store32(struct tlb *tlb, addr_t addr, uint32_t value) {
    void *ptr = __tlb_write_ptr(tlb, addr);
    if (ptr == NULL) {
        return -1;
    }
    *(uint32_t *)ptr = value;
    return 0;
}

__no_instrument int c_store16(struct tlb *tlb, addr_t addr, uint16_t value) {
    void *ptr = __tlb_write_ptr(tlb, addr);
    if (ptr == NULL) {
        return -1;
    }
    *(uint16_t *)ptr = value;
    return 0;
}

__no_instrument int c_store8(struct tlb *tlb, addr_t addr, uint8_t value) {
    void *ptr = __tlb_write_ptr(tlb, addr);
    if (ptr == NULL) {
        return -1;
    }
    *(uint8_t *)ptr = value;
    return 0;
}

// Atomic memory operations (LSE): LDADD/LDCLR/LDEOR/LDSET/LDSMAX/LDSMIN/LDUMAX/LDUMIN/SWP
// Return: 0 on success, -1 on segfault or unsupported op
// Helper macros for atomic RMW with CAS loop (for min/max that lack atomic builtins)
#define ATOMIC_RMW_CAS_LOOP(type, ptr, val, op_expr) do { \
    type old = __atomic_load_n((type *)(ptr), __ATOMIC_SEQ_CST); \
    type newv; \
    do { \
        newv = (op_expr); \
    } while (!__atomic_compare_exchange_n((type *)(ptr), &old, newv, \
             true, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)); \
    *old_out = old; \
} while(0)

__no_instrument int c_atomic_rmw(struct tlb *tlb, addr_t addr, uint64_t value,
                                 uint32_t size, uint32_t op, uint64_t *old_out) {
    if (old_out == NULL)
        return -1;

    void *ptr = __tlb_write_ptr(tlb, addr);
    if (ptr == NULL)
        return -1;

    switch (size) {
        case 0: { // 8-bit
            uint8_t val = (uint8_t)value;
            switch (op) {
                case 0: *old_out = __atomic_fetch_add((uint8_t *)ptr, val, __ATOMIC_SEQ_CST); return 0;
                case 1: *old_out = __atomic_fetch_and((uint8_t *)ptr, ~val, __ATOMIC_SEQ_CST); return 0;
                case 2: *old_out = __atomic_fetch_xor((uint8_t *)ptr, val, __ATOMIC_SEQ_CST); return 0;
                case 3: *old_out = __atomic_fetch_or((uint8_t *)ptr, val, __ATOMIC_SEQ_CST); return 0;
                case 4: ATOMIC_RMW_CAS_LOOP(uint8_t, ptr, val, ((int8_t)old > (int8_t)val) ? old : val); return 0;
                case 5: ATOMIC_RMW_CAS_LOOP(uint8_t, ptr, val, ((int8_t)old < (int8_t)val) ? old : val); return 0;
                case 6: ATOMIC_RMW_CAS_LOOP(uint8_t, ptr, val, (old > val) ? old : val); return 0;
                case 7: ATOMIC_RMW_CAS_LOOP(uint8_t, ptr, val, (old < val) ? old : val); return 0;
                case 8: *old_out = __atomic_exchange_n((uint8_t *)ptr, val, __ATOMIC_SEQ_CST); return 0;
                default: return -1;
            }
        }
        case 1: { // 16-bit
            uint16_t val = (uint16_t)value;
            switch (op) {
                case 0: *old_out = __atomic_fetch_add((uint16_t *)ptr, val, __ATOMIC_SEQ_CST); return 0;
                case 1: *old_out = __atomic_fetch_and((uint16_t *)ptr, ~val, __ATOMIC_SEQ_CST); return 0;
                case 2: *old_out = __atomic_fetch_xor((uint16_t *)ptr, val, __ATOMIC_SEQ_CST); return 0;
                case 3: *old_out = __atomic_fetch_or((uint16_t *)ptr, val, __ATOMIC_SEQ_CST); return 0;
                case 4: ATOMIC_RMW_CAS_LOOP(uint16_t, ptr, val, ((int16_t)old > (int16_t)val) ? old : val); return 0;
                case 5: ATOMIC_RMW_CAS_LOOP(uint16_t, ptr, val, ((int16_t)old < (int16_t)val) ? old : val); return 0;
                case 6: ATOMIC_RMW_CAS_LOOP(uint16_t, ptr, val, (old > val) ? old : val); return 0;
                case 7: ATOMIC_RMW_CAS_LOOP(uint16_t, ptr, val, (old < val) ? old : val); return 0;
                case 8: *old_out = __atomic_exchange_n((uint16_t *)ptr, val, __ATOMIC_SEQ_CST); return 0;
                default: return -1;
            }
        }
        case 2: { // 32-bit
            uint32_t val = (uint32_t)value;
            switch (op) {
                case 0: *old_out = __atomic_fetch_add((uint32_t *)ptr, val, __ATOMIC_SEQ_CST); return 0;
                case 1: *old_out = __atomic_fetch_and((uint32_t *)ptr, ~val, __ATOMIC_SEQ_CST); return 0;
                case 2: *old_out = __atomic_fetch_xor((uint32_t *)ptr, val, __ATOMIC_SEQ_CST); return 0;
                case 3: *old_out = __atomic_fetch_or((uint32_t *)ptr, val, __ATOMIC_SEQ_CST); return 0;
                case 4: ATOMIC_RMW_CAS_LOOP(uint32_t, ptr, val, ((int32_t)old > (int32_t)val) ? old : val); return 0;
                case 5: ATOMIC_RMW_CAS_LOOP(uint32_t, ptr, val, ((int32_t)old < (int32_t)val) ? old : val); return 0;
                case 6: ATOMIC_RMW_CAS_LOOP(uint32_t, ptr, val, (old > val) ? old : val); return 0;
                case 7: ATOMIC_RMW_CAS_LOOP(uint32_t, ptr, val, (old < val) ? old : val); return 0;
                case 8: *old_out = __atomic_exchange_n((uint32_t *)ptr, val, __ATOMIC_SEQ_CST); return 0;
                default: return -1;
            }
        }
        case 3: { // 64-bit
            uint64_t val = value;
            switch (op) {
                case 0: *old_out = __atomic_fetch_add((uint64_t *)ptr, val, __ATOMIC_SEQ_CST); return 0;
                case 1: *old_out = __atomic_fetch_and((uint64_t *)ptr, ~val, __ATOMIC_SEQ_CST); return 0;
                case 2: *old_out = __atomic_fetch_xor((uint64_t *)ptr, val, __ATOMIC_SEQ_CST); return 0;
                case 3: *old_out = __atomic_fetch_or((uint64_t *)ptr, val, __ATOMIC_SEQ_CST); return 0;
                case 4: ATOMIC_RMW_CAS_LOOP(uint64_t, ptr, val, ((int64_t)old > (int64_t)val) ? old : val); return 0;
                case 5: ATOMIC_RMW_CAS_LOOP(uint64_t, ptr, val, ((int64_t)old < (int64_t)val) ? old : val); return 0;
                case 6: ATOMIC_RMW_CAS_LOOP(uint64_t, ptr, val, (old > val) ? old : val); return 0;
                case 7: ATOMIC_RMW_CAS_LOOP(uint64_t, ptr, val, (old < val) ? old : val); return 0;
                case 8: *old_out = __atomic_exchange_n((uint64_t *)ptr, val, __ATOMIC_SEQ_CST); return 0;
                default: return -1;
            }
        }
        default:
            return -1;
    }
}

#undef ATOMIC_RMW_CAS_LOOP

// Atomic compare-and-swap (CAS/CASA/CASL/CASAL)
// Uses host atomic CAS for thread safety.
// Return: 0 on success, -1 on segfault or unsupported size
__no_instrument int c_atomic_cas(struct tlb *tlb, addr_t addr, uint64_t expected,
                                 uint64_t desired, uint32_t size, uint64_t *old_out) {
    if (old_out == NULL)
        return -1;

    // Get writable host pointer (CAS needs both read and write)
    void *ptr = __tlb_write_ptr(tlb, addr);
    if (ptr == NULL)
        return -1;

    switch (size) {
        case 0: { // 8-bit
            uint8_t exp = (uint8_t)expected;
            __atomic_compare_exchange_n((uint8_t *)ptr, &exp, (uint8_t)desired,
                                       false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
            *old_out = exp;  // exp is updated to actual value on failure
            return 0;
        }
        case 1: { // 16-bit
            uint16_t exp = (uint16_t)expected;
            __atomic_compare_exchange_n((uint16_t *)ptr, &exp, (uint16_t)desired,
                                       false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
            *old_out = exp;
            return 0;
        }
        case 2: { // 32-bit
            uint32_t exp = (uint32_t)expected;
            __atomic_compare_exchange_n((uint32_t *)ptr, &exp, (uint32_t)desired,
                                       false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
            *old_out = exp;
            return 0;
        }
        case 3: { // 64-bit
            uint64_t exp = expected;
            __atomic_compare_exchange_n((uint64_t *)ptr, &exp, desired,
                                       false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
            *old_out = exp;
            return 0;
        }
        default:
            return -1;
    }
}

// STXR atomic compare-and-swap helper for LDXR/STXR emulation.
// Compares memory at addr with expected_val, if equal stores new_val.
// Returns 0 on success (CAS succeeded), 1 on failure (CAS lost race),
// or -1 on segfault.
//
// To prevent CoW from invalidating the host pointer between getting it
// and the CAS, we snapshot mmu->changes before and after. If it changed,
// another thread did CoW/mmap, so we retry with a fresh TLB lookup.
__no_instrument int c_stxr_cas(struct tlb *tlb, addr_t addr,
                               uint64_t expected_val, uint64_t new_val,
                               uint32_t size) {
    void *ptr = __tlb_write_ptr(tlb, addr);
    if (ptr == NULL)
        return -1;

    switch (size) {
        case 0: { // 8-bit
            uint8_t exp = (uint8_t)expected_val;
            uint8_t des = (uint8_t)new_val;
            return __atomic_compare_exchange_n((uint8_t *)ptr, &exp, des,
                                              false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST) ? 0 : 1;
        }
        case 1: { // 16-bit
            uint16_t exp = (uint16_t)expected_val;
            uint16_t des = (uint16_t)new_val;
            return __atomic_compare_exchange_n((uint16_t *)ptr, &exp, des,
                                              false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST) ? 0 : 1;
        }
        case 2: { // 32-bit
            uint32_t exp = (uint32_t)expected_val;
            uint32_t des = (uint32_t)new_val;
            return __atomic_compare_exchange_n((uint32_t *)ptr, &exp, des,
                                              false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST) ? 0 : 1;
        }
        case 3: { // 64-bit
            uint64_t exp = expected_val;
            uint64_t des = new_val;
            return __atomic_compare_exchange_n((uint64_t *)ptr, &exp, des,
                                              false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST) ? 0 : 1;
        }
        default:
            return -1;
    }
}

// LDP/STP helper functions for pair loads/stores
// Return: 0 on success, -1 on segfault
__no_instrument int c_ldp64(struct tlb *tlb, addr_t addr, uint64_t *val1, uint64_t *val2) {
    void *ptr1 = __tlb_read_ptr(tlb, addr);
    if (ptr1 == NULL) {
        return -1;
    }
    void *ptr2 = __tlb_read_ptr(tlb, addr + 8);
    if (ptr2 == NULL) {
        return -1;
    }
    *val1 = *(uint64_t *)ptr1;
    *val2 = *(uint64_t *)ptr2;
    (void)0;
    return 0;
}

__no_instrument int c_ldp32(struct tlb *tlb, addr_t addr, uint32_t *val1, uint32_t *val2) {
    void *ptr1 = __tlb_read_ptr(tlb, addr);
    if (ptr1 == NULL) {
        return -1;
    }
    void *ptr2 = __tlb_read_ptr(tlb, addr + 4);
    if (ptr2 == NULL) {
        return -1;
    }
    *val1 = *(uint32_t *)ptr1;
    *val2 = *(uint32_t *)ptr2;
    return 0;
}

__no_instrument int c_stp64(struct tlb *tlb, addr_t addr, uint64_t val1, uint64_t val2) {
    void *ptr1 = __tlb_write_ptr(tlb, addr);
    if (ptr1 == NULL) {
        return -1;
    }
    void *ptr2 = __tlb_write_ptr(tlb, addr + 8);
    if (ptr2 == NULL) {
        return -1;
    }
    *(uint64_t *)ptr1 = val1;
    *(uint64_t *)ptr2 = val2;
    return 0;
}

__no_instrument int c_stp32(struct tlb *tlb, addr_t addr, uint32_t val1, uint32_t val2) {
    void *ptr1 = __tlb_write_ptr(tlb, addr);
    if (ptr1 == NULL) {
        return -1;
    }
    void *ptr2 = __tlb_write_ptr(tlb, addr + 4);
    if (ptr2 == NULL) {
        return -1;
    }
    *(uint32_t *)ptr1 = val1;
    *(uint32_t *)ptr2 = val2;
    return 0;
}

// Interleaved SIMD load (LD2/LD3/LD4)
// Reads interleaved elements from memory and deinterleaves into consecutive Vt registers.
// num_regs: 2 (LD2), 3 (LD3), or 4 (LD4)
// elem_size: 0=byte, 1=halfword, 2=word, 3=doubleword
// Q: 0=64-bit (lower half), 1=128-bit (full register)
// rt: first destination register (Vt, Vt+1, ..., Vt+num_regs-1 mod 32)
__no_instrument int c_simd_load_interleaved(struct cpu_state *cpu, struct tlb *tlb,
                                             addr_t addr, uint32_t rt, uint32_t num_regs,
                                             uint32_t elem_size, uint32_t Q) {
    unsigned elem_bytes = 1u << elem_size;
    unsigned elems_per_reg = (Q ? 16 : 8) / elem_bytes;
    unsigned total_bytes = num_regs * elems_per_reg * elem_bytes;
    if (total_bytes > 64 || num_regs > 4 || elem_size > 3) return -1;

    // Read all raw bytes from guest memory (bulk read via host pointer)
    uint8_t buf[64];
    if (PGOFFSET(addr) + total_bytes <= PAGE_SIZE) {
        // Fast path: entire access fits within one page
        char *ptr = __tlb_read_ptr(tlb, addr);
        if (ptr == NULL) return -1;
        memcpy(buf, ptr, total_bytes);
    } else {
        // Slow path: crosses page boundary
        if (!__tlb_read_cross_page(tlb, addr, (char *)buf, total_bytes))
            return -1;
    }

    // Deinterleave: memory has [elem0_reg0, elem0_reg1, ..., elem0_regN-1, elem1_reg0, ...]
    for (unsigned r = 0; r < num_regs; r++) {
        uint32_t vt = (rt + r) & 0x1f;
        cpu->fp[vt].q = 0;
        for (unsigned e = 0; e < elems_per_reg; e++) {
            unsigned src_offset = (e * num_regs + r) * elem_bytes;
            unsigned dst_offset = e * elem_bytes;
            memcpy(&cpu->fp[vt].b[dst_offset], &buf[src_offset], elem_bytes);
        }
    }
    return 0;
}

// Interleaved SIMD store (ST2/ST3/ST4)
// Reads consecutive Vt registers and writes interleaved elements to memory.
__no_instrument int c_simd_store_interleaved(struct cpu_state *cpu, struct tlb *tlb,
                                              addr_t addr, uint32_t rt, uint32_t num_regs,
                                              uint32_t elem_size, uint32_t Q) {
    unsigned elem_bytes = 1u << elem_size;
    unsigned elems_per_reg = (Q ? 16 : 8) / elem_bytes;
    unsigned total_bytes = num_regs * elems_per_reg * elem_bytes;
    if (total_bytes > 64 || num_regs > 4 || elem_size > 3) return -1;

    // Interleave register data into buffer
    uint8_t buf[64];
    for (unsigned r = 0; r < num_regs; r++) {
        uint32_t vt = (rt + r) & 0x1f;
        for (unsigned e = 0; e < elems_per_reg; e++) {
            unsigned dst_offset = (e * num_regs + r) * elem_bytes;
            unsigned src_offset = e * elem_bytes;
            memcpy(&buf[dst_offset], &cpu->fp[vt].b[src_offset], elem_bytes);
        }
    }

    // Write all bytes to guest memory (bulk write via host pointer)
    if (PGOFFSET(addr) + total_bytes <= PAGE_SIZE) {
        char *ptr = __tlb_write_ptr(tlb, addr);
        if (ptr == NULL) return -1;
        memcpy(ptr, buf, total_bytes);
    } else {
        if (!__tlb_write_cross_page(tlb, addr, (char *)buf, total_bytes))
            return -1;
    }
    return 0;
}
#endif
