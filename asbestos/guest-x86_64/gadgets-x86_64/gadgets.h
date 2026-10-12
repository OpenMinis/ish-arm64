#include "../../gadgets-generic.h"

// x86_64 guest on an x86_64 host. Same layout as the i386 gadgets
// (asbestos/guest-x86/gadgets-x86_64), widened: guest rax, rcx, rdx, rbx,
// rsi, rdi and rbp live in the host registers of the same name and guest
// rsp in r8; guest r8-r15 live in the cpu struct (CPU_r8...) and are used
// as memory operands. Guest addresses are 64 bits.

// 16 registers, then the 8-bit low bytes of rsp/rbp/rsi/rdi that a REX
// prefix selects (without one, those encodings mean ah/ch/dh/bh).
#undef REG_LIST
#define REG_LIST reg_a,reg_c,reg_d,reg_b,reg_sp,reg_bp,reg_si,reg_di,reg_r8,reg_r9,reg_r10,reg_r11,reg_r12,reg_r13,reg_r14,reg_r15,reg_spl,reg_bpl,reg_sil,reg_dil
#undef GADGET_LIST
#define GADGET_LIST REG_LIST,imm,mem,addr,gs
#undef SIZE_LIST
#define SIZE_LIST 8,16,32,64
// gadgets-generic.h expanded the old GADGET_LIST into this one
.purgem .gadget_array
.macro .gadget_array type
    .gadget_list_size \type, GADGET_LIST
.endm

# register assignments
#define _rsp r8
#define _esp r8d
#define _sp r8w
#define _ip r9
#define _eip r9d
#define _tmp r10d
#define tmp r10
#define tmpq r10
#define tmpd r10d
#define tmpw r10w
#define tmpb r10b
#define _cpu r11
#define _tlb r12
#define _addr r13
#define _addrq r13
#define _addrd r13d

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
        # 8-byte alignment, so the 40-byte entries of different objects
        # follow each other without padding
        .p2align 3
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
    # TLB_INDEX in emu/tlb.h; 32-byte entries (page, page_if_writable,
    # data_minus_addr, gen)
    movq %_addr, %r14
    shrq $(12 + TLB_BITS - 5), %r14
    movq %_addr, %r15
    shrq $(12 - 5), %r15
    xorq %r15, %r14
    andl $(((1 << TLB_BITS) - 1) << 5), %r14d
    movl %_addrd, %r15d
    andl $0xfff, %r15d
    cmpl $(0x1000-(\size/8)), %r15d
    ja crosspage_load_\id
    movq %_addr, %r15
    andq $~0xfff, %r15
    movq %r15, -TLB_entries+TLB_dirty_page(%_tlb)
    // a stale TLB (another thread changed the mappings) misses on every page
    orq -TLB_entries+TLB_stale(%_tlb), %r15
    .ifc \type,read
        cmpq TLB_ENTRY_page(%_tlb,%r14), %r15
    .else
        cmpq TLB_ENTRY_page_if_writable(%_tlb,%r14), %r15
    .endif
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

// \macro name, operand, kind for every guest register at a size. kind is
// "host" for registers held in host registers, "slot" for r8-r15 (memory in
// the cpu struct: a 32-bit write there must zero the upper half itself).
// At size 8, reg_sp/bp/si/di are ah/ch/dh/bh ("hi") and the REX low bytes
// come as reg_spl...dil.
.macro .each_reg_size size, macro:vararg
    .if \size == 64
        \macro reg_a, %rax, host
        \macro reg_c, %rcx, host
        \macro reg_d, %rdx, host
        \macro reg_b, %rbx, host
        \macro reg_sp, %_rsp, host
        \macro reg_bp, %rbp, host
        \macro reg_si, %rsi, host
        \macro reg_di, %rdi, host
    .elseif \size == 32
        \macro reg_a, %eax, host
        \macro reg_c, %ecx, host
        \macro reg_d, %edx, host
        \macro reg_b, %ebx, host
        \macro reg_sp, %_esp, host
        \macro reg_bp, %ebp, host
        \macro reg_si, %esi, host
        \macro reg_di, %edi, host
    .elseif \size == 16
        \macro reg_a, %ax, host
        \macro reg_c, %cx, host
        \macro reg_d, %dx, host
        \macro reg_b, %bx, host
        \macro reg_sp, %_sp, host
        \macro reg_bp, %bp, host
        \macro reg_si, %si, host
        \macro reg_di, %di, host
    .elseif \size == 8
        \macro reg_a, %al, host
        \macro reg_c, %cl, host
        \macro reg_d, %dl, host
        \macro reg_b, %bl, host
        \macro reg_sp, %ah, hi
        \macro reg_bp, %ch, hi
        \macro reg_si, %dh, hi
        \macro reg_di, %bh, hi
        \macro reg_spl, %r8b, host
        \macro reg_bpl, %bpl, host
        \macro reg_sil, %sil, host
        \macro reg_dil, %dil, host
    .endif
    \macro reg_r8, CPU_r8(%_cpu), slot
    \macro reg_r9, CPU_r9(%_cpu), slot
    \macro reg_r10, CPU_r10(%_cpu), slot
    \macro reg_r11, CPU_r11(%_cpu), slot
    \macro reg_r12, CPU_r12(%_cpu), slot
    \macro reg_r13, CPU_r13(%_cpu), slot
    \macro reg_r14, CPU_r14(%_cpu), slot
    \macro reg_r15, CPU_r15(%_cpu), slot
.endm
// all 16 registers as 64-bit operands (addressing)
.macro .each_reg macro:vararg
    .each_reg_size 64, \macro
.endm

.macro ss size, macro, args:vararg
    .ifnb \args
        .if \size == 8
            \macro \args, \size, b, b
        .elseif \size == 16
            \macro \args, \size, w, w
        .elseif \size == 32
            \macro \args, \size, d, l
        .elseif \size == 64
            \macro \args, \size, , q
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
        .elseif \size == 64
            \macro \size, , q
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
// res is the result register at its own width and full the same register at
// 64 bits; the result is stored sign-extended to 64 bits (SF reads its top
// bit). The extension happens in place: only the low \ss part of the register
// is the result, so its top bits are free, and no other register is touched
// (the callers keep values in r14/r15).
.macro setf_zsp res, ss, full=%tmp
    .ifnc \ss,q
        movs\ss\()q \res, \full
    .endif
    movq \full, CPU_res(%_cpu)
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
    movq CPU_eax(%_cpu), %rax
    movq CPU_ebx(%_cpu), %rbx
    movq CPU_ecx(%_cpu), %rcx
    movq CPU_edx(%_cpu), %rdx
    movq CPU_esi(%_cpu), %rsi
    movq CPU_edi(%_cpu), %rdi
    movq CPU_ebp(%_cpu), %rbp
    movq CPU_esp(%_cpu), %_rsp
.endm

.macro save_regs
    movq %rax, CPU_eax(%_cpu)
    movq %rbx, CPU_ebx(%_cpu)
    movq %rcx, CPU_ecx(%_cpu)
    movq %rdx, CPU_edx(%_cpu)
    movq %rsi, CPU_esi(%_cpu)
    movq %rdi, CPU_edi(%_cpu)
    movq %rbp, CPU_ebp(%_cpu)
    movq %_rsp, CPU_esp(%_cpu)
.endm

# vim: ft=gas
