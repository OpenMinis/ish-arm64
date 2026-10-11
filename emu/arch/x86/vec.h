#ifndef EMU_SSE_H
#define EMU_SSE_H

#include "emu/cpu.h"

#define NO_CPU struct cpu_state *UNUSED(cpu)

// arguments are in src, dst order

void vec_zero128_copy128(NO_CPU, const void *src, void *dst);
void vec_zero128_copy64(NO_CPU, const void *src, void *dst);
void vec_zero128_copy32(NO_CPU, const void *src, void *dst);
void vec_zero64_copy64(NO_CPU, const void *src, void *dst);
void vec_zero64_copy32(NO_CPU, const void *src, void *dst);
void vec_zero32_copy32(NO_CPU, const void *src, void *dst);
// "merge" means don't zero the register before writing to it
void vec_merge32(NO_CPU, const void *src, void *dst);
void vec_merge64(NO_CPU, const void *src, void *dst);
void vec_merge128(NO_CPU, const void *src, void *dst);

void vec_shiftl_w64(NO_CPU, const union mm_reg *src, union mm_reg *dst);
void vec_shiftl_d64(NO_CPU, const union mm_reg *src, union mm_reg *dst);
void vec_shiftl_q64(NO_CPU, const union mm_reg *src, union mm_reg *dst);
void vec_shiftr_w64(NO_CPU, const union mm_reg *src, union mm_reg *dst);
void vec_shiftr_d64(NO_CPU, const union mm_reg *src, union mm_reg *dst);
void vec_shiftr_q64(NO_CPU, const union mm_reg *src, union mm_reg *dst);
void vec_shiftrs_w64(NO_CPU, const union mm_reg *src, union mm_reg *dst);
void vec_shiftrs_d64(NO_CPU, const union mm_reg *src, union mm_reg *dst);

void vec_shiftl_w128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst);
void vec_shiftl_d128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst);
void vec_shiftl_q128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst);
void vec_shiftr_w128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst);
void vec_shiftr_d128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst);
void vec_shiftr_q128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst);
void vec_shiftrs_w128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst);
void vec_shiftrs_d128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst);

void vec_imm_shiftl_w64(NO_CPU, const uint8_t amount, union mm_reg *dst);
void vec_imm_shiftl_d64(NO_CPU, const uint8_t amount, union mm_reg *dst);
void vec_imm_shiftl_q64(NO_CPU, const uint8_t amount, union mm_reg *dst);
void vec_imm_shiftr_w64(NO_CPU, const uint8_t amount, union mm_reg *dst);
void vec_imm_shiftr_d64(NO_CPU, const uint8_t amount, union mm_reg *dst);
void vec_imm_shiftr_q64(NO_CPU, const uint8_t amount, union mm_reg *dst);
void vec_imm_shiftrs_w64(NO_CPU, const uint8_t amount, union mm_reg *dst);
void vec_imm_shiftrs_d64(NO_CPU, const uint8_t amount, union mm_reg *dst);

void vec_imm_shiftl_w128(NO_CPU, const uint8_t amount, union xmm_reg *dst);
void vec_imm_shiftl_q128(NO_CPU, const uint8_t amount, union xmm_reg *dst);
void vec_imm_shiftl_d128(NO_CPU, const uint8_t amount, union xmm_reg *dst);
void vec_imm_shiftl_dq128(NO_CPU, const uint8_t amount, union xmm_reg *dst);

void vec_imm_shiftr_q128(NO_CPU, const uint8_t amount, union xmm_reg *dst);
void vec_imm_shiftr_w128(NO_CPU, const uint8_t amount, union xmm_reg *dst);
void vec_imm_shiftr_d128(NO_CPU, const uint8_t amount, union xmm_reg *dst);

void vec_imm_shiftr_dq128(NO_CPU, uint8_t amount, union xmm_reg *dst);
void vec_imm_shiftrs_w128(NO_CPU, const uint8_t amount, union xmm_reg *dst);
void vec_imm_shiftrs_d128(NO_CPU, const uint8_t amount, union xmm_reg *dst);

void vec_add_b64(NO_CPU, const union mm_reg *src, union mm_reg *dst);
void vec_add_w64(NO_CPU, const union mm_reg *src, union mm_reg *dst);
void vec_add_d64(NO_CPU, const union mm_reg *src, union mm_reg *dst);
void vec_add_q64(NO_CPU, const union mm_reg *src, union mm_reg *dst);

