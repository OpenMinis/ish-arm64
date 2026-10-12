#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "emu/arch/x86/vec.h"
#include "emu/cpu.h"

union vec {
    uint8_t u8[16];
    uint16_t u16[8];
    uint32_t u32[4];
    uint64_t u64[2];
    __uint128_t u128[1];
    __uint128_t dqw;
};

static inline void zero_xmm(union xmm_reg *xmm) {
    xmm->qw[0] = 0;
    xmm->qw[1] = 0;
}

static inline int32_t satsw(int32_t dw) {
    if (dw > 0xff80)
        dw &= 0xff;
    else if (dw > 0x7fff)
        dw = 0x80;
    else if (dw > 0x7f)
        dw = 0x7f;
    return dw;
}
static inline uint32_t satud(uint32_t dw) {
    if (dw > 0xffff8000)
        dw &= 0xffff;
    else if (dw > 0x7fffffff)
        dw = 0x8000;
    else if (dw > 0x7fff)
        dw = 0x7fff;
    return dw;
}
static inline uint32_t satub(uint32_t dw) {
    if (dw >= 0x8000)
        dw = 0;
    else if (dw > 0xff)
        dw = 0xff;
    return dw;
}
static inline uint32_t satsb(uint32_t dw) {
    if (dw > 0xffffff80)
        dw &= 0xff;
    else if (dw > 0x7fffffff)
        dw = 0x80;
    else if (dw > 0x7f)
        dw = 0x7f;
    return dw;
}

#define VEC_ZERO_COPY(zero, copy) \
    void vec_zero##zero##_copy##copy(NO_CPU, const void *src, void *dst) { \
        memcpy(dst, src, copy/8); \
        memset((char *) dst + copy/8, 0, (zero-copy)/8); \
    }
VEC_ZERO_COPY(128, 128)
VEC_ZERO_COPY(128, 64)
VEC_ZERO_COPY(128, 32)
VEC_ZERO_COPY(64, 64)
VEC_ZERO_COPY(64, 32)
VEC_ZERO_COPY(32, 32)

void vec_merge32(NO_CPU, const void *src, void *dst) {
    memcpy(dst, src, 4);
}
void vec_merge64(NO_CPU, const void *src, void *dst) {
    memcpy(dst, src, 8);
}
void vec_merge128(NO_CPU, const void *src, void *dst) {
    memcpy(dst, src, 16);
}

#define _SHIFT(op, size) \
    do { \
        if (unlikely(amount > (size)-1)) { \
            zero_xmm(dst); \
        } else { \
            union vec d = { .dqw = dst->u128 }; \
            for (unsigned i = 0; i < array_size(d.u##size); i++) \
                d.u##size[i] op##= amount; \
            dst->u128 = d.dqw; \
        } \
    } while (0)

#define VEC_SSE_SHIFT(dir, suffix, op, size) \
    void vec_shift##dir##_##suffix##128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst) { \
        const uint8_t amount = src->u8[0]; \
        _SHIFT(op, size); \
    } \
    void vec_imm_shift##dir##_##suffix##128(NO_CPU, const uint8_t amount, union xmm_reg *dst) { \
        _SHIFT(op, size); \
    }

#define _VEC_SSE_CMP(sgn, usgn, suffix, relop, size) \
    void vec_compare##sgn##_##suffix##128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst) { \
        union vec s = { .dqw = src->u128 }, d = { .dqw = dst->u128 }; \
        for (unsigned i = 0; i < array_size(s.u##size); i++) \
            d.u##size[i] = (usgn##int##size##_t)d.u##size[i] relop (usgn##int##size##_t)s.u##size[i] ? ~0 : 0;\
        dst->u128 = d.dqw; \
    }

#define VEC_SSE_CMPD(suffix, relop, size) \
    _VEC_SSE_CMP(, u, suffix, relop, size)
#define VEC_SSE_CMPS(suffix, relop, size) \
    _VEC_SSE_CMP(s,, suffix, relop, size)

#define VEC_SSE_OP(name, suffix, op, size) \
    void vec_##name##_##suffix##128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst) { \
        union vec s = { .dqw = src->u128 }, d = { .dqw = dst->u128 }; \
        for (unsigned i = 0; i < array_size(s.u##size); i++) \
            d.u##size[i] op##= s.u##size[i]; \
        dst->u128 = d.dqw; \
    }

VEC_SSE_SHIFT(r, w, >>, 16)
VEC_SSE_SHIFT(r, d, >>, 32)
VEC_SSE_SHIFT(r, q, >>, 64)

VEC_SSE_SHIFT(l, w, <<, 16)
VEC_SSE_SHIFT(l, d, <<, 32)
VEC_SSE_SHIFT(l, q, <<, 64)

VEC_SSE_CMPD(eqb, ==,  8)
VEC_SSE_CMPD(eqw, ==, 16)
VEC_SSE_CMPD(eqd, ==, 32)

VEC_SSE_CMPS(gtb, >,  8)
VEC_SSE_CMPS(gtw, >, 16)
VEC_SSE_CMPS(gtd, >, 32)

VEC_SSE_OP(add, b, +, 8)
VEC_SSE_OP(add, w, +, 16)
VEC_SSE_OP(add, d, +, 32)
VEC_SSE_OP(add, q, +, 64)

VEC_SSE_OP(sub, b, -, 8)
VEC_SSE_OP(sub, w, -, 16)
VEC_SSE_OP(sub, d, -, 32)
VEC_SSE_OP(sub, q, -, 64)

VEC_SSE_OP(and, dq, &, 128)
VEC_SSE_OP(or,  dq, |, 128)
VEC_SSE_OP(xor, dq, ^, 128)

void vec_imm_shiftl_dq128(NO_CPU, uint8_t amount, union xmm_reg *dst) {
    if (amount >= 16)
        zero_xmm(dst);
    else
        dst->u128 <<= amount * 8;
}
void vec_imm_shiftr_dq128(NO_CPU, uint8_t amount, union xmm_reg *dst) {
    if (amount >= 16)
        zero_xmm(dst);
    else
        dst->u128 >>= amount * 8;
}
void vec_shiftrs_w128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst) {
    const uint8_t amount = src->u8[0];
    for (unsigned i = 0; i < 8; i++) {
        if (unlikely(amount > 15))
            dst->u16[i] = ((dst->u16[i] >> 15) & (uint16_t)1) ? 0xffff : 0;
        else
            dst->u16[i] = ((int16_t)(dst->u16[i])) >> amount;
    }
}
void vec_shiftrs_d128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst) {
    const uint8_t amount = src->u8[0];
    for (unsigned i = 0; i < 4; i++) {
        if (unlikely(amount > 31))
            dst->u32[i] = ((dst->u32[i] >> 31) & (uint32_t)1) ? 0xffffffff : 0;
        else
            dst->u32[i] = ((int32_t)(dst->u32[i])) >> amount;
    }
}
void vec_imm_shiftrs_w128(NO_CPU, const uint8_t amount, union xmm_reg *dst) {
    for (unsigned i = 0; i < 8; i++) {
        if (unlikely(amount > 15))
            dst->u16[i] = ((dst->u16[i] >> 15) & (uint16_t)1) ? 0xffff : 0;
        else
            dst->u16[i] = ((int16_t)(dst->u16[i])) >> amount;
    }
}
void vec_imm_shiftrs_d128(NO_CPU, const uint8_t amount, union xmm_reg *dst) {
    for (unsigned i = 0; i < 4; i++) {
        if (unlikely(amount > 31))
            dst->u32[i] = ((dst->u32[i] >> 31) & (uint32_t)1) ? 0xffffffff : 0;
        else
            dst->u32[i] = ((int32_t)(dst->u32[i])) >> amount;
    }
}

