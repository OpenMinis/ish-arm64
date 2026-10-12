// NaN through ucomiss/comiss/ucomisd (flags, setcc, jcc) and the float/double conversions (cvtss2sd, cvtsd2ss), as an x86_64 guest program; compare with the gadget build.
#include <stdio.h>
#include <stdint.h>
#include <string.h>
static volatile uint32_t nan32 = 0xffffffff, nan32b = 0xfffffffe, one32 = 0x3f800000;
static volatile uint64_t nan64 = ~0ull, one64 = 0x3ff0000000000000ull;
static float f32(volatile uint32_t *p) { float f; uint32_t v = *p; memcpy(&f, &v, 4); return f; }
static double f64(volatile uint64_t *p) { double f; uint64_t v = *p; memcpy(&f, &v, 8); return f; }
#define FLAGS(insn, a, b) ({ uint64_t fl; __asm__ volatile(insn "\n pushfq\n pop %0" : "=r"(fl) : "x"(a), "x"(b) : "cc"); fl & 0x8d5; })
#define FLAGSM(insn, a, m) ({ uint64_t fl; __asm__ volatile(insn "\n pushfq\n pop %0" : "=r"(fl) : "x"(a), "m"(m) : "cc"); fl & 0x8d5; })
int main(void) {
    float n = f32(&nan32), n2 = f32(&nan32b), o = f32(&one32);
    double d = f64(&nan64), od = f64(&one64);
    printf("c: n==n %d n!=n %d n==n2 %d d==d %d\n", n == n, n != n, n == n2, d == d);
    printf("ucomiss nn %03llx no %03llx on %03llx oo %03llx\n", FLAGS("ucomiss %2, %1", n, n), FLAGS("ucomiss %2, %1", n, o), FLAGS("ucomiss %2, %1", o, n), FLAGS("ucomiss %2, %1", o, o));
    printf("comiss  nn %03lx oo %03lx\n", FLAGS("comiss %2, %1", n, n), FLAGS("comiss %2, %1", o, o));
    printf("ucomisd dd %03lx dD %03lx DD %03lx\n", FLAGS("ucomisd %2, %1", d, d), FLAGS("ucomisd %2, %1", d, od), FLAGS("ucomisd %2, %1", od, od));
    printf("ucomiss m  %03lx %03lx\n", FLAGSM("ucomiss %2, %1", n, nan32), FLAGSM("ucomiss %2, %1", o, one32));
    printf("ucomisd m  %03lx %03lx\n", FLAGSM("ucomisd %2, %1", d, nan64), FLAGSM("ucomisd %2, %1", od, one64));
    int p, np, e, ne;
    __asm__ volatile("ucomiss %4, %4\n setp %b0\n setnp %b1\n sete %b2\n setne %b3" : "=r"(p), "=r"(np), "=r"(e), "=r"(ne) : "x"(n) : "cc");
    printf("set p %d np %d e %d ne %d\n", p & 1, np & 1, e & 1, ne & 1);
    int j = 0;
    __asm__ volatile("ucomiss %1, %1\n jne 1f\n jp 1f\n mov $1, %0\n jmp 2f\n1: mov $2, %0\n2:" : "+r"(j) : "x"(n) : "cc");
    printf("jne/jp %d\n", j);
    uint64_t fl[6];
    __asm__ volatile("movss %6, %%xmm9\n ucomiss %%xmm9, %%xmm9\n pushfq\n pop %0\n"
                     "movss %6, %%xmm1\n ucomiss %%xmm1, %%xmm9\n pushfq\n pop %1\n"
                     "movss %6, %%xmm12\n ucomiss %%xmm1, %%xmm12\n pushfq\n pop %2\n"
                     "movss %7, %%xmm13\n ucomiss %%xmm13, %%xmm13\n pushfq\n pop %3\n"
                     "movsd %8, %%xmm10\n ucomisd %%xmm10, %%xmm10\n pushfq\n pop %4\n"
                     "ucomiss %6, %%xmm9\n pushfq\n pop %5\n"
                     : "=r"(fl[0]), "=r"(fl[1]), "=r"(fl[2]), "=r"(fl[3]), "=r"(fl[4]), "=r"(fl[5])
                     : "m"(nan32), "m"(one32), "m"(nan64) : "xmm1", "xmm9", "xmm10", "xmm12", "xmm13", "cc");
    for (int k = 0; k < 6; k++) printf("hi%d %03llx ", k, (unsigned long long) (fl[k] & 0x8d5));
    printf("\n");
    {
        volatile float vn = n; volatile double vd = d;
        double w = (double) vn; float nn = (float) vd;
        uint64_t wb; uint32_t nb; memcpy(&wb, &w, 8); memcpy(&nb, &nn, 4);
        printf("cvtss2sd %016llx eq %d | cvtsd2ss %08x eq %d\n", (unsigned long long) wb, w == w, nb, nn == nn);
        uint32_t snan = 0x7fa00000; float sf; memcpy(&sf, &snan, 4); volatile float vs = sf;
        double ws = (double) vs; memcpy(&wb, &ws, 8);
        printf("snan->d %016llx\n", (unsigned long long) wb);
        volatile int64_t big = -1; volatile double bd = (double) big; float bf = (float) bd;
        memcpy(&nb, &bf, 4); printf("i2d2f %08x\n", nb);
    }
    return 0;
}
