// Native JIT for the ARM64 guest -- see jit.h. Built only with -Djit=true.

#if !defined(__APPLE__) || !defined(__aarch64__)
#error "the ARM64 JIT needs an Apple arm64 host (MAP_JIT)"
#endif

#include <TargetConditionals.h>
#include <dlfcn.h>
#include <fnmatch.h>
#include <libkern/OSCacheControl.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/ucontext.h>
#include "asbestos/guest-arm64/jit.h"
#include "asbestos/guest-arm64/aot.h"
#include "asbestos/asbestos.h"
#include "emu/cpu.h"
#include "emu/mmu.h"
#include "emu/tlb.h"
#include "kernel/fs.h"
#include "kernel/memory.h"
#include "cpu-offsets.h"

extern void jit_fiber_ret(void) __asm("fiber_ret");
extern void jit_fiber_ret_chain(void) __asm("fiber_ret_chain");

// ---------------------------------------------------------------- options

static bool jit_on;       // ISH_JIT=0 turns the translator off
static bool link_off;     // ISH_JIT_LINK=0: no direct links between blocks
static bool loop_off;     // ISH_JIT_LOOP=0: no register promotion in self-loops
static bool simd_on;      // ISH_JIT_SIMD=0: leave SIMD/FP instructions to the gadgets
static bool pin_on;       // ISH_JIT_PIN=0: no guest registers pinned in host registers
static bool pic_on;       // ISH_JIT_PIC=1: position-independent code (see "PIC" below)
static bool aot_only;     // ISH_JIT_AOT_ONLY=1: install AOT translations, translate nothing
                          // (what a build without the JIT would run)

static bool env_off(const char *name) {
    const char *v = getenv(name);
    return v && (!strcmp(v, "0") || !strcmp(v, "off"));
}

// ---------------------------------------------------------------- stats

static _Atomic uint64_t st_blocks, st_segments, st_units, st_bytes, st_ns;
static _Atomic uint64_t st_unsupported_insns, st_fail_regs;
static _Atomic uint64_t st_block_ends, st_block_end_fail, st_links, st_loops;
static _Atomic uint64_t st_shared, st_aot, st_aot_moved, st_aot_busy, st_aot_fixed;
static _Atomic uint64_t st_find_ticks;   // mach ticks spent in aot_lookup()

// ---------------------------------------------------------------- code region
//
// One region, bump-allocated and never freed; branches between native code
// and to the exit stub stay within +-128MB. Every write goes through
// install_code() / patch_insn(), so the code is only ever written through
// `region + rw_delta`:
// - MAP_JIT (macOS): rw_delta = 0 and the thread toggles the region between
//   writable and executable (pthread_jit_write_protect_np).
// - Dual mapping (iOS, or ISH_JIT_DUALMAP=1 on macOS): a second, writable
//   view of the same pages; the executable view is never writable.

#define REGION_SIZE (120u << 20)
static uint8_t *region;   // executable view
static intptr_t rw_delta; // writable view - executable view
static bool dual_map;
static _Atomic size_t region_used;