void vec_addus_b128(NO_CPU, union xmm_reg *src, union xmm_reg *dst) {
    for (unsigned i = 0; i < 16; i++) {
        const int32_t sb = dst->u8[i] + src->u8[i];
        dst->u8[i] = sb > 0xff ? 0xff : sb;
    }
}
void vec_addus_w128(NO_CPU, union xmm_reg *src, union xmm_reg *dst) {
    for (unsigned i = 0; i < 8; i++) {
        const int32_t sw = dst->u16[i] + src->u16[i];
        dst->u16[i] = sw > 0xffff ? 0xffff : sw;
    }
}
void vec_addss_b128(NO_CPU, union xmm_reg *src, union xmm_reg *dst) {
    for (unsigned i = 0; i < 16; i++)
        dst->u8[i] = satsb((int8_t)dst->u8[i] + (int8_t)src->u8[i]);
}
void vec_addss_w128(NO_CPU, union xmm_reg *src, union xmm_reg *dst) {
    for (unsigned i = 0; i < 8; i++)
        dst->u16[i] = satud((int16_t)dst->u16[i] + (int16_t)src->u16[i]);
}

void vec_subus_b128(NO_CPU, union xmm_reg *src, union xmm_reg *dst) {
    for (unsigned i = 0; i < 16; i++) {
        const int32_t sb = dst->u8[i] - src->u8[i];
        dst->u8[i] = sb < 0 ? 0 : sb;
    }
}
void vec_subus_w128(NO_CPU, union xmm_reg *src, union xmm_reg *dst) {
    for (unsigned i = 0; i < 8; i++) {
        const int32_t sw = dst->u16[i] - src->u16[i];
        dst->u16[i] = sw < 0 ? 0 : sw;
    }
}
void vec_subss_b128(NO_CPU, union xmm_reg *src, union xmm_reg *dst) {
    for (unsigned i = 0; i < 16; i++)
        dst->u8[i] = satsb((int8_t)dst->u8[i] - (int8_t)src->u8[i]);
}
void vec_subss_w128(NO_CPU, union xmm_reg *src, union xmm_reg *dst) {
    for (unsigned i = 0; i < 8; i++)
        dst->u16[i] = satud((int16_t)dst->u16[i] - (int16_t)src->u16[i]);
}

void vec_madd_d128(NO_CPU, union xmm_reg *src, union xmm_reg *dst) {
    dst->u32[0] = (int32_t)((int16_t)dst->u16[0] * (int16_t)src->u16[0]) +
                  (int32_t)((int16_t)dst->u16[1] * (int16_t)src->u16[1]);
    dst->u32[1] = (int32_t)((int16_t)dst->u16[2] * (int16_t)src->u16[2]) +
                  (int32_t)((int16_t)dst->u16[3] * (int16_t)src->u16[3]);
    dst->u32[2] = (int32_t)((int16_t)dst->u16[4] * (int16_t)src->u16[4]) +
                  (int32_t)((int16_t)dst->u16[5] * (int16_t)src->u16[5]);
    dst->u32[3] = (int32_t)((int16_t)dst->u16[6] * (int16_t)src->u16[6]) +
                  (int32_t)((int16_t)dst->u16[7] * (int16_t)src->u16[7]);
}

void vec_sumabs_w128(NO_CPU, union xmm_reg *src, union xmm_reg *dst) {
    uint32_t sum[2] = { 0, 0 };
    for (unsigned i = 0; i < 8; i++) {
        int32_t difflo = dst->u8[i + 0] - src->u8[i + 0];
        int32_t diffhi = dst->u8[i + 8] - src->u8[i + 8];
        sum[0] += (difflo < 0) ? -(uint32_t)difflo : difflo;
        sum[1] += (diffhi < 0) ? -(uint32_t)diffhi : diffhi;
    }
    dst->u32[0] = sum[0];
    dst->u32[2] = sum[1];
    dst->u32[1] = dst->u32[3] = 0;
}

void vec_mulu_dq128(NO_CPU, union xmm_reg *src, union xmm_reg *dst) {
    dst->qw[0] = (uint64_t) src->u32[0] * dst->u32[0];
    dst->qw[1] = (uint64_t) src->u32[2] * dst->u32[2];
}

void vec_andn128(NO_CPU, union xmm_reg *src, union xmm_reg *dst) {
    dst->qw[0] = ~dst->qw[0] & src->qw[0];
    dst->qw[1] = ~dst->qw[1] & src->qw[1];
}

void vec_min_ub128(NO_CPU, union xmm_reg *src, union xmm_reg *dst) {
    for (unsigned i = 0; i < array_size(src->u8); i++)
        if (src->u8[i] < dst->u8[i])
            dst->u8[i] = src->u8[i];
}
void vec_max_ub128(NO_CPU, union xmm_reg *src, union xmm_reg *dst) {
    for (unsigned i = 0; i < array_size(src->u8); i++)
        if (src->u8[i] > dst->u8[i])
            dst->u8[i] = src->u8[i];
}
void vec_mins_w128(NO_CPU, union xmm_reg *src, union xmm_reg *dst) {
    for (unsigned i = 0; i < 8; i++)
        dst->u16[i] = (int16_t)dst->u16[i] < (int16_t)src->u16[i] ? dst->u16[i] : src->u16[i];
}

void vec_maxs_w128(NO_CPU, union xmm_reg *src, union xmm_reg *dst) {
    for (unsigned i = 0; i < 8; i++)
        dst->u16[i] = (int16_t)dst->u16[i] > (int16_t)src->u16[i] ? dst->u16[i] : src->u16[i];
}

static bool cmpd(double a, double b, int type) {
    bool res;
    switch (type % 4) {
        case 0: res = a == b; break;
        case 1: res = a < b; break;
        case 2: res = a <= b; break;
        case 3: res = isnan(a) || isnan(b); break;
    }
    if (type >= 4) res = !res;
    return res;
}
static bool cmps(float a, float b, int type) {
    bool res;
    switch (type % 4) {
        case 0: res = a == b; break;
        case 1: res = a < b; break;
        case 2: res = a <= b; break;
        case 3: res = isnan(a) || isnan(b); break;
    }
    if (type >= 4) res = !res;
    return res;
}

void vec_single_fcmp64(NO_CPU, const double *src, union xmm_reg *dst, uint8_t type) {
    dst->qw[0] = cmpd(dst->f64[0], *src, type) ? -1 : 0;
}
void vec_single_fcmp32(NO_CPU, const float *src, union xmm_reg *dst, uint8_t type) {
    dst->u32[0] = cmps(dst->f32[0], *src, type) ? -1 : 0;
}

void vec_single_fadd64(NO_CPU, const double *src, double *dst) { *dst += *src; }
void vec_single_fadd32(NO_CPU, const float *src, float *dst) { *dst += *src; }
void vec_single_fmul64(NO_CPU, const double *src, double *dst) { *dst *= *src; }
void vec_single_fmul32(NO_CPU, const float *src, float *dst) { *dst *= *src; }
void vec_single_fsub64(NO_CPU, const double *src, double *dst) { *dst -= *src; }
void vec_single_fsub32(NO_CPU, const float *src, float *dst) { *dst -= *src; }
void vec_single_fdiv64(NO_CPU, const double *src, double *dst) { *dst /= *src; }
void vec_single_fdiv32(NO_CPU, const float *src, float *dst) { *dst /= *src; }

void vec_single_fsqrt64(NO_CPU, const double *src, double *dst) { *dst = sqrt(*src); }
void vec_single_fsqrt32(NO_CPU, const float *src, float *dst) { *dst = sqrtf(*src); }

void vec_single_fmax64(NO_CPU, const double *src, double *dst) {
    if (*src > *dst || isnan(*src) || isnan(*dst)) *dst = *src;
}
void vec_single_fmin64(NO_CPU, const double *src, double *dst) {
    if (*src < *dst || isnan(*src) || isnan(*dst)) *dst = *src;
}
void vec_single_fmax32(NO_CPU, const float *src, float *dst) {
    if (*src > *dst || isnan(*src) || isnan(*dst)) *dst = *src;
}
void vec_single_fmin32(NO_CPU, const float *src, float *dst) {
    if (*src < *dst || isnan(*src) || isnan(*dst)) *dst = *src;
}

