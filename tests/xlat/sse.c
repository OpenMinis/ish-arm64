// Every SSE form xlat (asbestos/guest-x86_64/xlat.c) translates natively, register and memory (unaligned)
// forms, high xmm and r8-r15 registers; prints the results.
#include <stdio.h>
#include <stdint.h>
#include <string.h>

static uint8_t buf[256] __attribute__((aligned(64)));
static uint64_t seed = 0x123456789abcdef1;
static uint64_t rnd(void) { seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17; return seed; }
static void fill(void *p, size_t n) { for (size_t i = 0; i < n; i++) ((uint8_t *) p)[i] = (uint8_t) rnd(); }
static void dump(const char *name, const void *p, size_t n) {
    printf("%-12s", name);
    for (size_t i = 0; i < n; i++) printf("%02x", ((const uint8_t *) p)[i]);
    printf("\n");
}

typedef struct { uint8_t b[16]; } v16;

// reg-reg: dst (xmm9) op= src (xmm3), result of xmm9 and xmm3
#define RR(name, insn) static void t_rr_##name(void) { \
    v16 a, b, r1, r2; fill(&a, 16); fill(&b, 16); \
    __asm__ volatile("movdqu %2, %%xmm9\n movdqu %3, %%xmm3\n" insn " %%xmm3, %%xmm9\n movdqu %%xmm9, %0\n movdqu %%xmm3, %1" \
        : "=m"(r1), "=m"(r2) : "m"(a), "m"(b) : "xmm9", "xmm3", "memory"); \
    dump(#name " rr", &r1, 16); dump(#name " rr2", &r2, 16); }
// reg-mem: dst (xmm12) op= unaligned memory
#define RM(name, insn, size) static void t_rm_##name(int off) { \
    v16 a, r1; fill(&a, 16); fill(buf, sizeof(buf)); \
    __asm__ volatile("movdqu %1, %%xmm12\n" insn " %2, %%xmm12\n movdqu %%xmm12, %0" \
        : "=m"(r1) : "m"(a), "m"(*(uint8_t (*)[size]) (buf + off)) : "xmm12", "memory"); \
    dump(#name " rm", &r1, 16); }
// mem-reg store: memory (unaligned) <- xmm5
#define MR(name, insn, size) static void t_mr_##name(int off) { \
    v16 a; fill(&a, 16); fill(buf, sizeof(buf)); \
    __asm__ volatile("movdqu %0, %%xmm5\n" insn " %%xmm5, %1" \
        : : "m"(a), "m"(*(uint8_t (*)[size]) (buf + off)) : "xmm5", "memory"); \
    dump(#name " mr", buf + off - 4, size + 8); }

RR(movups, "movups") RR(movaps, "movaps") RR(movss, "movss") RR(movsd, "movsd")
RR(movdqa, "movdqa") RR(movdqu, "movdqu") RR(movhlps, "movhlps") RR(movlhps, "movlhps")
RR(movq, "movq") RR(pxor, "pxor") RR(por, "por") RR(pand, "pand") RR(pandn, "pandn")
RR(xorps, "xorps") RR(andps, "andps") RR(andnps, "andnps") RR(orps, "orps") RR(xorpd, "xorpd")
RR(unpcklps, "unpcklps") RR(unpckhpd, "unpckhpd")
RR(punpcklbw, "punpcklbw") RR(punpcklwd, "punpcklwd") RR(punpckldq, "punpckldq") RR(punpcklqdq, "punpcklqdq")
RR(punpckhbw, "punpckhbw") RR(punpckhqdq, "punpckhqdq") RR(packsswb, "packsswb") RR(packuswb, "packuswb")
RR(pcmpeqb, "pcmpeqb") RR(pcmpeqw, "pcmpeqw") RR(pcmpeqd, "pcmpeqd") RR(pcmpgtb, "pcmpgtb") RR(pcmpgtd, "pcmpgtd")
RR(paddb, "paddb") RR(paddw, "paddw") RR(paddd, "paddd") RR(paddq, "paddq") RR(psubb, "psubb") RR(psubq, "psubq")
RR(pminub, "pminub") RR(pmaxub, "pmaxub") RR(pmullw, "pmullw") RR(pmuludq, "pmuludq") RR(psadbw, "psadbw")
RR(pavgb, "pavgb") RR(paddusb, "paddusb") RR(psubusw, "psubusw") RR(pmulhw, "pmulhw") RR(pmaddwd, "pmaddwd")

RM(movups, "movups", 16) RM(movss, "movss", 4) RM(movsd, "movsd", 8) RM(movdqu, "movdqu", 16)
RM(movlps, "movlps", 8) RM(movhps, "movhps", 8) RM(movlpd, "movlpd", 8) RM(movhpd, "movhpd", 8)
RM(movq, "movq", 8) RM(pxor, "pxor", 16) RM(por, "por", 16) RM(xorps, "xorps", 16)
RM(punpcklqdq, "punpcklqdq", 16) RM(punpcklbw, "punpcklbw", 16) RM(pcmpeqb, "pcmpeqb", 16)
RM(paddq, "paddq", 16) RM(pminub, "pminub", 16) RM(unpcklps, "unpcklps", 16)

MR(movups, "movups", 16) MR(movss, "movss", 4) MR(movsd, "movsd", 8) MR(movdqu, "movdqu", 16)
MR(movlps, "movlps", 8) MR(movhps, "movhps", 8) MR(movq, "movq", 8)

static void t_misc(int off) {
    v16 a, b, r; uint64_t g64 = rnd(), out64 = 0; uint32_t g32 = (uint32_t) rnd(), out32 = 0, mask = 0;
    fill(&a, 16); fill(&b, 16); fill(buf, sizeof(buf));
    // movq/movd from r8-r15 and memory, into xmm and back
    __asm__ volatile("mov %3, %%r10\n movq %%r10, %%xmm7\n movdqu %%xmm7, %0\n movq %%xmm7, %%r11\n mov %%r11, %1"
        : "=m"(r), "=m"(out64) : "m"(a), "m"(g64) : "r10", "r11", "xmm7", "memory");
    dump("movq g->x", &r, 16); printf("movq x->g   %016llx\n", (unsigned long long) out64);
    __asm__ volatile("movdqu %2, %%xmm11\n movd %3, %%xmm11\n movdqu %%xmm11, %0\n movd %%xmm11, %%r9d\n mov %%r9d, %1"
        : "=m"(r), "=m"(out32) : "m"(a), "m"(g32) : "r9", "xmm11", "memory");
    dump("movd m->x", &r, 16); printf("movd x->g   %08x\n", out32);
    __asm__ volatile("movdqu %1, %%xmm2\n movq %2, %%xmm2\n movdqu %%xmm2, %0"
        : "=m"(r) : "m"(a), "m"(*(uint64_t *) (buf + off)) : "xmm2", "memory");
    dump("movq m->x", &r, 16);
    __asm__ volatile("movdqu %1, %%xmm13\n movq %%xmm13, %0" : "=m"(*(uint64_t *) (buf + off + 3)) : "m"(b) : "xmm13", "memory");
    dump("movq x->m", buf + off, 16);
    __asm__ volatile("movdqu %1, %%xmm13\n movd %%xmm13, %0" : "=m"(*(uint32_t *) (buf + off + 1)) : "m"(b) : "xmm13", "memory");
    dump("movd x->m", buf + off, 12);
    // pmovmskb into a low and a high register
    __asm__ volatile("movdqu %1, %%xmm14\n pmovmskb %%xmm14, %%r12d\n mov %%r12d, %0" : "=m"(mask) : "m"(a) : "r12", "xmm14");
    printf("pmovmskb    %08x\n", mask);
    __asm__ volatile("movdqu %1, %%xmm1\n pmovmskb %%xmm1, %%eax\n mov %%eax, %0" : "=m"(mask) : "m"(b) : "rax", "xmm1");
    printf("pmovmskb2   %08x\n", mask);
    // pshufd/pshufhw/pshuflw with imm, register and memory
    __asm__ volatile("movdqu %1, %%xmm6\n pshufd $0x1b, %%xmm6, %%xmm10\n movdqu %%xmm10, %0" : "=m"(r) : "m"(a) : "xmm6", "xmm10");
    dump("pshufd rr", &r, 16);
    __asm__ volatile("pshufd $0x4e, %1, %%xmm10\n movdqu %%xmm10, %0" : "=m"(r) : "m"(*(v16 *) (buf + off)) : "xmm10");
    dump("pshufd rm", &r, 16);
    __asm__ volatile("movdqu %1, %%xmm6\n pshufhw $0x93, %%xmm6, %%xmm6\n pshuflw $0x39, %%xmm6, %%xmm6\n movdqu %%xmm6, %0" : "=m"(r) : "m"(a) : "xmm6");
    dump("pshufhw/lw", &r, 16);
    // the same register as both operands; zeroing idiom; a store between two xmm
    __asm__ volatile("movdqu %1, %%xmm8\n pxor %%xmm8, %%xmm8\n paddq %%xmm8, %%xmm8\n movdqu %%xmm8, %0" : "=m"(r) : "m"(a) : "xmm8");
    dump("pxor self", &r, 16);
    __asm__ volatile("movdqu %1, %%xmm15\n movdqu %2, %%xmm4\n movss %%xmm15, %%xmm4\n movups %%xmm4, %%xmm15\n movdqu %%xmm15, %0" : "=m"(r) : "m"(a), "m"(b) : "xmm15", "xmm4");
    dump("movss/ups", &r, 16);
    // flags must survive: compare, SSE ops, then use the flags
    int lt = 0;
    __asm__ volatile("cmp %2, %1\n pxor %%xmm0, %%xmm0\n movups %%xmm0, (%3)\n setb %b0" : "=r"(lt) : "r"(g64), "r"(out64), "r"(buf + off) : "xmm0", "memory", "cc");
    printf("flags kept  %d\n", lt);
}

int main(void) {
    for (int round = 0; round < 3; round++) {
#define X(n) t_rr_##n();
        X(movups) X(movaps) X(movss) X(movsd) X(movdqa) X(movdqu) X(movhlps) X(movlhps) X(movq) X(pxor) X(por)
        X(pand) X(pandn) X(xorps) X(andps) X(andnps) X(orps) X(xorpd) X(unpcklps) X(unpckhpd) X(punpcklbw)
        X(punpcklwd) X(punpckldq) X(punpcklqdq) X(punpckhbw) X(punpckhqdq) X(packsswb) X(packuswb) X(pcmpeqb)
        X(pcmpeqw) X(pcmpeqd) X(pcmpgtb) X(pcmpgtd) X(paddb) X(paddw) X(paddd) X(paddq) X(psubb) X(psubq)
        X(pminub) X(pmaxub) X(pmullw) X(pmuludq) X(psadbw) X(pavgb) X(paddusb) X(psubusw) X(pmulhw) X(pmaddwd)
#undef X
        for (int off = 1; off < 120; off += 37) {
#define X(n) t_rm_##n(off);
            X(movups) X(movss) X(movsd) X(movdqu) X(movlps) X(movhps) X(movlpd) X(movhpd) X(movq) X(pxor) X(por)
            X(xorps) X(punpcklqdq) X(punpcklbw) X(pcmpeqb) X(paddq) X(pminub) X(unpcklps)
#undef X
#define X(n) t_mr_##n(off);
            X(movups) X(movss) X(movsd) X(movdqu) X(movlps) X(movhps) X(movq)
#undef X
            t_misc(off);
        }
    }
    // page-crossing accesses: the gadgets take over
    static uint8_t two[8192] __attribute__((aligned(4096)));
    fill(two, sizeof(two));
    for (int off = 4096 - 15; off < 4096; off += 7) {
        v16 r, a; fill(&a, 16);
        __asm__ volatile("movdqu %1, %%xmm9\n movdqu %%xmm9, %0" : "=m"(r) : "m"(*(v16 *) (two + off)) : "xmm9");
        dump("cross ld", &r, 16);
        __asm__ volatile("movdqu %1, %%xmm9\n pxor %2, %%xmm9\n movups %%xmm9, %0" : "=m"(*(v16 *) (two + off + 3)) : "m"(a), "m"(*(v16 *) (two + off - 16)) : "xmm9");
        dump("cross st", two + off - 4, 28);
    }
    return 0;
}
