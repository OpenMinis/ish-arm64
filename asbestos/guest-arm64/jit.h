#ifndef ASBESTOS_ARM64_JIT_H
#define ASBESTOS_ARM64_JIT_H

// Native JIT for the ARM64 guest (build option -Djit=true, defines ISH_JIT).
//
// A layer on top of the threaded-code blocks: after a block's gadget stream is
// generated, runs of supported guest instructions are translated to host code
// in a MAP_JIT region and the first gadget pointer of each run is replaced by
// a pointer to that code. Anything else still runs as gadgets, so block
// chaining, ret_cache and the fallback paths are those of the gadget engine.
//
// - The hottest guest registers live in fixed host registers across native
//   code and are stored back whenever control leaves it.
// - Loads/stores inline the TLB lookup; a miss resumes the unit's gadget.
// - Block ends (b/bl/b.cond/cbz/tbz/ret/br/blr) are translated; chained
//   successors are reached through patched direct branches.
//
// Without ISH_JIT none of this is compiled and every hook below is absent, so
// the default build is the plain gadget engine. With it, ISH_JIT=0 in the
// environment turns the translator off at run time (the BLR code stream keeps
// its JIT layout). Tuning / diagnostics: ISH_JIT_PIN=0, ISH_JIT_LINK=0,
// ISH_JIT_LOOP=0, ISH_JIT_SIMD=0, ISH_JIT_STATS=1, ISH_JIT_DUMP=<file>,
// ISH_JIT_MAP=<file> (native code -> guest module and file offset).
//
// ISH_JIT_PIC=1 emits position-independent code (jit.c, "PIC"): nothing in it
// depends on where the guest module or its blocks live, and a registry reuses
// a translation for every block with the same key, across processes.
//
// AOT: in PIC mode ISH_JIT_RECORD=<file> writes the translations of one module
// (ISH_JIT_RECORD_MOD, default ld-musl) at exit, and tools/jit_aot turns them
// into an image that a target links in (aot.h). A build with -Djit_emit=false
// runs only such images and never maps executable memory; ISH_JIT_AOT_ONLY=1
// does the same in a JIT build.

#ifdef ISH_JIT

#include <stdbool.h>
#include <stdint.h>
#include "asbestos/gen.h"

#define JIT_MAX_UNITS 1100

// One gen_step() worth of code-stream words and the guest code it covers.
struct jit_unit {
    unsigned start, end;   // [start, end) in block->code
    uint64_t pc;           // guest address of the first instruction
    uint8_t n;             // guest instructions consumed (0..2)
    bool follow;           // a forward `b` that was followed inline
    bool last;             // the block-ending unit
    uint32_t w[2];         // instruction words
};

struct jit_units {
    unsigned n;            // > JIT_MAX_UNITS: overflowed, block is not translated
    struct jit_unit u[JIT_MAX_UNITS];
};

struct fiber_block;
struct tlb;

// Compile-side hooks (fiber_block_compile).
struct jit_units *jit_units_begin(void);   // NULL when the JIT is off
struct jit_step { unsigned size; addr_t ip; unsigned follow_depth; };
static inline struct jit_step jit_step_begin(const struct gen_state *state) {
    return (struct jit_step) {state->size, state->ip, state->b_follow_depth};
}
void jit_step_end(struct jit_units *units, const struct jit_step *step,
                  const struct gen_state *state, bool more, struct tlb *tlb);
void jit_block(struct fiber_block *block, struct jit_units *units);

// Per-block state, set up by gen_start().
void jit_block_init(struct fiber_block *block);
struct asbestos;
// The address space is going away: release its module contexts.
void jit_asbestos_free(struct asbestos *asbestos);

// Block chaining (asbestos lock held): patch / restore the direct branch in
// `from`'s native exit for jump_ip[i]. No-ops for blocks without one.
void jit_link(struct fiber_block *from, int i, struct fiber_block *to);
void jit_unlink(struct fiber_block *from, int i);
// May jump_ip[i] of `from` be chained to `to`? PIC code branches directly to
// the successor it was linked to first, so its slot may only ever hold that.
bool jit_chain_ok(struct fiber_block *from, int i, struct fiber_block *to);

// Leave the thread's JIT write mode (if a compile or link entered it) before
// running guest code.
void jit_exec_ready(void);

// Host fault in native code: sync pinned guest registers into cpu_state.
bool jit_crash_sync(void *ucontext);

// Guest init is exiting: ISH_JIT_STATS=1 prints counters, ISH_JIT_DUMP=<file>
// writes the code region.
void jit_report(void);

// Human-readable state for /proc/ish/jit (mode, AOT images, hits per module).
size_t jit_describe(char *buf, size_t size);
// Writes to /proc/ish/jit: "off" / "on" stop / resume installing AOT images
// in blocks compiled afterwards (for A/B timing on a device). -1: unknown.
int jit_control(const char *cmd, size_t len);

#endif
#endif