void vec_sub_b64(NO_CPU, const union mm_reg *src, union mm_reg *dst);
void vec_sub_w64(NO_CPU, const union mm_reg *src, union mm_reg *dst);
void vec_sub_d64(NO_CPU, const union mm_reg *src, union mm_reg *dst);
void vec_sub_q64(NO_CPU, const union mm_reg *src, union mm_reg *dst);

void vec_add_b128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst);
void vec_add_w128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst);
void vec_add_d128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst);
void vec_add_q128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst);

void vec_addus_b128(NO_CPU, union xmm_reg *src, union xmm_reg *dst);
void vec_addus_w128(NO_CPU, union xmm_reg *src, union xmm_reg *dst);
void vec_addss_b128(NO_CPU, union xmm_reg *src, union xmm_reg *dst);
void vec_addss_w128(NO_CPU, union xmm_reg *src, union xmm_reg *dst);

void vec_sub_b128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst);
void vec_sub_w128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst);
void vec_sub_d128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst);
void vec_sub_q128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst);

void vec_subus_b128(NO_CPU, union xmm_reg *src, union xmm_reg *dst);
void vec_subus_w128(NO_CPU, union xmm_reg *src, union xmm_reg *dst);
void vec_subss_b128(NO_CPU, union xmm_reg *src, union xmm_reg *dst);
void vec_subss_w128(NO_CPU, union xmm_reg *src, union xmm_reg *dst);
void vec_mulu_dq128(NO_CPU, union xmm_reg *src, union xmm_reg *dst);
void vec_mulu_dq64(NO_CPU, union mm_reg *src, union mm_reg *dst);
void vec_mulu64(NO_CPU, const union mm_reg *src, union mm_reg *dst);
void vec_mull64(NO_CPU, const union mm_reg *src, union mm_reg *dst);
void vec_mulu128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst);
void vec_muluu128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst);
void vec_mull128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst);
void vec_madd_d128(NO_CPU, union xmm_reg *src, union xmm_reg *dst);
void vec_sumabs_w128(NO_CPU, union xmm_reg *src, union xmm_reg *dst);

void vec_add_p64(NO_CPU, union xmm_reg *src, union xmm_reg *dst);
void vec_add_p32(NO_CPU, union xmm_reg *src, union xmm_reg *dst);
void vec_sub_p64(NO_CPU, union xmm_reg *src, union xmm_reg *dst);
void vec_sub_p32(NO_CPU, union xmm_reg *src, union xmm_reg *dst);
void vec_mul_p64(NO_CPU, union xmm_reg *src, union xmm_reg *dst);
void vec_mul_p32(NO_CPU, union xmm_reg *src, union xmm_reg *dst);

void vec_or_dq128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst);
void vec_xor_dq128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst);
void vec_and_dq128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst);
void vec_andn128(NO_CPU, union xmm_reg *src, union xmm_reg *dst);

void vec_or_q64(NO_CPU, const union mm_reg *src, union mm_reg *dst);
void vec_and_q64(NO_CPU, const union mm_reg *src, union mm_reg *dst);
void vec_xor_q64(NO_CPU, const union mm_reg *src, union mm_reg *dst);

void vec_min_ub128(NO_CPU, union xmm_reg *src, union xmm_reg *dst);
void vec_mins_w128(NO_CPU, union xmm_reg *src, union xmm_reg *dst);
void vec_max_ub128(NO_CPU, union xmm_reg *src, union xmm_reg *dst);
void vec_maxs_w128(NO_CPU, union xmm_reg *src, union xmm_reg *dst);

void vec_single_fadd64(NO_CPU, const double *src, double *dst);
void vec_single_fadd32(NO_CPU, const float *src, float *dst);
void vec_single_fmul64(NO_CPU, const double *src, double *dst);
void vec_single_fmul32(NO_CPU, const float *src, float *dst);
void vec_single_fsub64(NO_CPU, const double *src, double *dst);
void vec_single_fsub32(NO_CPU, const float *src, float *dst);
void vec_single_fdiv64(NO_CPU, const double *src, double *dst);
void vec_single_fdiv32(NO_CPU, const float *src, float *dst);
void vec_single_fsqrt64(NO_CPU, const double *src, double *dst);
void vec_single_fsqrt32(NO_CPU, const float *src, float *dst);