void vec_single_ucomi32(struct cpu_state *cpu, const float *src, const float *dst) {
    cpu->zf_res = cpu->pf_res = 0;
    cpu->zf = *src == *dst;
    cpu->cf = *src > *dst;
    cpu->pf = 0;
    if (isnan(*src) || isnan(*dst))
        cpu->zf = cpu->cf = cpu->pf = 1;
    cpu->of = cpu->sf = cpu->af = 0;
    cpu->sf_res = 0;
}

void vec_single_ucomi64(struct cpu_state *cpu, const double *src, const double *dst) {
    cpu->zf_res = cpu->pf_res = 0;
    cpu->zf = *src == *dst;
    cpu->cf = *src > *dst;
    cpu->pf = 0;
    if (isnan(*src) || isnan(*dst))
        cpu->zf = cpu->cf = cpu->pf = 1;
    cpu->of = cpu->sf = cpu->af = 0;
    cpu->sf_res = 0;
}

#define VEC_PACKED_OP(name, op, field, size, n) \
    void vec_##name##size(NO_CPU, union xmm_reg *src, union xmm_reg *dst) { \
        for (int i = 0; i < n; ++i) { \
            dst->field[i] op##= src->field[i]; \
        } \
    }

VEC_PACKED_OP(add_p, +, f64, 64, 2)
VEC_PACKED_OP(add_p, +, f32, 32, 4)
VEC_PACKED_OP(sub_p, -, f64, 64, 2)
VEC_PACKED_OP(sub_p, -, f32, 32, 4)
VEC_PACKED_OP(mul_p, *, f64, 64, 2)
VEC_PACKED_OP(mul_p, *, f32, 32, 4)

void vec_fcmp_p64(NO_CPU, const union xmm_reg *src, union xmm_reg *dst, uint8_t type) {
    for (size_t i = 0; i < sizeof(dst->f64) / sizeof(*dst->f64); ++i) {
        dst->qw[i] = cmpd(dst->f64[i], src->f64[i], type) ? -1 : 0;
    }
}

// come to the dark side of macros
// A float that doesn't fit in an int32 (or is NaN) converts to the "integer
// indefinite" 0x80000000 on x86; a C cast is undefined there, and saturates
// on arm64 hosts.
#define _NOFIT_int32_t(x) false
#define _NOFIT_float(x) (isnan(x) || (x) >= 2147483648.0f || (x) < -2147483648.0f)
#define _NOFIT_double(x) (isnan(x) || (x) >= 2147483648.0 || (x) < -2147483648.0)
#define _NOFIT(x, t) _NOFIT_##t(x)
#define _VEC_CVT(src, dst, src_t, dst_t, n) \
    do { \
        for (int i = 0; i < n; ++i) { \
            if (_NOFIT(((src_t *)src)[i], src_t)) \
                ((dst_t *)dst)[i] = INT32_MIN; \
            else \
                ((dst_t *)dst)[i] = ((src_t *)src)[i]; \
        } \
    } while (0)

#define VEC_CVT(name, src_t, dst_t) \
    void vec_cvt##name(NO_CPU, const src_t *src, dst_t *dst) { \
        _VEC_CVT(src, dst, src_t, dst_t, 1); \
    }

#define PACKED_VEC_CVT(name, src_field, dst_field, src_t, dst_t, n) \
    void vec_cvt##name(NO_CPU, const union xmm_reg *src, union xmm_reg *dst) { \
        _VEC_CVT(src->src_field, dst->dst_field, src_t, dst_t, n); \
        /* Note: this needs to be second, because src and dst may alias */ \
        memset(dst->dst_field + n, 0, sizeof(*dst) - n * sizeof(*dst->dst_field)); \
    }

// A 32-bit result written to a general register clears its top half on
// x86_64 (dst points at the 8-byte register there).
#ifdef GUEST_X86_64
#define gpr_zext(dst) (((uint32_t *) (dst))[1] = 0)
#else
#define gpr_zext(dst) ((void) 0)
#endif

VEC_CVT(si2sd32, int32_t, double)
VEC_CVT(si2ss32, int32_t, float)
// Between float widths a NaN stays a NaN (a signaling one turns quiet), as
// the host's own conversion does: VEC_CVT's out-of-range check is for
// integer results, and made a NaN -2^31. (Go's compiler folds float32 NaN
// comparisons through this: binaries it built in the guest threw
// "float32nan" at startup.)
void vec_cvtsd2ss64(NO_CPU, const double *src, float *dst) { *dst = (float) *src; }
void vec_cvtss2sd32(NO_CPU, const float *src, double *dst) { *dst = (double) *src; }
void vec_cvttsd2si64(NO_CPU, const double *src, int32_t *dst) {
    _VEC_CVT(src, dst, double, int32_t, 1);
    gpr_zext(dst);
}
void vec_cvttss2si32(NO_CPU, const float *src, int32_t *dst) {
    _VEC_CVT(src, dst, float, int32_t, 1);
    gpr_zext(dst);
}

#ifdef GUEST_X86_64
// the REX.W forms, to and from 64-bit integers
#define NOFIT64(x) (isnan(x) || (x) >= 9223372036854775808.0 || (x) < -9223372036854775808.0)
void vec_cvtsi2sdq64(NO_CPU, const int64_t *src, double *dst) { *dst = (double) *src; }
void vec_cvtsi2ssq64(NO_CPU, const int64_t *src, float *dst) { *dst = (float) *src; }
void vec_cvttsd2siq64(NO_CPU, const double *src, int64_t *dst) {
    double x = *src;
    *dst = NOFIT64(x) ? INT64_MIN : (int64_t) x;
}
void vec_cvttss2siq32(NO_CPU, const float *src, int64_t *dst) {
    double x = *src;
    *dst = NOFIT64(x) ? INT64_MIN : (int64_t) x;
}
#endif

PACKED_VEC_CVT(tpd2dq64, f64, u32, double, int32_t, 2)
PACKED_VEC_CVT(tps2dq32, f32, u32, float, int32_t, 4)

void vec_unpackl_bw128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst) {
    for (int i = 7; i >= 0; i--) {
        dst->u8[i*2 + 1] = src->u8[i];
        dst->u8[i*2] = dst->u8[i];
    }
}
void vec_unpackl_w128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst) {
    for (int i = 3; i >= 0; i--) {
        dst->u16[i*2 + 1] = src->u16[i];
        dst->u16[i*2] = dst->u16[i];
    }
}
void vec_unpackl_dq128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst) {
    dst->u32[3] = src->u32[1];
    dst->u32[2] = dst->u32[1];
    dst->u32[1] = src->u32[0];
}
void vec_unpackl_qdq128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst) {
    dst->qw[1] = src->qw[0];
}
void vec_unpackl_ps128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst) {
    union xmm_reg s = *src; // src may be dst
    dst->u32[2] = dst->u32[1];
    dst->u32[1] = s.u32[0];
    dst->u32[3] = s.u32[1];
}
void vec_unpackl_pd128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst) {
    dst->f64[1] = src->f64[0];
}
void vec_unpackh_bw128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst) {
    for (int i = 0; i < 8; i++) {
        dst->u8[2 * i + 0] = dst->u8[i + 8];
        dst->u8[2 * i + 1] = src->u8[i + 8];
    }
}
void vec_unpackh_w128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst) {
    for (int i = 0; i < 4; i++) {
        dst->u16[2 * i + 0] = dst->u16[i + 4];
        dst->u16[2 * i + 1] = src->u16[i + 4];
    }
}
void vec_unpackh_d128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst) {
    dst->u32[0] = dst->u32[2];
    dst->u32[1] = src->u32[2];
    dst->u32[2] = dst->u32[3];
    dst->u32[3] = src->u32[3];
}
void vec_unpackh_dq128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst) {
    dst->qw[0] = dst->qw[1];
    dst->qw[1] = src->qw[1];
}
void vec_unpackh_ps128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst) {
    dst->u32[0] = dst->u32[2];
    dst->u32[1] = src->u32[2];
    dst->u32[2] = dst->u32[3];
    dst->u32[3] = src->u32[3];
}
void vec_unpackh_pd128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst) {
    dst->f64[0] = dst->f64[1];
    dst->f64[1] = src->f64[1];
}

