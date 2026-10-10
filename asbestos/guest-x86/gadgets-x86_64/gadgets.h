#include "../../gadgets-generic.h"

# register assignments
#define _esp r8d
#define _sp r8w
#define _ip r9
#define _eip r9d
#define _tmp r10d
#define tmp r10
#define tmpd r10d
#define tmpw r10w
#define tmpb r10b
#define _cpu r11
#define _tlb r12
#define _addr r13d
#define _addrq r13

.extern fiber_exit

# Calls into C, after save_c, with System V argument registers (at most
# four). Windows wants them in rcx, rdx, r8, r9 and 32 bytes of shadow space
# under the return address; the shuffle order matters (rcx is both the 4th
# System V and the 1st Microsoft argument). ccall_indir loads the target
# first because r9 is _ip.
.macro ccall target
#ifdef _WIN32
    movq %rcx, %r9
    movq %rdx, %r8
    movq %rsi, %rdx
    movq %rdi, %rcx
    subq $32, %rsp
    call \target
    addq $32, %rsp
#else
    call \target
#endif
.endm
.macro ccall_indir target
#ifdef _WIN32
    movq \target, %rax
    ccall *%rax
#else
    callq *\target
#endif
.endm

#ifndef ISH_GADGET_TEMPLATES
.macro .gadget name
    .global.name gadget_\()\name
.endm
.macro gret pop=0
    addq $((\pop+1)*8), %_ip
    jmp *-8(%_ip)
.endm
.macro .slowpath_begin
    .pushsection_bullshit
.endm
.macro .slowpath_end
    .popsection
.endm
.macro .tpl_noinline
.endm
.macro .tpl_close
.endm
#else
# Template build (asbestos/guest-x86/xjit.c). The same sources assembled a
# second time, each gadget as one contiguous, copyable piece of code: gret
# falls through to the end of the template instead of dispatching, slow paths
# sit inline, and a table entry maps the real gadget to its template, the
# number of argument words it consumes (-1: never falls through, -2: varies)
# and whether it must not be inlined (.tpl_noinline).
.macro .gadget name
    .tpl_close
    .set tplv_pop, -1
    .set tplv_noinline, 0
    .set tplv_open, 1
    .macro .tpl_entry
        .pushsection_xjit_table
        .quad NAME(gadget_\()\name), tpl_\name, 9999b
        .tpl_quad_pop
        .popsection
    .endm
tpl_\name\():
.endm
# The table gets a section of its own so xjit.c can find its bounds.
.macro .pushsection_xjit_table
#if __APPLE__
    .pushsection __DATA,__ish_xjit
#elif defined(_WIN32)
    .pushsection .xjit$m,"dr"
#else
    .pushsection ish_xjit,"aw"
#endif
.endm
# tplv_pop/tplv_noinline as literals: referencing the symbols from data
# would freeze them.
.macro .tpl_quad_pop
    .irp n, -2,-1,0,1,2,3,4,5,6,7,8
    .if tplv_pop == \n
        .quad \n
    .endif
    .endr
    .if tplv_noinline
        .quad 1
    .else
        .quad 0
    .endif
.endm
.macro .tpl_close
    .ifdef tplv_open
    .if tplv_open
9999:
        .tpl_entry
        .purgem .tpl_entry
        .set tplv_open, 0
    .endif
    .endif
.endm
.macro gret pop=0
    addq $((\pop+1)*8), %_ip
    .if tplv_pop == -1
        .set tplv_pop, \pop
    .elseif tplv_pop != \pop
        .set tplv_pop, -2
    .endif
    jmp 9999f
.endm
.macro .tpl_noinline
    .set tplv_noinline, 1
.endm
.macro .slowpath_begin
    jmp 8888f
.endm
.macro .slowpath_end
8888:
.endm
# Gadget arrays and other global data belong to the real gadgets.
.purgem .gadget_list
.macro .gadget_list type, list:vararg
.endm
.purgem .gadget_list_size
.macro .gadget_list_size type, list:vararg
.endm
#endif

# memory reading and writing
.irp type, read,write

.macro \type\()_prep size, id
    # TLB_INDEX in emu/tlb.h
    movl %_addr, %r14d
    shrl $12, %r14d
    andl $((1 << TLB_BITS) - 1), %r14d
    movl %_addr, %r15d
    shrl $(12 + TLB_BITS), %r15d
    xor %r15d, %r14d
    shll $4, %r14d
    movl %_addr, %r15d
    andl $0xfff, %r15d
    cmpl $(0x1000-(\size/8)), %r15d
    ja crosspage_load_\id
    movl %_addr, %r15d
    andl $0xfffff000, %r15d
    .ifc \type,read
        cmpl TLB_ENTRY_page(%_tlb,%r14), %r15d
    .else
        cmpl TLB_ENTRY_page_if_writable(%_tlb,%r14), %r15d
    .endif
    movl %r15d, -TLB_entries+TLB_dirty_page(%_tlb)
    jne handle_miss_\id
    addq TLB_ENTRY_data_minus_addr(%_tlb,%r14), %_addrq