void vec_single_fmax64(NO_CPU, const double *src, double *dst);
void vec_single_fmax32(NO_CPU, const float *src, float *dst);
void vec_single_fmin64(NO_CPU, const double *src, double *dst);
void vec_single_fmin32(NO_CPU, const float *src, float *dst);
void vec_single_ucomi32(struct cpu_state *cpu, const float *src, const float *dst);
void vec_single_ucomi64(struct cpu_state *cpu, const double *src, const double *dst);
void vec_single_fcmp64(NO_CPU, const double *src, union xmm_reg *dst, uint8_t type);
void vec_single_fcmp32(NO_CPU, const float *src, union xmm_reg *dst, uint8_t type);
void vec_fcmp_p64(NO_CPU, const union xmm_reg *src, union xmm_reg *dst, uint8_t type);

void vec_cvtsi2sd32(NO_CPU, const int32_t *src, double *dst);
#ifdef GUEST_X86_64
void vec_cvtsi2sdq64(NO_CPU, const int64_t *src, double *dst);
void vec_cvtsi2ssq64(NO_CPU, const int64_t *src, float *dst);
void vec_cvttsd2siq64(NO_CPU, const double *src, int64_t *dst);
void vec_cvttss2siq32(NO_CPU, const float *src, int64_t *dst);
void vec_cvtsd2siq64(struct cpu_state *cpu, const double *src, int64_t *dst);
void vec_cvtss2siq32(struct cpu_state *cpu, const float *src, int64_t *dst);
#endif
void vec_cvttsd2si64(NO_CPU, const double *src, int32_t *dst);
void vec_cvtsd2ss64(NO_CPU, const double *src, float *dst);
void vec_cvtsi2ss32(NO_CPU, const int32_t *src, float *dst);
void vec_cvttss2si32(NO_CPU, const float *src, int32_t *dst);
void vec_cvtss2sd32(NO_CPU, const float *src, double *dst);

void vec_cvttpd2dq64(NO_CPU, const union xmm_reg *src, union xmm_reg *dst);
void vec_cvttps2dq32(NO_CPU, const union xmm_reg *src, union xmm_reg *dst);

// TODO organize
void vec_packss_w128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst);
void vec_packsu_w128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst);
void vec_packss_d128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst);

void vec_unpackl_bw128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst);
void vec_unpackl_w128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst);
void vec_unpackl_dq128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst);
void vec_unpackl_dq64(NO_CPU, const union mm_reg *src, union mm_reg *dst);
void vec_unpackl_qdq128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst);
void vec_unpackl_ps128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst);
void vec_unpackl_pd128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst);
void vec_unpackh_bw128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst);
void vec_unpackh_w128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst);
void vec_unpackh_d128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst);
void vec_unpackh_dq128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst);
void vec_unpackh_ps128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst);
void vec_unpackh_pd128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst);

void vec_shuffle_w64(NO_CPU, const union mm_reg *src, union mm_reg *dst, uint8_t encoding);

void vec_shuffle_lw128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst, uint8_t encoding);
void vec_shuffle_hw128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst, uint8_t encoding);

void vec_shuffle_d128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst, uint8_t encoding);
void vec_shuffle_ps128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst, uint8_t encoding);
void vec_shuffle_pd128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst, uint8_t encoding);

void vec_compare_eqb64(NO_CPU, const union mm_reg *src, union mm_reg *dst);
void vec_compare_eqw64(NO_CPU, const union mm_reg *src, union mm_reg *dst);
void vec_compare_eqd64(NO_CPU, const union mm_reg *src, union mm_reg *dst);
void vec_compares_gtb64(NO_CPU, const union mm_reg *src, union mm_reg *dst);
void vec_compares_gtw64(NO_CPU, const union mm_reg *src, union mm_reg *dst);
void vec_compares_gtd64(NO_CPU, const union mm_reg *src, union mm_reg *dst);

void vec_compare_eqb128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst);
void vec_compare_eqw128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst);
void vec_compare_eqd128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst);
void vec_compares_gtb128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst);
void vec_compares_gtw128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst);
void vec_compares_gtd128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst);

void vec_movl_p64(NO_CPU, const uint64_t *src, union xmm_reg *dst);
void vec_movl_pm64(NO_CPU, const union xmm_reg *src, uint64_t *dst);
void vec_movh_p64(NO_CPU, const uint64_t *src, union xmm_reg *dst);
void vec_movh_pm64(NO_CPU, const union xmm_reg *src, uint64_t *dst);