void vec_packss_w128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst) {
    union xmm_reg s = *src; // src may be dst
    dst->u32[0] = (satsw(dst->u16[0]) << 0x00) | (satsw(dst->u16[1]) << 0x08) |
                  (satsw(dst->u16[2]) << 0x10) | (satsw(dst->u16[3]) << 0x18);
    dst->u32[1] = (satsw(dst->u16[4]) << 0x00) | (satsw(dst->u16[5]) << 0x08) |
                  (satsw(dst->u16[6]) << 0x10) | (satsw(dst->u16[7]) << 0x18);
    dst->u32[2] = (satsw(s.u16[0]) << 0x00) | (satsw(s.u16[1]) << 0x08) |
                  (satsw(s.u16[2]) << 0x10) | (satsw(s.u16[3]) << 0x18);
    dst->u32[3] = (satsw(s.u16[4]) << 0x00) | (satsw(s.u16[5]) << 0x08) |
                  (satsw(s.u16[6]) << 0x10) | (satsw(s.u16[7]) << 0x18);
}
void vec_packss_d128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst) {
    union xmm_reg s = *src; // src may be dst
    dst->u32[0] = satud(dst->u32[0]) | (satud(dst->u32[1]) << 16);
    dst->u32[1] = satud(dst->u32[2]) | (satud(dst->u32[3]) << 16);
    dst->u32[2] = satud(s.u32[0]) | (satud(s.u32[1]) << 16);
    dst->u32[3] = satud(s.u32[2]) | (satud(s.u32[3]) << 16);
}
void vec_packsu_w128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst) {
    union xmm_reg s = *src; // src may be dst
    dst->u32[0] = (satub(dst->u16[0]) << 0x00) | (satub(dst->u16[1]) << 0x08) |
                  (satub(dst->u16[2]) << 0x10) | (satub(dst->u16[3]) << 0x18);
    dst->u32[1] = (satub(dst->u16[4]) << 0x00) | (satub(dst->u16[5]) << 0x08) |
                  (satub(dst->u16[6]) << 0x10) | (satub(dst->u16[7]) << 0x18);
    dst->u32[2] = (satub(s.u16[0]) << 0x00) | (satub(s.u16[1]) << 0x08) |
                  (satub(s.u16[2]) << 0x10) | (satub(s.u16[3]) << 0x18);
    dst->u32[3] = (satub(s.u16[4]) << 0x00) | (satub(s.u16[5]) << 0x08) |
                  (satub(s.u16[6]) << 0x10) | (satub(s.u16[7]) << 0x18);
}

void vec_shuffle_lw128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst, uint8_t encoding) {
    union xmm_reg src_copy = *src;
    for (int i = 0; i < 4; i++)
        dst->u16[i] = src_copy.u16[(encoding >> (i*2)) % 4];
    dst->qw[1] = src->qw[1];
}
void vec_shuffle_hw128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst, uint8_t encoding) {
    union xmm_reg src_copy = *src;
    dst->qw[0] = src->qw[0];
    dst->u32[2] = src_copy.u16[(encoding >> 0 & 3) | 4] | src_copy.u16[(encoding >> 2 & 3) | 4] << 16;
    dst->u32[3] = src_copy.u16[(encoding >> 4 & 3) | 4] | src_copy.u16[(encoding >> 6 & 3) | 4] << 16;
}

void vec_shuffle_d128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst, uint8_t encoding) {
    union xmm_reg src_copy = *src;
    for (int i = 0; i < 4; i++)
        dst->u32[i] = src_copy.u32[(encoding >> (i*2)) % 4];
}
// the lanes are picked from the old dst and src, which may be the same register
void vec_shuffle_ps128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst, uint8_t encoding) {
    union xmm_reg s = *src, d = *dst;
    dst->u32[0] = d.u32[(encoding >> 0) & 3];
    dst->u32[1] = d.u32[(encoding >> 2) & 3];
    dst->u32[2] = s.u32[(encoding >> 4) & 3];
    dst->u32[3] = s.u32[(encoding >> 6) & 3];
}
void vec_shuffle_pd128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst, uint8_t encoding) {
    union xmm_reg s = *src, d = *dst;
    dst->qw[0] = d.qw[(encoding >> 0) & 1];
    dst->qw[1] = s.qw[(encoding >> 1) & 1];
}

void vec_movmask_b128(NO_CPU, const union xmm_reg *src, uint32_t *dst) {
    *dst = 0;
    for (unsigned i = 0; i < array_size(src->u8); i++) {
        if (src->u8[i] & (1 << 7))
            *dst |= 1 << i;
    }
    gpr_zext(dst);
}
void vec_fmovmask_d128(NO_CPU, const union xmm_reg *src, uint32_t *dst) {
    *dst = 0;
    for (unsigned i = 0; i < array_size(src->f64); i++) {
        if (signbit(src->f64[i]))
            *dst |= 1 << i;
    }
    gpr_zext(dst);
}

void vec_movl_p64(NO_CPU, const uint64_t *src, union xmm_reg *dst) {
    dst->qw[0] = *src;
}
void vec_movl_pm64(NO_CPU, const union xmm_reg *src, uint64_t *dst) {
    *dst = src->qw[0];
}
void vec_movh_p64(NO_CPU, const uint64_t *src, union xmm_reg *dst) {
    dst->qw[1] = *src;
}
void vec_movh_pm64(NO_CPU, const union xmm_reg *src, uint64_t *dst) {
    *dst = src->qw[1];
}

void vec_insert_w128(NO_CPU, const uint32_t *src, union xmm_reg *dst, uint8_t index) {
    dst->u16[index % 8] = (uint16_t)*src;
}
void vec_extract_w128(NO_CPU, const union xmm_reg *src, uint32_t *dst, uint8_t index) {
    *dst = src->u16[index % 8];
    gpr_zext(dst);
}

void vec_avg_b128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst) {
    for (unsigned i = 0; i < 16; i++)
        dst->u8[i] = (1 + dst->u8[i] + src->u8[i]) >> 1;
}
void vec_avg_w128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst) {
    for (unsigned i = 0; i < 8; i++)
        dst->u16[i] = (1 + dst->u16[i] + src->u16[i]) >> 1;
}

void vec_mull128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst) {
    for (int i = 0; i < 8; i++) {
        dst->u16[i] = (uint16_t)(dst->u16[i] * src->u16[i]);
    }
}

void vec_mulu128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst) {
    for (int i = 0; i < 8; i++) {
        uint32_t res = ((int16_t)dst->u16[i] * (int16_t)src->u16[i]);
        dst->u16[i] = ((res >> 16) & 0xffff);
    }
}
void vec_muluu128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst) {
    for (int i = 0; i < 8; i++) {
        uint32_t res = dst->u16[i] * src->u16[i];
        dst->u16[i] = ((res >> 16) & 0xffff);
    }
}

// ---- SSE/SSE2 completions: packed arithmetic, reciprocals, compares,
// conversions, and MXCSR

