#ifndef EMU_GEN_H
#define EMU_GEN_H

#include <stdbool.h>
#include <stdint.h>

#include "asbestos/asbestos.h"
#include "emu/tlb.h"

struct gen_state {
    addr_t ip;
    addr_t orig_ip;
    unsigned long orig_ip_extra;
    struct fiber_block *block;
    unsigned size;
    unsigned capacity;
    unsigned jump_ip[2];
    unsigned block_patch_ip; // for call/call_indir gadgets
    uint32_t last_insn;
    struct tlb *tlb; // for peephole optimization (peek at next instruction)
    unsigned b_follow_depth; // how many unconditional B's we've followed inline
    // Set when growing the gadget buffer ran out of memory. gen() cannot report
    // failure to its ~2000 call sites, so it records it here and emits nothing
    // further; fiber_block_compile() checks this and fails the compile, which
    // the run loop turns into a guest INT_GPF. See [T-ish-jit-oom-abort].
    bool oom;
};

bool gen_start(addr_t addr, struct gen_state *state);
void gen_exit(struct gen_state *state);
void gen_end(struct gen_state *state);

int gen_step(struct gen_state *state, struct tlb *tlb);

// Emit a single "prebuilt gadget" for a whole registered function: replaces the
// normal gadget stream with [gadget_prebuilt_entry][spec_fn]. spec_fn is a native
// void(*)(cpu, tlb) that reproduces the guest function; the block resumes at
// guest LR (ret). See kernel/native_offload_prebuilt.
void gen_prebuilt_block(struct gen_state *state, void *spec_fn);

#endif