back_\id :

.slowpath_begin
handle_miss_\id :
    call handle_\type\()_miss
    jmp back_\id
crosspage_load_\id :
    movq $(\size/8), %r14
    call crosspage_load
    jmp back_\id
.slowpath_end
.endm

.endr
.macro write_done size, id
    leaq LOCAL_value(%_cpu), %r14
    cmpq %_addrq, %r14
    je crosspage_store_\id
back_write_done_\id :
.slowpath_begin
crosspage_store_\id :
    movq $(\size/8), %r14
    call crosspage_store
    jmp back_write_done_\id
.slowpath_end
.endm

.macro _invoke size, reg, post, macro:vararg
    .if \size == 32
        \macro reg_\reg, e\reg\post
    .else
        \macro reg_\reg, \reg\post
    .endif
.endm
.macro .each_reg_size size, macro:vararg
    .irp reg, a,b,c,d
        _invoke \size, \reg, x, \macro
    .endr
    .irp reg, si,di,bp
        _invoke \size, \reg, , \macro
    .endr
    .if \size == 32
        \macro reg_sp, _esp
    .else
        \macro reg_sp, _sp
    .endif
.endm
.macro .each_reg macro:vararg
    .each_reg_size 32, \macro
.endm

.macro ss size, macro, args:vararg
    .ifnb \args
        .if \size == 8
            \macro \args, \size, b, b
        .elseif \size == 16
            \macro \args, \size, w, w
        .elseif \size == 32
            \macro \args, \size, d, l
        .else
            .error "bad size"
        .endif
    .else
        .if \size == 8
            \macro \size, b, b
        .elseif \size == 16
            \macro \size, w, w
        .elseif \size == 32
            \macro \size, d, l
        .else
            .error "bad size"
        .endif
    .endif
.endm

.macro setf_c
    setc CPU_cf(%_cpu)
.endm
.macro setf_oc
    seto CPU_of(%_cpu)
    setf_c
.endm
.macro setf_a src, dst, ss
    mov\ss \src, CPU_op1(%_cpu)
    mov\ss \dst, CPU_op2(%_cpu)
    orl $AF_OPS, CPU_flags_res(%_cpu)
.endm
.macro clearf_a
    andl $~AF_FLAG, CPU_eflags(%_cpu)
    andl $~AF_OPS, CPU_flags_res(%_cpu)
.endm
#if __APPLE__
#define DOLLAR(x) $$x
#else
#define DOLLAR(x) $x
#endif
.macro clearf_oc
    movl DOLLAR(0), CPU_of(%_cpu)
    movl DOLLAR(0), CPU_cf(%_cpu)
.endm
.macro setf_zsp res, ss
    .ifnc \ss,l
        movs\ss\()l \res, %_tmp
    .endif
    movl %_tmp, CPU_res(%_cpu)
    orl $(ZF_RES|SF_RES|PF_RES), CPU_flags_res(%_cpu)
.endm

.macro save_c odd
    push %rax
    push %rcx
    push %rdx
    push %rsi
    push %rdi
    push %r8
    push %r9
    push %r10
    push %r11
    .ifnc \odd,odd
        sub $8, %rsp # 16 byte alignment is so annoying
    .endif
.endm
.macro restore_c odd
    .ifnc \odd,odd
        add $8, %rsp
    .endif
    pop %r11
    pop %r10
    pop %r9
    pop %r8
    pop %rdi
    pop %rsi
    pop %rdx
    pop %rcx
    pop %rax
.endm

.macro load_regs
    movl CPU_eax(%_cpu), %eax
    movl CPU_ebx(%_cpu), %ebx
    movl CPU_ecx(%_cpu), %ecx
    movl CPU_edx(%_cpu), %edx
    movl CPU_esi(%_cpu), %esi
    movl CPU_edi(%_cpu), %edi
    movl CPU_ebp(%_cpu), %ebp
    movl CPU_esp(%_cpu), %_esp
.endm

.macro save_regs
    movl %eax, CPU_eax(%_cpu)
    movl %ebx, CPU_ebx(%_cpu)
    movl %ecx, CPU_ecx(%_cpu)
    movl %edx, CPU_edx(%_cpu)
    movl %esi, CPU_esi(%_cpu)
    movl %edi, CPU_edi(%_cpu)
    movl %ebp, CPU_ebp(%_cpu)
    movl %_esp, CPU_esp(%_cpu)
.endm

# vim: ft=gas
