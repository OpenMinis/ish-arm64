.text
.global probe_call
.type probe_call,%function
probe_call:
 stp x19,x20,[sp,#-112]!
 stp x21,x22,[sp,#16]
 stp x23,x24,[sp,#32]
 stp x25,x26,[sp,#48]
 stp x27,x28,[sp,#64]
 stp x29,x30,[sp,#80]
 blr x0
 ldp x21,x22,[sp,#16]
 ldp x23,x24,[sp,#32]
 ldp x25,x26,[sp,#48]
 ldp x27,x28,[sp,#64]
 ldp x29,x30,[sp,#80]
 ldp x19,x20,[sp],#112
 ret
.section .note.GNU-stack,"",%progbits
