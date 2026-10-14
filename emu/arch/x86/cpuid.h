#ifndef CPUID_H
#define CPUID_H

#include "misc.h"

static inline void do_cpuid(dword_t *eax, dword_t *ebx, dword_t *ecx, dword_t *edx) {
    dword_t leaf = *eax;
    switch (leaf) {
        case 0:
            *eax = 0x01; // we support barely anything
            *ebx = 0x756e6547; // Genu
            *edx = 0x49656e69; // ineI
            *ecx = 0x6c65746e; // ntel
            break;
        case 0x80000000:
            *eax = 0x80000006; // highest extended leaf
            *ebx = *ecx = *edx = 0;
            break;
#ifdef GUEST_X86_64
        case 0x80000001:
            *eax = *ebx = 0;
            *ecx = 0;
            *edx = (1 << 11) // syscall/sysret
                | (1 << 20) // nx
                | (1 << 29) // long mode
                ;
            break;
        case 0x80000002 ... 0x80000005:
#else
        case 0x80000001:
        case 0x80000002 ... 0x80000005:
#endif
            *eax = *ebx = *ecx = *edx = 0;
            break;
        case 0x80000006:
            // L2 cache: 256KB, 8-way, 64-byte lines (OpenBLAS sizes its
            // blocking from this and warns when it is zero)
            *eax = *ebx = *edx = 0;
            *ecx = (256 << 16) | (0x6 << 12) | 64;
            break;
        default: // if leaf is too high, use highest supported leaf
        case 1:
            *eax = 0x0; // say nothing about cpu model number
            *ebx = 0x0; // processor number 0, flushes 0 bytes on clflush
#ifdef GUEST_X86_64
            // Family 6, model 0x2c, stepping 2 (Westmere: the generation with
            // the features below and no AVX). Family 0 is "older than a
            // Pentium" to software that looks: the JVM reads no feature bit
            // of such a CPU and gives up ("SSE2 not supported").
            *eax = 0x000206c2;
            *ebx = 8 << 8; // clflush works on 64 bytes
            // x86-64-v2 (what bun and claude's native build require)
            *ecx = (1 << 0) // sse3
                | (1 << 9) // ssse3
                | (1 << 13) // cmpxchg16b
                | (1 << 19) // sse4.1
                | (1 << 20) // sse4.2
                | (1 << 23) // popcnt
                ;
#else
            *ecx = 0; // we support none of the features in ecx
#endif
            *edx = (1 << 0) // fpu
#ifdef GUEST_X86_64
                | (1 << 4) // tsc
                | (1 << 8) // cmpxchg8b
                | (1 << 19) // clflush (decoded, does nothing: writes to code are noticed anyway)
#endif
                | (1 << 15) // cmov
                | (1 << 23) // mmx
                | (1 << 24) // fxsr: fxsave/fxrstor, the OS saves SSE state
                | (1 << 25) // sse
                | (1 << 26) // sse2
                ;
            break;
    }
}

#endif
