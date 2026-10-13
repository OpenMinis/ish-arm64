// The same machine code native and as a guest: asm kernels, one pattern each, N iterations.
#include <stdio.h>
#include <stdint.h>
#include <time.h>
static double now(void) { struct timespec ts; timespec_get(&ts, TIME_UTC); return ts.tv_sec + ts.tv_nsec / 1e9; }
#define N 300000000
#define STR2(x) #x
#define STR(x) STR2(x)
uint64_t kbuf[1024], kbuf2[1024];
#define KERNEL(name, body) \
    uint64_t name(void); \
    __asm__(".text\n.globl " #name "\n" #name ":\n push %rbx\n push %rsi\n push %rdi\n push %rbp\n push %r12\n push %r13\n" \
            " lea kbuf(%rip), %rdi\n lea kbuf2(%rip), %rsi\n xor %eax, %eax\n xor %ebx, %ebx\n mov $" STR(N) ", %ecx\n" \
            "1:\n" body "\n dec %ecx\n jnz 1b\n pop %r13\n pop %r12\n pop %rbp\n pop %rdi\n pop %rsi\n pop %rbx\n ret\n");
__asm__(".text\nk_empty_fn:\n ret\nk_leaf_fn:\n push %rbp\n mov %rsp, %rbp\n add $1, %rax\n pop %rbp\n ret\n");
KERNEL(k_loop,      " add $1, %rax")
KERNEL(k_call,      " call k_empty_fn")
KERNEL(k_call_leaf, " call k_leaf_fn")
KERNEL(k_calli,     " lea k_empty_fn(%rip), %rdx\n call *%rdx")
KERNEL(k_pushpop,   " push %rax\n pop %rbx")
KERNEL(k_pushpop3,  " push %rax\n push %rbx\n push %rcx\n pop %rcx\n pop %rbx\n pop %rax")
KERNEL(k_stk_fwd,   " mov %rax, -8(%rsp)\n mov -8(%rsp), %rbx\n add $1, %rax")
KERNEL(k_heap_fwd,  " mov %rax, (%rdi)\n mov (%rdi), %rbx\n add $1, %rax")
KERNEL(k_load1,     " add (%rdi), %rax")
KERNEL(k_load2grp,  " add (%rdi), %rax\n add 8(%rdi), %rax")
KERNEL(k_load2,     " add (%rdi), %rax\n add (%rsi), %rax")
KERNEL(k_store2,    " mov %rax, (%rdi)\n mov %rax, (%rsi)\n add $1, %rax")
KERNEL(k_rmw,       " addq $1, (%rdi)")
KERNEL(k_jcc_nt,    " test %eax, %eax\n js 1b\n add $1, %rax")
KERNEL(k_jcc_alt,   " add $1, %rax\n test $1, %al\n jz 2f\n add $2, %rbx\n2:")
KERNEL(k_jmp,       " jmp 2f\n2: add $1, %rax")
KERNEL(k_r12,       " add $1, %r12")
KERNEL(k_r12_use,   " add %r12, %rax\n add $1, %r12")
KERNEL(k_r12_r13,   " add %r12, %r13\n add %r13, %r12")
KERNEL(k_rbx_rbp,   " add %rbx, %rbp\n add %rbp, %rbx")
KERNEL(k_r12_ptr,   " mov (%rdi), %r12\n add %r12, %rax")
KERNEL(k_r13_base,  " lea kbuf(%rip), %r13\n add 8(%r13), %rax")
KERNEL(k_cmp_mem,   " cmp %rax, (%rdi)\n je 1b\n add $1, %rax")
int main(void) {
    static const struct { const char *name; uint64_t (*fn)(void); } ks[] = {
#define K(n) {#n, n}
        K(k_loop), K(k_call), K(k_call_leaf), K(k_calli), K(k_pushpop), K(k_pushpop3), K(k_stk_fwd), K(k_heap_fwd),
        K(k_load1), K(k_load2grp), K(k_load2), K(k_store2), K(k_rmw), K(k_jcc_nt), K(k_jcc_alt), K(k_jmp), K(k_r12), K(k_r12_use), K(k_r12_r13), K(k_rbx_rbp), K(k_r12_ptr), K(k_r13_base), K(k_cmp_mem),
    };
    for (unsigned i = 0; i < sizeof(ks) / sizeof(ks[0]); i++) {
        double t = now();
        uint64_t r = ks[i].fn();
        printf("%-12s %6.2f ns/iter  %llx\n", ks[i].name, (now() - t) * 1e9 / N, (unsigned long long) (r & 0xff));
        fflush(stdout);
    }
    return 0;
}
