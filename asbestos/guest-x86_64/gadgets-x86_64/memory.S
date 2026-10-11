#include "gadgets.h"
#include "emu/interrupt.h"

// push/pop are 64-bit in long mode
.gadget push
    leaq -8(%_rsp), %_addr
    write_prep 64, push
    movq %tmp, (%_addrq)
    write_done 64, push
    subq $8, %_rsp
    gret 1
.gadget pop
    movq %_rsp, %_addr
    read_prep 64, pop
    movq (%_addrq), %tmp
    addq $8, %_rsp
    gret 1

// address = register + offset (the offset is a sign-extended 64-bit word)
.macro x name, reg, kind
    .gadget addr_\name
        movq \reg, %_addr
        addq (%_ip), %_addr
        gret 1
.endm
.each_reg x
.purgem x
// no base register: an absolute (or rip-relative, already resolved) address
.gadget addr_none
    movq (%_ip), %_addr
    gret 1
.gadget_list addr, REG_LIST

.macro x name, reg, kind
    .irp times, 1,2,4,8
        .gadget si_\name\()_\times
            .ifc \kind,slot
                movq \reg, %r14
                leaq (%_addr,%r14,\times), %_addr
            .else
                leaq (%_addr,\reg,\times), %_addr
            .endif
            gret
    .endr
.endm
.each_reg x
.purgem x

#ifndef ISH_GADGET_TEMPLATES
.pushsection_rodata
.global.name si_gadgets
.irp reg, reg_a,reg_c,reg_d,reg_b,reg_sp,reg_bp,reg_si,reg_di,reg_r8,reg_r9,reg_r10,reg_r11,reg_r12,reg_r13,reg_r14,reg_r15
    .irp times, 1,2,4,8
        .quad NAME(gadget_si_\reg\()_\times)
    .endr
.endr
.popsection
#endif

// %fs: TLS (tls_ptr holds the fs base); %gs: the gs base
.gadget seg_fs
    addq CPU_tls_ptr(%_cpu), %_addr
    gret
.gadget seg_gs
    addq CPU_gs_base(%_cpu), %_addr
    gret

#ifndef ISH_GADGET_TEMPLATES
.irp type, read,write

.global handle_\type\()_miss
handle_\type\()_miss:
    subq $8, %rsp
    save_c
    # %tlb actually points to tlb->entries
    leaq -TLB_entries(%_tlb), %rdi
    movq %_addr, %rsi
    .ifc \type,read
        movq $0, %rdx
    .else
        movq $1, %rdx
    .endif
    ccall NAME(tlb_handle_miss)
    movq %rax, %r15
    restore_c
    testq %r15, %r15
    jz segfault_\type
    movq %r15, %_addrq
    addq $8, %rsp
    ret

segfault_\type:
    movq -TLB_entries+TLB_segfault_addr(%_tlb), %_addr
    movq %_addr, CPU_segfault_addr(%_cpu)
    .ifc \type,read
        movb $0, CPU_segfault_was_write(%_cpu)
    .else
        movb $1, CPU_segfault_was_write(%_cpu)
    .endif
    // the faulting instruction's address, with gen.c's marker bits: 62
    // means "a pop already moved rsp up by 8 before the store faulted, undo it"
    movq (%_ip), %_ip
    leaq -8(%_rsp), %r14
    btq $62, %_ip
    cmovcq %r14, %_rsp
    btrq $62, %_ip
    btrq $63, %_ip
    movl $INT_GPF, %_tmp
    jmp fiber_exit

.endr

.global crosspage_load
crosspage_load:
    save_c odd
    leaq -TLB_entries(%_tlb), %rdi
    movq %_addr, %rsi
    leaq LOCAL_value(%_cpu), %rdx
    movq %r14, %rcx
    ccall NAME(__tlb_read_cross_page)
    movq %rax, %r14
    restore_c odd
    testq %r14, %r14
    jz segfault_read
    movq %_addr, LOCAL_value_addr(%_cpu)
    leaq LOCAL_value(%_cpu), %_addrq
    ret

.global crosspage_store
crosspage_store:
    save_c odd
    leaq -TLB_entries(%_tlb), %rdi
    movq LOCAL_value_addr(%_cpu), %rsi
    leaq LOCAL_value(%_cpu), %rdx
    movq %r14, %rcx
    ccall NAME(__tlb_write_cross_page)
    movq %rax, %r14
    restore_c odd
    testq %r14, %r14
    jz segfault_write
    ret
#endif

.tpl_close