#ifndef ISH_JIT_NO_EMIT   // AOT-only builds never map executable memory
static bool map_dual(void) {
    void *rw = mmap(NULL, REGION_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (rw == MAP_FAILED)
        return false;
    vm_address_t rx = 0;
    vm_prot_t cur, max;
    kern_return_t kr = vm_remap(mach_task_self(), &rx, REGION_SIZE, 0, VM_FLAGS_ANYWHERE, mach_task_self(),
                                (vm_address_t) rw, false, &cur, &max, VM_INHERIT_NONE);
    if (kr != KERN_SUCCESS) {
        munmap(rw, REGION_SIZE);
        return false;
    }
    if (mprotect((void *) rx, REGION_SIZE, PROT_READ | PROT_EXEC) != 0) {
        munmap((void *) rx, REGION_SIZE);
        munmap(rw, REGION_SIZE);
        return false;
    }
    region = (uint8_t *) rx;
    rw_delta = (intptr_t) rw - (intptr_t) rx;
    dual_map = true;
    return true;
}

static bool map_jit(void) {
#if TARGET_OS_OSX
    // MAP_JIT memory is placed by the kernel (hints are ignored).
    void *p = mmap(NULL, REGION_SIZE, PROT_READ | PROT_WRITE | PROT_EXEC,
                   MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0);
    if (p == MAP_FAILED)
        return false;
    region = p;
    return true;
#else
    return false;
#endif
}
#endif

static uint8_t *region_alloc(size_t bytes) {
    size_t off = atomic_fetch_add(&region_used, (bytes + 15) & ~(size_t) 15);
    if (off + bytes > REGION_SIZE)
        return NULL;
    return region + off;
}

// MAP_JIT W^X is per thread and only this thread's fiber runs its native
// code, so a thread stays writable across the compiles and links of one
// run-loop pass and turns executable again in jit_exec_ready(), right before
// fiber_enter.
#if TARGET_OS_OSX
static __thread bool jit_writable;
#endif

static void jit_write_begin(void) {
#if TARGET_OS_OSX
    if (dual_map || jit_writable)
        return;
    pthread_jit_write_protect_np(0);
    jit_writable = true;
#endif
}

void jit_exec_ready(void) {
#if TARGET_OS_OSX
    if (!jit_writable)
        return;
    pthread_jit_write_protect_np(1);
    jit_writable = false;
#endif
}

static void install_code(uint8_t *dst, const uint32_t *src, size_t n) {
    jit_write_begin();
    memcpy(dst + rw_delta, src, n * 4);
    sys_icache_invalidate(dst, n * 4);
    atomic_fetch_add_explicit(&st_bytes, n * 4, memory_order_relaxed);
}

// Unlinks also happen while the thread is running guest code (a guest store
// into a code page invalidates its blocks from inside a gadget's C call), so
// a patch leaves the thread's W^X state as it found it.
// native_entry relative to the code: the hot entry, or in PIC code the
// cross-module entry just before it.
static uintptr_t entry_off(void);

static void patch_insn(uint32_t *site, uint32_t insn) {
#if TARGET_OS_OSX
    bool was_writable = jit_writable;
#endif
    jit_write_begin();
    __atomic_store_n((uint32_t *) ((uint8_t *) site + rw_delta), insn, __ATOMIC_RELEASE);
    sys_icache_invalidate(site, 4);
#if TARGET_OS_OSX
    if (!was_writable)
        jit_exec_ready();
#endif
}

// ---------------------------------------------------------------- encoders

static inline int64_t sext(uint64_t v, int bits) { return (int64_t) (v << (64 - bits)) >> (64 - bits); }
static inline bool fits(int64_t v, int bits) { return v >= -(1LL << (bits - 1)) && v < (1LL << (bits - 1)); }

#define ENC_B(off) (0x14000000u | ((uint32_t) ((off) >> 2) & 0x3ffffff))
#define NOP 0xD503201Fu

static void jit_segment_installed(struct fiber_block *b, const struct jit_units *U, unsigned first,
                                  unsigned end, const uint8_t *code, unsigned words);

// ================================================================ native

#define G_SP 32
// The hottest guest registers live in fixed host registers across all native
// code (ISH_JIT_PIN=0 turns this off). Native code is entered "cold" from the
// gadget world (a prologue loads them) or "hot" from other native code (they
// are already in place: entry + 4*n_pinned); every exit to gadgets or the run
// loop stores them back first. Other guest registers are loaded on first use
// into the lazy pool and written through. x8-x12 are scratch, x28 = stream
// pointer at exits, x1 = cpu, x2 = tlb, x18 platform, x29/x30/sp untouched.
static const int8_t pin_guest[] = {0, G_SP, 1, 2, 19, 3, 4, 21, 20, 22, 23, 30, 29, 24, 6, 5};
static const int8_t pin_host[]  = {19, 20, 21, 22, 23, 24, 25, 26, 27, 3, 4, 5, 6, 7, 13, 14};
static const uint8_t lazy_pool[] = {16, 17, 0};
#define H_CYC 15   // pinned mode: cpu->cycle lives in w15 between cold entry and exit
static const uint8_t pool_nopin[] = {0, 3, 4, 5, 6, 7, 13, 14, 15, 16, 17, 19, 20, 21, 22, 23, 24, 25, 26, 27};
static int n_pinned;               // 0 when pinning is off
static int prologue_words;          // cold-entry prologue length (hot entry offset)
static uintptr_t exit_stub;         // shared: store pinned registers; br x8
static unsigned exit_stub_words;
static int8_t pin_of[33];          // guest -> host register, -1 if not pinned

static void pin_init(bool on) {
    n_pinned = on ? (int) sizeof(pin_guest) : 0;
    // load_pinned(); PIC adds x9 = &code[0] before it and the link entry
    // (x29 = x9 - code) as its last word
    // x9 = &code[0] (2 words), load_pinned(), cross-module entry (1 word)
    prologue_words = n_pinned + (n_pinned ? 1 : 0) + (pic_on ? 3 : 0);
    memset(pin_of, -1, sizeof(pin_of));
    for (int i = 0; i < n_pinned; i++)
        pin_of[pin_guest[i]] = pin_host[i];
}
enum { T_P = 8, T_A = 9, T_I = 10, T_E = 11, T_Q = 12 };

enum { REL_SYM, REL_EXIT };

// A recorded segment (ISH_JIT_RECORD): the code as emitted, before links are
// patched in, and what an exporter needs to place it elsewhere.
struct rec_seg {
    unsigned nwords, nrel;
    uint32_t *words;
    struct rel *rel;
    int link_at[2];        // direct-link branch per jump_ip slot, -1 none
    bool loop;             // self-loop block: promoted registers in [loop_head, body_end)
    int loop_head, body_end;
    uint8_t npromo;
    int8_t promo_g[8], donor_g[8], promo_h[8];
};

struct em {
    struct fiber_block *b; // block being translated
    unsigned idx;          // PIC: the translation's index in its module context
    uint64_t base;         // PIC: guest address the module's constants are relative to
    uint32_t buf[8192];
    unsigned n;
    int8_t host[33];
    uint8_t npool;
    int8_t owner[32];      // host register -> guest register (lazy pool), -1 free
    uint32_t locked;       // host registers used by the instruction being emitted
    uint8_t victim;        // round-robin eviction cursor
    // bailout / exit sites: branch at buf[at] must go to the stub for stream position spos
    struct { unsigned at; unsigned spos; uint8_t kind; uintptr_t abs; } fix[1024];
    unsigned nfix;
    bool fail;
    bool hflags;   // host NZCV currently equals the guest flags
    int link_at[2];        // buffer index of the patchable direct-link nop per jump_ip slot
    // Relocations of the segment being emitted (PIC): host symbols
    // materialized with movz/movk, and the branch to the exit stub.
    struct rel { unsigned at; uint8_t kind; uintptr_t val; } rel[512];
    unsigned nrel;
    bool record;           // keep a copy of every installed segment (ISH_JIT_RECORD)
    struct rec_seg *last_rec;
    // segments of the current block installed so far (translation registry)
    unsigned ninst;
    struct { unsigned pos; uint8_t *code; struct rec_seg *rec; } inst[256];
    int8_t pin[33];        // guest -> host for this segment: pin_of plus loop promotions
    uint16_t uses[33];     // hreg() requests per guest register (loop planning)
    // Self-loop block: guest registers the loop body uses but that are not
    // pinned borrow the host registers of pinned ones it does not use. The hot
    // entry swaps them in (store donor, load promoted); the back edge branches
    // to loop_head with them still in place; every exit swaps them back.
    bool loop;
    uint8_t npromo;
    int8_t promo_g[8], donor_g[8], promo_h[8];
    int loop_head, body_end;
};

static void put(struct em *e, uint32_t x) {
    if (e->n >= sizeof(e->buf) / 4) { e->fail = true; return; }
    e->buf[e->n++] = x;
}

static unsigned greg_off(int g) {
    return g == G_SP ? offsetof(struct cpu_state, sp) : offsetof(struct cpu_state, regs) + 8 * g;
}

// host register holding guest register g (0..30 or G_SP), loading it on first
// use. Lazy registers are written through, so when the pool is full a mapping
// not used by the current instruction can simply be dropped.
static int hreg(struct em *e, int g) {
    if (e->uses[g] < 0xffff) e->uses[g]++;
    if (e->pin[g] >= 0)
        return e->pin[g];
    if (e->host[g] >= 0) {
        e->locked |= 1u << e->host[g];
        return e->host[g];
    }
    const uint8_t *pool = n_pinned ? lazy_pool : pool_nopin;
    unsigned npool = n_pinned ? sizeof(lazy_pool) : sizeof(pool_nopin);
    int h = -1;
    if (e->npool < npool) {
        h = pool[e->npool++];
    } else {
        for (unsigned tries = 0; tries < npool; tries++) {
            int cand = pool[e->victim++ % npool];
            if (!(e->locked & (1u << cand))) { h = cand; break; }
        }
        if (h < 0) {
            e->fail = true;
            atomic_fetch_add_explicit(&st_fail_regs, 1, memory_order_relaxed);
            return 0;
        }
        if (e->owner[h] >= 0)
            e->host[e->owner[h]] = -1;
    }
    e->host[g] = (int8_t) h;
    e->owner[h] = (int8_t) g;
    e->locked |= 1u << h;
    put(e, 0xF9400000u | ((greg_off(g) / 8) << 10) | (1u << 5) | (uint32_t) h);  // ldr xh, [x1, #off]
    return h;
}

static void writeback(struct em *e, int g) {
    if (e->pin[g] >= 0)
        return;   // pinned: stored at exits
    put(e, 0xF9000000u | ((greg_off(g) / 8) << 10) | (1u << 5) | (uint32_t) e->host[g]);  // str xh, [x1, #off]
}

static void load_pinned(struct em *e) {
    for (int i = 0; i < n_pinned; i++)
        put(e, 0xF9400000u | ((greg_off(pin_guest[i]) / 8) << 10) | (1u << 5) | (uint32_t) pin_host[i]);
    if (n_pinned)
        put(e, 0xB9400000u | ((CPU_cycle / 4) << 10) | (1u << 5) | H_CYC);   // ldr w15, [x1, #cycle]
}

// loop block: promoted registers <-> canonical pinned state
static void emit_promote(struct em *e) {
    for (unsigned i = 0; i < e->npromo; i++) {
        put(e, 0xF9000000u | ((greg_off(e->donor_g[i]) / 8) << 10) | (1u << 5) | (uint32_t) e->promo_h[i]);
        put(e, 0xF9400000u | ((greg_off(e->promo_g[i]) / 8) << 10) | (1u << 5) | (uint32_t) e->promo_h[i]);
    }
}

static void emit_canon(struct em *e) {
    for (unsigned i = 0; i < e->npromo; i++) {
        put(e, 0xF9000000u | ((greg_off(e->promo_g[i]) / 8) << 10) | (1u << 5) | (uint32_t) e->promo_h[i]);
        put(e, 0xF9400000u | ((greg_off(e->donor_g[i]) / 8) << 10) | (1u << 5) | (uint32_t) e->promo_h[i]);
    }
}

static void mov_imm64(struct em *e, int rd, uint64_t v);

static uintptr_t entry_off(void) {
    return 4 * (uintptr_t) (prologue_words - (pic_on ? 1 : 0));
}

// Every host address baked into native code (blocks, code-stream slots, the
// code region, gadgets and fiber_ret) is emitted here, never as a plain
// immediate: this is the relocation point for exporting translations.
static void add_rel(struct em *e, uint8_t kind, uintptr_t val) {
    if (e->nrel == sizeof(e->rel) / sizeof(e->rel[0])) { e->fail = true; return; }
    e->rel[e->nrel].at = e->n;
    e->rel[e->nrel].kind = kind;
    e->rel[e->nrel].val = val;
    e->nrel++;
}

static void mov_host_ptr(struct em *e, int rd, const void *p) {
    uint64_t v = (uint64_t) (uintptr_t) p;
    if (!pic_on) {
        mov_imm64(e, rd, v);
        return;
    }
    // Always movz + 3 movk, so an exporter can put adrp/add in their place.
    add_rel(e, REL_SYM, (uintptr_t) p);
    put(e, 0xD2800000u | (uint32_t) (v & 0xffff) << 5 | (uint32_t) rd);
    for (int hw = 1; hw < 4; hw++)
        put(e, 0xF2800000u | (uint32_t) hw << 21 | (uint32_t) ((v >> (16 * hw)) & 0xffff) << 5 | (uint32_t) rd);
}

// ---- PIC (ISH_JIT_PIC=1)
//
// Native code that another process can run as is (AOT, or a translation
// reused by the registry below): nothing in it depends on where the guest
// module is mapped or where its fiber_blocks live, and nothing is patched
// after it is installed except direct links, whose target is also recorded as
// data next to the branch.
// - x29 holds the module context of this address space: the guest address the
//   module's constants are relative to, and per translation (ctx->slot[idx])
//   the fiber_block currently holding it and the guest address its constants
//   are relative to. It stays the same while
//   control moves between blocks of one module; a block pointer is loaded
//   from the context only where needed, so block transitions carry no chain
//   of dependent loads.
// - Every block has a cold entry (from the gadgets; prologue: x9 = &code[0],
//   load the pinned registers, then the cross-module entry
//   x29 = block->jit_ctx) and a hot entry right after it. native_entry is the
//   cross-module entry; code switching blocks with x9 = &code[0] enters there,
//   or at the hot entry when the successor's context is already in x29.
// - Guest addresses are the slot's base plus a constant: a module is mapped
//   at a page-aligned base, so pc, adr and adrp values differ from it by the
//   same amount in every process. The base is per translation so that an AOT
//   translation also serves a block whose bytes moved in another version of
//   the module: identical bytes keep every pc-relative distance, so the code
//   is right with base + the distance the block moved (aot_find_moved()).
// - Direct links need a check that the successor still runs the code the
//   branch goes to; jit_chain_ok() does it when the slot is chained, so the
//   branch itself is unconditional.
// What is left are link-time symbols (gadgets, fiber_ret, fiber_ret_chain,
// the exit stub and the code region), all emitted through mov_host_ptr() or
// the exit-stub branch.

#define FP 29
#define LINK_UNSET 1u   // literal of an unlinked direct link
static unsigned code_off(unsigned k) { return FIBER_BLOCK_code + 8 * k; }

// Per translation: what its code loads on the way (above x29, small offsets
// for the first indices)...
struct jit_slot {
    struct fiber_block *blk;    // the block holding the translation
    uint64_t base;              // guest address its constants are relative to
};
// ... and what it rarely needs, below x29 (far[idx] at x29 - CTX_FAR * (idx + 1)).
struct jit_far {
    uint64_t dbase;             // what base is for the targets of its adr / adrp
    uintptr_t alt[2];           // per jump_ip slot: &code[0] of a successor the direct link
                                // cannot go to (jit_chain_refused()), 0 if none
};
struct jit_ctx {
    uint64_t base;              // the module mapping's (for the C side)
    struct jit_slot slot[];
};
#define CTX_BLK 8               // offsetof(struct jit_ctx, slot)
#define CTX_SLOT 16             // sizeof(struct jit_slot)
#define CTX_FAR 24              // sizeof(struct jit_far)

static inline struct jit_far *ctx_far(struct jit_ctx *ctx, unsigned idx) {
    return (struct jit_far *) ((char *) ctx - CTX_FAR * ((size_t) idx + 1));
}
#define CTX_MAX (1u << 19)      // translations per module (reserved address space)

// xd = xn +/- magnitude (any size; large values go through `scratch`)
static void add_imm(struct em *e, int rd, int rn, int64_t d, int scratch) {
    uint64_t m = d < 0 ? (uint64_t) -d : (uint64_t) d;
    uint32_t op = d < 0 ? 0xD1000000u : 0x91000000u;   // sub / add (immediate)
    if (m >= (1u << 24)) {
        mov_imm64(e, scratch, (uint64_t) d);
        put(e, 0x8B000000u | ((uint32_t) scratch << 16) | ((uint32_t) rn << 5) | (uint32_t) rd);   // add xd, xn, xs
        return;
    }
    if (m >> 12) {
        put(e, op | (1u << 22) | (uint32_t) (m >> 12) << 10 | ((uint32_t) rn << 5) | (uint32_t) rd);
        rn = rd;
    }
    if ((m & 0xfff) || rn != rd)
        put(e, op | (uint32_t) (m & 0xfff) << 10 | ((uint32_t) rn << 5) | (uint32_t) rd);
}

// xd = [x29 - off] (off a multiple of 8, < 2^24): a field of ctx_far()
static void ldr_far(struct em *e, int rd, unsigned off) {
    if (off <= 256) {
        put(e, 0xF8400000u | ((uint32_t) (-(int) off) & 0x1ff) << 12 | (FP << 5) | (uint32_t) rd);   // ldur xd, [x29, #-off]
        return;
    }
    unsigned r = (off + 4095) & ~4095u;
    put(e, 0xD1400000u | (r >> 12) << 10 | (FP << 5) | (uint32_t) rd);                          // sub xd, x29, #r, lsl #12
    put(e, 0xF9400000u | ((r - off) / 8) << 10 | ((uint32_t) rd << 5) | (uint32_t) rd);          // ldr xd, [xd, #r - off]
}
static unsigned far_off(unsigned idx, unsigned field) { return CTX_FAR * (idx + 1) - field; }

// xd = [xn + off] (off a multiple of 8, < 2^24)
static void ldr_off(struct em *e, int rd, int rn, unsigned off) {
    if (off >> 15) {
        put(e, 0x91400000u | (off >> 12) << 10 | ((uint32_t) rn << 5) | (uint32_t) rd);   // add xd, xn, #off>>12, lsl #12
        rn = rd;
        off &= 0xfff;
    }
    put(e, 0xF9400000u | (off / 8) << 10 | ((uint32_t) rn << 5) | (uint32_t) rd);      // ldr xd, [xn, #off]
}

// PIC: xd = the block (ctx->slot[idx].blk)
static void ldr_ctx_block(struct em *e, int rd) {
    ldr_off(e, rd, FP, CTX_BLK + CTX_SLOT * e->idx);
}

// xd = (uintptr_t) b + off
static void mov_block_addr(struct em *e, int rd, unsigned off) {
    if (pic_on) {
        ldr_ctx_block(e, rd);
        add_imm(e, rd, rd, off, rd);
    } else {
        mov_imm64(e, rd, (uint64_t) (uintptr_t) e->b + off);
    }
}

// xd = *(uint64_t *) ((char *) b + off)
static void ldr_block(struct em *e, int rd, unsigned off) {
    if (pic_on) {
        ldr_ctx_block(e, rd);
        ldr_off(e, rd, rd, off);
        return;
    }
    mov_block_addr(e, rd, off);
    put(e, 0xF9400000u | ((uint32_t) rd << 5) | (uint32_t) rd);             // ldr xd, [xd]
}

// xd = guest address v
static void mov_guest(struct em *e, int rd, uint64_t v, int scratch) {
    if (!pic_on) {
        mov_imm64(e, rd, v);
        return;
    }
    ldr_off(e, rd, FP, CTX_BLK + CTX_SLOT * e->idx + 8);                    // ldr xd, [slot.base]
    add_imm(e, rd, rd, (int64_t) (v - e->base), scratch);
}

// xd = guest address v, the target of an adr / adrp. Relative to the slot's
// dbase: in another version of the module the data a block refers to may
// have moved by another distance than the block (aot_find_moved()).
static void mov_guest_ref(struct em *e, int rd, uint64_t v, int scratch) {
    if (!pic_on) {
        mov_imm64(e, rd, v);
        return;
    }
    ldr_far(e, rd, far_off(e->idx, offsetof(struct jit_far, dbase)));      // xd = far.dbase
    add_imm(e, rd, rd, (int64_t) (v - e->base), scratch);
}

// PIC: continue in another block's native code. x9 = its &code[0], x8 = its
// native_entry (cross-module entry). Within the module x29 stays as it is and
// the hot entry is used, so nothing after the branch waits for a load.
static void enter_native(struct em *e) {
    int co = (int) offsetof(struct fiber_block, jit_ctx) - FIBER_BLOCK_code;
    put(e, 0xF8400000u | ((uint32_t) (co & 0x1ff) << 12) | (9u << 5) | 10);  // ldur x10, [x9, #jit_ctx-code]
    put(e, 0xEB1D015Fu);                                                     // cmp x10, x29
    put(e, 0x54000041u);                                                     // b.ne 1f
    put(e, 0x91001108u);                                                     // add x8, x8, #4 (hot entry)
    put(e, 0xD61F0100u);                                                     // 1: br x8
}

// Shared exit stub: store the pinned guest registers, then br x8.
static void make_exit_stub(void) {
    uint32_t stub[40];
    unsigned n = 0;
    for (int i = 0; i < n_pinned; i++)
        stub[n++] = 0xF9000000u | ((greg_off(pin_guest[i]) / 8) << 10) | (1u << 5) | (uint32_t) pin_host[i];
    if (n_pinned)
        stub[n++] = 0xB9000000u | ((CPU_cycle / 4) << 10) | (1u << 5) | H_CYC;   // str w15, [x1, #cycle]
    stub[n++] = 0xD61F0100u;   // br x8
    uint8_t *dst = region_alloc(n * 4);
    install_code(dst, stub, n);
    exit_stub = (uintptr_t) dst;
    exit_stub_words = n;
}

// Leave native code for `target` (gadget, fiber_ret, ...): x8 = target, then
// the shared exit stub stores the pinned registers and branches to x8.
static void emit_exit_x8(struct em *e) {
    if (e->nfix == 1024) { e->fail = true; return; }
    e->fix[e->nfix].at = e->n; e->fix[e->nfix].spos = 0; e->fix[e->nfix].kind = 4; e->fix[e->nfix].abs = exit_stub;
    e->nfix++;
    put(e, 0x14000000u);                                           // b exit_stub (patched)
}

static void set_last_block(struct em *e, struct fiber_block *b) {
    if (pic_on)
        ldr_ctx_block(e, 8);
    else
        mov_imm64(e, 8, (uint64_t) (uintptr_t) b);
    put(e, 0xF9000000u | ((LOCAL_last_block / 8) << 10) | (1u << 5) | 8);        // str x8, [x1, #last_block]
}

static void set_pc_const(struct em *e, uint64_t pc) {
    mov_guest(e, 11, pc, 12);
    put(e, 0xF9000000u | ((CPU_pc / 8) << 10) | (1u << 5) | 11);                 // str x11, [x1, #pc]
}

static void emit_exit(struct em *e, uintptr_t target) {
    mov_host_ptr(e, 8, (const void *) target);
    emit_exit_x8(e);
}

static void load_flags(struct em *e) {
    e->hflags = true;
    put(e, 0xB9400000u | ((offsetof(struct cpu_state, nzcv) / 4) << 10) | (1u << 5) | T_P);  // ldr w8, [x1, #nzcv]
    put(e, 0xD51B4200u | T_P);                                                               // msr nzcv, x8
}

static void store_flags(struct em *e) {
    put(e, 0xD53B4200u | T_P);                                                               // mrs x8, nzcv
    put(e, 0xB9000000u | ((offsetof(struct cpu_state, nzcv) / 4) << 10) | (1u << 5) | T_P);  // str w8, [x1, #nzcv]
}

static void mov_imm64(struct em *e, int rd, uint64_t v) {
    put(e, 0xD2800000u | (uint32_t) (v & 0xffff) << 5 | (uint32_t) rd);
    for (int hw = 1; hw < 4; hw++)
        if ((v >> (16 * hw)) & 0xffff)
            put(e, 0xF2800000u | (uint32_t) hw << 21 | (uint32_t) ((v >> (16 * hw)) & 0xffff) << 5 | (uint32_t) rd);
}

// Branch (kind 0 = b, 1 = b.ne, 2 = b.hi) to the stub that resumes the gadget
// stream at position spos.
static void branch_stub(struct em *e, unsigned spos, int kind) {
    if (e->nfix == 1024) { e->fail = true; return; }
    e->fix[e->nfix].at = e->n;
    e->fix[e->nfix].spos = spos;
    e->fix[e->nfix].kind = (uint8_t) kind;
    e->nfix++;
    put(e, 0);
}

// Field kinds for register remapping.
enum { F_NONE, F_ZR, F_SP };

// Remap the rd/rn/rm/ra fields of an instruction. rd_write: rd is written.
// rd_read: rd is also read (movk, bfm). Returns the host encoding.
static uint32_t remap(struct em *e, uint32_t x, int kd, int kn, int km, int ka, bool rd_read, int *wrote) {
    static const int shift[4] = {0, 5, 16, 10};
    const int kinds[4] = {kd, kn, km, ka};
    *wrote = -1;
    uint32_t out = x;
    for (int f = 3; f >= 0; f--) {   // sources first, destination last
        if (kinds[f] == F_NONE)
            continue;
        int r = (x >> shift[f]) & 31;
        int g;
        if (r == 31) {
            if (kinds[f] == F_ZR)
                continue;   // xzr stays xzr
            g = G_SP;
        } else {
            g = r;
        }
        if (f != 0 || rd_read)
            (void) hreg(e, g);
        int h = hreg(e, g);
        out = (out & ~(31u << shift[f])) | ((uint32_t) h << shift[f]);
        if (f == 0)
            *wrote = g;
    }
    return out;
}

// Inline TLB lookup for guest address in T_A (bytes = access size); leaves
// the host address in T_A. Branches to the unit's stub on a miss or crossing.
static void tlb_lookup(struct em *e, unsigned bytes, bool write, unsigned spos) {
    e->hflags = false;   // the checks below use cmp
    put(e, 0x9240BC00u | (T_A << 5) | T_A);                    // and x9, x9, #0xffffffffffff
    put(e, 0x12002C00u | (T_A << 5) | T_P);                    // and w8, w9, #0xfff
    put(e, 0xF100001Fu | ((0x1000 - bytes) << 10) | (T_P << 5));   // cmp x8, #(0x1000-bytes)
    branch_stub(e, spos, 2);                                   // b.hi stub
    put(e, 0x9274CC00u | (T_A << 5) | T_P);                    // and x8, x9, #~0xfff
    put(e, 0xD34C6000u | (T_A << 5) | T_I);                    // ubfx x10, x9, #12, #13
    put(e, 0xCA400000u | (T_A << 16) | (25u << 10) | (T_I << 5) | T_I);  // eor x10, x10, x9, lsr #25
    put(e, 0x12003000u | (T_I << 5) | T_I);                    // and w10, w10, #0x1fff
    put(e, 0xD37BE800u | (T_I << 5) | T_I);                    // lsl x10, x10, #5
    put(e, 0x8B000000u | (2u << 16) | (T_I << 5) | T_I);       // add x10, x10, x2
    unsigned pg = write ? offsetof(struct tlb_entry, page_if_writable) : offsetof(struct tlb_entry, page);
    put(e, 0xF9400000u | ((pg / 8) << 10) | (T_I << 5) | T_E); // ldr x11, [x10, #page]
    put(e, 0xEB00001Fu | (T_E << 16) | (T_P << 5));            // cmp x8, x11
    branch_stub(e, spos, 1);                                   // b.ne stub
    put(e, 0xF9400000u | ((offsetof(struct tlb_entry, gen) / 8) << 10) | (T_I << 5) | T_E);  // ldr x11, [x10, #gen]
    int mmu_off = (int) offsetof(struct tlb, mmu) - (int) offsetof(struct tlb, entries);
    put(e, 0xF8400000u | ((uint32_t) (mmu_off & 0x1ff) << 12) | (2u << 5) | T_Q);             // ldur x12, [x2, #mmu]
    put(e, 0xF9400000u | ((offsetof(struct mmu, changes) / 8) << 10) | (T_Q << 5) | T_Q);    // ldr x12, [x12, #changes]
    put(e, 0xEB00001Fu | (T_Q << 16) | (T_E << 5));            // cmp x11, x12
    branch_stub(e, spos, 1);                                   // b.ne stub
    put(e, 0xF9400000u | ((offsetof(struct tlb_entry, data_minus_addr) / 8) << 10) | (T_I << 5) | T_E);  // ldr x11, [x10, #dma]
    put(e, 0x8B000000u | (T_A << 16) | (T_E << 5) | T_A);      // add x9, x11, x9
}

// Load/store translation. Returns false if the form is unsupported.
// ---- SIMD & FP: guest V registers live in cpu->fp; an instruction loads the
// ones it reads into host v16..v23 (caller-saved, unused by the gadgets),
// runs the same operation there and stores the destination back.
static unsigned fp_off(int v) { return (unsigned) offsetof(struct cpu_state, fp) + 16u * (unsigned) v; }
static void vload(struct em *e, int h, int v) {
    put(e, 0x3DC00000u | ((fp_off(v) / 16) << 10) | (1u << 5) | (uint32_t) h);  // ldr qh, [x1, #fp[v]]
}
static void vstore(struct em *e, int h, int v) {
    put(e, 0x3D800000u | ((fp_off(v) / 16) << 10) | (1u << 5) | (uint32_t) h);  // str qh, [x1, #fp[v]]
}
enum { V_N = 16, V_M = 17, V_D = 18, V_T = 19 };   // table registers: v19..v22

// Vector data processing without memory access or flags. Returns false for
// anything not handled (the gadget runs it).
static bool emit_simd(struct em *e, uint32_t x) {
    int rd = x & 31, rn = (x >> 5) & 31, rm = (x >> 16) & 31;
    uint32_t base = x & ~0x001F03FFu;   // opcode with rd/rn/rm cleared
    if ((x & 0xBFE0FC00u) == 0x0E000C00u) {                                   // dup (general)
        if ((x & 0x000F0000u) == 0 || (((x >> 16) & 0xF) == 8 && !(x & 0x40000000u))) return false;
        int hn = rn == 31 ? 31 : hreg(e, rn);
        put(e, (x & ~0x3FFu) | ((uint32_t) hn << 5) | V_D);
        vstore(e, V_D, rd);
        return true;
    }
    if ((x & 0xBFE0FC00u) == 0x0E003C00u) {                                   // umov / mov (to general)
        vload(e, V_N, rn);
        if (rd == 31) return true;
        int hd = hreg(e, rd);
        put(e, (x & ~0x3FFu) | (V_N << 5) | (uint32_t) hd);
        writeback(e, rd);
        return true;
    }
    if ((x & 0xFFE0FC00u) == 0x4E001C00u) {                                   // ins (general)
        vload(e, V_D, rd);
        int hn = rn == 31 ? 31 : hreg(e, rn);
        put(e, (x & ~0x3FFu) | ((uint32_t) hn << 5) | V_D);
        vstore(e, V_D, rd);
        return true;
    }
    if ((x & 0xFFE08400u) == 0x6E000400u) {                                   // ins (element)
        vload(e, V_D, rd);
        vload(e, V_N, rn);
        put(e, (x & ~0x3FFu) | (V_N << 5) | V_D);
        vstore(e, V_D, rd);
        return true;
    }
    uint32_t fm = x & 0xFFFFFC00u;
    if (fm == 0x9E670000u || fm == 0x1E270000u) {                              // fmov d, x / s, w
        int hn = rn == 31 ? 31 : hreg(e, rn);
        put(e, fm | ((uint32_t) hn << 5) | V_D);
        vstore(e, V_D, rd);
        return true;
    }
    if (fm == 0x9E660000u || fm == 0x1E260000u) {                              // fmov x, d / w, s
        vload(e, V_N, rn);
        if (rd == 31) return true;
        int hd = hreg(e, rd);
        put(e, fm | (V_N << 5) | (uint32_t) hd);
        writeback(e, rd);
        return true;
    }
    bool three = false, two = false, rd_in = true;
    if ((x & 0x9FF80400u) == 0x0F000400u) {                                   // modified immediate (movi/mvni/orr/bic/fmov)
        vload(e, V_D, rd);
        put(e, (x & ~0x1Fu) | V_D);
        vstore(e, V_D, rd);
        return true;
    } else if ((x & 0x9F200400u) == 0x0E200400u) {                            // three same (integer)
        unsigned op = (x >> 11) & 31;
        if (op >= 0x18 || op == 0x01 || op == 0x05 || op == 0x09 || op == 0x0B || op == 0x16) return false;
        three = true;
    } else if ((x & 0x9F3E0C00u) == 0x0E200800u) {                            // two-register misc (integer subset)
        unsigned op = (x >> 12) & 31, U = (x >> 29) & 1;
        if (!(op <= 0x02 || op == 0x05 || op == 0x06 || (op >= 0x08 && op <= 0x0B) || (op == 0x12 && !U))) return false;
        if (op == 0x05 && U && ((x >> 22) & 3) >= 2) return false;
        two = true;
    } else if ((x & 0x9F3E0C00u) == 0x0E300800u) {                            // across lanes (integer)
        unsigned op = (x >> 12) & 31;
        if (op != 0x03 && op != 0x0A && op != 0x1A && op != 0x1B) return false;
        two = true;
    } else if ((x & 0x9F800400u) == 0x0F000400u && (x & 0x00780000u)) {       // shift by immediate
        unsigned op = (x >> 11) & 31, U = (x >> 29) & 1;
        if (!(op == 0x00 || op == 0x02 || op == 0x04 || op == 0x06 || op == 0x08 || op == 0x0A || op == 0x14 ||
              ((op == 0x10 || op == 0x11) && !U)))
            return false;
        two = true;
    } else if ((x & 0xBF208C00u) == 0x0E000800u) {                            // zip / uzp / trn
        three = true;
    } else if ((x & 0xBFE08400u) == 0x2E000000u) {                            // ext
        three = true;
    } else if ((x & 0xBF209C00u) == 0x0E000000u) {                            // tbl / tbx
        unsigned len = ((x >> 13) & 3) + 1;
        for (unsigned i = 0; i < len; i++)
            vload(e, V_T + (int) i, (rn + (int) i) & 31);
        vload(e, V_M, rm);
        vload(e, V_D, rd);   // tbx keeps out-of-range lanes
        put(e, base | ((uint32_t) V_M << 16) | ((uint32_t) V_T << 5) | V_D);
        vstore(e, V_D, rd);
        return true;
    } else {
        return false;
    }
    vload(e, V_N, rn);
    if (three) vload(e, V_M, rm);
    if (rd_in) vload(e, V_D, rd);   // accumulating forms (bsl, sra, sadalp, ...)
    put(e, base | (three ? (uint32_t) V_M << 16 : (x & 0x001F0000u)) | ((uint32_t) V_N << 5) | V_D);
    (void) two;
    vstore(e, V_D, rd);
    return true;
}

// base (+ offset) -> x9 for a load/store; returns the host register of the base
static int ldst_addr(struct em *e, int gn, int64_t imm, bool post) {
    int hn = hreg(e, gn);
    if (post) put(e, 0xAA0003E0u | ((uint32_t) hn << 16) | T_A);                 // mov x9, xn
    else if (imm > 4095) { mov_imm64(e, T_A, (uint64_t) imm); put(e, 0x8B000000u | ((uint32_t) hn << 16) | (T_A << 5) | T_A); }
    else if (imm >= 0) put(e, 0x91000000u | ((uint32_t) imm << 10) | ((uint32_t) hn << 5) | T_A);
    else put(e, 0xD1000000u | ((uint32_t) -imm << 10) | ((uint32_t) hn << 5) | T_A);
    return hn;
}

static void ldst_wb(struct em *e, int gn, int hn, int64_t imm) {
    if (imm >= 0) put(e, 0x91000000u | ((uint32_t) imm << 10) | ((uint32_t) hn << 5) | (uint32_t) hn);
    else put(e, 0xD1000000u | ((uint32_t) -imm << 10) | ((uint32_t) hn << 5) | (uint32_t) hn);
    writeback(e, gn);
}

// SIMD & FP loads and stores (b/h/s/d/q, single and pair, no register offset)
static bool emit_ldst_fp(struct em *e, uint32_t x, unsigned spos) {
    if ((x & 0x3A000000u) == 0x28000000u) {                                   // pair
        unsigned opc = x >> 30, L = (x >> 22) & 1, idx = (x >> 23) & 3;
        if (opc == 3 || idx == 0) return false;                               // no-allocate pair: rare
        unsigned scale = 2 + opc;
        int64_t imm = sext((x >> 15) & 0x7f, 7) << scale;
        int rt = x & 31, rt2 = (x >> 10) & 31, rn = (x >> 5) & 31;
        if (imm < -4095 || imm > 4095) return false;
        int gn = rn == 31 ? G_SP : rn;
        int hn = ldst_addr(e, gn, imm, idx == 1);
        tlb_lookup(e, 2u << scale, !L, spos);
        if (!L) { vload(e, V_N, rt); vload(e, V_M, rt2); }
        put(e, (x & 0xC0400000u) | 0x2D000000u | ((uint32_t) V_M << 10) | (T_A << 5) | V_N);   // ldp/stp vN, vM, [x9]
        if (L) { vstore(e, V_N, rt); vstore(e, V_M, rt2); }
        if (idx == 1 || idx == 3) ldst_wb(e, gn, hn, imm);
        return true;
    }
    unsigned size = x >> 30, opc = (x >> 22) & 3;
    int rt = x & 31, rn = (x >> 5) & 31;
    bool q = opc >= 2;
    if (q && size) return false;
    unsigned scale = q ? 4 : size;
    int64_t imm;
    int wbmode = 0;
    if ((x & 0x3B000000u) == 0x39000000u) {                                   // unsigned offset
        imm = (int64_t) ((x >> 10) & 0xfff) << scale;
    } else if ((x & 0x3B200000u) == 0x38000000u) {                            // imm9 forms
        unsigned k = (x >> 10) & 3;
        if (k == 2) return false;
        imm = sext((x >> 12) & 0x1ff, 9);
        wbmode = k == 1 ? 1 : k == 3 ? 2 : 0;
    } else {
        return false;
    }
    bool load = opc & 1;
    int gn = rn == 31 ? G_SP : rn;
    int hn = ldst_addr(e, gn, imm, wbmode == 1);
    tlb_lookup(e, 1u << scale, !load, spos);
    if (!load) vload(e, V_N, rt);
    put(e, (x & 0xC0C00000u) | 0x3D000000u | (T_A << 5) | V_N);              // ldr/str vN, [x9]
    if (load) vstore(e, V_N, rt);   // the host load zeroed the rest of the register
    if (wbmode) ldst_wb(e, gn, hn, imm);
    return true;
}

static bool emit_ldst(struct em *e, uint32_t x, unsigned spos) {
    if (x & (1u << 26))
        return simd_on && emit_ldst_fp(e, x, spos);
    // --- load/store pair ---
    if ((x & 0x3A000000u) == 0x28000000u) {
        unsigned opc = x >> 30, L = (x >> 22) & 1, idx = (x >> 23) & 3;
        if (opc == 3 || (opc == 1 && !L)) return false;
        unsigned scale = opc == 2 ? 3 : 2;
        int64_t imm = sext((x >> 15) & 0x7f, 7) << scale;
        int rt = x & 31, rt2 = (x >> 10) & 31, rn = (x >> 5) & 31;
        bool wb = idx == 1 || idx == 3;
        if (wb && ((rn == rt && rt != 31) || (rn == rt2 && rt2 != 31))) return false;
        if (L && rt == rt2) return false;
        int gn = rn == 31 ? G_SP : rn;
        int hn = hreg(e, gn);
        if (idx == 1) put(e, 0xAA0003E0u | ((uint32_t) hn << 16) | T_A);       // mov x9, xn (post-index)
        else if (imm >= 0) put(e, 0x91000000u | ((uint32_t) imm << 10) | ((uint32_t) hn << 5) | T_A);
        else put(e, 0xD1000000u | ((uint32_t) -imm << 10) | ((uint32_t) hn << 5) | T_A);
        tlb_lookup(e, 2u << scale, !L, spos);
        int ht = rt == 31 ? 31 : hreg(e, rt), ht2 = rt2 == 31 ? 31 : hreg(e, rt2);
        // host pair access, signed-offset form, offset 0
        put(e, (x & 0xC0400000u) | 0x29000000u | ((uint32_t) ht2 << 10) | (T_A << 5) | (uint32_t) ht);
        if (L) {
            if (rt != 31) writeback(e, rt);
            if (rt2 != 31) writeback(e, rt2);
        }
        if (wb) {
            if (imm >= 0) put(e, 0x91000000u | ((uint32_t) imm << 10) | ((uint32_t) hn << 5) | (uint32_t) hn);
            else put(e, 0xD1000000u | ((uint32_t) -imm << 10) | ((uint32_t) hn << 5) | (uint32_t) hn);
            writeback(e, gn);
        }
        return true;
    }
    // --- single register ---
    unsigned size = x >> 30, opc = (x >> 22) & 3;
    int rt = x & 31, rn = (x >> 5) & 31;
    int64_t imm = 0;
    int wbmode = 0;   // 0 none, 1 post, 2 pre
    bool regoff = false;
    if ((x & 0x3B000000u) == 0x39000000u) {                       // unsigned offset
        imm = (int64_t) ((x >> 10) & 0xfff) << size;
    } else if ((x & 0x3B200000u) == 0x38000000u) {                // imm9 forms
        unsigned k = (x >> 10) & 3;
        if (k == 2) return false;                                 // unprivileged
        imm = sext((x >> 12) & 0x1ff, 9);
        wbmode = k == 1 ? 1 : k == 3 ? 2 : 0;
    } else if ((x & 0x3B200C00u) == 0x38200800u) {                // register offset
        regoff = true;
    } else {
        return false;
    }
    bool prfm = size == 3 && opc == 2;
    if (!prfm && (opc == 3 && size >= 2)) return false;
    if (size == 3 && opc == 2 && wbmode) return false;
    if (wbmode && rn == rt && rt != 31) return false;
    bool load = opc != 0;
    int gn = rn == 31 ? G_SP : rn;
    int hn = hreg(e, gn);
    if (regoff) {
        int rm = (x >> 16) & 31;
        int hm = rm == 31 ? 31 : hreg(e, rm);
        unsigned option = (x >> 13) & 7, S = (x >> 12) & 1;
        if (!(option & 2)) return false;                          // 32-bit index with option<2 undefined
        put(e, 0x8B200000u | ((uint32_t) hm << 16) | (option << 13) | ((S ? size : 0) << 10) | ((uint32_t) hn << 5) | T_A);
    } else if (wbmode == 1) {
        put(e, 0xAA0003E0u | ((uint32_t) hn << 16) | T_A);
    } else if (imm >= 0) {
        if (imm > 4095) { mov_imm64(e, T_A, (uint64_t) imm); put(e, 0x8B000000u | ((uint32_t) hn << 16) | (T_A << 5) | T_A); }
        else put(e, 0x91000000u | ((uint32_t) imm << 10) | ((uint32_t) hn << 5) | T_A);
    } else {
        put(e, 0xD1000000u | ((uint32_t) -imm << 10) | ((uint32_t) hn << 5) | T_A);
    }
    if (prfm)
        goto writeback_base;
    tlb_lookup(e, 1u << size, !load, spos);
    {
        int ht = rt == 31 ? 31 : hreg(e, rt);
        put(e, (x & 0xC0C00000u) | 0x39000000u | (T_A << 5) | (uint32_t) ht);   // host access: same size/opc, [x9]
        if (load && rt != 31)
            writeback(e, rt);
    }
writeback_base:
    if (wbmode) {
        if (imm >= 0) put(e, 0x91000000u | ((uint32_t) imm << 10) | ((uint32_t) hn << 5) | (uint32_t) hn);
        else put(e, 0xD1000000u | ((uint32_t) -imm << 10) | ((uint32_t) hn << 5) | (uint32_t) hn);
        writeback(e, gn);
    }
    return true;
}

// Translate one guest instruction. has_mem: set if it may bail out.
static bool emit_insn(struct em *e, uint32_t x, uint64_t pc, unsigned spos, bool *has_mem) {
    int wrote = -1;
    e->locked = 0;
    *has_mem = false;
    if (x == NOP || (x & 0xFFFFF01Fu) == 0xD503201Fu)     // hints
        return true;
    if ((x & 0xFFFFF0FFu) == 0xD50330BFu) {                // dmb: same barrier on the host
        put(e, x);
        return true;
    }
    if ((x & 0x0E000000u) == 0x0E000000u && (x & 0x1F000000u) != 0x1F000000u && simd_on) {   // SIMD data processing
        if (emit_simd(e, x)) return !e->fail;
    }
    if ((x & 0x9F000000u) == 0x10000000u || (x & 0x9F000000u) == 0x90000000u) {   // adr / adrp
        int rd = x & 31;
        int64_t imm = sext(((x >> 5) & 0x7ffff) << 2 | ((x >> 29) & 3), 21);
        uint64_t v = (x & 0x80000000u) ? (pc & ~0xfffULL) + ((uint64_t) imm << 12) : pc + (uint64_t) imm;
        if (rd == 31) return true;
        int h = hreg(e, rd);
        mov_guest_ref(e, h, v, T_Q);
        writeback(e, rd);
        return true;
    }
    if ((x & 0xFFFFFFE0u) == 0xD53BD040u || (x & 0xFFFFFFE0u) == 0xD51BD040u) {   // mrs/msr tpidr_el0
        int rt = x & 31;
        bool read = (x & 0x00200000u) != 0;
        int h = rt == 31 ? 31 : hreg(e, rt);
        put(e, (read ? 0xF9400000u : 0xF9000000u) | ((CPU_tls_ptr / 8) << 10) | (1u << 5) | (uint32_t) h);
        if (read && rt != 31) writeback(e, rt);
        return true;
    }
    uint32_t out;
    bool rflags = false, wflags = false;
    if ((x & 0x1F000000u) == 0x11000000u) {                // add/sub immediate
        bool S = (x >> 29) & 1;
        out = remap(e, x, S ? F_ZR : F_SP, F_SP, F_NONE, F_NONE, false, &wrote);
        wflags = S;
    } else if ((x & 0x1F800000u) == 0x12000000u) {         // logical immediate
        bool S = ((x >> 29) & 3) == 3;
        out = remap(e, x, S ? F_ZR : F_SP, F_ZR, F_NONE, F_NONE, false, &wrote);
        wflags = S;
    } else if ((x & 0x1F800000u) == 0x12800000u) {         // move wide
        unsigned opc = (x >> 29) & 3;
        if (opc == 1) return false;
        out = remap(e, x, F_ZR, F_NONE, F_NONE, F_NONE, opc == 3, &wrote);
    } else if ((x & 0x1F800000u) == 0x13000000u) {         // bitfield
        unsigned opc = (x >> 29) & 3;
        if (opc == 3) return false;
        out = remap(e, x, F_ZR, F_ZR, F_NONE, F_NONE, opc == 1, &wrote);
    } else if ((x & 0x7FA00000u) == 0x13800000u) {         // extr
        out = remap(e, x, F_ZR, F_ZR, F_ZR, F_NONE, false, &wrote);
    } else if ((x & 0x1F000000u) == 0x0A000000u) {         // logical shifted register
        bool S = ((x >> 29) & 3) == 3;
        out = remap(e, x, F_ZR, F_ZR, F_ZR, F_NONE, false, &wrote);
        wflags = S;
    } else if ((x & 0x1F200000u) == 0x0B000000u) {         // add/sub shifted register
        if (((x >> 22) & 3) == 3) return false;
        bool S = (x >> 29) & 1;
        out = remap(e, x, F_ZR, F_ZR, F_ZR, F_NONE, false, &wrote);
        wflags = S;
    } else if ((x & 0x1FE00000u) == 0x0B200000u) {         // add/sub extended register
        bool S = (x >> 29) & 1;
        out = remap(e, x, S ? F_ZR : F_SP, F_SP, F_ZR, F_NONE, false, &wrote);
        wflags = S;
    } else if ((x & 0x1FE0FC00u) == 0x1A000000u) {         // adc/sbc
        bool S = (x >> 29) & 1;
        rflags = true;
        wflags = S;
        out = remap(e, x, F_ZR, F_ZR, F_ZR, F_NONE, false, &wrote);
    } else if ((x & 0x1FE00800u) == 0x1A800000u) {         // conditional select
        rflags = true;
        out = remap(e, x, F_ZR, F_ZR, F_ZR, F_NONE, false, &wrote);
    } else if ((x & 0x1FE00010u) == 0x1A400000u) {         // conditional compare (reg / imm)
        rflags = wflags = true;
        bool immf = (x >> 11) & 1;
        out = remap(e, x, F_NONE, F_ZR, immf ? F_NONE : F_ZR, F_NONE, false, &wrote);
    } else if ((x & 0x7FE00000u) == 0x1AC00000u) {         // dp 2 source (32 and 64 bit)
        unsigned op = (x >> 10) & 0x3f;
        // udiv sdiv lslv lsrv asrv rorv crc32*; not pacga/subp/etc.
        if (!((op >= 2 && op <= 3) || (op >= 8 && op <= 11) || (op >= 16 && op <= 23))) return false;
        out = remap(e, x, F_ZR, F_ZR, F_ZR, F_NONE, false, &wrote);
    } else if ((x & 0x7FFF0000u) == 0x5AC00000u) {         // dp 1 source (opcode2 = 0: rbit/rev/clz/cls)
        if (((x >> 10) & 0x3f) > 5) return false;
        out = remap(e, x, F_ZR, F_ZR, F_NONE, F_NONE, false, &wrote);
    } else if ((x & 0x1F000000u) == 0x1B000000u) {         // data-processing 3 source
        out = remap(e, x, F_ZR, F_ZR, F_ZR, F_ZR, false, &wrote);
    } else if ((x & 0x0A000000u) == 0x08000000u) {         // loads and stores
        if ((x & 0x3F000000u) == 0x08000000u) return false;   // exclusives / atomics
        *has_mem = true;
        return emit_ldst(e, x, spos);
    } else {
        return false;
    }
    if (rflags && !e->hflags) load_flags(e);
    put(e, out);
    if (wflags) {
        store_flags(e);
        e->hflags = true;
    }
    if (wrote >= 0) writeback(e, wrote);
    return true;
}

// Loop blocks, for jit_crash_sync(): inside [lo, hi) guest register promo_g[i]
// lives in host register promo_h[i] and donor_g[i] is in cpu_state.
struct loopmap { uintptr_t lo, hi; uint8_t n; int8_t g[8], d[8], h[8]; };
static struct loopmap *loopmaps;
static unsigned nloopmaps, cap_loopmaps;
static pthread_mutex_t loopmap_lock = PTHREAD_MUTEX_INITIALIZER;

static void loopmap_add(uintptr_t lo, uintptr_t hi, const struct em *e) {
    pthread_mutex_lock(&loopmap_lock);
    if (nloopmaps == cap_loopmaps) {
        unsigned cap = cap_loopmaps ? 2 * cap_loopmaps : 1024;
        struct loopmap *m = realloc(loopmaps, cap * sizeof(*m));
        if (!m) { pthread_mutex_unlock(&loopmap_lock); return; }
        loopmaps = m;
        cap_loopmaps = cap;
    }
    struct loopmap *m = &loopmaps[nloopmaps++];
    m->lo = lo; m->hi = hi; m->n = e->npromo;
    memcpy(m->g, e->promo_g, 8); memcpy(m->d, e->donor_g, 8); memcpy(m->h, e->promo_h, 8);
    pthread_mutex_unlock(&loopmap_lock);
}

static struct rec_seg *record_segment(const struct em *e) {
    struct rec_seg *r = calloc(1, sizeof(*r));
    if (!r)
        return NULL;
    r->words = malloc(4 * e->n);
    r->rel = malloc(sizeof(*r->rel) * (e->nrel + 1));
    if (!r->words || !r->rel) {
        free(r->words);
        free(r->rel);
        free(r);
        return NULL;
    }
    r->nwords = e->n;
    memcpy(r->words, e->buf, 4 * e->n);
    r->nrel = e->nrel;
    memcpy(r->rel, e->rel, sizeof(*r->rel) * e->nrel);
    r->link_at[0] = e->link_at[0];
    r->link_at[1] = e->link_at[1];
    r->loop = e->loop;
    r->loop_head = e->loop_head;
    r->body_end = e->body_end;
    r->npromo = e->npromo;
    memcpy(r->promo_g, e->promo_g, 8);
    memcpy(r->donor_g, e->donor_g, 8);
    memcpy(r->promo_h, e->promo_h, 8);
    return r;
}

// Resolve stub branches and append stubs: x28 = &code[spos + 1]; b code[spos].
// Returns the installed code, NULL if the segment could not be installed.
static uint8_t *finish_native(struct em *e, struct fiber_block *b, uintptr_t orig_first, unsigned first_pos) {
    // one stub per distinct stream position
    unsigned stub_pos[256], stub_at[256], ns = 0;
    for (unsigned i = 0; i < e->nfix; i++) {
        if (e->fix[i].kind == 4)
            continue;
        unsigned s;
        for (s = 0; s < ns && stub_pos[s] != e->fix[i].spos; s++) {}
        if (s == ns) {
            if (ns == 256) return NULL;
            stub_pos[ns++] = e->fix[i].spos;
        }
    }
    for (unsigned s = 0; s < ns; s++) {
        stub_at[s] = e->n;
        if (e->loop) emit_canon(e);
        if (n_pinned) set_last_block(e, b);
        set_pc_const(e, b->addr);   // linked entries skip the pc store
        mov_block_addr(e, 28, code_off(stub_pos[s] + 1));
        // x8 is free at a gadget boundary. The first unit's gadget is now
        // replaced by this code; every other one is still in the stream.
        if (pic_on && stub_pos[s] != first_pos) {
            ldr_block(e, 8, code_off(stub_pos[s]));
            emit_exit_x8(e);
        } else {
            emit_exit(e, stub_pos[s] == first_pos ? orig_first : b->code[stub_pos[s]]);
        }
    }
    uint8_t *dst = e->fail ? NULL : region_alloc(e->n * 4);
    if (!dst)
        return NULL;
    for (unsigned i = 0; i < e->nfix; i++) {
        if (e->fix[i].kind == 4) {   // absolute target (shared exit stub)
            int64_t d = (int64_t) e->fix[i].abs - (int64_t) ((uintptr_t) dst + 4 * e->fix[i].at);
            if (!fits(d >> 2, 26)) return NULL;
            e->buf[e->fix[i].at] = ENC_B(d);
            if (e->record) {
                unsigned n = e->n;
                e->n = e->fix[i].at;
                add_rel(e, REL_EXIT, 0);
                e->n = n;
                if (e->fail) return NULL;
            }
            continue;
        }
        unsigned s;
        for (s = 0; stub_pos[s] != e->fix[i].spos; s++) {}
        int64_t d = 4 * ((int64_t) stub_at[s] - (int64_t) e->fix[i].at);
        uint32_t enc;
        if (e->fix[i].kind == 0) enc = ENC_B(d);
        else {
            if (!fits(d >> 2, 19)) return NULL;
            enc = 0x54000000u | ((uint32_t) (d >> 2) & 0x7ffff) << 5 | (e->fix[i].kind == 1 ? 1u : 8u);  // ne / hi
        }
        e->buf[e->fix[i].at] = enc;
    }
    install_code(dst, e->buf, e->n);
    if (e->loop) {
        b->native_loop = (uint32_t *) (dst + 4 * e->loop_head);
        loopmap_add((uintptr_t) b->native_loop, (uintptr_t) dst + 4 * e->body_end, e);
        atomic_fetch_add_explicit(&st_loops, 1, memory_order_relaxed);
    }
    for (int i = 0; i < 2; i++)
        if (e->link_at[i] >= 0) {
            b->native_link[i] = (uint32_t *) (dst + 4 * e->link_at[i]);
            b->native_link_orig[i] = e->buf[e->link_at[i]];
        }
    if (e->record)
        e->last_rec = record_segment(e);
    b->code[first_pos] = (unsigned long) dst;
    if (first_pos == 0 && n_pinned)
        __atomic_store_n(&b->native_entry, (uintptr_t) dst + entry_off(), __ATOMIC_RELEASE);
    return dst;
}



// *site: expected -> v, atomically; false if it held something else.
static bool claim_u64(uint64_t *site, uint64_t expected, uint64_t v) {
    if (__atomic_load_n(site, __ATOMIC_ACQUIRE) != expected)
        return false;
#if TARGET_OS_OSX
    bool was_writable = jit_writable;
#endif
    jit_write_begin();
    bool ok = __atomic_compare_exchange_n((uint64_t *) ((uint8_t *) site + rw_delta), &expected, v, false,
                                          __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
#if TARGET_OS_OSX
    if (!was_writable)
        jit_exec_ready();
#endif
    return ok;
}

// Where a direct link from `from` to `to` goes: the hot entry within one
// module context, else the cross-module entry (which loads x29; x9 holds the
// slot, i.e. to's &code[0]). 0: `to` has no native code.

static uintptr_t link_target(struct fiber_block *from, struct fiber_block *to) {
    uintptr_t entry = __atomic_load_n(&to->native_entry, __ATOMIC_ACQUIRE);
    if (!entry)
        return 0;
    return to->jit_ctx == from->jit_ctx ? entry + 4 : entry;
}

// PIC code is shared: every block with the same translation (in this and
// other address spaces) has the same direct-link site. So deciding a chain
// and claiming the site are one step: a successor may be chained only once
// the site's literal names its code, since any block that claims the site
// later would take the branch in every chain made while it was unlinked.
bool jit_chain_ok(struct fiber_block *from, int i, struct fiber_block *to) {
    if (!pic_on || !from->native_link[i])
        return true;
    uint32_t *site = from->native_link[i];
    bool writable = (uintptr_t) site >= (uintptr_t) region && (uintptr_t) site < (uintptr_t) region + REGION_SIZE;
    uint64_t lit = __atomic_load_n((uint64_t *) (site + 1), __ATOMIC_ACQUIRE);
    if (lit == LINK_UNSET && (!writable || link_off))
        return true;   // never linked (read-only AOT code, or links off): the branch falls through
    uintptr_t target = link_target(from, to);
    if (!target)
        return false;
    int64_t d = (int64_t) target - (int64_t) (uintptr_t) site;
    if (lit == LINK_UNSET) {
        if (fits(d >> 2, 26) && claim_u64((uint64_t *) (site + 1), LINK_UNSET, (uint64_t) d)) {
            patch_insn(site, ENC_B(d));
            atomic_fetch_add_explicit(&st_links, 1, memory_order_relaxed);
            return true;
        }
        lit = __atomic_load_n((uint64_t *) (site + 1), __ATOMIC_ACQUIRE);
    }
    return lit == (uint64_t) d;
}

bool jit_chain_refused(struct fiber_block *from, int i, struct fiber_block *to) {
    // The slot stays unchained (bit 63, the guest address below: the gadgets
    // and the run loop read it as before); bit 62 tells from's native code to
    // enter the successor recorded in from's context slot instead.
    struct jit_ctx *ctx = from->jit_ctx;
    if (!pic_on || !ctx || !from->native_link[i] || !from->jump_ip[i] ||
        __atomic_load_n(&ctx->slot[from->jit_idx].blk, __ATOMIC_ACQUIRE) != from)
        return false;
    __atomic_store_n(&ctx_far(ctx, from->jit_idx)->alt[i], (uintptr_t) to->code, __ATOMIC_RELEASE);
    __atomic_store_n(from->jump_ip[i], *from->jump_ip[i] | 1UL << 62, __ATOMIC_RELEASE);
    return true;
}

void jit_link(struct fiber_block *from, int i, struct fiber_block *to) {
    if (!from->native_link[i] || link_off)
        return;
    if (pic_on)
        return;   // jit_chain_ok() claimed the site, if it could, as it allowed the chain
    uintptr_t entry = to->code[0], target = entry + 4 * (uintptr_t) prologue_words;
    if (from->native_loop && i == 0) {
        // back edge of a loop block: only into its own loop head (registers
        // promoted); a replacement block for the same address gets the slow path
        if (to != from)
            return;
        target = (uintptr_t) from->native_loop;
    } else if (entry < (uintptr_t) region || entry >= (uintptr_t) region + REGION_SIZE) {
        return;   // successor starts in the gadget world
    }
    int64_t d = (int64_t) target - (int64_t) (uintptr_t) from->native_link[i];
    if (!fits(d >> 2, 26))
        return;
    patch_insn(from->native_link[i], ENC_B(d));
    atomic_fetch_add_explicit(&st_links, 1, memory_order_relaxed);
}

void jit_unlink(struct fiber_block *from, int i) {
    if (pic_on) {
        // PIC links are guarded, never undone; a refused successor goes away
        struct jit_ctx *ctx = from->jit_ctx;
        if (ctx && __atomic_load_n(&ctx->slot[from->jit_idx].blk, __ATOMIC_ACQUIRE) == from)
            __atomic_store_n(&ctx_far(ctx, from->jit_idx)->alt[i], 0, __ATOMIC_RELEASE);
        return;
    }
    if (!from->native_link[i])
        return;
    patch_insn(from->native_link[i], from->native_link_orig[i]);
}

// ---- block ends ----

// Patch the branch at `at` (b.cond / cbz / tbz / b) to jump to the current end.
static void patch_here(struct em *e, unsigned at) {
    int64_t d = (int64_t) e->n - (int64_t) at;   // words
    uint32_t x = e->buf[at];
    if ((x & 0x7E000000u) == 0x36000000u) {                                     // tbz/tbnz
        if (!fits(d, 14)) { e->fail = true; return; }
        e->buf[at] = (x & ~(0x3fffu << 5)) | ((uint32_t) d & 0x3fff) << 5;
    } else if ((x & 0x7C000000u) == 0x14000000u) {                              // b
        e->buf[at] = (x & 0xFC000000u) | ((uint32_t) d & 0x3ffffff);
    } else {                                                                    // b.cond, cbz
        if (!fits(d, 19)) { e->fail = true; return; }
        e->buf[at] = (x & ~(0x7ffffu << 5)) | ((uint32_t) d & 0x7ffff) << 5;
    }
}

// x28 = successor's code stream (block->code or a mid-block slot). Dispatch
// into it: native entries are entered hot (skipping the pinned-register
// prologue), anything else gets the pinned registers stored first.
static void emit_native_dispatch_ex(struct em *e, bool block_start);
static void emit_native_dispatch(struct em *e) { emit_native_dispatch_ex(e, false); }

// block_start: x28 points at a successor's code[0]; if that block is not
// native, record it as last_block for the run loop (native blocks set
// last_block themselves on the way out).
static void emit_native_dispatch_ex(struct em *e, bool block_start) {
    put(e, 0xF8408788u);                                         // ldr x8, [x28], #8
    if (n_pinned == 0) {
        put(e, 0xD61F0100u);                                     // br x8
        return;
    }
    mov_host_ptr(e, 10, region);
    put(e, 0xCB0A010Au);                                         // sub x10, x8, x10
    put(e, 0xD35BFD4Au);                                         // lsr x10, x10, #27  (region < 128MB)
    unsigned cold = e->n; put(e, 0xB500000Au);                   // cbnz x10, cold
    if (pic_on && block_start) {
        put(e, 0x91000108u | ((uint32_t) (4 * (prologue_words - 1)) << 10));   // add x8, x8, #cross-module entry
        put(e, 0xD1002389u);                                     // sub x9, x28, #8
        enter_native(e);
    } else {
        put(e, 0x91000108u | ((uint32_t) (4 * prologue_words) << 10));   // add x8, x8, #hot entry
        put(e, 0xD61F0100u);                                     // br x8
    }
    patch_here(e, cold);
    if (block_start) {
        put(e, 0xD1000000u | ((FIBER_BLOCK_code + 8) << 10) | (28u << 5) | 10);   // sub x10, x28, #code+8
        if (n_pinned)
            put(e, 0xF9000000u | ((LOCAL_last_block / 8) << 10) | (1u << 5) | 10);   // str x10, [x1, #last_block]
        put(e, 0xF9400000u | ((FIBER_BLOCK_addr / 8) << 10) | (10u << 5) | 11);  // ldr x11, [x10, #addr]
        put(e, 0xF9000000u | ((CPU_pc / 8) << 10) | (1u << 5) | 11);             // str x11, [x1, #pc]
    }
    emit_exit_x8(e);                                             // not native: store, br x8
}

// Leave the block through code-stream slot `slot` exactly like inline_chain:
// chained -> count the cycle, check poke, set last_block, dispatch into the
// successor; unchained -> hand the guest PC back to the run loop.
// self_loop: the taken back edge of a loop block (e->loop). Linked, it goes to
// the loop head with the promoted registers in place; every other way out
// swaps them back first.
static void emit_refused_edge(struct em *e, unsigned generic, int link);

static void emit_chain_ex(struct em *e, struct fiber_block *b, unsigned long *slot, int link, bool self_loop) {
    bool reg_cycle = n_pinned > 0;
    unsigned generic = 0;   // PIC: chained successor in x9, entered natively or through its gadgets
    unsigned slot_off = (unsigned) ((char *) slot - (char *) b);
    int boff = FIBER_BLOCK_addr - FIBER_BLOCK_code;
    unsigned to_poke1, to_poke2 = 0;
    if (reg_cycle) {
        // Cycle counter lives in w15; poke is checked when it wraps. The
        // count comes first so a direct link needs nothing but the branch:
        // jit_link() turns the nop into `b <successor hot entry>`, skipping
        // the slot load and the pc store (every native exit stores pc).
        put(e, 0x110005EFu);                                                     // add w15, w15, #1
        put(e, 0x720025FFu);                                                     // tst w15, #0x3ff
        to_poke1 = e->n; put(e, 0x54000000u);                                    // b.eq poke
        if (!pic_on) {
            if (link >= 0 && link < 2) e->link_at[link] = (int) e->n;
            put(e, NOP);
        } else if (self_loop) {
            // Still chained to itself (an invalidation restores the slot):
            // straight back to the loop head, promoted registers in place.
            ldr_ctx_block(e, 10);
            ldr_off(e, 9, 10, slot_off);
            put(e, 0x91000000u | (FIBER_BLOCK_code << 10) | (10u << 5) | 10);  // add x10, x10, #code
            put(e, 0xEB0A013Fu);                                                 // cmp x9, x10
            put(e, 0x54000000u | ((uint32_t) (e->loop_head - (int) e->n) & 0x7ffff) << 5);   // b.eq loop_head
        }
        if (self_loop) emit_canon(e);
    }
    ldr_block(e, 9, slot_off);
    unsigned to_unch = e->n; put(e, 0xB7F80009u);                 // tbnz x9, #63, unchained
    if (pic_on && reg_cycle) {
        // Direct link: jit_link() turns this branch into `b <successor's hot
        // entry>` (an AOT image has it from the start) and records the target,
        // relative to the branch, in the literal after it. jit_chain_ok() only
        // lets the slot chain to a block entered there, so the branch needs no
        // check of its own; a successor it refuses marks the slot instead
        // (jit_chain_refused(), handled at `unchained` below).
        if (!(e->n & 1))
            put(e, NOP);   // keep the literal 8-byte aligned (segments start 16-aligned)
        unsigned site = e->n; put(e, 0x14000000u);                               // b <link entry> (patched)
        if (link >= 0 && link < 2) e->link_at[link] = (int) site;
        put(e, LINK_UNSET); put(e, 0);                                           // literal: target - branch
        patch_here(e, site);                                                     // unlinked: fall to the next line
        generic = e->n;
        // Not linked yet: the successor's native code, if it has some.
        int ne = (int) offsetof(struct fiber_block, native_entry) - FIBER_BLOCK_code;
        put(e, 0xF8400000u | ((uint32_t) (ne & 0x1ff) << 12) | (9u << 5) | 8);  // ldur x8, [x9, #native_entry-code]
        unsigned slow = e->n; put(e, 0xB4000008u);                               // cbz x8, slow
        enter_native(e);
        patch_here(e, slow);
    }
    put(e, 0xF8400000u | ((uint32_t) (boff & 0x1ff) << 12) | (9u << 5) | 11);   // ldur x11, [x9, #addr-code]
    put(e, 0xF9000000u | ((CPU_pc / 8) << 10) | (1u << 5) | 11);                 // str x11, [x1, #pc]
    if (!reg_cycle) {
        put(e, 0xB9400000u | ((CPU_cycle / 4) << 10) | (1u << 5) | 8);           // ldr w8, [x1, #cycle]
        put(e, 0x11000508u);                                                     // add w8, w8, #1
        put(e, 0xB9000000u | ((CPU_cycle / 4) << 10) | (1u << 5) | 8);           // str w8, [x1, #cycle]
        put(e, 0x7200251Fu);                                                     // tst w8, #0x3ff
        to_poke1 = e->n; put(e, 0x54000000u);                                    // b.eq poke
        put(e, 0xF9400000u | ((CPU_poked_ptr / 8) << 10) | (1u << 5) | 8);       // ldr x8, [x1, #poked_ptr]
        put(e, 0x39400108u);                                                     // ldrb w8, [x8]
        to_poke2 = e->n; put(e, 0x35000008u);                                    // cbnz w8, poke
        put(e, 0xD1000000u | (FIBER_BLOCK_code << 10) | (9u << 5) | 8);          // sub x8, x9, #code
        put(e, 0xF9000000u | ((LOCAL_last_block / 8) << 10) | (1u << 5) | 8);    // str x8, [x1, #last_block]
        // Direct link: jit_link() turns this nop into `b <successor hot entry>`.
        if (!pic_on) {
            if (link >= 0 && link < 2) e->link_at[link] = (int) e->n;
            put(e, NOP);
        }
    }
    put(e, 0xAA0903FCu);                                                         // mov x28, x9
    emit_native_dispatch_ex(e, true);
    patch_here(e, to_poke1);
    if (!reg_cycle) patch_here(e, to_poke2);
    // timer / poke: enter the successor through the run loop
    if (reg_cycle) {
        if (self_loop) emit_canon(e);
        ldr_block(e, 9, slot_off);
        unsigned unch2 = e->n; put(e, 0xB7F80009u);                              // tbnz x9, #63, unchained
        put(e, 0xF8400000u | ((uint32_t) (boff & 0x1ff) << 12) | (9u << 5) | 11);   // ldur x11, [x9, #addr-code]
        put(e, 0xF9000000u | ((CPU_pc / 8) << 10) | (1u << 5) | 11);             // str x11, [x1, #pc]
        put(e, 0xD1000000u | (FIBER_BLOCK_code << 10) | (9u << 5) | 8);          // sub x8, x9, #code
        put(e, 0xF9000000u | ((LOCAL_last_block / 8) << 10) | (1u << 5) | 8);    // str x8, [x1, #last_block]
        put(e, 0xAA0903FCu);                                                     // mov x28, x9
        emit_exit(e, (uintptr_t) jit_fiber_ret);
        patch_here(e, to_unch);
        if (pic_on)
            emit_refused_edge(e, generic, link);   // not after a poke: that goes to the run loop
        patch_here(e, unch2);
    } else {
        put(e, 0xD1000000u | (FIBER_BLOCK_code << 10) | (9u << 5) | 8);          // sub x8, x9, #code
        put(e, 0xF9000000u | ((LOCAL_last_block / 8) << 10) | (1u << 5) | 8);    // str x8, [x1, #last_block]
        put(e, 0xAA0903FCu);                                                     // mov x28, x9
        emit_exit(e, (uintptr_t) jit_fiber_ret);
        patch_here(e, to_unch);
    }
    set_last_block(e, b);   // the run loop chains our slot on its way back
    put(e, 0x9240BC00u | (9u << 5) | 11);                                        // and x11, x9, #0xffffffffffff
    put(e, 0xF9000000u | ((CPU_pc / 8) << 10) | (1u << 5) | 11);                 // str x11, [x1, #pc]
    put(e, 0xAA0903FCu);                                                         // mov x28, x9
    emit_exit(e, (uintptr_t) jit_fiber_ret_chain);
}

static void emit_chain(struct em *e, struct fiber_block *b, unsigned long *slot, int link) {
    emit_chain_ex(e, b, slot, link, false);
}

// ret: same as the RET gadget, but with its own dispatch branch per site.
static void emit_ret(struct em *e, struct fiber_block *b, int rn) {
    int h = rn == 31 ? 31 : hreg(e, rn);
    put(e, 0x9240BC00u | ((uint32_t) h << 5) | 10);                              // and x10, xh, #0xffffffffffff
    put(e, 0xF9000000u | ((CPU_pc / 8) << 10) | (1u << 5) | 10);                 // str x10, [x1, #pc]
    put(e, 0xD3443D4Bu);                                                         // ubfx x11, x10, #4, #12
    put(e, 0x91000000u | (LOCAL_ret_cache << 10) | (1u << 5) | 12);              // add x12, x1, #ret_cache
    put(e, 0xF86B798Cu);                                                         // ldr x12, [x12, x11, lsl #3]
    unsigned miss1 = e->n; put(e, 0xB400000Cu);                                  // cbz x12, miss
    put(e, 0xF9400589u);                                                         // ldr x9, [x12, #8]
    put(e, 0xEB09015Fu);                                                         // cmp x10, x9
    unsigned miss2 = e->n; put(e, 0x54000001u);                                  // b.ne miss
    put(e, 0xF940099Cu);                                                         // ldr x28, [x12, #16]
    unsigned unch = e->n; put(e, 0xB7F8001Cu);                                   // tbnz x28, #63, unchained
    if (n_pinned) {
        // successor native: straight into its hot entry
        int ne = (int) offsetof(struct fiber_block, native_entry) - (int) offsetof(struct fiber_block, code);
        put(e, 0xF8400000u | ((uint32_t) (ne & 0x1ff) << 12) | (28u << 5) | 8);  // ldur x8, [x28, #native_entry-code]
        unsigned slow = e->n; put(e, 0xB4000008u);                               // cbz x8, slow
        if (pic_on) {
            put(e, 0xAA1C03E9u);                                                 // mov x9, x28
            enter_native(e);
        } else {
            put(e, 0xD61F0100u);                                                 // br x8
        }
        patch_here(e, slow);
    }
    if (!n_pinned) {
        put(e, 0xD1000000u | (FIBER_BLOCK_code << 10) | (28u << 5) | 8);         // sub x8, x28, #code
        put(e, 0xF9000000u | ((LOCAL_last_block / 8) << 10) | (1u << 5) | 8);    // str x8, [x1, #last_block]
    }
    emit_native_dispatch_ex(e, true);
    patch_here(e, unch);
    put(e, 0xF9400188u);                                                         // ldr x8, [x12]
    put(e, 0xF9000000u | ((LOCAL_last_block / 8) << 10) | (1u << 5) | 8);        // str x8, [x1, #last_block]
    emit_exit(e, (uintptr_t) jit_fiber_ret);
    patch_here(e, miss1);
    patch_here(e, miss2);
    set_last_block(e, b);
    emit_exit(e, (uintptr_t) jit_fiber_ret);
}

// br / blr: look the target up in the per-thread block cache like the run
// loop does (same hash, same staleness check) and dispatch straight into the
// block on a hit; otherwise hand the guest PC back to the run loop.
// xd = x2 + off
static void emit_add_x2(struct em *e, int rd, size_t off) {
    if (off < (1u << 24) && !(off & 0xfff)) {
        put(e, 0x91400000u | (uint32_t) (off >> 12) << 10 | (2u << 5) | (uint32_t) rd);   // add xd, x2, #off>>12, lsl #12
        return;
    }
    mov_imm64(e, rd, off);
    put(e, 0x8B020000u | ((uint32_t) rd << 5) | (uint32_t) rd);                // add xd, xd, x2
}

// ret_cache[(ret >> 4) & 0xfff] = x9 (xbase = &ret_cache, xh = ret). PIC code
// cannot bake the index: it depends on where the module is mapped.
static void store_ret_cache(struct em *e, int h, uint64_t ret, int base) {
    if (!pic_on) {
        put(e, 0xF9000000u | ((uint32_t) ((ret >> 4) & 0xfff) << 10) | ((uint32_t) base << 5) | 9);  // str x9, [xbase, #idx*8]
        return;
    }
    put(e, 0xD3443C00u | ((uint32_t) h << 5) | 11);                                  // ubfx x11, xh, #4, #12
    put(e, 0xF82B7809u | ((uint32_t) base << 5));                                    // str x9, [xbase, x11, lsl #3]
}

// x9 = the block cached for guest address x10 (tlb->block_cache, as
// fiber_ret's lookup keeps it); the three branches go to a miss.
static void emit_block_cache_lookup(struct em *e, unsigned *miss1, unsigned *miss2, unsigned *miss3) {
    // stale cache? tlb->block_cache_gen != mmu->asbestos->invalidate_gen
    int mmu_off = (int) offsetof(struct tlb, mmu) - (int) offsetof(struct tlb, entries);
    put(e, 0xF8400000u | ((uint32_t) (mmu_off & 0x1ff) << 12) | (2u << 5) | 12);  // ldur x12, [x2, #mmu]
    put(e, 0xF9400000u | ((offsetof(struct mmu, asbestos) / 8) << 10) | (12u << 5) | 12);  // ldr x12, [x12, #asbestos]
    put(e, 0xB9400000u | ((offsetof(struct asbestos, invalidate_gen) / 4) << 10) | (12u << 5) | 12);  // ldr w12, [x12, #gen]
    emit_add_x2(e, 11, offsetof(struct tlb, block_cache_gen) - offsetof(struct tlb, entries));
    put(e, 0xB9400000u | (11u << 5) | 11);                                       // ldr w11, [x11]
    put(e, 0x6B0C017Fu);                                                         // cmp w11, w12
    *miss1 = e->n; put(e, 0x54000001u);                                  // b.ne miss
    // x9 = block_cache[(ip ^ (ip >> 12)) & 4095]
    put(e, 0xCA4A314Bu);                                                         // eor x11, x10, x10, lsr #12
    put(e, 0x92402D6Bu);                                                         // and x11, x11, #0xfff
    emit_add_x2(e, 12, offsetof(struct tlb, block_cache) - offsetof(struct tlb, entries));
    put(e, 0xF86B7989u);                                                         // ldr x9, [x12, x11, lsl #3]
    *miss2 = e->n; put(e, 0xB4000009u);                                  // cbz x9, miss
    put(e, 0xF9400000u | ((FIBER_BLOCK_addr / 8) << 10) | (9u << 5) | 12);       // ldr x12, [x9, #addr]
    put(e, 0xEB0A019Fu);                                                         // cmp x12, x10
    *miss3 = e->n; put(e, 0x54000001u);                                  // b.ne miss
}

// PIC, at a block end's `unchained` exit with the slot in x9: a slot
// jit_chain_refused() marked (bit 62) has a successor whose native code is
// not the one the direct link goes to, recorded in this translation's
// context slot (alt[link]). Enter it (natively or through its gadgets, at
// `generic`); without one, fall through to the exit.
static void emit_refused_edge(struct em *e, unsigned generic, int link) {
    if (link < 0 || link > 1)
        return;
    unsigned plain = e->n; put(e, 0xB6F00009u);                              // tbz x9, #62, plain
    ldr_far(e, 10, far_off(e->idx, offsetof(struct jit_far, alt) + 8 * (unsigned) link));   // x10 = far.alt[link]
    unsigned none = e->n; put(e, 0xB400000Au);                               // cbz x10, plain
    put(e, 0xAA0A03E9u);                                                     // mov x9, x10
    put(e, ENC_B(4 * ((int64_t) generic - (int64_t) e->n)));                  // b generic
    patch_here(e, plain); patch_here(e, none);
}

static void emit_indirect(struct em *e, struct fiber_block *b, const struct jit_unit *u, int rn, bool link, uint64_t ret) {
    int h = rn == 31 ? 31 : hreg(e, rn);
    put(e, 0x9240BC00u | ((uint32_t) h << 5) | 10);                              // and x10, xh, #0xffffffffffff
    if (link) {
        int h30 = hreg(e, 30);
        mov_guest(e, h30, ret, 11);
        writeback(e, 30);
        mov_block_addr(e, 9, code_off(u->start + 1));      // BLR params: [self][ret][cont][rn]
        put(e, 0x91000000u | (LOCAL_ret_cache << 10) | (1u << 5) | 12);          // add x12, x1, #ret_cache
        store_ret_cache(e, h30, ret, 12);
    }
    put(e, 0xF9000000u | ((CPU_pc / 8) << 10) | (1u << 5) | 10);                 // str x10, [x1, #pc]
    unsigned miss1, miss2, miss3;
    emit_block_cache_lookup(e, &miss1, &miss2, &miss3);
    // hit: same bookkeeping as a chained branch
    unsigned miss4, miss5;
    if (n_pinned) {
        put(e, 0x110005EFu);                                                     // add w15, w15, #1
        put(e, 0x720025FFu);                                                     // tst w15, #0x3ff
        miss4 = e->n; put(e, 0x54000000u);                                       // b.eq miss (timer/poke)
        miss5 = miss4;
    } else {
        put(e, 0xB9400000u | ((CPU_cycle / 4) << 10) | (1u << 5) | 8);           // ldr w8, [x1, #cycle]
        put(e, 0x11000508u);                                                     // add w8, w8, #1
        put(e, 0xB9000000u | ((CPU_cycle / 4) << 10) | (1u << 5) | 8);           // str w8, [x1, #cycle]
        put(e, 0x7200251Fu);                                                     // tst w8, #0x3ff
        miss4 = e->n; put(e, 0x54000000u);                                       // b.eq miss (timer)
        put(e, 0xF9400000u | ((CPU_poked_ptr / 8) << 10) | (1u << 5) | 8);       // ldr x8, [x1, #poked_ptr]
        put(e, 0x39400108u);                                                     // ldrb w8, [x8]
        miss5 = e->n; put(e, 0x35000008u);                                       // cbnz w8, miss
    }
    if (!n_pinned)
        put(e, 0xF9000000u | ((LOCAL_last_block / 8) << 10) | (1u << 5) | 9);    // str x9, [x1, #last_block]
    if (n_pinned) {
        put(e, 0xF9400000u | ((offsetof(struct fiber_block, native_entry) / 8) << 10) | (9u << 5) | 8);  // ldr x8, [x9, #native_entry]
        unsigned slow = e->n; put(e, 0xB4000008u);                               // cbz x8, slow
        if (pic_on) {
            put(e, 0x91000000u | (FIBER_BLOCK_code << 10) | (9u << 5) | 9);      // add x9, x9, #code
            enter_native(e);
        } else {
            put(e, 0xD61F0100u);                                                 // br x8
        }
        patch_here(e, slow);
    }
    put(e, 0x91000000u | (FIBER_BLOCK_code << 10) | (9u << 5) | 28);             // add x28, x9, #code
    emit_native_dispatch_ex(e, true);
    patch_here(e, miss1); patch_here(e, miss2); patch_here(e, miss3);
    patch_here(e, miss4);
    if (miss5 != miss4) patch_here(e, miss5);
    set_last_block(e, b);
    emit_exit(e, (uintptr_t) jit_fiber_ret);
}

static bool is_pure_const(uint32_t x) {
    return (x & 0x1F000000u) == 0x10000000u ||                       // adr/adrp
           ((x & 0x1F800000u) == 0x12800000u && ((x >> 29) & 3) != 3 && ((x >> 29) & 3) != 1);  // movn/movz
}

// Translate the block-ending unit (optional leading instruction + branch).
static bool emit_block_end(struct em *e, struct fiber_block *b, const struct jit_unit *u) {
    if (u->n == 0 || u->follow)
        return false;
    e->locked = 0;
    for (unsigned k = 0; k + 1 < u->n; k++) {
        bool has_mem;
        if (!emit_insn(e, u->w[k], u->pc + 4 * k, u->start, &has_mem) || has_mem || e->fail)
            return false;
    }
    uint32_t x = u->w[u->n - 1];
    uint64_t pc = u->pc + 4 * (u->n - 1);
    e->locked = 0;
    if (e->loop && ((x & 0xFFFFFC1Fu) == 0xD65F0000u || (x & 0xFFFFFC1Fu) == 0xD61F0000u || (x & 0xFFFFFC1Fu) == 0xD63F0000u))
        return false;
    if ((x & 0xFFFFFC1Fu) == 0xD65F0000u) {                                 // ret
        emit_ret(e, b, (x >> 5) & 31);
        return !e->fail;
    }
    if ((x & 0xFFFFFC1Fu) == 0xD61F0000u || (x & 0xFFFFFC1Fu) == 0xD63F0000u) {   // br / blr
        emit_indirect(e, b, u, (x >> 5) & 31, (x & 0x00200000u) != 0, pc + 4);
        return !e->fail;
    }
    if ((x & 0xFC000000u) == 0x14000000u) {                                 // b
        if (!b->jump_ip[0]) return false;
        emit_chain_ex(e, b, b->jump_ip[0], 0, e->loop);
        return !e->fail;
    }
    if ((x & 0xFC000000u) == 0x94000000u) {                                 // bl
        if (!b->jump_ip[1] || e->loop) return false;
        uint64_t ret = pc + 4;
        int h = hreg(e, 30);
        mov_guest(e, h, ret, 11);
        writeback(e, 30);
        // ret_cache[(ret >> 4) & 0xfff] = &BL params (what the RET gadget expects)
        mov_block_addr(e, 9, code_off(u->start + 1));
        put(e, 0x91000000u | (LOCAL_ret_cache << 10) | (1u << 5) | 10);          // add x10, x1, #ret_cache
        store_ret_cache(e, h, ret, 10);
        emit_chain(e, b, b->jump_ip[1], 1);
        return !e->fail;
    }
    bool is_bcond = (x & 0xFF000010u) == 0x54000000u;
    bool is_cbz = (x & 0x7E000000u) == 0x34000000u;
    bool is_tbz = (x & 0x7E000000u) == 0x36000000u;
    if (!is_bcond && !is_cbz && !is_tbz)
        return false;
    if (!b->jump_ip[0] || !b->jump_ip[1])
        return false;
    unsigned to_taken;
    if (is_bcond) {
        unsigned cond = x & 15;
        if (cond >= 14) {   // al / nv
            emit_chain_ex(e, b, b->jump_ip[0], 0, e->loop);
            return !e->fail;
        }
        if (!e->hflags) load_flags(e);
        to_taken = e->n; put(e, 0x54000000u | cond);                        // b.cond taken
    } else {
        int rt = x & 31;
        if (rt == 31) return false;
        int h = hreg(e, rt);
        to_taken = e->n;
        put(e, (x & 0xFF000000u & ~(0u)) | (uint32_t) h);                   // cbz/cbnz/tbz/tbnz (same bit/size), offset patched
        if (is_tbz) e->buf[to_taken] = (x & 0xFFF80000u) | (uint32_t) h;    // keep b5/b40
        else e->buf[to_taken] = (x & 0xFF000000u) | (uint32_t) h;
    }
    if (e->loop) emit_canon(e);
    emit_chain(e, b, b->jump_ip[1], 1);    // not taken: fall through
    patch_here(e, to_taken);
    emit_chain_ex(e, b, b->jump_ip[0], 0, e->loop);    // taken
    return !e->fail;
}

// Emit one segment starting at unit ui (not installed). Returns the next unit.
static unsigned emit_segment(struct em *e, struct fiber_block *b, const struct jit_units *U, unsigned ui,
                             bool *ended_out, unsigned *covered_out, unsigned *real_out) {
    memset(e->host, -1, sizeof(e->host));
    memset(e->owner, -1, sizeof(e->owner));
    memset(e->uses, 0, sizeof(e->uses));
    memcpy(e->pin, pin_of, sizeof(e->pin));
    for (unsigned i = 0; e->loop && i < e->npromo; i++) {
        e->pin[e->donor_g[i]] = -1;
        e->pin[e->promo_g[i]] = e->promo_h[i];
    }
    e->npool = 0;
    e->victim = 0;
    e->n = 0;
    e->nfix = 0;
    e->nrel = 0;
    e->link_at[0] = e->link_at[1] = -1;
    e->fail = false;
    e->hflags = false;
    e->body_end = -1;
    unsigned covered = 0, real = 0;   // real: units that emitted code
    bool ended = false;
    e->b = b;
    // Cold entry: load pinned; hot entry = +prologue_words. PIC: x9 = &code[0]
    // from x28 (= &code[first + 1] here) first, and the cross-module entry --
    // the last prologue word -- loads x29 from the block.
    if (pic_on) {
        unsigned k = 8 * (U->u[ui].start + 1);
        put(e, 0xD1400000u | (k >> 12) << 10 | (28u << 5) | 9);                // sub x9, x28, #k>>12, lsl #12
        put(e, 0xD1000000u | (k & 0xfff) << 10 | (9u << 5) | 9);                // sub x9, x9, #k&0xfff
    }
    load_pinned(e);
    if (pic_on) {
        int co = (int) offsetof(struct fiber_block, jit_ctx) - FIBER_BLOCK_code;
        put(e, 0xF8400000u | ((uint32_t) (co & 0x1ff) << 12) | (9u << 5) | FP);   // ldur x29, [x9, #jit_ctx-code]
    }
    if (e->loop) {
        emit_promote(e);
        e->loop_head = (int) e->n;
    }
    while (ui < U->n) {
        const struct jit_unit *u = &U->u[ui];
        if (u->last) {
            unsigned n0 = e->n, nfix0 = e->nfix, nrel0 = e->nrel;
            uint8_t npool0 = e->npool, victim0 = e->victim;
            bool hf0 = e->hflags;
            int8_t host0[33], owner0[32];
            memcpy(host0, e->host, sizeof(host0));
            memcpy(owner0, e->owner, sizeof(owner0));
            e->body_end = (int) e->n;
            if (emit_block_end(e, b, u) && !e->fail) {
                covered++;
                real++;
                ui++;
                ended = true;
            } else {
                e->link_at[0] = e->link_at[1] = -1;   // only the block end sets them
                e->n = n0; e->nfix = nfix0; e->nrel = nrel0; e->npool = npool0; e->fail = false; e->hflags = hf0;
                e->victim = victim0;
                memcpy(e->host, host0, sizeof(host0));
                memcpy(e->owner, owner0, sizeof(owner0));
            }
            break;
        }
        if (!u->follow && (u->n == 0 || u->start == u->end))
            break;
        // snapshot for rollback
        unsigned n0 = e->n, nfix0 = e->nfix, nrel0 = e->nrel;
        uint8_t npool0 = e->npool, victim0 = e->victim;
        bool hf0 = e->hflags;
        int8_t host0[33], owner0[32];
        memcpy(host0, e->host, sizeof(host0));
        memcpy(owner0, e->owner, sizeof(owner0));
        bool ok = true, pure = true;
        for (unsigned k = 0; ok && k < u->n && !u->follow; k++) {
            bool has_mem;
            ok = emit_insn(e, u->w[k], u->pc + 4 * k, u->start, &has_mem) && !e->fail;
            // A bailout re-runs the whole unit's gadget, so everything the
            // unit did before the access must be safe to redo.
            if (has_mem && k > 0 && !pure) ok = false;
            pure &= is_pure_const(u->w[k]);
            if (!ok && !has_mem)
                atomic_fetch_add_explicit(&st_unsupported_insns, 1, memory_order_relaxed);
        }
        if (!ok) {
            e->n = n0; e->nfix = nfix0; e->nrel = nrel0; e->npool = npool0; e->fail = false; e->hflags = hf0;
            e->victim = victim0;
            memcpy(e->host, host0, sizeof(host0));
            memcpy(e->owner, owner0, sizeof(owner0));
            break;
        }
        covered++;
        if (!u->follow)
            real++;
        ui++;
    }
    *ended_out = ended;
    *covered_out = covered;
    *real_out = real;
    return ui;
}

// A block whose taken branch goes back to its own start, translated whole:
// pick the promotions from the register use counts of that translation.
static bool plan_loop(struct em *e, struct fiber_block *b, const struct jit_units *U) {
    if (loop_off || !n_pinned || !U->n)
        return false;
    const struct jit_unit *u = &U->u[U->n - 1];
    if (!u->last || !u->n)
        return false;
    uint32_t x = u->w[u->n - 1];
    uint64_t pc = u->pc + 4 * (u->n - 1);
    int64_t off;
    if ((x & 0xFC000000u) == 0x14000000u) off = sext(x & 0x3ffffff, 26);                          // b
    else if ((x & 0xFF000010u) == 0x54000000u || (x & 0x7E000000u) == 0x34000000u)
        off = sext((x >> 5) & 0x7ffff, 19);                                                     // b.cond, cbz
    else if ((x & 0x7E000000u) == 0x36000000u) off = sext((x >> 5) & 0x3fff, 14);              // tbz
    else return false;
    if (pc + 4 * off != b->addr)
        return false;
    // candidates: unpinned registers by use count; donors: unused pinned ones
    int8_t cand[33], donor[33];
    unsigned nc = 0, nd = 0;
    for (int g = 0; g <= G_SP; g++) {
        if (!e->uses[g] && pin_of[g] >= 0) donor[nd++] = (int8_t) g;
        if (e->uses[g] && pin_of[g] < 0) cand[nc++] = (int8_t) g;
    }
    for (unsigned i = 1; i < nc; i++)
        for (unsigned j = i; j > 0 && e->uses[cand[j]] > e->uses[cand[j - 1]]; j--) {
            int8_t t = cand[j]; cand[j] = cand[j - 1]; cand[j - 1] = t;
        }
    unsigned n = nc < nd ? nc : nd;
    if (n > 8) n = 8;
    if (!n)
        return false;
    e->npromo = (uint8_t) n;
    for (unsigned i = 0; i < n; i++) {
        e->promo_g[i] = cand[i];
        e->donor_g[i] = donor[i];
        e->promo_h[i] = pin_of[donor[i]];
    }
    return true;
}

static void jit_native(struct fiber_block *b, const struct jit_units *U, struct em *e) {
    unsigned ui = 0;
    e->ninst = 0;
    while (ui < U->n) {
        unsigned first = ui, covered, real;
        bool ended;
        e->loop = false;
        e->npromo = 0;
        ui = emit_segment(e, b, U, first, &ended, &covered, &real);
        if (first == 0 && ended && ui == U->n && plan_loop(e, b, U)) {
            e->loop = true;
            ui = emit_segment(e, b, U, first, &ended, &covered, &real);
            if (!ended || ui != U->n) {
                e->loop = false;
                e->npromo = 0;
                ui = emit_segment(e, b, U, first, &ended, &covered, &real);
            }
        }
        if (ended)
            atomic_fetch_add_explicit(&st_block_ends, 1, memory_order_relaxed);
        else if (ui < U->n && U->u[ui].last)
            atomic_fetch_add_explicit(&st_block_end_fail, 1, memory_order_relaxed);
        // A segment of nothing but followed `b` units would be installed in
        // the slot of the unit after them (they own no words) and fall
        // through into that same slot: never install it.
        if (real >= 1) {
            // fall through into the gadget of the next unit
            // ...or into gen_exit when the block was cut short at a page limit
            if (!ended) {
                // Resume at whatever the next slot holds when we get there:
                // the next native segment (installed after this one) or the
                // unit's gadget.
                unsigned next = ui < U->n ? U->u[ui].start : U->u[U->n - 1].end;
                if (n_pinned) set_last_block(e, b);
                set_pc_const(e, b->addr);
                mov_block_addr(e, 28, code_off(next));
                emit_native_dispatch(e);
            }
            uintptr_t orig_first = b->code[U->u[first].start];
            e->last_rec = NULL;
            uint8_t *code = e->fail ? NULL : finish_native(e, b, orig_first, U->u[first].start);
            if (code) {
                jit_segment_installed(b, U, first, ui, code, e->n);
                if (e->ninst < sizeof(e->inst) / sizeof(e->inst[0])) {
                    e->inst[e->ninst].pos = U->u[first].start;
                    e->inst[e->ninst].code = code;
                    e->inst[e->ninst].rec = e->last_rec;
                }
                e->ninst++;
            }
        }
        if (covered == 0)
            ui++;
    }
}


// ---- translation registry (PIC)
//
// PIC code depends only on what goes into the key below, so a block with the
// same key -- in another process, at another base, with another fiber_block
// -- runs the code already generated instead of translating again. This is
// the lookup an AOT image needs, with the image kept in memory; it also stops
// short-lived processes from filling the region with copies of the same
// library code. The key names the module and the block's offset in it, which
// also gives each translation its own index in the module contexts.

struct reg_entry {
    struct reg_entry *next;
    uint64_t hash;
    unsigned nkey, nseg, idx;
    uint32_t *link[2];
    struct { unsigned pos; uint8_t *code; struct rec_seg *rec; } *seg;
    uint32_t key[];
};
#define REG_BUCKETS (1u << 16)
static struct reg_entry **reg_table;
static pthread_mutex_t reg_lock = PTHREAD_MUTEX_INITIALIZER;

// Everything the generated code depends on: the module and offset, page
// offset of the block (adrp), stream layout, each unit's words, pc relative
// to the block, and the gadget at each unit start (a segment's first gadget
// is baked into its stubs).
static unsigned reg_key(const struct fiber_block *b, const struct jit_units *U, int mod, uint64_t off,
                        uint32_t *k) {
    unsigned n = 0;
    k[n++] = (uint32_t) mod;
    k[n++] = (uint32_t) off;
    k[n++] = (uint32_t) (off >> 32);
    k[n++] = (uint32_t) (b->addr & 0xfff);
    k[n++] = U->n;
    for (int i = 0; i < 2; i++)
        k[n++] = b->jump_ip[i] ? (uint32_t) (b->jump_ip[i] - b->code) : ~0u;
    for (unsigned i = 0; i < U->n; i++) {
        const struct jit_unit *u = &U->u[i];
        uint64_t dpc = u->pc - b->addr;
        uint64_t g = u->start < b->used ? b->code[u->start] : 0;
        k[n++] = u->start;
        k[n++] = u->end;
        k[n++] = u->n | (uint32_t) u->follow << 8 | (uint32_t) u->last << 16;
        k[n++] = (uint32_t) dpc;
        k[n++] = (uint32_t) (dpc >> 32);
        k[n++] = u->w[0];
        k[n++] = u->w[1];
        k[n++] = (uint32_t) g;
        k[n++] = (uint32_t) (g >> 32);
    }
    return n;
}
#define REG_KEY_MAX (7 + 9 * JIT_MAX_UNITS)

// Two independent hashes of a key in one pass (the second only checks a hit in
// the AOT memo; its chain runs alongside the first).
static uint64_t reg_hash(const uint32_t *k, unsigned n, uint64_t *h2_out) {
    uint64_t h = 0xcbf29ce484222325ull, h2 = 0x84222325cbf29ce4ull;
    for (unsigned i = 0; i < n; i++) {
        h = (h ^ k[i]) * 0x100000001b3ull;
        h2 = (h2 ^ (k[i] + 0x9e3779b9u)) * 0xff51afd7ed558ccdull;
    }
    *h2_out = h2;
    return h;
}

// ---- module contexts: one per (address space, module, base)

// One per (module mapping, owner): translations made here share one index
// space, and so does each AOT image -- its indices are baked into its code,
// so two images of one module (two versions of the file) need two contexts.
struct ctx_node {
    struct ctx_node *next;
    int mod;
    uint64_t base;
    const void *owner;      // the AOT image, NULL for this process's own translations
    size_t bytes;
    void *map;              // the mapping: ctx_far() of every index, then ctx
    struct jit_ctx *ctx;
};
static pthread_mutex_t ctx_lock = PTHREAD_MUTEX_INITIALIZER;

static struct jit_ctx *ctx_get(struct asbestos *a, int mod, uint64_t base, const struct aot_module *owner) {
    pthread_mutex_lock(&ctx_lock);
    struct ctx_node *n;
    for (n = a->jit_ctxs; n; n = n->next)
        if (n->mod == mod && n->base == base && n->owner == owner)
            break;
    if (!n && (n = malloc(sizeof(*n)))) {
        // Address space only: pages are touched as translations get indices.
        // An image knows how many it uses.
        size_t nidx = owner ? owner->nidx : CTX_MAX;
        size_t far = CTX_FAR * nidx, bytes = far + CTX_BLK + CTX_SLOT * nidx;
        void *p = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        if (p == MAP_FAILED) {
            free(n);
            n = NULL;
        } else {
            n->mod = mod;
            n->base = base;
            n->owner = owner;
            n->bytes = bytes;
            n->map = p;
            n->ctx = (struct jit_ctx *) ((char *) p + far);
            n->ctx->base = base;
            n->next = a->jit_ctxs;
            a->jit_ctxs = n;
        }
    }
    pthread_mutex_unlock(&ctx_lock);
    return n ? n->ctx : NULL;
}

void jit_asbestos_free(struct asbestos *a) {
    pthread_mutex_lock(&ctx_lock);
    struct ctx_node *n = a->jit_ctxs;
    a->jit_ctxs = NULL;
    pthread_mutex_unlock(&ctx_lock);
    while (n) {
        struct ctx_node *next = n->next;
        munmap(n->map, n->bytes);
        free(n);
        n = next;
    }
}

static __thread struct tlb *cm_tlb;   // tlb of the block being compiled (code map below)

// Module, offset and base of the guest code at pc; anonymous code is its own
// module per address, at base 0. -1 if pc is not mapped.
static int jit_locate(addr_t pc, uint64_t *off, uint64_t *base);

// Every unit must come from the same mapping as the block start: constants
// are relative to one base.
static bool one_mapping(const struct jit_units *U, int mod, uint64_t base) {
    addr_t page = PAGE(U->u[0].pc);
    for (unsigned i = 1; i < U->n; i++) {
        if (PAGE(U->u[i].pc) == page)
            continue;
        page = PAGE(U->u[i].pc);
        uint64_t off, b2;
        if (jit_locate(U->u[i].pc, &off, &b2) != mod || b2 != base)
            return false;
    }
    return true;
}

void jit_block_free(struct fiber_block *b) {
    struct jit_ctx *ctx = b->jit_ctx;
    if (!ctx || !pic_on)
        return;
    struct fiber_block *expected = b;
    __atomic_compare_exchange_n(&ctx->slot[b->jit_idx].blk, &expected, NULL, false,
                                __ATOMIC_ACQ_REL, __ATOMIC_RELAXED);
}

// Point a slot at block b, whose translation's constants are relative to base.
// The slot must be free or b's (slot_claim()).
static void slot_set(struct jit_ctx *ctx, unsigned idx, struct fiber_block *b, uint64_t base, uint64_t dbase) {
    b->jit_idx = idx;
    ctx_far(ctx, idx)->alt[0] = ctx_far(ctx, idx)->alt[1] = 0;
    ctx_far(ctx, idx)->dbase = dbase;
    ctx->slot[idx].base = base;
    __atomic_store_n(&ctx->slot[idx].blk, b, __ATOMIC_RELEASE);
}

// Take slot idx for b if it is free (or b's already): a slot has one owner,
// whose native code its translation's direct links were made to reach.
static bool slot_claim(struct jit_ctx *ctx, unsigned idx, struct fiber_block *b, uint64_t base, uint64_t dbase) {
    pthread_mutex_lock(&ctx_lock);
    struct fiber_block *owner = ctx->slot[idx].blk;
    bool ok = !owner || owner == b;
    if (ok)
        slot_set(ctx, idx, b, base, dbase);
    pthread_mutex_unlock(&ctx_lock);
    return ok;
}

static void reg_install(struct fiber_block *b, struct jit_ctx *ctx, const struct reg_entry *r) {
    // Another block of this address space holds the translation (the same
    // code at the same place, still in the cache): b keeps its gadgets.
    if (!slot_claim(ctx, r->idx, b, ctx->base, ctx->base))
        return;
    b->jit_ctx = ctx;
    b->native_link[0] = r->link[0];
    b->native_link[1] = r->link[1];
    for (unsigned i = 0; i < r->nseg; i++) {
        b->code[r->seg[i].pos] = (unsigned long) r->seg[i].code;
        if (r->seg[i].pos == 0 && n_pinned)
            __atomic_store_n(&b->native_entry, (uintptr_t) r->seg[i].code + entry_off(), __ATOMIC_RELEASE);
    }
}

// ISH_JIT_RECORD=<file>: at exit, write every translation of the modules whose
// path contains ISH_JIT_RECORD_MOD (default "ld-musl") for an AOT exporter.
static const char *rec_file, *rec_mod;
static bool module_path_has(int mod, const char *sub);
static bool rec_module(int mod) {
    return rec_file && module_path_has(mod, rec_mod);
}

static unsigned *mod_next_idx;   // per module: indices handed out (reg_lock)
static const struct aot_module **mod_aot;   // per module: its AOT image, if any (reg_lock)
static uint8_t *mod_aot_known;
static uint64_t *mod_same, *mod_family;   // per module: bit i = aot_images[i] is its build / of its family (reg_lock)
static uint64_t *mod_blocks, *mod_hits, *mod_moved;   // per module: blocks translated or looked up, AOT installs,
                                                     // of which from an image of another version
static unsigned mod_next_cap;

// ---- AOT images (asbestos/guest-arm64/aot.h)
//
// Translations of a module recorded from this PIC code and linked into the
// binary. A block whose key matches a recorded translation at the same file
// offset gets that code installed instead of being translated: the key
// covers every guest word, the stream layout and the gadgets, so a match
// means the recorded code is exactly what translating would produce.

static const struct aot_module **aot_images;
static unsigned aot_nimages;
static const struct aot_module *aot_rejected[64];   // registered, made with other conventions or layouts
static unsigned aot_nrejected;
static _Atomic bool aot_off;    // /proc/ish/jit "off": blocks compiled from now on skip the images
static bool aot_family = true;  // images also serve other versions of their module (ISH_AOT_FAMILY=0: no)

// Images register themselves from a constructor in their own object (see
// tools/jit_aot/gen.py), before main: the binary that links an image in is
// the only thing that knows about it.
#define AOT_MAX 64
static const struct aot_module *aot_registered[AOT_MAX];
static unsigned aot_nregistered;

void ish_aot_register(const struct aot_module *m) {
    if (aot_nregistered < AOT_MAX)
        aot_registered[aot_nregistered++] = m;
}

// Bump whenever the code emitted for some guest instruction, stub or exit
// changes: images made before would still pass every other check.
#define JIT_CODE_VERSION 7

// Everything the emitted code bakes in besides the gadgets it names: the
// conventions, the struct layouts it loads from and the TLB / block cache
// hashing. An image made by an ish that differs in any of these would run
// with stale offsets, so it is rejected (never 0: images from before this
// check carry 0 there).
static uint32_t jit_abi(void) {
    const uint64_t v[] = {
        JIT_CODE_VERSION, (uint64_t) prologue_words, entry_off(), (uint64_t) n_pinned, CTX_BLK, CTX_SLOT, CTX_FAR,
        FIBER_BLOCK_code, FIBER_BLOCK_addr, offsetof(struct fiber_block, native_entry),
        offsetof(struct fiber_block, jit_ctx),
        CPU_pc, CPU_cycle, CPU_poked_ptr, CPU_tls_ptr, offsetof(struct cpu_state, sp),
        offsetof(struct cpu_state, regs), offsetof(struct cpu_state, nzcv), offsetof(struct cpu_state, fp),
        LOCAL_last_block, LOCAL_ret_cache,
        sizeof(struct tlb_entry), offsetof(struct tlb_entry, page), offsetof(struct tlb_entry, page_if_writable),
        offsetof(struct tlb_entry, gen), offsetof(struct tlb_entry, data_minus_addr), TLB_BITS, PAGE_BITS,
        offsetof(struct tlb, entries), offsetof(struct tlb, mmu), offsetof(struct tlb, block_cache_gen),
        offsetof(struct tlb, block_cache), sizeof(((struct tlb *) 0)->block_cache),
        offsetof(struct mmu, changes), offsetof(struct mmu, asbestos), offsetof(struct asbestos, invalidate_gen),
        sizeof(struct aot_module), sizeof(struct aot_trans),
    };
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < sizeof(v); i++)
        h = (h ^ ((const uint8_t *) v)[i]) * 16777619u;
    return h ? h : 1;
}

static void aot_init(void) {
    unsigned n = aot_nregistered;
    aot_images = n ? malloc(n * sizeof(*aot_images)) : NULL;
    uint32_t abi = jit_abi();
    for (unsigned i = 0; aot_images && i < n; i++) {
        const struct aot_module *m = aot_registered[i];
        if (m->abi != abi || m->prologue_words != prologue_words || (uintptr_t) m->entry_off != entry_off() ||
            m->n_pinned != n_pinned) {
            fprintf(stderr, "⚠️  AOT image of %s was made by an ish with other conventions or layouts "
                    "(abi %08x, this ish %08x), not used\n", m->path, m->abi, abi);
            if (aot_nrejected < 64)
                aot_rejected[aot_nrejected++] = m;
            continue;
        }
        aot_images[aot_nimages++] = m;
    }
}

enum { IMG_NONE, IMG_SAME, IMG_FAMILY };
static int mod_match(int mod, const struct aot_module *m);

static bool mod_tables_fit(int mod) {   // reg_lock held
    if ((unsigned) mod < mod_next_cap)
        return true;
    unsigned cap = mod_next_cap ? 2 * mod_next_cap : 64;
    while (cap <= (unsigned) mod) cap *= 2;
    unsigned *idx = realloc(mod_next_idx, cap * sizeof(*idx));
    if (idx) mod_next_idx = idx;
    const struct aot_module **img = realloc(mod_aot, cap * sizeof(*img));
    if (img) mod_aot = img;
    uint8_t *known = realloc(mod_aot_known, cap);
    if (known) mod_aot_known = known;
    uint64_t *blocks = realloc(mod_blocks, cap * sizeof(*blocks));
    if (blocks) mod_blocks = blocks;
    uint64_t *hits = realloc(mod_hits, cap * sizeof(*hits));
    if (hits) mod_hits = hits;
    uint64_t *moved = realloc(mod_moved, cap * sizeof(*moved));
    if (moved) mod_moved = moved;
    uint64_t *same = realloc(mod_same, cap * sizeof(*same));
    if (same) mod_same = same;
    uint64_t *family = realloc(mod_family, cap * sizeof(*family));
    if (family) mod_family = family;
    if (!idx || !img || !known || !blocks || !hits || !moved || !same || !family)
        return false;
    memset(mod_next_idx + mod_next_cap, 0, (cap - mod_next_cap) * sizeof(*idx));
    memset(mod_aot + mod_next_cap, 0, (cap - mod_next_cap) * sizeof(*img));
    memset(mod_aot_known + mod_next_cap, 0, cap - mod_next_cap);
    memset(mod_blocks + mod_next_cap, 0, (cap - mod_next_cap) * sizeof(*blocks));
    memset(mod_hits + mod_next_cap, 0, (cap - mod_next_cap) * sizeof(*hits));
    memset(mod_moved + mod_next_cap, 0, (cap - mod_next_cap) * sizeof(*moved));
    memset(mod_same + mod_next_cap, 0, (cap - mod_next_cap) * sizeof(*same));
    memset(mod_family + mod_next_cap, 0, (cap - mod_next_cap) * sizeof(*family));
    mod_next_cap = cap;
    return true;
}

// The first image of a module (reg_lock held), NULL if none.
static const struct aot_module *mod_image(int mod) {
    if (!mod_tables_fit(mod))
        return NULL;
    if (!mod_aot_known[mod]) {
        mod_aot_known[mod] = 1;
        // Several images may fit one module (versions of a file without a
        // build-id share its path); each block matches at most the one
        // recorded from its bytes.
        for (unsigned i = 0; i < aot_nimages; i++) {
            int match = mod_match(mod, aot_images[i]);
            if (match == IMG_NONE)
                continue;
            if (match == IMG_SAME)
                mod_same[mod] |= 1ULL << i;
            else
                mod_family[mod] |= 1ULL << i;
            if (!mod_aot[mod])
                mod_aot[mod] = aot_images[i];
        }
    }
    return mod_aot[mod];
}

static bool key_is_gadget(unsigned i) {
    return i >= 7 && (i - 7) % 9 >= 7;
}

// The recorded translation of b (key as built by reg_key), if the image has it.
static const struct aot_trans *aot_find(const struct aot_module *m, const struct fiber_block *b,
                                        const struct jit_units *U, const uint32_t *key, unsigned nkey) {
    uint64_t off = key[1] | (uint64_t) key[2] << 32;
    size_t lo = 0, hi = m->ntrans;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (m->trans[mid].off < off) lo = mid + 1; else hi = mid;
    }
    for (; lo < m->ntrans && m->trans[lo].off == off; lo++) {
        const struct aot_trans *t = &m->trans[lo];
        if (t->nkey != nkey || t->nunits != U->n)
            continue;
        bool same = true;
        for (unsigned i = 1; same && i < nkey; i++)
            same = t->key[i] == (key_is_gadget(i) ? 0 : key[i]);
        for (unsigned u = 0; same && u < U->n; u++) {
            uintptr_t g = U->u[u].start < b->used ? b->code[U->u[u].start] : 0;
            same = (uintptr_t) t->gadget[u] == g;
        }
        if (same)
            return t;
    }
    return NULL;
}

