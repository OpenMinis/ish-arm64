// Floating point compares: comiss/ucomiss/comisd/ucomisd (the flags, read
// back with pushfq and through setcc/jcc) and cmpss/cmpsd/cmpps/cmppd with
// each predicate, register and memory operands, xmm8-15, on NaNs, infinities,
// signed zeros, denormals and ordinary values. Builds natively too (clang on
// Windows, gcc on Linux): compare the output with the hardware's.
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define FMASK 0x8d5   // CF PF AF ZF SF OF

static const uint64_t d_vals[] = {
    0x0000000000000000, 0x8000000000000000, 0x3ff0000000000000, 0xbff0000000000000,
    0x7ff0000000000000, 0xfff0000000000000, 0x7ff8000000000000, 0x7ff4000000000000,
    0x0000000000000001, 0x3ff0000000000001, 0x4000000000000000, 0xc059000000000000,
};
static const uint32_t s_vals[] = {
    0x00000000, 0x80000000, 0x3f800000, 0xbf800000, 0x7f800000, 0xff800000,
    0x7fc00000, 0x7fa00000, 0x00000001, 0x3f800001, 0x40000000, 0xc2c80000,
};
#define N 12

static double d(uint64_t b) { double x; memcpy(&x, &b, 8); return x; }
static float s(uint32_t b) { float x; memcpy(&x, &b, 4); return x; }

#define FL(insn, a, b, cons) ({ uint64_t f; \
    __asm__ volatile("xor %%eax, %%eax\n " insn " %2, %1\n pushfq\n pop %0" : "=r"(f) : "x"(a), cons(b) : "rax", "cc"); \
    f & FMASK; })

// the same with the operands in xmm8 and xmm13
#define FL_HI(insn, mov, a, b) ({ uint64_t f; \
    __asm__ volatile(mov " %1, %%xmm8\n " mov " %2, %%xmm13\n " insn " %%xmm13, %%xmm8\n pushfq\n pop %0" \
                     : "=r"(f) : "x"(a), "x"(b) : "xmm8", "xmm13", "cc"); f & FMASK; })

// the way compilers use them: a branch or a setcc right after
static int branches(double a, double b) {
    int r = 0;
    __asm__ volatile("ucomisd %2, %1\n jp 1f\n jne 1f\n or $1, %0\n1: ucomisd %2, %1\n jbe 2f\n or $2, %0\n"
                     "2: comisd %2, %1\n jae 3f\n or $4, %0\n3:"
                     : "+r"(r) : "x"(a), "x"(b) : "cc");
    unsigned char lt, gt, e, p;
    __asm__ volatile("ucomisd %5, %4\n setb %0\n seta %1\n sete %2\n setp %3"
                     : "=r"(lt), "=r"(gt), "=r"(e), "=r"(p) : "x"(a), "x"(b) : "cc");
    return r | lt << 4 | gt << 5 | e << 6 | p << 7;
}

#define CMP(insn, imm, a, b, cons) ({ __typeof__(a) r = a; \
    __asm__ volatile(insn " $" #imm ", %1, %0" : "+x"(r) : cons(b)); r; })
#define CMPS_ALL(insn, a, b, cons, out, T) do { T r_; \
    r_ = CMP(insn, 0, a, b, cons); memcpy(&out[0], &r_, sizeof(T)); \
    r_ = CMP(insn, 1, a, b, cons); memcpy(&out[1], &r_, sizeof(T)); \
    r_ = CMP(insn, 2, a, b, cons); memcpy(&out[2], &r_, sizeof(T)); \
    r_ = CMP(insn, 3, a, b, cons); memcpy(&out[3], &r_, sizeof(T)); \
    r_ = CMP(insn, 4, a, b, cons); memcpy(&out[4], &r_, sizeof(T)); \
    r_ = CMP(insn, 5, a, b, cons); memcpy(&out[5], &r_, sizeof(T)); \
    r_ = CMP(insn, 6, a, b, cons); memcpy(&out[6], &r_, sizeof(T)); \
    r_ = CMP(insn, 7, a, b, cons); memcpy(&out[7], &r_, sizeof(T)); } while (0)

