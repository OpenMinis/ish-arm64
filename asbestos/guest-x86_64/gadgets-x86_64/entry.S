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