void vec_movmask_b64(NO_CPU, const union mm_reg *src, uint32_t *dst);
void vec_movmask_b128(NO_CPU, const union xmm_reg *src, uint32_t *dst);
void vec_fmovmask_d128(NO_CPU, const union xmm_reg *src, uint32_t *dst);

void vec_div_p32(NO_CPU, const union xmm_reg *src, union xmm_reg *dst);
void vec_div_p64(NO_CPU, const union xmm_reg *src, union xmm_reg *dst);
void vec_min_p32(NO_CPU, const union xmm_reg *src, union xmm_reg *dst);
void vec_max_p32(NO_CPU, const union xmm_reg *src, union xmm_reg *dst);
void vec_min_p64(NO_CPU, const union xmm_reg *src, union xmm_reg *dst);
void vec_max_p64(NO_CPU, const union xmm_reg *src, union xmm_reg *dst);
void vec_sqrt_p32(NO_CPU, const union xmm_reg *src, union xmm_reg *dst);
void vec_sqrt_p64(NO_CPU, const union xmm_reg *src, union xmm_reg *dst);
void vec_rcp_p32(NO_CPU, const union xmm_reg *src, union xmm_reg *dst);
void vec_rsqrt_p32(NO_CPU, const union xmm_reg *src, union xmm_reg *dst);
void vec_single_rcp32(NO_CPU, const float *src, float *dst);
void vec_single_rsqrt32(NO_CPU, const float *src, float *dst);
void vec_fcmp_p32(NO_CPU, const union xmm_reg *src, union xmm_reg *dst, uint8_t type);
void vec_fmovmask_s128(NO_CPU, const union xmm_reg *src, uint32_t *dst);
void vec_movhl_ps128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst);
void vec_movlh_ps128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst);
void vec_cvtps2pd64(NO_CPU, const union xmm_reg *src, union xmm_reg *dst);
void vec_cvtpd2ps128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst);
void vec_cvtdq2ps128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst);
void vec_cvtdq2pd64(NO_CPU, const union xmm_reg *src, union xmm_reg *dst);
void vec_cvtps2dq128(struct cpu_state *cpu, const union xmm_reg *src, union xmm_reg *dst);
void vec_cvtpd2dq128(struct cpu_state *cpu, const union xmm_reg *src, union xmm_reg *dst);
void vec_cvtsd2si64(struct cpu_state *cpu, const double *src, int32_t *dst);
void vec_cvtss2si32(struct cpu_state *cpu, const float *src, int32_t *dst);
void sse_ldmxcsr32(struct cpu_state *cpu, const uint32_t *src);
void vec_pshufb128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst);
void vec_phaddw128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst);
void vec_phaddd128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst);
void vec_phaddsw128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst);
void vec_phsubw128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst);
void vec_phsubd128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst);
void vec_phsubsw128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst);
void vec_pmaddubsw128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst);
void vec_pmulhrsw128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst);
void vec_psignb128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst);
void vec_psignw128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst);
void vec_psignd128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst);
void vec_pabsb128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst);
void vec_pabsw128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst);
void vec_pabsd128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst);
void vec_palignr128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst, uint8_t imm);
void sse_stmxcsr32(struct cpu_state *cpu, uint32_t *dst);

void vec_insert_w64(NO_CPU, const uint32_t *src, union mm_reg *dst, uint8_t index);
void vec_insert_w128(NO_CPU, const uint32_t *src, union xmm_reg *dst, uint8_t index);
void vec_extract_w128(NO_CPU, const union xmm_reg *src, uint32_t *dst, uint8_t index);

void vec_avg_b128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst);
void vec_avg_w128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst);