// Instruction words of a key: the guest words of each unit.
static bool key_is_insn(unsigned i) {
    return i >= 7 && ((i - 7) % 9 == 5 || (i - 7) % 9 == 6);
}

// A guest word with the pc-relative immediate left out where the translation
// does not depend on it: block-end branches get their targets from the
// block's code stream (built from the module's own bytes), and adr / adrp
// targets are relative to the slot's dbase (mov_guest_ref()), set from the
// block's own words when it is installed.
static uint32_t insn_norm(uint32_t w) {
    if ((w & 0x7C000000u) == 0x14000000u) return w & 0xFC000000u;            // b, bl
    if ((w & 0xFF000010u) == 0x54000000u) return w & 0xFF00001Fu;            // b.cond
    if ((w & 0x7E000000u) == 0x34000000u) return w & 0xFF00001Fu;            // cbz / cbnz
    if ((w & 0x7E000000u) == 0x36000000u) return w & 0xFFF8001Fu;            // tbz / tbnz
    if ((w & 0x1F000000u) == 0x10000000u) return w & 0x9F00001Fu;            // adr / adrp
    return w;
}

// Target of adr / adrp w at pc.
static uint64_t adr_target(uint32_t w, uint64_t pc) {
    int64_t imm = sext(((w >> 5) & 0x7ffff) << 2 | ((w >> 29) & 3), 21);
    return (w & 0x80000000u) ? (pc & ~0xfffULL) + ((uint64_t) imm << 12) : pc + (uint64_t) imm;
}

