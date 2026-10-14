#include <time.h>
#include <stdio.h>
#include "emu/cpu.h"

#if defined(GUEST_X86) || !defined(GUEST_ARM64)
#include "emu/arch/x86/cpuid.h"

void helper_cpuid(dword_t *a, dword_t *b, dword_t *c, dword_t *d) {
    do_cpuid(a, b, c, d);
}

#ifdef GUEST_X86_64
#include <string.h>
#include "emu/tlb.h"
// fxsave / fxrstor (0f ae /0, /1): the 512-byte FXSAVE area. Programs save
// and restore all their registers around calls into other code with them --
// the JVM's safepoint and deoptimization stubs do -- so as no-ops the XMM
// registers came back as whatever that code left in them. The FPU and data
// pointers are written as 0; the MMX registers, kept apart from the x87 stack
// by iSH, are not in it. Through the TLB, a page at a time: the area is 16-
// byte aligned and may span two.
static void fx_copy(struct tlb *tlb, addr_t addr, uint8_t *buf, bool write) {
    for (unsigned off = 0; off < 512;) {
        unsigned n = 512 - off, room = PAGE_SIZE - PGOFFSET(addr + off);
        if (n > room)
            n = room;
        if (write)
            tlb_write(tlb, addr + off, buf + off, n);
        else
            tlb_read(tlb, addr + off, buf + off, n);
        off += n;
    }
}

_Static_assert(sizeof(((struct cpu_state *) 0)->xmm) == 16 * 16, "16 xmm registers");
void helper_fxsave(struct cpu_state *cpu, struct tlb *tlb, addr_t addr) {
    uint8_t b[512];
    memset(b, 0, sizeof(b));
    uint16_t fcw = cpu->fcw, fsw = cpu->fsw;
    fsw = (fsw & ~0x3800) | (uint16_t) ((cpu->top & 7) << 11);
    memcpy(b + 0, &fcw, 2);
    memcpy(b + 2, &fsw, 2);
    b[4] = 0xff;   // abridged tag: every register valid
    uint32_t mxcsr = cpu->mxcsr, mask = 0xffff;
    memcpy(b + 24, &mxcsr, 4);
    memcpy(b + 28, &mask, 4);
    for (int i = 0; i < 8; i++)
        memcpy(b + 32 + 16 * i, &cpu->fp[(cpu->top + i) % 8], 10);
    memcpy(b + 160, cpu->xmm, 16 * 16);
    fx_copy(tlb, addr, b, true);
}

void helper_fxrstor(struct cpu_state *cpu, struct tlb *tlb, addr_t addr) {
    uint8_t b[512];
    fx_copy(tlb, addr, b, false);
    uint16_t fcw, fsw;
    memcpy(&fcw, b + 0, 2);
    memcpy(&fsw, b + 2, 2);
    cpu->fcw = fcw;
    cpu->top = (fsw >> 11) & 7;
    cpu->fsw = fsw;
    uint32_t mxcsr;
    memcpy(&mxcsr, b + 24, 4);
    cpu->mxcsr = mxcsr & 0xffff;
    for (int i = 0; i < 8; i++)
        memcpy(&cpu->fp[(cpu->top + i) % 8], b + 32 + 16 * i, 10);
    memcpy(cpu->xmm, b + 160, 16 * 16);
}
#endif

void helper_rdtsc(struct cpu_state *cpu) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    uint64_t tsc = now.tv_sec * 1000000000l + now.tv_nsec;
    cpu->eax = tsc & 0xffffffff;
    cpu->edx = tsc >> 32;
}

#elif defined(GUEST_ARM64)

// ARM64 doesn't have CPUID - this is a stub for compatibility
void helper_cpuid(dword_t *a, dword_t *b, dword_t *c, dword_t *d) {
    (void)a; (void)b; (void)c; (void)d;
}

// ARM64 counter register access
void helper_rdtsc(struct cpu_state *cpu) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    uint64_t tsc = now.tv_sec * 1000000000l + now.tv_nsec;
    // Store in x0 for ARM64
    cpu->regs[0] = tsc;
}

#endif

void helper_expand_flags(struct cpu_state *cpu) {
    expand_flags(cpu);
}

void helper_collapse_flags(struct cpu_state *cpu) {
    collapse_flags(cpu);
}