// SSE3, SSE4.1, SSE4.2 (vec.c)
void vec_haddps128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst);
void vec_hsubps128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst);
void vec_haddpd128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst);
void vec_hsubpd128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst);
void vec_addsubps128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst);
void vec_addsubpd128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst);
void vec_movddup64(NO_CPU, const uint64_t *src, union xmm_reg *dst);
void vec_movsldup128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst);
void vec_movshdup128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst);
#define PMOVX_DECL(name, size) void vec_##name##size(NO_CPU, const void *src, union xmm_reg *dst);
PMOVX_DECL(pmovsxbw, 64) PMOVX_DECL(pmovsxbd, 32) PMOVX_DECL(pmovsxbq, 16)
PMOVX_DECL(pmovsxwd, 64) PMOVX_DECL(pmovsxwq, 32) PMOVX_DECL(pmovsxdq, 64)
PMOVX_DECL(pmovzxbw, 64) PMOVX_DECL(pmovzxbd, 32) PMOVX_DECL(pmovzxbq, 16)
PMOVX_DECL(pmovzxwd, 64) PMOVX_DECL(pmovzxwq, 32) PMOVX_DECL(pmovzxdq, 64)
#undef PMOVX_DECL
void vec_pminsb128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst);
void vec_pmaxsb128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst);
void vec_pminuw128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst);
void vec_pmaxuw128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst);
void vec_pminsd128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst);
void vec_pmaxsd128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst);
void vec_pminud128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst);
void vec_pmaxud128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst);
void vec_pmulld128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst);
void vec_pmuldq128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst);
void vec_pcmpeqq128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst);
void vec_pcmpgtq128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst);
void vec_packusdw128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst);
void vec_phminposuw128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst);
void vec_ptest128(struct cpu_state *cpu, const union xmm_reg *src, const union xmm_reg *dst);
void vec_blendps128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst, uint8_t imm);
void vec_blendpd128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst, uint8_t imm);
void vec_pblendw128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst, uint8_t imm);
void vec_pblendvb128(struct cpu_state *cpu, const union xmm_reg *src, union xmm_reg *dst);
void vec_blendvps128(struct cpu_state *cpu, const union xmm_reg *src, union xmm_reg *dst);
void vec_blendvpd128(struct cpu_state *cpu, const union xmm_reg *src, union xmm_reg *dst);
void vec_roundps128(struct cpu_state *cpu, const union xmm_reg *src, union xmm_reg *dst, uint8_t imm);
void vec_roundpd128(struct cpu_state *cpu, const union xmm_reg *src, union xmm_reg *dst, uint8_t imm);
void vec_roundss32(struct cpu_state *cpu, const float *src, union xmm_reg *dst, uint8_t imm);
void vec_roundsd64(struct cpu_state *cpu, const double *src, union xmm_reg *dst, uint8_t imm);
void vec_dpps128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst, uint8_t imm);
void vec_dppd128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst, uint8_t imm);
void vec_insertps128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst, uint8_t imm);
void vec_insertps32(NO_CPU, const uint32_t *src, union xmm_reg *dst, uint8_t imm);
void vec_mpsadbw128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst, uint8_t imm);
void vec_pextrb64(NO_CPU, const union xmm_reg *src, uint64_t *dst, uint8_t imm);
void vec_pextrb8(NO_CPU, const union xmm_reg *src, uint8_t *dst, uint8_t imm);
void vec_pextrw16(NO_CPU, const union xmm_reg *src, uint16_t *dst, uint8_t imm);
void vec_pextrw64(NO_CPU, const union xmm_reg *src, uint64_t *dst, uint8_t imm);
void vec_pextrd64(NO_CPU, const union xmm_reg *src, uint64_t *dst, uint8_t imm);
void vec_pextrd32(NO_CPU, const union xmm_reg *src, uint32_t *dst, uint8_t imm);
void vec_pextrqq64(NO_CPU, const union xmm_reg *src, uint64_t *dst, uint8_t imm);
void vec_pinsrb8(NO_CPU, const uint8_t *src, union xmm_reg *dst, uint8_t imm);
void vec_pinsrd32(NO_CPU, const uint32_t *src, union xmm_reg *dst, uint8_t imm);
void vec_pinsrq64(NO_CPU, const uint64_t *src, union xmm_reg *dst, uint8_t imm);
void vec_pcmpestri128(struct cpu_state *cpu, const union xmm_reg *src, const union xmm_reg *dst, uint8_t imm);
void vec_pcmpestrm128(struct cpu_state *cpu, const union xmm_reg *src, const union xmm_reg *dst, uint8_t imm);
void vec_pcmpistri128(struct cpu_state *cpu, const union xmm_reg *src, const union xmm_reg *dst, uint8_t imm);
void vec_pcmpistrm128(struct cpu_state *cpu, const union xmm_reg *src, const union xmm_reg *dst, uint8_t imm);
void vec_crc32_8(NO_CPU, const void *src, uint64_t *dst);
void vec_crc32_16(NO_CPU, const void *src, uint64_t *dst);
void vec_crc32_32(NO_CPU, const void *src, uint64_t *dst);
void vec_crc32_64(NO_CPU, const void *src, uint64_t *dst);

#endif