// Content of a translation's key, wherever the block is: every word after
// the module, file offset and page offset (instruction words as insn_norm()
// has them), except the gadgets (runtime pointers; compared separately).
// gen.py computes the same hash.
static uint64_t key_content_hash(const uint32_t *k, unsigned n) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (unsigned i = 4; i < n; i++)
        if (!key_is_gadget(i))
            h = (h ^ (key_is_insn(i) ? insn_norm(k[i]) : k[i])) * 0x100000001b3ull;
    return h;
}

// Where the block would run: its address space and module mapping. A moved
// block can only have a translation whose slot is free or serves its place.
struct aot_place {
    struct asbestos *a; int mod; uint64_t base;
    const struct aot_module *m; struct jit_ctx *ctx;   // the last context looked up for image m
};

static bool aot_slot_usable(struct aot_place *p, const struct aot_module *m, const struct aot_trans *t,
                            const struct fiber_block *b) {
    if (p->m != m) {
        p->ctx = ctx_get(p->a, p->mod, p->base, m);
        p->m = m;
    }
    struct jit_ctx *ctx = p->ctx;
    if (!ctx)
        return false;
    struct fiber_block *owner = __atomic_load_n(&ctx->slot[t->idx].blk, __ATOMIC_ACQUIRE);
    return !owner || owner == b;
}

