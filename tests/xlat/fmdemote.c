// The guest window (kernel/memory.c FASTMEM, asbestos/guest-x86_64/xlat.c): an
// access to memory outside the window (a shared mapping) faults to its checked
// path every time until its site is demoted (re-translated checked), between
// window accesses of the same block. Prints sums; compare ISH_FASTMEM=2 (and
// ISH_FASTMEM=1 ISH_FASTMEM_TICKS=1) with ISH_FASTMEM=0. ISH_XJIT_STATS=1
// shows the demotions. A window access given a wrong base after a demoted
// one mostly faults and recovers, so this doesn't catch that reliably: npm
// install in benchmark/daily did.  gcc -O1 -static fmdemote.c
#include <stdio.h>
#include <stdint.h>
#include <sys/mman.h>

uint64_t mix(volatile uint64_t *shared, uint64_t *local, int n);
__asm__(
"    .text\n"
"    .globl mix\n"
"mix:\n"
"    xor %eax, %eax\n"
"1:  mov (%rsi), %r8\n"                   // local: in the window first,
"    add (%rdi), %r8\n"                   // then shared, outside it,
"    mov %r8, 8(%rsi)\n"                  // then local again, same block
"    add %r8, %rax\n"
"    add $1, %r8\n"
"    mov %r8, (%rdi)\n"                   // shared again
"    add 8(%rsi), %rax\n"                  // and local again
"    dec %edx\n"
"    jnz 1b\n"
"    ret\n");

int main(void) {
    volatile uint64_t *shared = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    uint64_t local[2] = {3, 0};
    for (int round = 0; round < 6; round++) {
        shared[0] = (uint64_t) round;
        uint64_t s = mix(shared, local, 500);
        printf("round %d: %llu %llu %llu\n", round, (unsigned long long) s,
               (unsigned long long) shared[0], (unsigned long long) local[1]);
    }
    return 0;
}
