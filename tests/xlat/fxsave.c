// fxsave / fxrstor: XMM registers and MXCSR saved, clobbered, restored, with the 512-byte area at
// every 16-byte position across a page boundary. Prints what came back; native is the reference.
// gcc -O1 fxsave.c
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

static uint8_t pages[3 * 4096] __attribute__((aligned(4096)));

static int fx_round(uint8_t *area, unsigned seed) {
    uint8_t in[16 * 16], out[16 * 16];
    for (unsigned i = 0; i < sizeof(in); i++)
        in[i] = (uint8_t) (seed * 131 + i * 7);
    uint32_t mx_in = 0x1f80 | ((seed & 3) << 13), mx_out;
    __asm__ volatile(
        "ldmxcsr %[mx]\n"
        "movdqu 0(%[in]), %%xmm0\n movdqu 16(%[in]), %%xmm1\n movdqu 32(%[in]), %%xmm2\n movdqu 48(%[in]), %%xmm3\n"
        "movdqu 64(%[in]), %%xmm4\n movdqu 80(%[in]), %%xmm5\n movdqu 96(%[in]), %%xmm6\n movdqu 112(%[in]), %%xmm7\n"
        "movdqu 128(%[in]), %%xmm8\n movdqu 144(%[in]), %%xmm9\n movdqu 160(%[in]), %%xmm10\n movdqu 176(%[in]), %%xmm11\n"
        "movdqu 192(%[in]), %%xmm12\n movdqu 208(%[in]), %%xmm13\n movdqu 224(%[in]), %%xmm14\n movdqu 240(%[in]), %%xmm15\n"
        "fxsave64 (%[area])\n"
        // what code in between does to them
        "pxor %%xmm0, %%xmm0\n pcmpeqb %%xmm1, %%xmm1\n xorps %%xmm8, %%xmm8\n movaps %%xmm1, %%xmm15\n"
        "mov $0x1f80, %%eax\n push %%rax\n ldmxcsr (%%rsp)\n pop %%rax\n"
        "fxrstor64 (%[area])\n"
        "movdqu %%xmm0, 0(%[out])\n movdqu %%xmm1, 16(%[out])\n movdqu %%xmm2, 32(%[out])\n movdqu %%xmm3, 48(%[out])\n"
        "movdqu %%xmm4, 64(%[out])\n movdqu %%xmm5, 80(%[out])\n movdqu %%xmm6, 96(%[out])\n movdqu %%xmm7, 112(%[out])\n"
        "movdqu %%xmm8, 128(%[out])\n movdqu %%xmm9, 144(%[out])\n movdqu %%xmm10, 160(%[out])\n movdqu %%xmm11, 176(%[out])\n"
        "movdqu %%xmm12, 192(%[out])\n movdqu %%xmm13, 208(%[out])\n movdqu %%xmm14, 224(%[out])\n movdqu %%xmm15, 240(%[out])\n"
        "stmxcsr %[mxo]\n"
        : [mxo] "=m"(mx_out) : [mx] "m"(mx_in), [in] "r"(in), [out] "r"(out), [area] "r"(area)
        : "rax", "memory", "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7", "xmm8", "xmm9", "xmm10",
          "xmm11", "xmm12", "xmm13", "xmm14", "xmm15");
    uint32_t mx_area;
    memcpy(&mx_area, area + 24, 4);
    return memcmp(in, out, sizeof(in)) == 0 && mx_out == mx_in && mx_area == mx_in &&
           memcmp(area + 160, in, sizeof(in)) == 0;
}

int main(void) {
    int bad = 0;
    unsigned n = 0;
    for (unsigned rep = 0; rep < 8; rep++)
        for (unsigned off = 4096 - 512 - 64; off <= 4096 + 64; off += 16, n++)
            if (!fx_round(pages + off, rep * 1000 + off))
                bad++, printf("wrong at offset %u\n", off);
    printf("fxsave/fxrstor: %u rounds, %d wrong\n", n, bad);
    return bad != 0;
}