void vec_div_p32(NO_CPU, const union xmm_reg *src, union xmm_reg *dst) {
    for (int i = 0; i < 4; i++) dst->f32[i] /= src->f32[i];
}
void vec_div_p64(NO_CPU, const union xmm_reg *src, union xmm_reg *dst) {
    for (int i = 0; i < 2; i++) dst->f64[i] /= src->f64[i];
}
// min/max return the second (source) operand when either is NaN or both are
// zero, like the scalar versions above
void vec_min_p32(NO_CPU, const union xmm_reg *src, union xmm_reg *dst) {
    for (int i = 0; i < 4; i++)
        if (!(dst->f32[i] < src->f32[i])) dst->f32[i] = src->f32[i];
}
void vec_max_p32(NO_CPU, const union xmm_reg *src, union xmm_reg *dst) {
    for (int i = 0; i < 4; i++)
        if (!(dst->f32[i] > src->f32[i])) dst->f32[i] = src->f32[i];
}
void vec_min_p64(NO_CPU, const union xmm_reg *src, union xmm_reg *dst) {
    for (int i = 0; i < 2; i++)
        if (!(dst->f64[i] < src->f64[i])) dst->f64[i] = src->f64[i];
}
void vec_max_p64(NO_CPU, const union xmm_reg *src, union xmm_reg *dst) {
    for (int i = 0; i < 2; i++)
        if (!(dst->f64[i] > src->f64[i])) dst->f64[i] = src->f64[i];
}
void vec_sqrt_p32(NO_CPU, const union xmm_reg *src, union xmm_reg *dst) {
    union xmm_reg s = *src;
    for (int i = 0; i < 4; i++) dst->f32[i] = sqrtf(s.f32[i]);
}
void vec_sqrt_p64(NO_CPU, const union xmm_reg *src, union xmm_reg *dst) {
    union xmm_reg s = *src;
    for (int i = 0; i < 2; i++) dst->f64[i] = sqrt(s.f64[i]);
}
// rcp/rsqrt are approximations on hardware (12 bits); exact is within spec
void vec_rcp_p32(NO_CPU, const union xmm_reg *src, union xmm_reg *dst) {
    union xmm_reg s = *src;
    for (int i = 0; i < 4; i++) dst->f32[i] = 1.0f / s.f32[i];
}
void vec_rsqrt_p32(NO_CPU, const union xmm_reg *src, union xmm_reg *dst) {
    union xmm_reg s = *src;
    for (int i = 0; i < 4; i++) dst->f32[i] = 1.0f / sqrtf(s.f32[i]);
}
void vec_single_rcp32(NO_CPU, const float *src, float *dst) { *dst = 1.0f / *src; }
void vec_single_rsqrt32(NO_CPU, const float *src, float *dst) { *dst = 1.0f / sqrtf(*src); }

void vec_fcmp_p32(NO_CPU, const union xmm_reg *src, union xmm_reg *dst, uint8_t type) {
    for (int i = 0; i < 4; i++)
        dst->u32[i] = cmps(dst->f32[i], src->f32[i], type) ? -1 : 0;
}

void vec_fmovmask_s128(NO_CPU, const union xmm_reg *src, uint32_t *dst) {
    *dst = 0;
    for (int i = 0; i < 4; i++)
        if (signbit(src->f32[i]))
            *dst |= 1 << i;
    gpr_zext(dst);
}

void vec_movhl_ps128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst) {
    dst->qw[0] = src->qw[1];
}
void vec_movlh_ps128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst) {
    dst->qw[1] = src->qw[0];
}

// widening/narrowing conversions; src may be memory of the size in the name
void vec_cvtps2pd64(NO_CPU, const union xmm_reg *src, union xmm_reg *dst) {
    float a = src->f32[0], b = src->f32[1];
    dst->f64[0] = a;
    dst->f64[1] = b;
}
void vec_cvtpd2ps128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst) {
    double a = src->f64[0], b = src->f64[1];
    dst->f32[0] = (float) a;
    dst->f32[1] = (float) b;
    dst->qw[1] = 0;
}
void vec_cvtdq2ps128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst) {
    union xmm_reg s = *src;
    for (int i = 0; i < 4; i++) dst->f32[i] = (float) (int32_t) s.u32[i];
}
void vec_cvtdq2pd64(NO_CPU, const union xmm_reg *src, union xmm_reg *dst) {
    int32_t a = (int32_t) src->u32[0], b = (int32_t) src->u32[1];
    dst->f64[0] = a;
    dst->f64[1] = b;
}

// The rounding conversions round as MXCSR.RC says.
static double round_mxcsr(struct cpu_state *cpu, double x) {
    switch ((cpu->mxcsr >> 13) & 3) {
        case 0: return nearbyint(x); // to nearest even (the host's default mode)
        case 1: return floor(x);
        case 2: return ceil(x);
        default: return trunc(x);
    }
}
static int32_t round_to_i32(struct cpu_state *cpu, double x) {
    double r = round_mxcsr(cpu, x);
    if (isnan(r) || r >= 2147483648.0 || r < -2147483648.0)
        return INT32_MIN;
    return (int32_t) r;
}
void vec_cvtps2dq128(struct cpu_state *cpu, const union xmm_reg *src, union xmm_reg *dst) {
    union xmm_reg s = *src;
    for (int i = 0; i < 4; i++) dst->u32[i] = round_to_i32(cpu, s.f32[i]);
}
void vec_cvtpd2dq128(struct cpu_state *cpu, const union xmm_reg *src, union xmm_reg *dst) {
    union xmm_reg s = *src;
    dst->u32[0] = round_to_i32(cpu, s.f64[0]);
    dst->u32[1] = round_to_i32(cpu, s.f64[1]);
    dst->qw[1] = 0;
}
void vec_cvtsd2si64(struct cpu_state *cpu, const double *src, int32_t *dst) {
    *dst = round_to_i32(cpu, *src);
    gpr_zext(dst);
}
void vec_cvtss2si32(struct cpu_state *cpu, const float *src, int32_t *dst) {
    *dst = round_to_i32(cpu, *src);
    gpr_zext(dst);
}
#ifdef GUEST_X86_64
void vec_cvtsd2siq64(struct cpu_state *cpu, const double *src, int64_t *dst) {
    double r = round_mxcsr(cpu, *src);
    *dst = NOFIT64(r) ? INT64_MIN : (int64_t) r;
}
void vec_cvtss2siq32(struct cpu_state *cpu, const float *src, int64_t *dst) {
    double r = round_mxcsr(cpu, *src);
    *dst = NOFIT64(r) ? INT64_MIN : (int64_t) r;
}
#endif

// ldmxcsr/stmxcsr through the memory helpers
void sse_ldmxcsr32(struct cpu_state *cpu, const uint32_t *src) {
    cpu->mxcsr = *src & 0xffff;
}
void sse_stmxcsr32(struct cpu_state *cpu, uint32_t *dst) {
    *dst = cpu->mxcsr;
}

// ---- SSSE3 (66 0f 38 xx, 66 0f 3a 0f). Not advertised in CPUID, but used
// unconditionally by code that picks its kernels from the CPU model
// (OpenBLAS), so they have to work anyway.

static int16_t sat16(int32_t x) { return x > INT16_MAX ? INT16_MAX : x < INT16_MIN ? INT16_MIN : x; }

void vec_pshufb128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst) {
    union xmm_reg d = *dst, s = *src;
    for (int i = 0; i < 16; i++)
        dst->u8[i] = (s.u8[i] & 0x80) ? 0 : d.u8[s.u8[i] & 15];
}
#define HORIZONTAL(name, type, field, n, expr) \
    void vec_##name##128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst) { \
        union xmm_reg d = *dst, s = *src; \
        for (int i = 0; i < n / 2; i++) { \
            type a = (type) d.field[2 * i], b = (type) d.field[2 * i + 1]; \
            dst->field[i] = expr; \
            a = (type) s.field[2 * i]; b = (type) s.field[2 * i + 1]; \
            dst->field[n / 2 + i] = expr; \
        } \
    }
