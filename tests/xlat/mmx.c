// MMX forms of the integer SIMD instructions, register and memory operands,
// on pseudo-random values with the edge cases for saturation mixed in. The
// output is compared with the same program run on real hardware.
#include <stdint.h>
#include <stdio.h>

static uint64_t seed = 0x9e3779b97f4a7c15;
static uint64_t next(void) {
    seed ^= seed << 13;
    seed ^= seed >> 7;
    seed ^= seed << 17;
    return seed;
}
static uint64_t value(int i) {
    static const uint64_t edges[] = {
        0, ~0ull, 0x8000800080008000, 0x7fff7fff7fff7fff, 0x8080808080808080,
        0x7f7f7f7f7f7f7f7f, 0x80000000ffffffff, 0x00ff00ff00ff00ff,
    };
    if (i < 8)
        return edges[i];
    return next();
}

#define OP(name) \
    static uint64_t name##_rr(uint64_t a, uint64_t b) { \
        __asm__("movq %0, %%mm0\n movq %1, %%mm1\n " #name " %%mm1, %%mm0\n movq %%mm0, %0" \
                : "+r" (a) : "r" (b) : "mm0", "mm1"); \
        return a; \
    } \
    static uint64_t name##_rm(uint64_t a, uint64_t b) { \
        __asm__("movq %0, %%mm2\n " #name " %1, %%mm2\n movq %%mm2, %0" \
                : "+r" (a) : "m" (b) : "mm2"); \
        return a; \
    }
#define OPS(_) \
    _(punpcklbw) _(punpcklwd) _(punpckldq) _(punpckhbw) _(punpckhwd) _(punpckhdq) \
    _(packsswb) _(packssdw) _(packuswb) \
    _(paddusb) _(paddusw) _(psubusb) _(psubusw) _(paddsb) _(paddsw) _(psubsb) _(psubsw) \
    _(pminub) _(pmaxub) _(pminsw) _(pmaxsw) _(pavgb) _(pavgw) \
    _(pandn) _(pmulhuw) _(pmaddwd) _(psadbw) \
    _(pshufb) _(phaddw) _(phaddd) _(phaddsw) _(phsubw) _(phsubd) _(phsubsw) \
    _(pmaddubsw) _(psignb) _(psignw) _(psignd) _(pmulhrsw) _(pabsb) _(pabsw) _(pabsd) \
    _(paddb) _(pmullw) _(pcmpgtw) _(pxor)
OPS(OP)

#define PALIGNR(n) \
    static uint64_t palignr##n(uint64_t a, uint64_t b) { \
        __asm__("movq %0, %%mm0\n movq %1, %%mm1\n palignr $" #n ", %%mm1, %%mm0\n movq %%mm0, %0" \
                : "+r" (a) : "r" (b) : "mm0", "mm1"); \
        return a; \
    }
PALIGNR(0) PALIGNR(3) PALIGNR(8) PALIGNR(13) PALIGNR(16) PALIGNR(20)

static uint32_t pextrw(uint64_t a, int which) {
    uint64_t r = 0xdeadbeefdeadbeef;
    switch (which) {
#define X(n) case n: __asm__("movq %1, %%mm0\n pextrw $" #n ", %%mm0, %k0" : "+r" (r) : "r" (a) : "mm0"); break;
        X(0) X(1) X(2) X(3) X(5)
#undef X
    }
    return (uint32_t) (r >> 32) ^ (uint32_t) r; // top half must be zeroed
}

// between MMX and XMM registers: the low half, the rest of an xmm zeroed
static uint64_t movq2dq_hi, movdq2q;
static void moves(uint64_t a, uint64_t b) {
    uint64_t lo, hi;
    __asm__("movq %2, %%xmm0\n pinsrq $1, %3, %%xmm0\n movq %3, %%mm4\n movq2dq %%mm4, %%xmm0\n"
            " movq %%xmm0, %0\n pextrq $1, %%xmm0, %1"
            : "=r" (lo), "=r" (hi) : "r" (a), "r" (b) : "xmm0", "mm4");
    movq2dq_hi = hi ^ lo;
    __asm__("movq %1, %%xmm1\n movdq2q %%xmm1, %%mm5\n movq %%mm5, %0" : "=r" (movdq2q) : "r" (a) : "xmm1", "mm5");
}

int main(void) {
    for (int i = 0; i < 40; i++) {
        uint64_t a = value(i), b = value((i * 7 + 3) % 40);
        if (i >= 8)
            b = next();
#define PRINT(name) printf("%-10s %016llx %016llx  %016llx %016llx\n", #name, \
        (unsigned long long) a, (unsigned long long) b, \
        (unsigned long long) name##_rr(a, b), (unsigned long long) name##_rm(a, b));
        OPS(PRINT)
        printf("palignr    %016llx %016llx  %016llx %016llx %016llx %016llx %016llx %016llx\n",
               (unsigned long long) a, (unsigned long long) b,
               (unsigned long long) palignr0(a, b), (unsigned long long) palignr3(a, b),
               (unsigned long long) palignr8(a, b), (unsigned long long) palignr13(a, b),
               (unsigned long long) palignr16(a, b), (unsigned long long) palignr20(a, b));
        printf("pextrw     %016llx  %08x %08x %08x %08x %08x\n", (unsigned long long) a,
               pextrw(a, 0), pextrw(a, 1), pextrw(a, 2), pextrw(a, 3), pextrw(a, 5));
        moves(a, b);
        printf("movq2dq    %016llx %016llx  %016llx %016llx\n", (unsigned long long) a, (unsigned long long) b,
               (unsigned long long) movq2dq_hi, (unsigned long long) movdq2q);
    }
    __asm__("emms");
    return 0;
}
