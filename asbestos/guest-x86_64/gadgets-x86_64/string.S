#include "gadgets.h"

#define REP_LIST once,rep,repnz

// CF is in cpu->cf (collapse_flags() puts it into eflags)
.gadget cmc
    xorb $1, CPU_cf(%_cpu)
    gret

.gadget clc
    movb $0, CPU_cf(%_cpu)
    gret

.gadget stc
    movb $1, CPU_cf(%_cpu)
    gret

.gadget cld
    andl $~DF_FLAG, CPU_eflags(%_cpu)
    gret

.gadget std
    orl $DF_FLAG, CPU_eflags(%_cpu)
    gret

.macro do_strop op, size, rep, s, ss, a
    # repnz is only a thing for cmps and scas
    .ifc \rep,repnz
        .ifnc \op,cmps; .ifnc \op,scas
            .exitm
        .endif; .endif
    .endif

    .gadget \op\size\()_\rep
        .ifnc \rep,once
            testq %rcx, %rcx
            jz 2f
    1:
        .endif
        movl $-(\size/8), CPU_df_offset(%_cpu)
        testl $DF_FLAG, CPU_eflags(%_cpu)
        jnz 3f
        negl CPU_df_offset(%_cpu)
    3:

        .ifc \op,lods
            movq %rsi, %_addr
            read_prep \size, \op\size\()_\rep
            mov\ss (%_addrq), %\a

        .else; .ifc \op,stos
            movq %rdi, %_addr
            write_prep \size, \op\size\()_\rep
            mov\ss %\a, (%_addrq)
            write_done \size, \op\size\()_\rep

        .else; .ifc \op,movs
            movq %rsi, %_addr
            read_prep \size, \op\size\()_\rep
            mov\ss (%_addrq), %tmp\s
            movq %rdi, %_addr
            write_prep \size, \op\size\()_\rep\()2
            mov\ss %tmp\s, (%_addrq)
            write_done \size, \op\size\()_\rep

        .else; .ifc \op, scas
            movq %rdi, %_addr
            read_prep \size, \op\size\()_\rep
            mov\ss (%_addrq), %r14\s
            setf_a src=%\a, dst=%r14\s, ss=\ss
            mov\ss %\a, %tmp\s
            sub\ss %r14\s, %tmp\s
            setf_oc
            setf_zsp %tmp\s, \ss

        .else; .ifc \op, cmps
            movq %rsi, %_addr
            read_prep \size, \op\size\()_\rep
            mov\ss (%_addrq), %tmp\s
            movq %rdi, %_addr
            read_prep \size, \op\size\()_\rep\()2
            mov\ss (%_addrq), %r14\s
            setf_a src=%r14\s, dst=%tmp\s, ss=\ss
            sub\ss %r14\s, %tmp\s
            setf_oc
            setf_zsp %tmp\s, \ss
        .endif; .endif; .endif; .endif; .endif

        movslq CPU_df_offset(%_cpu), %r15
        .ifin(\op, lods,movs,cmps)
            addq %r15, %rsi
        .endifin
        .ifin(\op, movs,stos,cmps,scas)
            addq %r15, %rdi
        .endifin

        .ifnc \rep,once
            decq %rcx
            .ifin(\op, scas,cmps)
                .ifc \rep,rep
                    test\ss %tmp\s, %tmp\s
                    jnz 2f
                .else; .ifc \rep,repnz
                    test\ss %tmp\s, %tmp\s
                    jz 2f
                .endif; .endif
            .endifin
            testq %rcx, %rcx
            jnz 1b
    2:
        .endif
        gret 1
.endm

.irp op, lods,stos,movs,scas,cmps
    .irp size, 8,16,32,64
        .irp rep, REP_LIST
            .if \size == 8
                do_strop \op, \size, \rep, b, b, al
            .elseif \size == 16
                do_strop \op, \size, \rep, w, w, ax
            .elseif \size == 32
                do_strop \op, \size, \rep, d, l, eax
            .elseif \size == 64
                do_strop \op, \size, \rep, , q, rax
            .endif
        .endr
    .endr
    .gadget_list_size \op, REP_LIST
.endr
# temporary

.tpl_close