HORIZONTAL(phaddw, int16_t, u16, 8, (int16_t) (a + b))
HORIZONTAL(phaddd, int32_t, u32, 4, (int32_t) ((uint32_t) a + (uint32_t) b))
HORIZONTAL(phaddsw, int16_t, u16, 8, sat16(a + b))
HORIZONTAL(phsubw, int16_t, u16, 8, (int16_t) (a - b))
HORIZONTAL(phsubd, int32_t, u32, 4, (int32_t) ((uint32_t) a - (uint32_t) b))
HORIZONTAL(phsubsw, int16_t, u16, 8, sat16(a - b))
#undef HORIZONTAL

void vec_pmaddubsw128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst) {
    union xmm_reg d = *dst, s = *src;
    for (int i = 0; i < 8; i++)
        dst->u16[i] = sat16(d.u8[2 * i] * (int8_t) s.u8[2 * i] + d.u8[2 * i + 1] * (int8_t) s.u8[2 * i + 1]);
}
void vec_pmulhrsw128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst) {
    for (int i = 0; i < 8; i++)
        dst->u16[i] = (uint16_t) (((((int32_t) (int16_t) dst->u16[i] * (int16_t) src->u16[i]) >> 14) + 1) >> 1);
}
#define SIGN(name, type, field, n) \
    void vec_##name##128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst) { \
        for (int i = 0; i < n; i++) { \
            type s = (type) src->field[i]; \
            if (s < 0) dst->field[i] = -(type) dst->field[i]; \
            else if (s == 0) dst->field[i] = 0; \
        } \
    }
SIGN(psignb, int8_t, u8, 16)
SIGN(psignw, int16_t, u16, 8)
SIGN(psignd, int32_t, u32, 4)
#undef SIGN
#define ABS(name, type, field, n) \
    void vec_##name##128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst) { \
        for (int i = 0; i < n; i++) { \
            type s = (type) src->field[i]; \
            dst->field[i] = s < 0 ? -s : s; \
        } \
    }
ABS(pabsb, int8_t, u8, 16)
ABS(pabsw, int16_t, u16, 8)
ABS(pabsd, int32_t, u32, 4)
#undef ABS

void vec_palignr128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst, uint8_t imm) {
    uint8_t cat[32];
    memcpy(cat, src->u8, 16);       // low half: the source
    memcpy(cat + 16, dst->u8, 16);  // high half: the destination
    for (int i = 0; i < 16; i++)
        dst->u8[i] = imm + i < 32 ? cat[imm + i] : 0;
}

// ---- SSE3, SSE4.1, SSE4.2 (the x86-64-v2 baseline of the x86_64 guest).
// The operands are copied first: src and dst may be the same register.

void vec_haddps128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst) {
    union xmm_reg s = *src, d = *dst;
    dst->f32[0] = d.f32[0] + d.f32[1];
    dst->f32[1] = d.f32[2] + d.f32[3];
    dst->f32[2] = s.f32[0] + s.f32[1];
    dst->f32[3] = s.f32[2] + s.f32[3];
}
void vec_hsubps128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst) {
    union xmm_reg s = *src, d = *dst;
    dst->f32[0] = d.f32[0] - d.f32[1];
    dst->f32[1] = d.f32[2] - d.f32[3];
    dst->f32[2] = s.f32[0] - s.f32[1];
    dst->f32[3] = s.f32[2] - s.f32[3];
}
void vec_haddpd128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst) {
    union xmm_reg s = *src, d = *dst;
    dst->f64[0] = d.f64[0] + d.f64[1];
    dst->f64[1] = s.f64[0] + s.f64[1];
}
void vec_hsubpd128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst) {
    union xmm_reg s = *src, d = *dst;
    dst->f64[0] = d.f64[0] - d.f64[1];
    dst->f64[1] = s.f64[0] - s.f64[1];
}
void vec_addsubps128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst) {
    union xmm_reg s = *src;
    dst->f32[0] -= s.f32[0];
    dst->f32[1] += s.f32[1];
    dst->f32[2] -= s.f32[2];
    dst->f32[3] += s.f32[3];
}
void vec_addsubpd128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst) {
    union xmm_reg s = *src;
    dst->f64[0] -= s.f64[0];
    dst->f64[1] += s.f64[1];
}
void vec_movddup64(NO_CPU, const uint64_t *src, union xmm_reg *dst) {
    uint64_t q = *src;
    dst->qw[0] = dst->qw[1] = q;
}
void vec_movsldup128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst) {
    union xmm_reg s = *src;
    dst->u32[0] = dst->u32[1] = s.u32[0];
    dst->u32[2] = dst->u32[3] = s.u32[2];
}
void vec_movshdup128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst) {
    union xmm_reg s = *src;
    dst->u32[0] = dst->u32[1] = s.u32[1];
    dst->u32[2] = dst->u32[3] = s.u32[3];
}

// pmovsx/pmovzx: src is memory of the size in the name, or the low bytes of
// an xmm register
#define PMOVX(name, size, src_t, dst_t, n) \
    void vec_##name##size(NO_CPU, const void *src, union xmm_reg *dst) { \
        src_t s[n]; \
        dst_t d[n]; \
        memcpy(s, src, sizeof(s)); \
        for (int i = 0; i < n; i++) \
            d[i] = (dst_t) s[i]; \
        memcpy(dst, d, sizeof(d)); \
    }
PMOVX(pmovsxbw, 64, int8_t, int16_t, 8)
PMOVX(pmovsxbd, 32, int8_t, int32_t, 4)
PMOVX(pmovsxbq, 16, int8_t, int64_t, 2)
PMOVX(pmovsxwd, 64, int16_t, int32_t, 4)
PMOVX(pmovsxwq, 32, int16_t, int64_t, 2)
PMOVX(pmovsxdq, 64, int32_t, int64_t, 2)
PMOVX(pmovzxbw, 64, uint8_t, uint16_t, 8)
PMOVX(pmovzxbd, 32, uint8_t, uint32_t, 4)
PMOVX(pmovzxbq, 16, uint8_t, uint64_t, 2)
PMOVX(pmovzxwd, 64, uint16_t, uint32_t, 4)
PMOVX(pmovzxwq, 32, uint16_t, uint64_t, 2)
PMOVX(pmovzxdq, 64, uint32_t, uint64_t, 2)
#undef PMOVX

#define PMINMAX(name, t, n, op) \
    void vec_##name##128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst) { \
        t s[n], d[n]; \
        memcpy(s, src, 16); \
        memcpy(d, dst, 16); \
        for (int i = 0; i < n; i++) \
            d[i] = (s[i] op d[i]) ? s[i] : d[i]; \
        memcpy(dst, d, 16); \
    }
PMINMAX(pminsb, int8_t, 16, <)
PMINMAX(pmaxsb, int8_t, 16, >)
PMINMAX(pminuw, uint16_t, 8, <)
PMINMAX(pmaxuw, uint16_t, 8, >)
PMINMAX(pminsd, int32_t, 4, <)
PMINMAX(pmaxsd, int32_t, 4, >)
PMINMAX(pminud, uint32_t, 4, <)
PMINMAX(pmaxud, uint32_t, 4, >)
#undef PMINMAX