// Would recorded translation t (key tk) run block b (key k, at file offset
// off) right, and with what dbase? Every word must be the same except the
// immediates insn_norm() leaves out; where adr / adrp targets moved, all of
// the block's must have moved by the same distance, which dbase then adds.
// A self-loop translation also keeps its branches.
static bool aot_fits_moved(const struct aot_trans *t, const uint32_t *k, unsigned nkey, uint64_t off,
                           const struct fiber_block *b, uint64_t *dbase, bool *fixed_out) {
    bool loop = t->loop != NULL, fixed = false, have = false;
    int64_t shift = (int64_t) (off - t->off);   // no adr / adrp: as the code moved
    for (unsigned i = 4; i < nkey; i++) {
        if (key_is_gadget(i))
            continue;
        uint32_t w = t->key[i];
        if (!key_is_insn(i) || loop) {
            if (w != k[i]) return false;
            continue;
        }
        if (insn_norm(w) != insn_norm(k[i])) return false;
        fixed |= w != k[i];
    }
    for (unsigned u = 0; u < k[4]; u++) {
        const uint32_t *ku = &k[7 + 9 * u], *tu = &t->key[7 + 9 * u];
        uint64_t dpc = ku[3] | (uint64_t) ku[4] << 32;
        for (unsigned j = 0; j < (ku[2] & 0xff) && j < 2; j++) {
            if ((ku[5 + j] & 0x1F000000u) != 0x10000000u)
                continue;
            int64_t d = (int64_t) (adr_target(ku[5 + j], off + dpc + 4 * j) -
                                   adr_target(tu[5 + j], t->off + dpc + 4 * j));
            if (have && d != shift)
                return false;
            have = true;
            shift = d;
        }
    }
    *dbase = b->addr - off + (uint64_t) shift;
    *fixed_out = fixed;
    return true;
}

