// SSE floating point arithmetic and conversions as xlat translates them
// (asbestos/guest-x86_64/xlat.c, classify_sse): scalar and packed, register
// and unaligned memory operands, xmm8-15, r8-r15, special values (NaN, inf,
// +-0, denormals, values that round), and a changed MXCSR rounding mode.
// Prints the results; the JIT build must print what the gadget build does.
#include <stdio.h>
#include <stdint.h>
#include <string.h>

static const uint64_t dvals[] = {
    0x0000000000000000, 0x8000000000000000, 0x3ff0000000000000, 0xbff8000000000000,
    0x7ff0000000000000, 0xfff0000000000000, 0x7ff8000000000000, 0xfff8000000000001,
    0x7ff4000000000000, 0x0000000000000001, 0x800fffffffffffff, 0x400921fb54442d18,
    0x43e0000000000000, 0xc3e0000000000001, 0x41dfffffffc00000, 0x3fb999999999999a,
};
static const uint32_t fvals[] = {
    0x00000000, 0x80000000, 0x3f800000, 0xbfc00000, 0x7f800000, 0xff800000, 0x7fc00000, 0xffc00001,
    0x7fa00000, 0x00000001, 0x807fffff, 0x40490fdb, 0x5f000000, 0xdf000001, 0x4effffff, 0x3dcccccd,
};
#define ND (sizeof(dvals) / sizeof(dvals[0]))
static uint8_t buf[64] __attribute__((aligned(64)));
static void dump(const char *name, const void *p, size_t n) {
    printf("%-14s", name);
    for (size_t i = 0; i < n; i++) printf("%02x", ((const uint8_t *) p)[i]);
    printf("\n");
}
typedef struct { uint8_t b[16]; } v16;
static v16 pair_d(uint64_t a, uint64_t b) { v16 v; memcpy(v.b, &a, 8); memcpy(v.b + 8, &b, 8); return v; }
static v16 quad_f(uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
    v16 v; memcpy(v.b, &a, 4); memcpy(v.b + 4, &b, 4); memcpy(v.b + 8, &c, 4); memcpy(v.b + 12, &d, 4); return v;
}

