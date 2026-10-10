#ifndef ASBESTOS_X86_XJIT_H
#define ASBESTOS_X86_XJIT_H

// Gadget-template JIT for the x86 guest on x86_64 hosts (-Djit=true, ISH_XJIT).
//
// The x86_64 gadgets keep the guest registers in host registers, so a block's
// gadget stream is already "native code with a dispatch after each gadget".
// For every run of gadgets in a block this splices copies of the gadgets'
// code (built a second time as templates, see ISH_GADGET_TEMPLATES in
// gadgets-x86_64/gadgets.h) into one piece of host code and points the run's
// first stream slot at it. The copies execute the same instructions as the
// gadgets -- same lazy flags, same TLB and fault paths, same reads of their
// arguments from the stream -- minus the indirect jump between gadgets.
// Everything else (chaining, ret_cache, interrupts) is the gadget engine's.
//
// ISH_XJIT=0 turns it off at run time; ISH_XJIT_STATS=1 prints counters when
// the guest's init exits.

#ifdef ISH_XJIT

struct fiber_block;

// After gen_end(), before the block is published.
void xjit_block(struct fiber_block *block);
// The block is being freed (no thread runs it).
void xjit_block_free(struct fiber_block *block);
void xjit_dump_stats(void);

#endif
#endif