void vec_pmulld128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst) {
    union xmm_reg s = *src;
    for (int i = 0; i < 4; i++)
        dst->u32[i] *= s.u32[i];
}
void vec_pmuldq128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst) {
    union xmm_reg s = *src, d = *dst;
    dst->qw[0] = (uint64_t) ((int64_t) (int32_t) d.u32[0] * (int32_t) s.u32[0]);
    dst->qw[1] = (uint64_t) ((int64_t) (int32_t) d.u32[2] * (int32_t) s.u32[2]);
}
void vec_pcmpeqq128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst) {
    union xmm_reg s = *src;
    for (int i = 0; i < 2; i++)
        dst->qw[i] = dst->qw[i] == s.qw[i] ? ~0ull : 0;
}
void vec_pcmpgtq128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst) {
    union xmm_reg s = *src;
    for (int i = 0; i < 2; i++)
        dst->qw[i] = (int64_t) dst->qw[i] > (int64_t) s.qw[i] ? ~0ull : 0;
}
static uint16_t sat_u16(int32_t v) { return v < 0 ? 0 : v > 0xffff ? 0xffff : (uint16_t) v; }
void vec_packusdw128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst) {
    union xmm_reg s = *src, d = *dst;
    for (int i = 0; i < 4; i++) {
        dst->u16[i] = sat_u16((int32_t) d.u32[i]);
        dst->u16[i + 4] = sat_u16((int32_t) s.u32[i]);
    }
}
void vec_phminposuw128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst) {
    union xmm_reg s = *src;
    int min = 0;
    for (int i = 1; i < 8; i++)
        if (s.u16[i] < s.u16[min])
            min = i;
    dst->qw[0] = s.u16[min] | (uint64_t) min << 16;
    dst->qw[1] = 0;
}
void vec_ptest128(struct cpu_state *cpu, const union xmm_reg *src, const union xmm_reg *dst) {
    cpu->zf = (src->qw[0] & dst->qw[0]) == 0 && (src->qw[1] & dst->qw[1]) == 0;
    cpu->cf = (src->qw[0] & ~dst->qw[0]) == 0 && (src->qw[1] & ~dst->qw[1]) == 0;
    cpu->zf_res = cpu->sf_res = cpu->pf_res = cpu->af_ops = 0;
    cpu->of = cpu->sf = cpu->pf = cpu->af = 0;
}

// blends: imm8 selects per lane, the v forms use the top bits of xmm0
void vec_blendps128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst, uint8_t imm) {
    union xmm_reg s = *src;
    for (int i = 0; i < 4; i++)
        if (imm & (1 << i))
            dst->u32[i] = s.u32[i];
}
void vec_blendpd128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst, uint8_t imm) {
    union xmm_reg s = *src;
    for (int i = 0; i < 2; i++)
        if (imm & (1 << i))
            dst->qw[i] = s.qw[i];
}
void vec_pblendw128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst, uint8_t imm) {
    union xmm_reg s = *src;
    for (int i = 0; i < 8; i++)
        if (imm & (1 << i))
            dst->u16[i] = s.u16[i];
}
void vec_pblendvb128(struct cpu_state *cpu, const union xmm_reg *src, union xmm_reg *dst) {
    union xmm_reg s = *src, m = cpu->xmm[0];
    for (int i = 0; i < 16; i++)
        if (m.u8[i] & 0x80)
            dst->u8[i] = s.u8[i];
}
void vec_blendvps128(struct cpu_state *cpu, const union xmm_reg *src, union xmm_reg *dst) {
    union xmm_reg s = *src, m = cpu->xmm[0];
    for (int i = 0; i < 4; i++)
        if (m.u32[i] & 0x80000000)
            dst->u32[i] = s.u32[i];
}
void vec_blendvpd128(struct cpu_state *cpu, const union xmm_reg *src, union xmm_reg *dst) {
    union xmm_reg s = *src, m = cpu->xmm[0];
    for (int i = 0; i < 2; i++)
        if (m.qw[i] & 0x8000000000000000ull)
            dst->qw[i] = s.qw[i];
}

// round: imm8 bits 1:0 the mode, bit 2 says use MXCSR.RC instead
static double round_imm(struct cpu_state *cpu, double x, uint8_t imm) {
    switch (imm & 4 ? (cpu->mxcsr >> 13) & 3 : imm & 3) {
        case 0: return nearbyint(x);
        case 1: return floor(x);
        case 2: return ceil(x);
        default: return trunc(x);
    }
}
void vec_roundps128(struct cpu_state *cpu, const union xmm_reg *src, union xmm_reg *dst, uint8_t imm) {
    union xmm_reg s = *src;
    for (int i = 0; i < 4; i++)
        dst->f32[i] = isnan(s.f32[i]) ? s.f32[i] : (float) round_imm(cpu, s.f32[i], imm);
}
void vec_roundpd128(struct cpu_state *cpu, const union xmm_reg *src, union xmm_reg *dst, uint8_t imm) {
    union xmm_reg s = *src;
    for (int i = 0; i < 2; i++)
        dst->f64[i] = isnan(s.f64[i]) ? s.f64[i] : round_imm(cpu, s.f64[i], imm);
}
void vec_roundss32(struct cpu_state *cpu, const float *src, union xmm_reg *dst, uint8_t imm) {
    float s = *src;
    dst->f32[0] = isnan(s) ? s : (float) round_imm(cpu, s, imm);
}
void vec_roundsd64(struct cpu_state *cpu, const double *src, union xmm_reg *dst, uint8_t imm) {
    double s = *src;
    dst->f64[0] = isnan(s) ? s : round_imm(cpu, s, imm);
}

void vec_dpps128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst, uint8_t imm) {
    union xmm_reg s = *src, d = *dst;
    float p[4];
    for (int i = 0; i < 4; i++)
        p[i] = imm & (0x10 << i) ? d.f32[i] * s.f32[i] : 0.0f;
    float sum = (p[0] + p[1]) + (p[2] + p[3]);
    for (int i = 0; i < 4; i++)
        dst->f32[i] = imm & (1 << i) ? sum : 0.0f;
}
void vec_dppd128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst, uint8_t imm) {
    union xmm_reg s = *src, d = *dst;
    double p0 = imm & 0x10 ? d.f64[0] * s.f64[0] : 0.0;
    double p1 = imm & 0x20 ? d.f64[1] * s.f64[1] : 0.0;
    double sum = p0 + p1;
    for (int i = 0; i < 2; i++)
        dst->f64[i] = imm & (1 << i) ? sum : 0.0;
}
// insertps: a register source gives lane imm[7:6], a memory one its m32
void vec_insertps128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst, uint8_t imm) {
    uint32_t v = src->u32[(imm >> 6) & 3];
    dst->u32[(imm >> 4) & 3] = v;
    for (int i = 0; i < 4; i++)
        if (imm & (1 << i))
            dst->u32[i] = 0;
}
void vec_insertps32(NO_CPU, const uint32_t *src, union xmm_reg *dst, uint8_t imm) {
    uint32_t v = *src;
    dst->u32[(imm >> 4) & 3] = v;
    for (int i = 0; i < 4; i++)
        if (imm & (1 << i))
            dst->u32[i] = 0;
}
void vec_mpsadbw128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst, uint8_t imm) {
    union xmm_reg s = *src, d = *dst;
    int doff = (imm & 4) ? 4 : 0, soff = (imm & 3) * 4;
    for (int i = 0; i < 8; i++) {
        int sum = 0;
        for (int k = 0; k < 4; k++)
            sum += abs((int) d.u8[doff + i + k] - (int) s.u8[soff + k]);
        dst->u16[i] = (uint16_t) sum;
    }
}

