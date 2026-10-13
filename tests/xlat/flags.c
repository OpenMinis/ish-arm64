// Flags across the ends of blocks, for xlat's write-back to the lazy flags (asbestos/guest-x86_64/
// xlat.c e_sync_flags): an operation that sets them, a transfer (jump, call and return, indirect
// jump, conditional jumps), an operation that sets some, all or none, then pushfq. Pure user-mode
// code: the same program built natively is the reference.  gcc -O1 -static -no-pie flags.c
#include <stdio.h>
#include <stdint.h>

uint64_t in_a, in_b, in_c;
__asm__(".text\nf_nop:\n ret\n");

#define FN(n, op1, xfer, op2) \
    uint64_t n(void); \
    __asm__(".text\n.globl " #n "\n" #n ":\n push %rbx\n mov in_a(%rip), %rax\n mov in_b(%rip), %rbx\n mov in_c(%rip), %rcx\n" \
            " xor %edx, %edx\n" op1 "\n" xfer "\n" op2 "\n pushfq\n pop %rax\n shl $8, %rdx\n and $0x8d5, %eax\n or %rdx, %rax\n pop %rbx\n ret\n");

// what sets the flags first; whether it defines AF
#define OPS1(X) \
    X(add, " add %rbx, %rax", 1) X(sub, " sub %rbx, %rax", 1) X(cmp, " cmp %rbx, %rax", 1) X(and, " and %rbx, %rax", 0) \
    X(xor, " xor %rbx, %rax", 0) X(test, " test %rbx, %rax", 0) X(neg, " neg %rax", 1) X(inc, " stc\n inc %rax", 1) \
    X(dec, " clc\n dec %rax", 1) X(adc, " stc\n adc %rbx, %rax", 1) X(sbb, " stc\n sbb %rbx, %rax", 1) \
    X(shl, " shl $1, %rax", 0) X(sar, " sar $1, %rax", 0) X(add32, " add %ebx, %eax", 1) X(sub8, " sub %bl, %al", 1)
// then how the next block is reached (D1), and what follows it there (D2): writing no flags (0),
// all the arithmetic ones (1), or all but CF (2)
#define D3(a, o1, af, x, xs, b, o2, w) FN(t_##a##_##x##_##b, o1, xs, o2)
#define D2(a, o1, af, x, xs) \
    D3(a, o1, af, x, xs, nop, " mov %rax, %r9", 0) D3(a, o1, af, x, xs, incc, " inc %rcx", 2) D3(a, o1, af, x, xs, decc, " dec %rcx", 2) \
    D3(a, o1, af, x, xs, addc, " add %rbx, %rcx", 1) D3(a, o1, af, x, xs, adc0, " adc $0, %rcx\n mov %rcx, %rdx\n and $0xff, %edx", 1) \
    D3(a, o1, af, x, xs, sbb0, " sbb %rcx, %rcx\n mov %rcx, %rdx\n and $0xff, %edx", 1) D3(a, o1, af, x, xs, setc, " setc %dl", 0) \
    D3(a, o1, af, x, xs, seto, " seto %dl", 0) D3(a, o1, af, x, xs, setl, " setl %dl", 0) D3(a, o1, af, x, xs, setbe, " setbe %dl", 0) \
    D3(a, o1, af, x, xs, cmovz, " mov $7, %r8d\n cmovz %r8d, %edx", 0) D3(a, o1, af, x, xs, cmovs, " mov $9, %r8d\n cmovs %r8d, %edx", 0)
#define D1(a, o1, af) \
    D2(a, o1, af, none, " nop") D2(a, o1, af, jmp, " jmp 1f\n1:") D2(a, o1, af, call, " call f_nop") \
    D2(a, o1, af, jmpi, " lea 1f(%rip), %r8\n jmp *%r8\n1:") D2(a, o1, af, jc, " jc 1f\n nop\n1:") D2(a, o1, af, jnz, " jnz 1f\n nop\n1:") \
    D2(a, o1, af, jl, " jl 2f\n jmp 1f\n2: nop\n1:")
OPS1(D1)

struct t { const char *name; uint64_t (*fn)(void); int af1, w2; };
#define E3(a, af, x, b, w) {#a " " #x " " #b, t_##a##_##x##_##b, af, w},
#define E2(a, af, x) E3(a, af, x, nop, 0) E3(a, af, x, incc, 2) E3(a, af, x, decc, 2) E3(a, af, x, addc, 1) E3(a, af, x, adc0, 1) \
    E3(a, af, x, sbb0, 1) E3(a, af, x, setc, 0) E3(a, af, x, seto, 0) E3(a, af, x, setl, 0) E3(a, af, x, setbe, 0) \
    E3(a, af, x, cmovz, 0) E3(a, af, x, cmovs, 0)
#define E1(a, o1, af) E2(a, af, none) E2(a, af, jmp) E2(a, af, call) E2(a, af, jmpi) E2(a, af, jc) E2(a, af, jnz) E2(a, af, jl)
static const struct t tests[] = { OPS1(E1) };

int main(void) {
    static const uint64_t vals[][3] = {
        {0, 0, 0}, {1, 1, 0xffffffffffffffffull}, {0x7fffffffffffffffull, 1, 0x7fffffffffffffffull}, {0x8000000000000000ull, 0x8000000000000000ull, 1},
        {5, 9, 0xff}, {0xffffffffull, 1, 0x10}, {0x80, 0x7f, 0xffffffffffffffffull}, {0x1234567890abcdefull, 0xfedcba0987654321ull, 0xf},
    };
    for (unsigned i = 0; i < sizeof(tests) / sizeof(tests[0]); i++) {
        printf("%-18s", tests[i].name);
        for (unsigned v = 0; v < sizeof(vals) / sizeof(vals[0]); v++) {
            // several times: the first runs are gadgets, the later ones translated
            uint64_t r = 0;
            for (int rep = 0; rep < 8; rep++) {
                in_a = vals[v][0]; in_b = vals[v][1]; in_c = vals[v][2];
                uint64_t x = tests[i].fn();
                if (rep > 0 && x != r) printf(" UNSTABLE");
                r = x;
            }
            // AF only where the last one to write the flags defines it
            if (!(tests[i].w2 || tests[i].af1))
                r &= ~0x10ull;
            printf(" %05llx", (unsigned long long) r);
        }
        printf("\n");
    }
    return 0;
}
