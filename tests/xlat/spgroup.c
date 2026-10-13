// Stack access groups of xlat (asbestos/guest-x86_64/xlat.c find_sp_groups): runs of pushes, pops,
// rsp-relative operands, calls and returns with one page check, at every stack position around a
// page boundary and on pages never touched before. Prints a checksum per position; compare with
// ISH_XLAT=0 and ISH_XLAT_NOSPGROUP=1.  gcc -O1 -static -no-pie spgroup.c
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>

uint64_t saved_rsp;
void sp_seq(void *top, uint64_t *vals);

__asm__(
"    .text\n"
"    .globl sp_seq\n"
"sp_seq:\n"
"    mov %rsp, saved_rsp(%rip)\n"
"    mov %rdi, %rsp\n"
"    push %rbx\n push %rbp\n push %r12\n push %r13\n push %r14\n push %r15\n"
"    mov 0(%rsi), %rbx\n mov 8(%rsi), %rbp\n mov 16(%rsi), %r12\n mov 24(%rsi), %r13\n"
"    mov 32(%rsi), %r14\n mov 40(%rsi), %r15\n"
"    push %rsi\n push %rbx\n push %rbp\n push %r12\n push %r13\n"
"    sub $40, %rsp\n"
"    mov %r12, 8(%rsp)\n"
"    mov %ebx, 20(%rsp)\n"
"    movb $0x5a, 39(%rsp)\n"
"    mov %r14, 24(%rsp)\n"
"    add %r13, 24(%rsp)\n"
"    movw $0x1234, (%rsp)\n"
"    call sp_leaf\n"
"    mov 8(%rsp), %r14\n"
"    add 24(%rsp), %r14\n"
"    movzbl 39(%rsp), %eax\n"
"    add %rax, %r15\n"
"    add $40, %rsp\n"
"    pop %rax\n pop %rcx\n pop %rdx\n"
"    lea 8(%rsp), %rsp\n"
"    pushq $0x1234567\n"
"    push $-3\n"
"    push %rsp\n"
"    pop %r8\n"
"    sub %rsp, %r8\n"            // 0: push %rsp pushes the old value
"    pop %r9\n pop %r10\n"
"    push %r14\n push %r15\n push %r8\n push %r9\n"
"    call sp_retn\n"
"    add %rax, %r10\n"
"    mov 0(%rsp), %rsi\n"         // the vals pointer pushed first
"    pop %rdi\n"
"    mov %rbx, 0(%rsi)\n mov %rbp, 8(%rsi)\n mov %r12, 16(%rsi)\n mov %r13, 24(%rsi)\n"
"    mov %r14, 32(%rsi)\n mov %r15, 40(%rsi)\n mov %rcx, 48(%rsi)\n mov %rdx, 56(%rsi)\n"
"    mov %r8, 64(%rsi)\n mov %r9, 72(%rsi)\n mov %r10, 80(%rsi)\n"
"    pop %r15\n pop %r14\n pop %r13\n pop %r12\n pop %rbp\n pop %rbx\n"
"    mov saved_rsp(%rip), %rsp\n"
"    ret\n"
"sp_leaf:\n"
"    push %rbp\n mov %rsp, %rbp\n push %rbx\n push %r12\n sub $24, %rsp\n"
"    mov %r12, (%rsp)\n mov %rbx, 16(%rsp)\n"
"    lea (%r12,%rbx,2), %rbx\n"
"    xor 16(%rsp), %rbx\n"
"    mov %rbx, %r12\n"
"    add (%rsp), %r12\n"
"    mov %r12, 8(%rsp)\n"
"    mov 8(%rsp), %r13\n"
"    add $24, %rsp\n pop %r12\n pop %rbx\n pop %rbp\n"
"    ret\n"
"sp_retn:\n"                     // four arguments on the stack, popped by the return
"    mov 8(%rsp), %rax\n"
"    add 16(%rsp), %rax\n"
"    xor 24(%rsp), %rax\n"
"    add 32(%rsp), %rax\n"
"    push %rax\n"
"    pop %rax\n"
"    ret $32\n"
);

static uint64_t hash(const void *p, size_t n, uint64_t h) {
    const uint8_t *b = p;
    for (size_t i = 0; i < n; i++)
        h = (h ^ b[i]) * 0x100000001b3ull;
    return h;
}

int main(void) {
    uint64_t seed = 0x9e3779b97f4a7c15ull, total = 0;
    // every position around a page boundary, six times each (the blocks get translated)
    uint8_t *region = mmap(NULL, 4 * 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    for (int off = -320; off <= 320; off += 8) {
        uint64_t h = 0xcbf29ce484222325ull;
        for (int rep = 0; rep < 6; rep++) {
            uint64_t vals[11];
            for (int i = 0; i < 11; i++) {
                seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17;
                vals[i] = seed;
            }
            memset(region, rep, 4 * 4096);
            sp_seq(region + 2 * 4096 + off, vals);
            // the call's return addresses are on the stack: the same in every run (static, no PIE)
            h = hash(vals, sizeof(vals), hash(region, 4 * 4096, h));
        }
        printf("off %4d %016llx\n", off, (unsigned long long) h);
        total ^= h;
    }
    // pages never touched: the group's check has to fault them in, at and around the boundary
    for (int k = 0; k < 64; k++) {
        uint8_t *fresh = mmap(NULL, 4 * 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        uint64_t vals[11];
        for (int i = 0; i < 11; i++)
            vals[i] = seed + (uint64_t) i * k;
        int off = (k % 16 - 8) * 8;
        sp_seq(fresh + 2 * 4096 + off, vals);
        uint64_t h = hash(vals, sizeof(vals), hash(fresh, 4 * 4096, 0xcbf29ce484222325ull));
        printf("fresh %2d off %4d %016llx\n", k, off, (unsigned long long) h);
        total ^= h;
        munmap(fresh, 4 * 4096);
    }
    printf("total %016llx\n", (unsigned long long) total);
    return 0;
}