typedef double v2d __attribute__((vector_size(16)));
typedef float v4f __attribute__((vector_size(16)));

int main(void) {
    for (int i = 0; i < N; i++) {
        for (int j = 0; j < N; j++) {
            double a = d(d_vals[i]), b = d(d_vals[j]);
            float x = s(s_vals[i]), y = s(s_vals[j]);
            printf("%2d %2d sd %03llx %03llx %03llx %03llx hi %03llx %03llx  ss %03llx %03llx %03llx %03llx hi %03llx %03llx  br %02x\n",
                   i, j,
                   (unsigned long long) FL("ucomisd", a, b, "x"), (unsigned long long) FL("comisd", a, b, "x"),
                   (unsigned long long) FL("ucomisd", a, b, "m"), (unsigned long long) FL("comisd", a, b, "m"),
                   (unsigned long long) FL_HI("ucomisd", "movsd", a, b), (unsigned long long) FL_HI("comisd", "movsd", a, b),
                   (unsigned long long) FL("ucomiss", x, y, "x"), (unsigned long long) FL("comiss", x, y, "x"),
                   (unsigned long long) FL("ucomiss", x, y, "m"), (unsigned long long) FL("comiss", x, y, "m"),
                   (unsigned long long) FL_HI("ucomiss", "movss", x, y), (unsigned long long) FL_HI("comiss", "movss", x, y),
                   branches(a, b));
            uint64_t sd[8], sdm[8];
            uint32_t ss[8], ssm[8];
            CMPS_ALL("cmpsd", a, b, "x", sd, double);
            CMPS_ALL("cmpsd", a, b, "m", sdm, double);
            CMPS_ALL("cmpss", x, y, "x", ss, float);
            CMPS_ALL("cmpss", x, y, "m", ssm, float);
            printf("      cmpsd");
            for (int k = 0; k < 8; k++) printf(" %d%d", sd[k] != 0, sdm[k] != 0);
            printf("  cmpss");
            for (int k = 0; k < 8; k++) printf(" %d%d", ss[k] != 0, ssm[k] != 0);
            printf("\n");
        }
        // packed: lane k compares a[i] with b[(i + k) % N]
        v2d pa = {d(d_vals[i]), d(d_vals[(i + 5) % N])};
        v2d pb = {d(d_vals[(i + 1) % N]), d(d_vals[(i + 5) % N])};
        v4f qa = {s(s_vals[i]), s(s_vals[(i + 3) % N]), s(s_vals[(i + 6) % N]), s(s_vals[i])};
        v4f qb = {s(s_vals[(i + 2) % N]), s(s_vals[(i + 3) % N]), s(s_vals[(i + 7) % N]), s(s_vals[(i + 9) % N])};
        uint64_t pd[8][2], pdm[8][2];
        uint32_t ps[8][4], psm[8][4];
        CMPS_ALL("cmppd", pa, pb, "x", pd, v2d);
        CMPS_ALL("cmppd", pa, pb, "m", pdm, v2d);
        CMPS_ALL("cmpps", qa, qb, "x", ps, v4f);
        CMPS_ALL("cmpps", qa, qb, "m", psm, v4f);
        printf("%2d    cmppd", i);
        for (int k = 0; k < 8; k++) printf(" %d%d%d%d", pd[k][0] != 0, pd[k][1] != 0, pdm[k][0] != 0, pdm[k][1] != 0);
        printf("  cmpps");
        for (int k = 0; k < 8; k++)
            printf(" %d%d%d%d", ps[k][0] != 0, ps[k][1] != 0, ps[k][2] != 0, ps[k][3] != 0);
        for (int k = 0; k < 8; k++)
            printf("%s%d%d%d%d", k ? " " : " m ", psm[k][0] != 0, psm[k][1] != 0, psm[k][2] != 0, psm[k][3] != 0);
        printf("\n");
    }
    return 0;
}