// The recorded translation of a block with the same code as b at another
// file offset (another version of the module, where code before it grew or
// shrank): the code is right with its constants relative to base + the
// distance the block moved (identical bytes keep every pc-relative distance)
// and adr / adrp targets relative to dbase (aot_fits_moved()).
static const struct aot_trans *aot_find_moved(const struct aot_module *m, const struct fiber_block *b,
                                              const struct jit_units *U, const uint32_t *key, unsigned nkey,
                                              struct aot_place *place, uint64_t off, uint64_t *dbase,
                                              bool *fixed) {
    if (!m->by_hash)
        return NULL;
    uint64_t h = key_content_hash(key, nkey);
    size_t lo = 0, hi = m->nhash;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (m->by_hash[mid].hash < h) lo = mid + 1; else hi = mid;
    }
    for (; lo < m->nhash && m->by_hash[lo].hash == h; lo++) {
        const struct aot_trans *t = &m->trans[m->by_hash[lo].trans];
        if (t->nkey != nkey || t->nunits != U->n)
            continue;
        bool same = true;
        for (unsigned u = 0; same && u < U->n; u++) {
            uintptr_t g = U->u[u].start < b->used ? b->code[U->u[u].start] : 0;
            same = (uintptr_t) t->gadget[u] == g;
        }
        // The same code is often recorded at several places (short
        // epilogues and the like, one per function): take a copy whose slot
        // is free rather than give up on the first.
        if (same && aot_fits_moved(t, key, nkey, off, b, dbase, fixed) && aot_slot_usable(place, m, t, b))
            return t;
    }
    return NULL;
}

