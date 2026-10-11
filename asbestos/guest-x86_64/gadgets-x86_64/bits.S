#include "gadgets.h"

.macro do_shift type, size, s, ss
    .irp arg, reg_c,imm
        .gadget \type\size\()_\arg
            .ifc \arg,imm
                movq %rcx, %r14
                movb (%_ip), %cl
            .endif
            .if \size == 64
                testb $63, %cl
            .else
                testb $31, %cl
            .endif
            jz 1f
            .ifin(\type, rcl,rcr)
                btw $0, CPU_cf(%_cpu)
            .endifin
            \type\()\ss %cl, %tmp\s
            setf_oc
            .ifin(\type, shl,shr,sar)
                setf_zsp %tmp\s, \ss
                clearf_a
            .endifin
        1:
            .ifc \arg,imm
                movq %r14, %rcx
            .endif
            .ifc \arg,imm
                gret 1
            .else
                gret
            .endif
    .endr
.endm

.irp type, shl,shr,sar,rol,ror,rcl,rcr
    .irp size, 8,16,32,64
        ss \size, do_shift, \type
    .endr
    .gadget_array \type
.endr

// shld/shrd dst, src, count: tmp is dst, the gadget's register is src.
// src goes through r15 (it may be an r8-r15 slot in memory); the immediate
// count needs cl, so rcx is saved around it.
.macro do_shiftd op, arg, size, s, ss
    .macro x name, reg, kind
        .gadget \op\()_\arg\()\size\()_\name
            .ifnc \kind,hi
            mov\ss \reg, %r15\s
            .ifc \arg,imm
                movq %rcx, %r14
                movb (%_ip), %cl
            .endif
            .if \size == 64
                testb $63, %cl
            .else
                testb $31, %cl
            .endif
            jz 1f
            \op\ss %cl, %r15\s, %tmp\s
            setf_oc
            setf_zsp %tmp\s, \ss
        1:
            .ifc \arg,imm
                movq %r14, %rcx
            .endif
            .endif
            .ifc \arg,imm
                gret 1
            .else
                gret
            .endif
    .endm
    .each_reg_size \size, x
    .purgem x
.endm
.irp op, shrd,shld
    .irp arg, imm,cl
        .irp size, 16,32,64
            ss \size, do_shiftd, \op, \arg
        .endr
        .gadget_array \op\()_\arg
    .endr
.endr

.macro do_bt_op op, arg, size, s, ss
    and\ss $(\size-1), %tmp\s
    \op\ss %tmp\s, \arg
    setf_c
.endm

.macro do_bt op, size, s, ss
    .gadget \op\size\()_mem
        .ifc \ss,q
            movq %tmp, %r14
        .else
            movs\ss\()q %tmp\s, %r14
        .endif
        sarq $3, %r14
        andq $~(\size/8-1), %r14
        addq %r14, %_addr
        // bts/btr/btc write: write_prep checks the page is writable and
        // write_done stores a page-crossing access back
        .ifc \op,bt
            read_prep \size, \op\size\()_mem
            do_bt_op \op, (%_addrq), \size, \s, \ss
        .else
            write_prep \size, \op\size\()_mem
            do_bt_op \op, (%_addrq), \size, \s, \ss
            write_done \size, \op\size\()_mem
        .endif
        gret 1

    .macro x name, reg, kind
        .gadget \op\size\()_\name
            .ifnc \kind,hi
            do_bt_op \op, \reg, \size, \s, \ss
            // a 32-bit write to an r8-r15 slot zero-extends
            .ifc \kind,slot
            .if \size == 32
            .ifnc \op,bt
                movl \reg, %r14d
                movq %r14, \reg
            .endif
            .endif
            .endif
            .endif
            gret
    .endm
    .each_reg_size \size, x
    .purgem x
.endm

.irp op, bt,btc,bts,btr
    .irp size, 16,32,64
        ss \size, do_bt, \op
    .endr
    .gadget_array \op
.endr

# atomic versions of the above

.macro do_bt_atomic op, size, s, ss
    .gadget atomic_\op\size\()_mem
        .ifc \ss,q
            movq %tmp, %r14
        .else
            movs\ss\()q %tmp\s, %r14
        .endif
        sarq $3, %r14
        andq $~(\size/8-1), %r14
        addq %r14, %_addr
        write_prep \size, atomic_\op\size\()_mem
        and\ss $(\size-1), %tmp\s
        lock \op\ss %tmp\s, (%_addrq)
        setf_c
        write_done \size, atomic_\op\size\()_mem
        gret 1
.endm

.irp op, btc,bts,btr
    .irp size, 16,32,64
        ss \size, do_bt_atomic, \op
    .endr
    .gadget_array atomic_\op
.endr

// bswap at 32 and 64 bits; a 32-bit one zero-extends
.macro x name, reg, kind
    .gadget bswap32_\name
        .ifnc \kind,hi
        .ifc \kind,slot
            movl \reg, %r14d
            bswap %r14d
            movq %r14, \reg
        .else
            bswap \reg
        .endif
        .endif
        gret
.endm
.each_reg_size 32, x
.purgem x
.macro x name, reg, kind
    .gadget bswap64_\name
        .ifc \kind,slot
            movq \reg, %r14
            bswap %r14
            movq %r14, \reg
        .else
            bswap \reg
        .endif
        gret
.endm
.each_reg_size 64, x
.purgem x
.gadget_array bswap

// popcnt: ZF says the source was zero, the other flags are cleared
.macro do_popcnt size, s, ss
    popcnt\ss %tmp\s, %tmp\s
    setz %r14b
    andl $~(ZF_RES|SF_RES|PF_RES|AF_OPS), CPU_flags_res(%_cpu)
    andl $~(ZF_FLAG|SF_FLAG|PF_FLAG|AF_FLAG), CPU_eflags(%_cpu)
    movb $0, CPU_cf(%_cpu)
    movb $0, CPU_of(%_cpu)
    shlb $6, %r14b
    orb %r14b, CPU_eflags(%_cpu)
.endm
.irp size, 16,32,64
    .gadget popcnt_\size
        ss \size, do_popcnt
        gret
.endr
.gadget_list popcnt, SIZE_LIST

.tpl_close