// xmm10 op= xmm2 (register form), and xmm11 op= memory at buf+off; packed
// operations take an aligned operand (unaligned faults on x86, so that the
// output can be checked against the hardware)
#define OP(insn) static void t_##insn(v16 a, v16 b, int off, int msize) { \
    v16 r1, r2; if (msize == 16) off = 16; memcpy(buf + off, &b, msize); \
    __asm__ volatile("movdqu %2, %%xmm10\n movdqu %3, %%xmm2\n " #insn " %%xmm2, %%xmm10\n movdqu %%xmm10, %0\n" \
                     "movdqu %2, %%xmm11\n " #insn " %4, %%xmm11\n movdqu %%xmm11, %1" \
        : "=m"(r1), "=m"(r2) : "m"(a), "m"(b), "m"(*(v16 *) (buf + off)) : "xmm2", "xmm10", "xmm11", "memory"); \
    dump(#insn, &r1, 16); dump(#insn " m", &r2, 16); }
OP(addsd) OP(subsd) OP(mulsd) OP(divsd) OP(sqrtsd) OP(minsd) OP(maxsd) OP(cvtsd2ss)
OP(addss) OP(subss) OP(mulss) OP(divss) OP(sqrtss) OP(minss) OP(maxss) OP(cvtss2sd)
OP(addpd) OP(subpd) OP(mulpd) OP(divpd) OP(sqrtpd) OP(minpd) OP(maxpd)
OP(addps) OP(subps) OP(mulps) OP(divps) OP(sqrtps) OP(minps) OP(maxps)

static void t_cvt(uint64_t i64, v16 x, int off) {
    v16 r; uint64_t o1, o2, o3, o4;
    // cvtsi2sd/ss from r9 (64 and 32 bit) and from memory
    memcpy(buf + off, &i64, 8);
    __asm__ volatile("movdqu %1, %%xmm13\n mov %2, %%r9\n cvtsi2sdq %%r9, %%xmm13\n cvtsi2ssl %%r9d, %%xmm13\n movdqu %%xmm13, %0"
        : "=m"(r) : "m"(x), "r"(i64) : "r9", "xmm13");
    dump("cvtsi2sd/ss r", &r, 16);
    __asm__ volatile("movdqu %1, %%xmm14\n cvtsi2sdq %2, %%xmm14\n cvtsi2ssl %3, %%xmm14\n movdqu %%xmm14, %0"
        : "=m"(r) : "m"(x), "m"(*(uint64_t *) (buf + off)), "m"(*(uint32_t *) (buf + off + 1)) : "xmm14");
    dump("cvtsi2sd/ss m", &r, 16);
    // cvttsd2si/ss2si into r12 and rax, from a register and from memory
    memcpy(buf + off, x.b, 16);
    __asm__ volatile("movdqu %4, %%xmm15\n cvttsd2si %%xmm15, %%r12\n mov %%r12, %0\n cvttss2si %%xmm15, %%eax\n mov %%rax, %1\n"
                     "cvttsd2si %5, %%rax\n mov %%rax, %2\n cvttss2si %6, %%r12d\n mov %%r12, %3"
        : "=m"(o1), "=m"(o2), "=m"(o3), "=m"(o4) : "m"(x), "m"(*(uint64_t *) (buf + off)), "m"(*(uint32_t *) (buf + off + 4))
        : "rax", "r12", "xmm15");
    printf("cvtt %016llx %016llx %016llx %016llx\n", (unsigned long long) o1, (unsigned long long) o2,
           (unsigned long long) o3, (unsigned long long) o4);
}

int main(void) {
    uint32_t mx;
    for (int rc = 0; rc < 4; rc++) {
        // the gadgets round arithmetic to nearest whatever MXCSR says; so must the JIT
        uint32_t set = 0x1f80 | (uint32_t) rc << 13;
        __asm__ volatile("ldmxcsr %0" : : "m"(set));
        for (unsigned i = 0; i < ND; i++) {
            for (unsigned j = 0; j < ND; j += 3) {
                int off = (int) ((i * 7 + j) % 40) + 1;
                v16 a = pair_d(dvals[i], dvals[(i + 5) % ND]), b = pair_d(dvals[j], dvals[(j + 9) % ND]);
                v16 fa = quad_f(fvals[i], fvals[(i + 3) % ND], fvals[(i + 7) % ND], fvals[(i + 11) % ND]);
                v16 fb = quad_f(fvals[j], fvals[(j + 1) % ND], fvals[(j + 5) % ND], fvals[(j + 13) % ND]);
                t_addsd(a, b, off, 8); t_subsd(a, b, off, 8); t_mulsd(a, b, off, 8); t_divsd(a, b, off, 8);
                t_sqrtsd(a, b, off, 8); t_minsd(a, b, off, 8); t_maxsd(a, b, off, 8); t_cvtsd2ss(a, b, off, 8);
                t_addss(fa, fb, off, 4); t_subss(fa, fb, off, 4); t_mulss(fa, fb, off, 4); t_divss(fa, fb, off, 4);
                t_sqrtss(fa, fb, off, 4); t_minss(fa, fb, off, 4); t_maxss(fa, fb, off, 4); t_cvtss2sd(fa, fb, off, 4);
                t_addpd(a, b, off, 16); t_subpd(a, b, off, 16); t_mulpd(a, b, off, 16); t_divpd(a, b, off, 16);
                t_sqrtpd(a, b, off, 16); t_minpd(a, b, off, 16); t_maxpd(a, b, off, 16);
                t_addps(fa, fb, off, 16); t_subps(fa, fb, off, 16); t_mulps(fa, fb, off, 16); t_divps(fa, fb, off, 16);
                t_sqrtps(fa, fb, off, 16); t_minps(fa, fb, off, 16); t_maxps(fa, fb, off, 16);
            }
            static const uint64_t ints[] = {0, 1, (uint64_t) -1, 0x7fffffffffffffff, 0x8000000000000000,
                                            0x20000000000001, 0xfffffffffffffc01, 0x123456789abcdef};
            t_cvt(ints[i % 8], pair_d(dvals[i], dvals[(i + 1) % ND]), (int) (i % 40) + 1);
            t_cvt(ints[(i + 3) % 8], quad_f(fvals[i], fvals[(i + 2) % ND], 0, 0), (int) (i % 33) + 3);
        }
        __asm__ volatile("stmxcsr %0" : "=m"(mx));
        printf("mxcsr %08x\n", mx);
    }
    return 0;
}
