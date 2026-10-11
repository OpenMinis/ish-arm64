// Intel standard interrupts
// Any interrupt not handled specially becomes a SIGSEGV
#define INT_NONE -1
#define INT_DIV 0
#define INT_DEBUG 1
#define INT_NMI 2
#define INT_BREAKPOINT 3
#define INT_OVERFLOW 4
#define INT_BOUND 5
#define INT_UNDEFINED 6
#define INT_FPU 7 // do not try to use the fpu. instead, try to realize the truth: there is no fpu.
#define INT_DOUBLE 8 // interrupt during interrupt, i.e. interruptception
#define INT_GPF 13
#define INT_TIMER 32
#define INT_SYSCALL 0x80
#define INT_JIT_CRASH 0x100  // JIT SIGSEGV recovery (stale TLB after CoW)
#define INT_XJIT_PROMOTE 0x300  // x86_64 xjit: a block ran often enough to translate
                                 // (frame->xjit_promote); the run loop does it and goes on
#define INT_PREBUILT_RET 0x200  // nested guest call from a prebuilt-gadget spec_fn
                                 // returned to its sentinel LR (ARM64 only)