// extraction to a general register (zero-extended, all 8 bytes written) or
// to memory of the size in the name; insertion from either
void vec_pextrb64(NO_CPU, const union xmm_reg *src, uint64_t *dst, uint8_t imm) { *dst = src->u8[imm & 15]; }
void vec_pextrb8(NO_CPU, const union xmm_reg *src, uint8_t *dst, uint8_t imm) { *dst = src->u8[imm & 15]; }
void vec_pextrw16(NO_CPU, const union xmm_reg *src, uint16_t *dst, uint8_t imm) { *dst = src->u16[imm & 7]; }
void vec_pextrw64(NO_CPU, const union xmm_reg *src, uint64_t *dst, uint8_t imm) { *dst = src->u16[imm & 7]; }
void vec_pextrd64(NO_CPU, const union xmm_reg *src, uint64_t *dst, uint8_t imm) { *dst = src->u32[imm & 3]; }
void vec_pextrd32(NO_CPU, const union xmm_reg *src, uint32_t *dst, uint8_t imm) { *dst = src->u32[imm & 3]; }
void vec_pextrqq64(NO_CPU, const union xmm_reg *src, uint64_t *dst, uint8_t imm) { *dst = src->qw[imm & 1]; }
void vec_pinsrb8(NO_CPU, const uint8_t *src, union xmm_reg *dst, uint8_t imm) { dst->u8[imm & 15] = *src; }
void vec_pinsrd32(NO_CPU, const uint32_t *src, union xmm_reg *dst, uint8_t imm) { dst->u32[imm & 3] = *src; }
void vec_pinsrq64(NO_CPU, const uint64_t *src, union xmm_reg *dst, uint8_t imm) { dst->qw[imm & 1] = *src; }

// crc32 (Castagnoli, reflected): dst is the 64-bit register, the result is
// 32 bits zero-extended for every operand size
static uint32_t crc32c_bytes(uint32_t crc, const uint8_t *p, int n) {
    for (int i = 0; i < n; i++) {
        crc ^= p[i];
        for (int k = 0; k < 8; k++)
            crc = (crc >> 1) ^ (0x82f63b78 & -(crc & 1));
    }
    return crc;
}
void vec_crc32_8(NO_CPU, const void *src, uint64_t *dst) { *dst = crc32c_bytes((uint32_t) *dst, src, 1); }
void vec_crc32_16(NO_CPU, const void *src, uint64_t *dst) { *dst = crc32c_bytes((uint32_t) *dst, src, 2); }
void vec_crc32_32(NO_CPU, const void *src, uint64_t *dst) { *dst = crc32c_bytes((uint32_t) *dst, src, 4); }
void vec_crc32_64(NO_CPU, const void *src, uint64_t *dst) { *dst = crc32c_bytes((uint32_t) *dst, src, 8); }

#ifdef GUEST_X86_64
// pcmpestri/pcmpestrm/pcmpistri/pcmpistrm (SSE4.2), as the SDM describes
// them: a is the destination register, b the source; the lengths come from
// rax/rdx (explicit) or the first zero element (implicit).
static int pcmpstr_elem(const union xmm_reg *x, int i, uint8_t imm) {
    switch (imm & 3) {
        case 0: return x->u8[i];
        case 1: return x->u16[i];
        case 2: return (int8_t) x->u8[i];
        default: return (int16_t) x->u16[i];
    }
}
static int pcmpstr_implicit_len(const union xmm_reg *x, uint8_t imm, int n) {
    for (int i = 0; i < n; i++)
        if (pcmpstr_elem(x, i, imm) == 0)
            return i;
    return n;
}
static int pcmpstr_explicit_len(int64_t len, int n) {
    if (len < 0)
        len = len == INT64_MIN ? n : -len;
    return len > n ? n : (int) len;
}
static unsigned pcmpstr(struct cpu_state *cpu, const union xmm_reg *a, const union xmm_reg *b,
        uint8_t imm, int la, int lb) {
    int n = imm & 1 ? 8 : 16;
    unsigned res = 0;
    for (int j = 0; j < n; j++) {
        bool r;
        switch ((imm >> 2) & 3) {
            case 0: // equal any
                r = false;
                if (j < lb)
                    for (int i = 0; i < la; i++)
                        if (pcmpstr_elem(a, i, imm) == pcmpstr_elem(b, j, imm))
                            r = true;
                break;
            case 1: // ranges: pairs of a are inclusive bounds
                r = false;
                if (j < lb)
                    for (int i = 0; i + 1 < la; i += 2)
                        if (pcmpstr_elem(a, i, imm) <= pcmpstr_elem(b, j, imm) &&
                                pcmpstr_elem(b, j, imm) <= pcmpstr_elem(a, i + 1, imm))
                            r = true;
                break;
            case 2: // equal each
                if (j >= la && j >= lb)
                    r = true;
                else if (j >= la || j >= lb)
                    r = false;
                else
                    r = pcmpstr_elem(a, j, imm) == pcmpstr_elem(b, j, imm);
                break;
            default: // equal ordered: a occurs in b at j
                r = true;
                for (int i = 0; i < n - j && r; i++) {
                    if (i >= la)
                        break; // the rest of a is invalid: true
                    if (j + i >= lb)
                        r = false;
                    else
                        r = pcmpstr_elem(a, i, imm) == pcmpstr_elem(b, j + i, imm);
                }
                break;
        }
        if (r)
            res |= 1u << j;
    }
    unsigned all = (1u << n) - 1;
    switch ((imm >> 4) & 3) {
        case 1: res ^= all; break;
        case 3: res ^= (1u << lb) - 1; break; // masked negative: valid b only
    }
    cpu->cf = res != 0;
    cpu->zf = lb < n;
    cpu->sf = la < n;
    cpu->of = res & 1;
    cpu->zf_res = cpu->sf_res = cpu->pf_res = cpu->af_ops = 0;
    cpu->pf = cpu->af = 0;
    return res;
}
static void pcmpstr_index(struct cpu_state *cpu, unsigned res, uint8_t imm) {
    int n = imm & 1 ? 8 : 16;
    if (res == 0)
        cpu->rcx = n;
    else if (imm & 0x40)
        cpu->rcx = 31 - __builtin_clz(res);
    else
        cpu->rcx = __builtin_ctz(res);
}
static void pcmpstr_mask(struct cpu_state *cpu, unsigned res, uint8_t imm) {
    union xmm_reg m = {};
    if (!(imm & 0x40)) {
        m.u16[0] = (uint16_t) res;
    } else if (imm & 1) {
        for (int i = 0; i < 8; i++)
            m.u16[i] = res & (1u << i) ? 0xffff : 0;
    } else {
        for (int i = 0; i < 16; i++)
            m.u8[i] = res & (1u << i) ? 0xff : 0;
    }
    cpu->xmm[0] = m;
}
#define PCMPSTR_LENS_E \
    int n = imm & 1 ? 8 : 16; \
    int la = pcmpstr_explicit_len((int32_t) cpu->rax, n), lb = pcmpstr_explicit_len((int32_t) cpu->rdx, n);
#define PCMPSTR_LENS_I \
    int n = imm & 1 ? 8 : 16; \
    int la = pcmpstr_implicit_len(&a, imm, n), lb = pcmpstr_implicit_len(&b, imm, n);
void vec_pcmpestri128(struct cpu_state *cpu, const union xmm_reg *src, const union xmm_reg *dst, uint8_t imm) {
    union xmm_reg a = *dst, b = *src;
    PCMPSTR_LENS_E
    pcmpstr_index(cpu, pcmpstr(cpu, &a, &b, imm, la, lb), imm);
}
void vec_pcmpestrm128(struct cpu_state *cpu, const union xmm_reg *src, const union xmm_reg *dst, uint8_t imm) {
    union xmm_reg a = *dst, b = *src;
    PCMPSTR_LENS_E
    pcmpstr_mask(cpu, pcmpstr(cpu, &a, &b, imm, la, lb), imm);
}
void vec_pcmpistri128(struct cpu_state *cpu, const union xmm_reg *src, const union xmm_reg *dst, uint8_t imm) {
    union xmm_reg a = *dst, b = *src;
    PCMPSTR_LENS_I
    pcmpstr_index(cpu, pcmpstr(cpu, &a, &b, imm, la, lb), imm);
}
void vec_pcmpistrm128(struct cpu_state *cpu, const union xmm_reg *src, const union xmm_reg *dst, uint8_t imm) {
    union xmm_reg a = *dst, b = *src;
    PCMPSTR_LENS_I
    pcmpstr_mask(cpu, pcmpstr(cpu, &a, &b, imm, la, lb), imm);
}
#endif