// Look b up in the images recorded from this very file (at its file offset).
static const struct aot_trans *aot_find_same(int mod, const struct fiber_block *b, const struct jit_units *U,
                                             const uint32_t *key, unsigned nkey, const struct aot_module **found) {
    uint64_t images = mod_same[mod];   // mod_image() made it
    for (unsigned i = 0; images; i++, images >>= 1) {
        if (!(images & 1))
            continue;
        const struct aot_trans *t = aot_find(aot_images[i], b, U, key, nkey);
        if (t) {
            *found = aot_images[i];
            return t;
        }
    }
    return NULL;
}

// Look b up in the images of other versions of its module: at its offset,
// else wherever its code was recorded.
static const struct aot_trans *aot_find_family(int mod, const struct fiber_block *b, const struct jit_units *U,
                                               const uint32_t *key, unsigned nkey, struct aot_place *place,
                                               uint64_t off, const struct aot_module **found, uint64_t *dbase,
                                               bool *fixed) {
    uint64_t images = mod_family[mod];
    for (unsigned i = 0; images; i++, images >>= 1) {
        if (!(images & 1))
            continue;
        const struct aot_module *m = aot_images[i];
        // at its recorded offset: base (a candidate aot_find_moved() turned
        // down may have left another dbase here)
        *dbase = b->addr - off;
        *fixed = false;
        const struct aot_trans *t = aot_find(m, b, U, key, nkey);
        if (!t)
            t = aot_find_moved(m, b, U, key, nkey, place, off, dbase, fixed);
        if (t) {
            *found = m;
            return t;
        }
    }
    return NULL;
}

// Install recorded translation t for b; false if its slot has another owner.
// A translation has one slot, so it serves one block at a time: a block
// matched by content can have the same bytes as one at another offset (a
// short epilogue, duplicated code), and the first to get the translation
// keeps it until it leaves the cache.
static bool aot_install(struct fiber_block *b, struct jit_ctx *ctx, const struct aot_trans *t, int32_t entry,
                        uint64_t dbase) {
    // The recorded constants are relative to the guest address of file
    // offset 0 where the block was recorded: here, b->addr - t->off.
    if (!slot_claim(ctx, t->idx, b, b->addr - t->off, dbase))
        return false;
    b->jit_ctx = ctx;
    b->native_link[0] = (uint32_t *) t->link[0];
    b->native_link[1] = (uint32_t *) t->link[1];
    for (unsigned i = 0; i < t->nseg; i++) {
        b->code[t->seg[i].pos] = (unsigned long) t->seg[i].code;
        if (t->seg[i].pos == 0 && n_pinned)
            __atomic_store_n(&b->native_entry, (uintptr_t) t->seg[i].code + (uintptr_t) entry, __ATOMIC_RELEASE);
    }
    return true;
}

static bool in_aot_text(uintptr_t pc, const struct aot_module **img) {
    for (unsigned i = 0; i < aot_nimages; i++)
        if (pc >= (uintptr_t) aot_images[i]->text_start && pc < (uintptr_t) aot_images[i]->text_end) {
            if (img) *img = aot_images[i];
            return true;
        }
    return false;
}

// What aot_find_family() found for a key, kept for the next block with the same
// key (in any process of this ish): another version's module only costs its
// search once. Keyed by the key's two independent hashes; a translation that
// no image has is remembered too (that is the costly search).
struct aot_memo {
    uint64_t h, h2;
    int32_t img, trans;        // trans < 0: no image has it
    int64_t dshift;            // dbase - (b->addr - off)
    uint32_t fixed, valid;
};
#define AOT_MEMO_BITS 17   // entries, 2-way: a python start looks up ~40k blocks
static struct aot_memo *aot_memo;   // reg_lock
static _Atomic uint64_t st_memo_hits, st_search_n, st_search_ticks;

// Look b up in the images of its module (reg_lock held): those recorded from
// this very file first, so that with several images of one family linked in,
// the one made from this build wins wherever it has the block; then other
// versions, through the memo.
static const struct aot_trans *aot_lookup(int mod, const struct fiber_block *b, const struct jit_units *U,
                                          const uint32_t *key, unsigned nkey, uint64_t h, uint64_t h2,
                                          struct aot_place *place, uint64_t off,
                                          const struct aot_module **found, uint64_t *dbase, bool *fixed) {
    *dbase = b->addr - off;
    *fixed = false;
    const struct aot_trans *t = aot_find_same(mod, b, U, key, nkey, found);
    if (t || !mod_family[mod])
        return t;
    if (!aot_memo)
        aot_memo = calloc((size_t) 1 << AOT_MEMO_BITS, sizeof(*aot_memo));
    struct aot_memo *mo = NULL;
    if (aot_memo) {
        struct aot_memo *set = &aot_memo[h & ((1u << AOT_MEMO_BITS) - 2)];
        mo = set[0].valid && set[0].h == h && set[0].h2 == h2 ? &set[0] :
             set[1].valid && set[1].h == h && set[1].h2 == h2 ? &set[1] : NULL;
        if (!mo)   // to fill: an empty way, else the one not refilled last time
            mo = !set[0].valid ? &set[0] : !set[1].valid ? &set[1] : &set[(h >> AOT_MEMO_BITS) & 1];
    }
    if (mo && mo->valid && mo->h == h && mo->h2 == h2) {
        // (its slot may be taken in this address space: aot_install() says)
        atomic_fetch_add_explicit(&st_memo_hits, 1, memory_order_relaxed);
        if (mo->trans < 0)
            return NULL;
        *found = aot_images[mo->img];
        *dbase = b->addr - off + (uint64_t) mo->dshift;
        *fixed = mo->fixed;
        return &(*found)->trans[mo->trans];
    }
    uint64_t ts = mach_absolute_time();
    t = aot_find_family(mod, b, U, key, nkey, place, off, found, dbase, fixed);
    atomic_fetch_add_explicit(&st_search_ticks, mach_absolute_time() - ts, memory_order_relaxed);
    atomic_fetch_add_explicit(&st_search_n, 1, memory_order_relaxed);
    if (mo) {
        mo->h = h; mo->h2 = h2; mo->valid = 1;
        mo->trans = -1;
        if (t) {
            for (unsigned i = 0; i < aot_nimages; i++)
                if (aot_images[i] == *found) mo->img = (int32_t) i;
            mo->trans = (int32_t) (t - (*found)->trans);
            mo->dshift = (int64_t) (*dbase - (b->addr - off));
            mo->fixed = *fixed;
        }
    }
    return t;
}

// Translate b, or install an earlier translation with the same key.
static void jit_translate(struct fiber_block *b, const struct jit_units *U, struct em *e, uint32_t *key) {
    e->record = false;
    if (!pic_on) {
        jit_native(b, U, e);
        return;
    }
    uint64_t off, base;
    int mod = jit_locate(b->addr, &off, &base);
    struct asbestos *a = cm_tlb ? cm_tlb->mmu->asbestos : NULL;
    if (mod < 0 || !a || !U->n || !one_mapping(U, mod, base))
        return;
    // This process's own translations need a context with room for CTX_MAX
    // indices (8MB of address space, per module mapping of every guest
    // process, all in this one host process): not made when only images run.
    struct jit_ctx *ctx = NULL;
    if (!aot_only && !(ctx = ctx_get(a, mod, base, NULL)))
        return;
    unsigned nkey = reg_key(b, U, mod, off, key);
    uint64_t h2, h = reg_hash(key, nkey, &h2);
    pthread_mutex_lock(&reg_lock);
    const struct aot_module *img = mod_image(mod);
    struct aot_place place = {a, mod, base, NULL, NULL};
    uint64_t dbase = base;
    bool fixed = false;
    uint64_t t0 = mach_absolute_time();
    const struct aot_trans *at = img && !aot_off ? aot_lookup(mod, b, U, key, nkey, h, h2, &place, off, &img, &dbase, &fixed)
                                                 : NULL;
    if (img)
        atomic_fetch_add_explicit(&st_find_ticks, mach_absolute_time() - t0, memory_order_relaxed);
    if ((unsigned) mod < mod_next_cap)
        mod_blocks[mod]++;
    if (at) {
        pthread_mutex_unlock(&reg_lock);
        struct jit_ctx *actx = ctx_get(a, mod, base, img);
        if (!actx)
            return;
        if (!aot_install(b, actx, at, img->entry_off, dbase)) {
            atomic_fetch_add_explicit(&st_aot_busy, 1, memory_order_relaxed);
            return;
        }
        bool moved = at->off != off;
        atomic_fetch_add_explicit(&st_aot, 1, memory_order_relaxed);
        if (moved)
            atomic_fetch_add_explicit(&st_aot_moved, 1, memory_order_relaxed);
        if (fixed)
            atomic_fetch_add_explicit(&st_aot_fixed, 1, memory_order_relaxed);
        pthread_mutex_lock(&reg_lock);
        if ((unsigned) mod < mod_next_cap) {
            mod_hits[mod]++;
            mod_moved[mod] += moved;
        }
        pthread_mutex_unlock(&reg_lock);
        return;
    }
    if (aot_only) {
        pthread_mutex_unlock(&reg_lock);
        return;
    }
    if (!reg_table)
        reg_table = calloc(REG_BUCKETS, sizeof(*reg_table));
    struct reg_entry *r = reg_table ? reg_table[h & (REG_BUCKETS - 1)] : NULL;
    for (; r; r = r->next)
        if (r->hash == h && r->nkey == nkey && !memcmp(r->key, key, nkey * 4))
            break;
    unsigned idx = 0;
    if (!r && reg_table)
        idx = mod_tables_fit(mod) ? mod_next_idx[mod]++ : CTX_MAX;
    pthread_mutex_unlock(&reg_lock);
    if (r) {
        reg_install(b, ctx, r);
        atomic_fetch_add_explicit(&st_shared, 1, memory_order_relaxed);
        return;
    }
    if (!reg_table || idx >= CTX_MAX)
        return;
    // The code may run as soon as the first segment is installed.
    e->record = rec_module(mod);
    e->idx = idx;
    e->base = base;
    b->jit_ctx = ctx;
    slot_set(ctx, idx, b, base, base);
    jit_native(b, U, e);
    if (e->ninst > sizeof(e->inst) / sizeof(e->inst[0]))
        return;
    r = malloc(sizeof(*r) + nkey * 4);
    if (!r)
        return;
    r->seg = malloc(e->ninst * sizeof(*r->seg) + 1);
    if (!r->seg) {
        free(r);
        return;
    }
    r->hash = h;
    r->nkey = nkey;
    r->idx = idx;
    r->link[0] = b->native_link[0];
    r->link[1] = b->native_link[1];
    r->nseg = e->ninst;
    memcpy(r->key, key, nkey * 4);
    for (unsigned i = 0; i < e->ninst; i++) {
        r->seg[i].pos = e->inst[i].pos;
        r->seg[i].code = e->inst[i].code;
        r->seg[i].rec = e->inst[i].rec;
    }
    pthread_mutex_lock(&reg_lock);
    r->next = reg_table[h & (REG_BUCKETS - 1)];
    reg_table[h & (REG_BUCKETS - 1)] = r;
    pthread_mutex_unlock(&reg_lock);
}

// Host fault inside native code: the pinned guest registers only live in host
// registers, so copy them from the signal context into cpu_state before the
// crash-recovery path re-runs the block.
bool jit_crash_sync(void *ucontext) {
    if (!region || n_pinned <= 0)
        return false;
    ucontext_t *uc = ucontext;
    uintptr_t pc = uc->uc_mcontext->__ss.__pc;
    const struct aot_module *img = NULL;
    bool in_region = pc >= (uintptr_t) region && pc < (uintptr_t) region + REGION_SIZE;
    if (!in_region && !in_aot_text(pc, &img))
        return false;
    char *cpu = (char *) uc->uc_mcontext->__ss.__x[1];
    // Faults only happen at guest accesses, which in a loop block are all in
    // the promoted body. (Blocking lock: this runs in a signal handler, but
    // only for faults inside native code, never from within loopmap_add.)
    struct loopmap lm = {0};
    if (in_region) {
        pthread_mutex_lock(&loopmap_lock);
        for (unsigned k = 0; k < nloopmaps; k++)
            if (pc >= loopmaps[k].lo && pc < loopmaps[k].hi) { lm = loopmaps[k]; break; }
        pthread_mutex_unlock(&loopmap_lock);
    } else {
        for (unsigned k = 0; k < img->ntrans; k++) {
            const struct aot_loop *l = img->trans[k].loop;
            if (!l || pc < (uintptr_t) l->lo || pc >= (uintptr_t) l->hi)
                continue;
            lm.n = l->n;
            memcpy(lm.g, l->g, 8); memcpy(lm.d, l->d, 8); memcpy(lm.h, l->h, 8);
            break;
        }
    }
    for (int i = 0; i < n_pinned; i++) {
        bool donor = false;
        for (unsigned k = 0; k < lm.n; k++) donor |= lm.d[k] == pin_guest[i];
        if (!donor)
            *(uint64_t *) (cpu + greg_off(pin_guest[i])) = uc->uc_mcontext->__ss.__x[pin_host[i]];
    }
    for (unsigned k = 0; k < lm.n; k++)
        *(uint64_t *) (cpu + greg_off(lm.g[k])) = uc->uc_mcontext->__ss.__x[lm.h[k]];
    return true;
}


// ================================================================ code map
//
// ISH_JIT_MAP=<file>: remember which guest code every block and native
// segment came from, as (module path, file offset), and write it at exit:
//   M <id> <blocks> <translated units> <segments> <code bytes> <path>
//   S <host code> <words> <id> <file offset of the first unit> <guest pc> <units> <first insn>
// Host PC samples of native code resolve to modules through the S lines; the
// same identity is what a recorder needs to find the code in another process.

// A module is one file build: its ELF build-id when it has one (wherever it
// is installed), else its path.
struct cm_module {
    char path[256];              // the first path it was seen at
    uint8_t id[20], id_len;      // NT_GNU_BUILD_ID, id_len 0 if none
    uint64_t blocks, units, segments, bytes;
};
struct cm_segment { uint64_t code, off, pc; uint32_t words, units, insn; int mod; };
static bool cm_on;
static pthread_mutex_t cm_lock = PTHREAD_MUTEX_INITIALIZER;
static struct cm_module *cm_mods;
static unsigned cm_nmods, cm_capmods;
static struct cm_segment *cm_segs;
static size_t cm_nsegs, cm_capsegs;

// NT_GNU_BUILD_ID of the ELF file behind fd (at most 20 bytes); 0 if none.
static unsigned elf_build_id(struct fd *fd, uint8_t id[20]) {
    uint8_t eh[64];
    if (!fd->ops->pread || fd->ops->pread(fd, eh, sizeof(eh), 0) != sizeof(eh))
        return 0;
    if (memcmp(eh, "\177ELF\2\1", 6) != 0)   // 64-bit little endian
        return 0;
    uint64_t phoff;
    uint16_t phentsize, phnum;
    memcpy(&phoff, eh + 0x20, 8);
    memcpy(&phentsize, eh + 0x36, 2);
    memcpy(&phnum, eh + 0x38, 2);
    uint8_t ph[64 * 56];
    if (phentsize != 56 || phnum == 0 || phnum > 64 ||
        fd->ops->pread(fd, ph, (size_t) phnum * 56, (off_t) phoff) != (ssize_t) phnum * 56)
        return 0;
    for (unsigned i = 0; i < phnum; i++) {
        uint32_t type;
        uint64_t noff, nsize;
        memcpy(&type, ph + 56 * i, 4);
        memcpy(&noff, ph + 56 * i + 8, 8);
        memcpy(&nsize, ph + 56 * i + 32, 8);
        uint8_t n[1024];
        if (type != 4 /* PT_NOTE */ || nsize > sizeof(n) || fd->ops->pread(fd, n, nsize, (off_t) noff) != (ssize_t) nsize)
            continue;
        for (uint64_t p = 0; p + 12 <= nsize;) {
            uint32_t namesz, descsz, ntype;
            memcpy(&namesz, n + p, 4);
            memcpy(&descsz, n + p + 4, 4);
            memcpy(&ntype, n + p + 8, 4);
            uint64_t name = p + 12, desc = name + ((namesz + 3u) & ~3u);
            if (desc + descsz > nsize)
                break;
            if (ntype == 3 /* NT_GNU_BUILD_ID */ && namesz == 4 && !memcmp(n + name, "GNU", 4) &&
                descsz > 0 && descsz <= 20) {
                memcpy(id, n + desc, descsz);
                return descsz;
            }
            p = desc + ((descsz + 3u) & ~3u);
        }
    }
    return 0;
}

static int cm_module_of(const char *path, const uint8_t *id, unsigned id_len) {
    for (unsigned i = 0; i < cm_nmods; i++)
        if (id_len ? cm_mods[i].id_len == id_len && !memcmp(cm_mods[i].id, id, id_len)
                   : !cm_mods[i].id_len && !strcmp(cm_mods[i].path, path))
            return (int) i;
    if (cm_nmods == cm_capmods) {
        unsigned cap = cm_capmods ? 2 * cm_capmods : 64;
        struct cm_module *m = realloc(cm_mods, cap * sizeof(*m));
        if (!m)
            return -1;
        cm_mods = m;
        cm_capmods = cap;
    }
    struct cm_module *m = &cm_mods[cm_nmods];
    memset(m, 0, sizeof(*m));
    snprintf(m->path, sizeof(m->path), "%s", path);
    memcpy(m->id, id, id_len);
    m->id_len = (uint8_t) id_len;
    return (int) cm_nmods++;
}

// Module and file offset of guest address pc (cm_lock held), and the data
// mapped there. The caller runs guest code, so the address space is
// read-locked.
static int cm_locate(addr_t pc, uint64_t *off, struct data **datap) {
    *off = 0;
    if (!cm_tlb)
        return -1;
    struct mem *mem = container_of(cm_tlb->mmu, struct mem, mmu);
    struct pt_entry *pt = mem_pt(mem, PAGE(pc));
    if (!pt)
        return -1;
    struct data *data = pt->data;
    if (datap)
        *datap = data;
    // realfs maps the file from file_offset rounded down to a host page, and
    // pt->offset counts from there.
    if (data->fd)
        *off = data->file_offset - data->file_offset % real_page_size + pt->offset + PGOFFSET(pc);
    // Looked up once per mapping (shared by forked address spaces).
    if (data->jit_mod)
        return data->jit_mod - 1;
    char path[MAX_PATH] = "[anon]";
    uint8_t id[20];
    unsigned id_len = 0;
    if (data->name) {
        snprintf(path, sizeof(path), "%s", data->name);
    } else if (data->fd) {
        if (generic_getpath(data->fd, path) < 0)
            snprintf(path, sizeof(path), "[unnamed file]");
        id_len = elf_build_id(data->fd, id);
    }
    int mod = cm_module_of(path, id, id_len);
    if (mod >= 0)
        data->jit_mod = mod + 1;
    return mod;
}

static int jit_locate(addr_t pc, uint64_t *off, uint64_t *base) {
    pthread_mutex_lock(&cm_lock);
    struct data *data;
    int mod = cm_locate(pc, off, &data);
    bool anon = mod >= 0 && !data->fd;
    pthread_mutex_unlock(&cm_lock);
    if (mod < 0)
        return -1;
    if (anon)
        *off = pc;
    *base = pc - *off;
    return mod;
}

// Was image m recorded from this module (IMG_SAME: by build-id when both have
// one, the same build anywhere in the file system, else by path), or from
// another version of it (IMG_FAMILY: the file name matches the image's
// family pattern, e.g. "libz.so.1*")? Either way every block is still
// checked word by word before it gets recorded code, so the pattern only
// picks which images to search.
static int mod_match(int mod, const struct aot_module *m) {
    pthread_mutex_lock(&cm_lock);
    int match = IMG_NONE;
    if (mod >= 0 && (unsigned) mod < cm_nmods) {
        const struct cm_module *c = &cm_mods[mod];
        bool same = c->id_len && m->build_id_len ? c->id_len == m->build_id_len && !memcmp(c->id, m->build_id, c->id_len)
                                                 : !strcmp(c->path, m->path);
        const char *name = strrchr(c->path, '/');
        name = name ? name + 1 : c->path;
        if (same)
            match = IMG_SAME;
        else if (aot_family && m->family && fnmatch(m->family, name, 0) == 0)
            match = IMG_FAMILY;
    }
    pthread_mutex_unlock(&cm_lock);
    return match;
}

static bool module_path_has(int mod, const char *sub) {
    pthread_mutex_lock(&cm_lock);
    bool has = mod >= 0 && (unsigned) mod < cm_nmods && strstr(cm_mods[mod].path, sub);
    pthread_mutex_unlock(&cm_lock);
    return has;
}

