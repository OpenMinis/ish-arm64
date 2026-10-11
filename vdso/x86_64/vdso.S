    .text
    // Signal return trampoline. musl and glibc pass their own restorer with
    // SA_RESTORER; the kernel falls back to this one.
    .global __kernel_rt_sigreturn
    .type __kernel_rt_sigreturn,@function
__kernel_rt_sigreturn:
    mov $15, %rax // NR_rt_sigreturn
    syscall
    nop
