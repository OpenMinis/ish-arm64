#include "gadgets.h"

.gadget call
    // save return address
    leaq -8(%_rsp), %_addr
    write_prep 64, call
    // the return address, without gen.c's fake_ip marker (bit 63)
    movq 16(%_ip), %r14
    btrq $63, %r14
    movq %r14, (%_addrq)
    // push stack pointer
    subq $8, %_rsp
    // save ip-to-arguments to return cache
    shrw $4, %r14w
    movzwl %r14w, %r14d
    movq %_ip, LOCAL_ret_cache(%_cpu, %r14, 8)
    write_done 64, call // clobbers r14
    // jump to target
    movq 32(%_ip), %_ip
    jmp fiber_ret_chain

.gadget call_indir
    // save return address
    leaq -8(%_rsp), %_addr
    write_prep 64, call_indir
    // the return address, without gen.c's fake_ip marker (bit 63)
    movq 16(%_ip), %r14
    btrq $63, %r14
    movq %r14, (%_addrq)
    // push stack pointer
    subq $8, %_rsp
    // save ip-to-arguments to return cache
    shrw $4, %r14w
    movzwl %r14w, %r14d
    movq %_ip, LOCAL_ret_cache(%_cpu, %r14, 8)
    write_done 64, call_indir // clobbers r14
    // jump to target
    movq %tmp, %_ip
    jmp fiber_ret

.gadget ret
    movq %_rsp, %_addr
    // load return address and save to _tmp
    read_prep 64, ret
    movq (%_addrq), %tmp
    movq %tmp, %r14
    // pop stack pointer
    addq 8(%_ip), %_rsp
    // load saved ip in return cache
    shrw $4, %r14w
    movzwq %r14w, %r14
    movq LOCAL_ret_cache(%_cpu, %r14, 8), %_ip
    // found?
    cmpq $0, %_ip
    jz 2f
    // check if we jumped to the correct CALL instruction
    movq 16(%_ip), %r14
    btrq $63, %r14
    movq 8(%_ip), %r15
    cmpq %r14, %tmp
    jnz 1f
    // good, now do return chaining, the logic is similar to `fiber_ret_chain`
    movq 24(%_ip), %_ip
    btq $63, %_ip
    jc 1f
    leaq -FIBER_BLOCK_code(%_ip), %r15
    movq %r15, LOCAL_last_block(%_cpu)
    gret
1:
    movq %r15, LOCAL_last_block(%_cpu)
    // fallthrough
2:
    movq %tmp, %_ip
    jmp fiber_ret

.gadget jmp_indir
    movq %tmp, %_ip
    jmp fiber_ret
.gadget jmp
    movq (%_ip), %_ip
    jmp fiber_ret_chain
// jrcxz (no address-size prefix in long mode)
.gadget jcxz
    cmpq $0, %rcx
    jne 1f
    movq (%_ip), %_ip
    jmp fiber_ret_chain
1:
    movq 8(%_ip), %_ip
    jmp fiber_ret_chain

#define COND_LIST o,c,z,cz,s,p,sxo,sxoz

.macro check_res
    cmpq DOLLAR(0), CPU_res(%_cpu)
.endm
.macro check_cf
    cmpb DOLLAR(0), CPU_cf(%_cpu)
.endm

.macro res_or_flag type, resflag, flag, target
    testl $\resflag, CPU_flags_res(%_cpu)
    jz 2f
    check_res
    j\type \target
    jmp 3f
    2:
    testl $\flag, CPU_eflags(%_cpu)
    jnz \target
    3:
.endm

// \reg8 = SF, from the lazy result or, after popf/sahf/sigreturn, eflags
.macro get_sf reg8
    testl $SF_RES, CPU_flags_res(%_cpu)
    jz 4f
    check_res
    sets \reg8
    jmp 5f
    4:
    testl $SF_FLAG, CPU_eflags(%_cpu)
    setnz \reg8
    5:
.endm

.macro do_jump cond, target
    # please tell me if you know a better way
    .ifc \cond,o
        cmpb $0, CPU_of(%_cpu)
        jnz \target
    .else; .ifc \cond,c
        check_cf
        jnz \target
    .else; .ifc \cond,z
        res_or_flag z, ZF_RES, ZF_FLAG, \target
    .else; .ifc \cond,cz
        check_cf
        jnz \target
        res_or_flag z, ZF_RES, ZF_FLAG, \target
    .else; .ifc \cond,s
        res_or_flag s, SF_RES, SF_FLAG, \target
    .else; .ifc \cond,p
        res_or_flag p, PF_RES, PF_FLAG, \target
    .else; .ifc \cond,sxo
        get_sf %r14b
        xorb CPU_of(%_cpu), %r14b
        jnz \target
    .else; .ifc \cond,sxoz
        res_or_flag z, ZF_RES, ZF_FLAG, \target
        get_sf %r14b
        xorb CPU_of(%_cpu), %r14b
        jnz \target
    .endif; .endif; .endif; .endif; .endif; .endif; .endif; .endif
.endm

.irp cond, COND_LIST
    .gadget jmp_\cond
        do_jump \cond, 1f
        movq 8(%_ip), %_ip
        jmp fiber_ret_chain
    1:
        movq (%_ip), %_ip
        jmp fiber_ret_chain

    .gadget set_\cond
        do_jump \cond, 1f
        movl $0, %_tmp
        gret
    1:
        movl $1, %_tmp
        gret
    .gadget setn_\cond
        do_jump \cond, 1f
        movl $1, %_tmp
        gret
    1:
        movl $0, %_tmp
        gret

    .gadget skip_\cond
        .tpl_noinline // moves _ip by a variable amount
        do_jump \cond, 1f
        gret 1
    1:
        addq (%_ip), %_ip
        gret 1
    .gadget skipn_\cond
        .tpl_noinline // moves _ip by a variable amount
        do_jump \cond, 1f
        addq (%_ip), %_ip
    1:
        gret 1
.endr
.gadget_list jmp, COND_LIST
.gadget_list set, COND_LIST
.gadget_list setn, COND_LIST
.gadget_list skip, COND_LIST
.gadget_list skipn, COND_LIST

.gadget pushf
    save_c
    movq %_cpu, %rdi
    ccall NAME(helper_collapse_flags)
    restore_c

    subq $8, %_rsp
    movq %_rsp, %_addr
    write_prep 64, pushf
    movl CPU_eflags(%_cpu), %tmpd
    movq %tmp, (%_addrq)
    write_done 64, pushf
    gret

.gadget popf
    movq %_rsp, %_addr
    read_prep 64, popf
    movl (%_addrq), %tmpd
    movl %tmpd, CPU_eflags(%_cpu)
    addq $8, %_rsp

    save_c
    movq %_cpu, %rdi
    ccall NAME(helper_expand_flags)
    restore_c
    gret

.gadget sahf
    xchgb %ah, %al
    movb %al, CPU_eflags(%_cpu)
    xchgb %al, %ah
    save_c
    movq %_cpu, %rdi
    ccall NAME(helper_expand_flags)
    restore_c
    gret

.tpl_close