// ---- ISH_JIT_RECORD: one JSON object per translation
//   {"mod", "off", "idx", "key": [...], "keysym": {"i": "sym"}, "segs": [
//     {"pos", "code" (host address), "words": [...],
//      "rel": [[at, "sym", name] | [at, "exit"]],
//      "links": [[slot, at, target translation | -1, target segment, target word]],
//      "loop": [head, end, [promoted], [donors], [host regs]] | null}]}
// Host addresses become symbol names (gadgets, fiber_ret, ...; "@region" for
// the code region); a direct link resolves to the translation it went to.

static void rec_sym(FILE *f, uintptr_t p) {
    Dl_info info;
    if (p == (uintptr_t) region) {
        fputs("\"@region\"", f);
    } else if (dladdr((void *) p, &info) && info.dli_sname && (uintptr_t) info.dli_saddr == p) {
        fprintf(f, "\"%s\"", info.dli_sname);
    } else if (dladdr((void *) p, &info) && info.dli_sname) {
        fprintf(f, "\"%s+%lu\"", info.dli_sname, (unsigned long) (p - (uintptr_t) info.dli_saddr));
    } else {
        fprintf(f, "\"?%lx\"", (unsigned long) p);
    }
}

struct rec_where { uintptr_t lo, hi; unsigned t, seg; };
static int rec_where_cmp(const void *a, const void *b) {
    uintptr_t x = ((const struct rec_where *) a)->lo, y = ((const struct rec_where *) b)->lo;
    return x < y ? -1 : x > y;
}

static void rec_write(const char *path) {
    FILE *f = fopen(path, "w");
    if (!f)
        return;
    pthread_mutex_lock(&reg_lock);
    // Pass 1: the recorded translations, in a fixed order, and where their code is.
    size_t n = 0, cap = 1024, nw = 0, capw = 4096;
    struct reg_entry **t = malloc(cap * sizeof(*t));
    struct rec_where *w = malloc(capw * sizeof(*w));
    for (unsigned bkt = 0; reg_table && bkt < REG_BUCKETS && t && w; bkt++) {
        for (struct reg_entry *r = reg_table[bkt]; r; r = r->next) {
            if (!r->nseg || !r->seg[0].rec)
                continue;
            if (n == cap) t = realloc(t, (cap *= 2) * sizeof(*t));
            for (unsigned i = 0; i < r->nseg && t; i++) {
                if (nw == capw) w = realloc(w, (capw *= 2) * sizeof(*w));
                if (!w || !r->seg[i].rec) break;
                w[nw++] = (struct rec_where) {(uintptr_t) r->seg[i].code,
                                              (uintptr_t) r->seg[i].code + 4 * r->seg[i].rec->nwords, (unsigned) n, i};
            }
            if (t) t[n++] = r;
        }
    }
    if (t && w)
        qsort(w, nw, sizeof(*w), rec_where_cmp);
    // The conventions the code was generated with.
    fprintf(f, "{\"header\": {\"abi\": %u, \"prologue_words\": %d, \"entry_off\": %lu, \"n_pinned\": %d, \"exit_stub\": [",
            jit_abi(), prologue_words, (unsigned long) entry_off(), n_pinned);
    for (unsigned i = 0; i < exit_stub_words; i++)
        fprintf(f, "%s%u", i ? ", " : "", ((uint32_t *) exit_stub)[i]);
    fprintf(f, "]}}\n");
    // Pass 2: write them.
    for (size_t k = 0; t && w && k < n; k++) {
        struct reg_entry *r = t[k];
        uint64_t off = r->key[1] | (uint64_t) r->key[2] << 32;
        fprintf(f, "{\"mod\": ");
        pthread_mutex_lock(&cm_lock);
        fprintf(f, "\"%s\"", r->key[0] < cm_nmods ? cm_mods[r->key[0]].path : "?");
        pthread_mutex_unlock(&cm_lock);
        fprintf(f, ", \"off\": %llu, \"idx\": %u, \"key\": [", (unsigned long long) off, r->idx);
        for (unsigned i = 0; i < r->nkey; i++)
            fprintf(f, "%s%u", i ? ", " : "", r->key[i]);
        fprintf(f, "], \"keysym\": {");
        // gadget pointer of each unit: key words 7 + 9 * unit + 7 / 8
        for (unsigned u = 0, first = 1; 7 + 9 * u + 8 < r->nkey; u++) {
            uintptr_t g = r->key[7 + 9 * u + 7] | (uintptr_t) r->key[7 + 9 * u + 8] << 32;
            if (!g) continue;
            fprintf(f, "%s\"%u\": ", first ? "" : ", ", 7 + 9 * u + 7);
            rec_sym(f, g);
            first = 0;
        }
        fprintf(f, "}, \"segs\": [");
        for (unsigned i = 0; i < r->nseg; i++) {
            const struct rec_seg *rs = r->seg[i].rec;
            if (!rs) continue;
            fprintf(f, "%s{\"pos\": %u, \"code\": %lu, \"words\": [", i ? ", " : "", r->seg[i].pos,
                    (unsigned long) r->seg[i].code);
            for (unsigned j = 0; j < rs->nwords; j++)
                fprintf(f, "%s%u", j ? ", " : "", rs->words[j]);
            fprintf(f, "], \"rel\": [");
            for (unsigned j = 0; j < rs->nrel; j++) {
                fprintf(f, "%s[%u, ", j ? ", " : "", rs->rel[j].at);
                if (rs->rel[j].kind == REL_EXIT) {
                    fprintf(f, "\"exit\"]");
                } else {
                    fprintf(f, "\"sym\", ");
                    rec_sym(f, rs->rel[j].val);
                    fprintf(f, "]");
                }
            }
            fprintf(f, "], \"links\": [");
            for (int slot = 0, first = 1; slot < 2; slot++) {
                if (rs->link_at[slot] < 0) continue;
                uint32_t *site = (uint32_t *) (r->seg[i].code + 4 * (unsigned) rs->link_at[slot]);
                uint64_t lit = __atomic_load_n((uint64_t *) (site + 1), __ATOMIC_ACQUIRE);
                long tt = -1, ts = 0, tw = 0;
                if (lit != LINK_UNSET) {
                    uintptr_t target = (uintptr_t) site + lit;
                    size_t lo = 0, hi = nw;
                    while (lo < hi) {
                        size_t mid = (lo + hi) / 2;
                        if (w[mid].lo <= target) lo = mid + 1; else hi = mid;
                    }
                    if (lo && target < w[lo - 1].hi) {
                        tt = w[lo - 1].t;
                        ts = w[lo - 1].seg;
                        tw = (long) (target - w[lo - 1].lo) / 4;
                    }
                }
                fprintf(f, "%s[%d, %d, %ld, %ld, %ld]", first ? "" : ", ", slot, rs->link_at[slot], tt, ts, tw);
                first = 0;
            }
            fprintf(f, "], \"loop\": ");
            if (rs->loop) {
                fprintf(f, "[%d, %d, [", rs->loop_head, rs->body_end);
                for (unsigned j = 0; j < rs->npromo; j++) fprintf(f, "%s%d", j ? ", " : "", rs->promo_g[j]);
                fprintf(f, "], [");
                for (unsigned j = 0; j < rs->npromo; j++) fprintf(f, "%s%d", j ? ", " : "", rs->donor_g[j]);
                fprintf(f, "], [");
                for (unsigned j = 0; j < rs->npromo; j++) fprintf(f, "%s%d", j ? ", " : "", rs->promo_h[j]);
                fprintf(f, "]]");
            } else {
                fprintf(f, "null");
            }
            fprintf(f, "}");
        }
        fprintf(f, "]}\n");
    }
    pthread_mutex_unlock(&reg_lock);
    free(t);
    free(w);
    fclose(f);
}

static void cm_block(struct fiber_block *b) {
    pthread_mutex_lock(&cm_lock);
    uint64_t off;
    int mod = cm_locate(b->addr, &off, NULL);
    if (mod >= 0)
        cm_mods[mod].blocks++;
    pthread_mutex_unlock(&cm_lock);
}

static void cm_segment(const struct jit_units *U, unsigned first, unsigned end,
                       const uint8_t *code, unsigned words) {
    pthread_mutex_lock(&cm_lock);
    uint64_t off;
    int mod = cm_locate(U->u[first].pc, &off, NULL);
    if (mod >= 0) {
        cm_mods[mod].units += end - first;
        cm_mods[mod].segments++;
        cm_mods[mod].bytes += 4 * (uint64_t) words;
    }
    if (cm_nsegs == cm_capsegs) {
        size_t cap = cm_capsegs ? 2 * cm_capsegs : 65536;
        struct cm_segment *n = realloc(cm_segs, cap * sizeof(*n));
        if (!n) {
            pthread_mutex_unlock(&cm_lock);
            return;
        }
        cm_segs = n;
        cm_capsegs = cap;
    }
    cm_segs[cm_nsegs++] = (struct cm_segment) {
        (uint64_t) (uintptr_t) code, off, U->u[first].pc, words, end - first, U->u[first].w[0], mod};
    pthread_mutex_unlock(&cm_lock);
}

static void cm_write(const char *path) {
    FILE *f = fopen(path, "w");
    if (!f)
        return;
    pthread_mutex_lock(&cm_lock);
    for (unsigned i = 0; i < cm_nmods; i++)
        fprintf(f, "M %u %llu %llu %llu %llu %s\n", i, (unsigned long long) cm_mods[i].blocks,
                (unsigned long long) cm_mods[i].units, (unsigned long long) cm_mods[i].segments,
                (unsigned long long) cm_mods[i].bytes, cm_mods[i].path);
    for (size_t i = 0; i < cm_nsegs; i++)
        fprintf(f, "S %llx %u %d %llx %llx %u %08x\n", (unsigned long long) cm_segs[i].code, cm_segs[i].words,
                cm_segs[i].mod, (unsigned long long) cm_segs[i].off, (unsigned long long) cm_segs[i].pc,
                cm_segs[i].units, cm_segs[i].insn);
    pthread_mutex_unlock(&cm_lock);
    fclose(f);
}

// ================================================================ entry

int jit_control(const char *cmd, size_t len) {
    while (len && (cmd[len - 1] == '\n' || cmd[len - 1] == ' '))
        len--;
    if (len == 2 && !memcmp(cmd, "on", 2)) {
        aot_off = false;
        return 0;
    }
    if (len == 3 && !memcmp(cmd, "off", 3)) {
        aot_off = true;
        return 0;
    }
    return -1;
}

// /proc/ish/jit: what runs and whether the AOT images are being used.
size_t jit_describe(char *buf, size_t size) {
    size_t n = 0;
#define OUT(...) do { if (n < size) n += (size_t) snprintf(buf + n, size - n, __VA_ARGS__); } while (0)
    const char *mode = !jit_on ? "off (gadgets only)" :
                       aot_only ? "AOT images only (no executable memory)" :
                       pic_on ? "JIT, position-independent" : "JIT";
    OUT("mode: %s\n", mode);
    OUT("AOT: %s (echo on|off > /proc/ish/jit; affects blocks compiled afterwards, i.e. new processes)\n",
        aot_off ? "off" : "on");
    OUT("blocks translated or looked up: %llu, AOT installs: %llu (moved: %llu, with pc-relative immediates "
        "fixed: %llu, slot taken elsewhere: %llu), reused: %llu\n", (unsigned long long) st_blocks,
        (unsigned long long) st_aot, (unsigned long long) st_aot_moved, (unsigned long long) st_aot_fixed,
        (unsigned long long) st_aot_busy, (unsigned long long) st_shared);
    OUT("other versions: %s (ISH_AOT_FAMILY=0 turns off)\n", aot_family ? "images serve their family" : "off");
    {
        mach_timebase_info_data_t tb;
        mach_timebase_info(&tb);
        OUT("image lookups: %.1f ms (%llu answered from the memo; %llu searches of other versions: %.1f ms)\n",
            (double) st_find_ticks * tb.numer / tb.denom / 1e6, (unsigned long long) st_memo_hits,
            (unsigned long long) st_search_n, (double) st_search_ticks * tb.numer / tb.denom / 1e6);
    }
    OUT("images: %u in use, %u rejected (abi %08x)\n", aot_nimages, aot_nrejected, jit_abi());
    for (unsigned i = 0; i < aot_nimages; i++)
        OUT("  ok        %s (%u translations, family %s)\n", aot_images[i]->path, aot_images[i]->ntrans,
            aot_images[i]->family ? aot_images[i]->family : "-");
    for (unsigned i = 0; i < aot_nrejected; i++)
        OUT("  rejected  %s (made by an ish with other conventions or layouts)\n", aot_rejected[i]->path);
    // Modules that have an image: hits tell whether the module file is the
    // one the image was recorded from (no hits at all: a different version).
    pthread_mutex_lock(&reg_lock);
    OUT("modules with an image (blocks / AOT hits / of which moved):\n");
    for (unsigned m = 0; m < mod_next_cap; m++) {
        if (!mod_aot[m])
            continue;
        pthread_mutex_lock(&cm_lock);
        const char *path = (unsigned) m < cm_nmods ? cm_mods[m].path : mod_aot[m]->path;
        pthread_mutex_unlock(&cm_lock);
        OUT("  %-40s %8llu / %-8llu / %-8llu %s\n", path, (unsigned long long) mod_blocks[m],
            (unsigned long long) mod_hits[m], (unsigned long long) mod_moved[m],
            mod_hits[m] ? "" : mod_blocks[m] ? "<- no match: module file differs from the recorded one" : "");
    }
    for (unsigned i = 0; i < aot_nimages; i++) {
        bool seen = false;
        for (unsigned m = 0; m < mod_next_cap && !seen; m++)
            seen = mod_aot[m] && !strcmp(mod_aot[m]->path, aot_images[i]->path);
        if (!seen)
            OUT("  %-40s not loaded by any process yet\n", aot_images[i]->path);
    }
    pthread_mutex_unlock(&reg_lock);
#undef OUT
    return n < size ? n : size;
}

void jit_report(void) {
    // ISH_JIT_DUMP=<file>: [u64 region base][u64 bytes][code], for attributing
    // host PC samples to generated code.
    if (!jit_on)
        return;
    const char *dump = getenv("ISH_JIT_DUMP");
    if (dump) {
        FILE *f = fopen(dump, "wb");
        if (f) {
            uint64_t base = (uint64_t) (uintptr_t) region, used = region_used;
            fwrite(&base, 8, 1, f);
            fwrite(&used, 8, 1, f);
            fwrite(region, 1, used, f);
            fclose(f);
        }
    }
    const char *map = getenv("ISH_JIT_MAP");
    if (map && cm_on)
        cm_write(map);
    if (rec_file)
        rec_write(rec_file);
    if (!getenv("ISH_JIT_STATS"))
        return;
    fprintf(stderr,
            "🛠  JIT(%s): blocks %llu, segments %llu, units %llu, code %llu KB, compile %.1f ms | "
            "block ends %llu (not translated %llu), direct links %llu, loop blocks %llu | "
            "unsupported insns %llu, out of regs %llu | shared %llu, AOT %llu (%u images)\n",
            dual_map ? (pic_on ? "dual-map, PIC" : "dual-map") : (pic_on ? "MAP_JIT, PIC" : "MAP_JIT"), st_blocks, st_segments, st_units, st_bytes / 1024, st_ns / 1e6,
            st_block_ends, st_block_end_fail, st_links, st_loops, st_unsupported_insns, st_fail_regs, st_shared, st_aot, aot_nimages);
}

static pthread_once_t init_once = PTHREAD_ONCE_INIT;

static void jit_init(void) {
    if (env_off("ISH_JIT"))
        return;
    // Without a code region (-Djit_emit=false, or no executable memory at
    // run time) only AOT images run: PIC conventions, nothing translated.
    bool have_region = false;
    bool dual = !TARGET_OS_OSX || getenv("ISH_JIT_DUALMAP") != NULL;
#ifndef ISH_JIT_NO_EMIT
    have_region = dual ? map_dual() : map_jit();
#endif
    bool have_images = aot_nregistered > 0;
    if (!have_region && !have_images) {
        fprintf(stderr, "⚠️  JIT: no executable code region (%s), running gadgets only\n",
                dual ? "dual mapping" : "MAP_JIT");
        return;
    }
    link_off = env_off("ISH_JIT_LINK");
    loop_off = env_off("ISH_JIT_LOOP");
    simd_on = !env_off("ISH_JIT_SIMD");
    pin_on = !env_off("ISH_JIT_PIN");
    // AOT images are PIC code, so PIC is the default once one is linked in.
    const char *pic = getenv("ISH_JIT_PIC");
    pic_on = pic ? pic[0] == '1' : have_images;
    if (!have_region)
        pic_on = true;
    pin_init(pin_on);
    cm_on = getenv("ISH_JIT_MAP") != NULL;
    rec_file = pic_on && have_region ? getenv("ISH_JIT_RECORD") : NULL;
    rec_mod = getenv("ISH_JIT_RECORD_MOD") ? getenv("ISH_JIT_RECORD_MOD") : "ld-musl";
    if (have_region) {
        make_exit_stub();
        jit_exec_ready();
    }
    if (pic_on)
        aot_init();
    const char *fam = getenv("ISH_AOT_FAMILY");
    aot_family = !(fam && fam[0] == '0');
    const char *ao = getenv("ISH_JIT_AOT_ONLY");
    aot_only = pic_on && (!have_region || (ao && ao[0] == '1'));
    if (aot_only && !aot_nimages)
        return;
    jit_on = true;
}

// Per-thread scratch: the units of the block being compiled and the emitter.
struct jit_scratch {
    struct jit_units units;
    struct em em;
    uint32_t key[REG_KEY_MAX];
};
static pthread_key_t scratch_key;
static pthread_once_t scratch_once = PTHREAD_ONCE_INIT;
static __thread struct jit_scratch *scratch;

static void scratch_key_init(void) {
    pthread_key_create(&scratch_key, free);
}

static struct jit_scratch *get_scratch(void) {
    if (scratch)
        return scratch;
    pthread_once(&scratch_once, scratch_key_init);
    scratch = malloc(sizeof(*scratch));
    if (scratch)
        pthread_setspecific(scratch_key, scratch);
    return scratch;
}

struct jit_units *jit_units_begin(void) {
    pthread_once(&init_once, jit_init);
    if (!jit_on)
        return NULL;
    struct jit_scratch *s = get_scratch();
    if (!s)
        return NULL;
    s->units.n = 0;
    return &s->units;
}

void jit_step_end(struct jit_units *U, const struct jit_step *step,
                  const struct gen_state *state, bool more, struct tlb *tlb) {
    if (!U || U->n > JIT_MAX_UNITS)
        return;
    cm_tlb = tlb;
    if (U->n == JIT_MAX_UNITS) {
        U->n++;   // overflow: leave the whole block to the gadgets
        return;
    }
    struct jit_unit *u = &U->u[U->n++];
    u->start = step->size;
    u->end = state->size;
    u->pc = step->ip;
    u->follow = step->follow_depth != state->b_follow_depth;
    u->last = !more;
    addr_t consumed = u->follow ? 4 : state->ip - step->ip;
    u->n = consumed == 4 ? 1 : consumed == 8 ? 2 : 0;
    u->w[0] = u->w[1] = 0;
    for (unsigned k = 0; k < u->n; k++)
        tlb_read(tlb, step->ip + 4 * k, &u->w[k], 4);
}

void jit_block_init(struct fiber_block *b) {
    b->native_link[0] = b->native_link[1] = NULL;
    b->native_loop = NULL;
    b->native_entry = 0;
    b->jit_ctx = NULL;
    b->jit_idx = 0;
}

void jit_block(struct fiber_block *b, struct jit_units *U) {
    if (!U || U->n > JIT_MAX_UNITS)
        return;
    uint64_t t0 = mach_absolute_time();
    if (cm_on)
        cm_block(b);
    jit_translate(b, U, &scratch->em, scratch->key);
    static mach_timebase_info_data_t tb;
    if (!tb.denom)
        mach_timebase_info(&tb);
    atomic_fetch_add_explicit(&st_ns, (mach_absolute_time() - t0) * tb.numer / tb.denom, memory_order_relaxed);
    atomic_fetch_add_explicit(&st_blocks, 1, memory_order_relaxed);
}

// A native segment covering units [first, end) of block b is installed at
// code (words instructions, stubs included). This is where a recording mode
// hooks in to capture translations.
static void jit_segment_installed(struct fiber_block *b, const struct jit_units *U, unsigned first,
                                  unsigned end, const uint8_t *code, unsigned words) {
    (void) b;
    atomic_fetch_add_explicit(&st_segments, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&st_units, end - first, memory_order_relaxed);
    if (cm_on)
        cm_segment(U, first, end, code, words);
}
