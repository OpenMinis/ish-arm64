#include "cpu-offsets.h"
.text
.global restart_call
.type restart_call,%function
// x0 = code, x1 = frame/CPU, x2 = tlb entries, x3 = PIC context
restart_call:
 stp x19,x20,[sp,#-112]!
 stp x21,x22,[sp,#16]
 stp x23,x24,[sp,#32]
 stp x25,x26,[sp,#48]
 stp x27,x28,[sp,#64]
 stp x29,x30,[sp,#80]
 mov x8,sp
 str x8,[x1,#LOCAL_jit_exit_sp]
 mov x29,x3
 blr x0
 mov w0,#0
 b restart_return
.global jit_crash_trampoline
.type jit_crash_trampoline,%function
jit_crash_trampoline:
 mov w0,#0x100
restart_return:
 ldp x21,x22,[sp,#16]
 ldp x23,x24,[sp,#32]
 ldp x25,x26,[sp,#48]
 ldp x27,x28,[sp,#64]
 ldp x29,x30,[sp,#80]
 ldp x19,x20,[sp],#112
 ret
.section .note.GNU-stack,"",%progbits
