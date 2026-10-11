#include "emu/interrupt.h"
#include "gadgets.h"

.global NAME(fiber_enter)
.type_compat NAME(fiber_enter),function
NAME(fiber_enter):
    push %rbp
    push %rbx
    push %r12
    push %r13
    push %r14
    push %r15
    # make stack 16 byte aligned
    # this is preserved elsewhere by only doing even numbers of pushes
    sub $0x8, %rsp
    leaq FIBER_BLOCK_code(%rdi), %_ip
    movq %rsi, %_cpu
    movq %rsp, LOCAL_bp(%_cpu)
    leaq TLB_entries(%rdx), %_tlb
    load_regs
    gret

.global fiber_ret_chain
fiber_ret_chain:
    btq $63, %_ip
    jc fiber_ret
    mov CPU_poked_ptr(%_cpu), %r10
    cmpb $0, (%r10)
    jnz poke
    leaq -FIBER_BLOCK_code(%_ip), %r10
    mov %r10, LOCAL_last_block(%_cpu)
    gret

poke:
    movq -FIBER_BLOCK_code+FIBER_BLOCK_addr(%_ip), %_ip
    # fallthrough

// An indirect jump, call or uncached return to the guest address in _ip:
// enter the block the run loop's cache has for it, as a chained jump would,
// without the round trip through the loop. Anything else goes there.
.global fiber_ret_lookup
fiber_ret_lookup:
    mov CPU_poked_ptr(%_cpu), %r14
    cmpb $0, (%r14)
    jnz fiber_ret
    // the run loop's timer tick: the dispatch that would make cycle a
    // multiple of 1024 goes there (which counts it and ticks)
    movq CPU_cycle(%_cpu), %r14
    incq %r14
    testl $0x3ff, %r14d
    jz fiber_ret
    movq %r14, CPU_cycle(%_cpu)
    // tlb->block_cache valid? (block_cache_gen == mmu->asbestos->invalidate_gen)
    movq -TLB_entries+TLB_mmu(%_tlb), %r14
    movq MMU_asbestos(%r14), %r14
    movl ASBESTOS_invalidate_gen(%r14), %r14d
    cmpl -TLB_entries+TLB_block_cache_gen(%_tlb), %r14d
    jne fiber_ret
    // block_cache[(ip ^ (ip >> 12)) & 4095]
    movq %_ip, %r14
    shrq $12, %r14
    xorq %_ip, %r14
    andl $4095, %r14d
    movq -TLB_entries+TLB_block_cache(%_tlb,%r14,8), %r14
    testq %r14, %r14
    jz fiber_ret
    cmpq FIBER_BLOCK_addr(%r14), %_ip
    jne fiber_ret
    leaq FIBER_BLOCK_code(%r14), %_ip
    gret

.global fiber_ret
fiber_ret:
    movl $-1, %_tmp
    # fallthrough

.global fiber_exit
fiber_exit:
    save_regs
    // an unchained jump target carries gen.c's fake_ip marker (bit 63)
    btrq $63, %_ip
    movq %_ip, CPU_rip(%_cpu)
    movq LOCAL_bp(%_cpu), %rsp
    add $0x8, %rsp # keep stack 16 byte aligned
    pop %r15
    pop %r14
    pop %r13
    pop %r12
    pop %rbx
    pop %rbp
    mov %_tmp, %eax
    ret

.gadget interrupt
    movl (%_ip), %_tmp
    movq 16(%_ip), %r14
    movq %r14, CPU_segfault_addr(%_cpu)
    movq 8(%_ip), %_ip
    movb $0, CPU_segfault_was_write(%_cpu)
    jmp fiber_exit

.gadget exit
    movq (%_ip), %_ip
    jmp fiber_ret
