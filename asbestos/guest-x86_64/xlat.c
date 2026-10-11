// Instruction translator for the x86_64 guest (xjit.c includes this file).
//
// Guest and host share an instruction set, so most guest instructions are
// re-encoded with their operands mapped to where the gadgets keep the guest
// state: rax..rdi (but rsp) in the same host registers, guest rsp in r8,
// guest r8-r15 in cpu slots (loaded into r14/r15 around the instruction), and
// a memory operand's address computed into r13 and translated through the
// TLB inline. Flags stay in the host's eflags: a liveness pass decides where
// they have to be written back to the lazy representation the gadgets use
// (cpu->cf/of/eflags/flags_res) and where they must survive the TLB lookup.
//
// An instruction the translator doesn't handle runs as copies of its gadget
// templates (as xjit does for the i386 guest) or, failing that, in the gadget
// stream itself: a run of native code starts at an instruction's first
// stream slot and hands over with _ip pointing back into the stream.
//
// ISH_XJIT_STATS=1 also counts how many instructions ran which way, and
// which opcodes fell back most.

#include <stddef.h>
#include "emu/tlb.h"
#include "emu/cpu.h"
#include "asbestos/gen.h"

#if defined(__x86_64__)
#include <cpuid.h>
#endif

// ---------------------------------------------------------------- log

// Per instruction: guest address, bytes and the range of stream slots its
// gadgets occupy.
struct xinsn {
    uint64_t ip;
    uint32_t s0, s1;
    uint8_t len;
    bool bad;   // its bytes couldn't be read, or gen made it an interrupt
    uint8_t bytes[15];
};

static __thread struct xinsn *xlog;
static __thread unsigned xlog_n, xlog_cap;
static __thread bool xlog_ok;
static __thread uint32_t xlog_cur_s0, xlog_size;
static __thread uint64_t xlog_cur_ip;

void xjit_insn_begin(struct gen_state *state) {
    if (state->size == 0) {
        xlog_n = 0;
        xlog_ok = true;
    }
    xlog_cur_s0 = state->size;
    xlog_cur_ip = state->ip;
}

void xjit_insn_end(struct gen_state *state, struct tlb *tlb) {
    if (!xlog_ok)
        return;
    if (xlog_n == xlog_cap) {
        unsigned cap = xlog_cap ? xlog_cap * 2 : 256;
        struct xinsn *bigger = realloc(xlog, cap * sizeof(*xlog));
        if (bigger == NULL) {
            xlog_ok = false;
            return;
        }
        xlog = bigger;
        xlog_cap = cap;
    }
    struct xinsn *in = &xlog[xlog_n++];
    in->ip = xlog_cur_ip;
    in->s0 = xlog_cur_s0;
    in->s1 = state->size;
    uint64_t len = state->ip - xlog_cur_ip;
    in->bad = len == 0 || len > 15;
    in->len = in->bad ? 0 : (uint8_t) len;
    if (!in->bad && !tlb_read(tlb, in->ip, in->bytes, in->len))
        in->bad = true;
    extern void gadget_interrupt(void);
    if (in->s1 > in->s0 && state->block->code[in->s0] == (uintptr_t) gadget_interrupt)
        in->bad = true;
}

void xjit_insns_done(struct gen_state *state) {
    xlog_size = state->size;
}

// ---------------------------------------------------------------- flags

#define F_CF 0x1
#define F_PF 0x4
#define F_AF 0x10
#define F_ZF 0x40
#define F_SF 0x80
#define F_OF 0x800
#define F_ALL (F_CF | F_PF | F_AF | F_ZF | F_SF | F_OF)

static uint16_t cond_flags(unsigned cc) {
    switch (cc >> 1) {
        case 0: return F_OF;
        case 1: return F_CF;
        case 2: return F_ZF;
        case 3: return F_CF | F_ZF;
        case 4: return F_SF;
        case 5: return F_PF;
        case 6: return F_SF | F_OF;
        default: return F_ZF | F_SF | F_OF;
    }
}

// ---------------------------------------------------------------- emitter

enum { SEC_MAIN, SEC_COLD };

struct xbuf {
    uint8_t *base, *p, *end;
};

struct xfix {
    uint8_t sec;
    uint8_t kind;      // FIX_ABS: rel32 to an absolute address; FIX_LABEL: to a label
    uint32_t off;      // of the rel32 field
    uintptr_t target;  // address or label number
};
enum { FIX_ABS, FIX_LABEL };

struct xlabel {
    uint8_t sec;
    uint32_t off;
};

struct xasm {
    struct xbuf sec[2];
    uint8_t cur;
    bool overflow;
    struct xfix *fix;
    unsigned nfix, capfix;
    struct xlabel *lab;
    unsigned nlab, caplab;
};

#define XBUF_SIZE (1u << 20)

static __thread struct xasm xa_tls;

static bool xa_init(struct xasm *a) {
    for (int s = 0; s < 2; s++) {
        if (a->sec[s].base == NULL) {
            a->sec[s].base = malloc(XBUF_SIZE);
            if (a->sec[s].base == NULL)
                return false;
            a->sec[s].end = a->sec[s].base + XBUF_SIZE;
        }
        a->sec[s].p = a->sec[s].base;
    }
    a->cur = SEC_MAIN;
    a->overflow = false;
    a->nfix = a->nlab = 0;
    return true;
}

static inline struct xbuf *xb(struct xasm *a) { return &a->sec[a->cur]; }
static inline uint32_t xpos(struct xasm *a) { return (uint32_t) (xb(a)->p - xb(a)->base); }

static inline void b1(struct xasm *a, uint8_t v) {
    struct xbuf *b = xb(a);
    if (b->p < b->end)
        *b->p++ = v;
    else
        a->overflow = true;
}
static inline void b4(struct xasm *a, uint32_t v) {
    for (int i = 0; i < 4; i++)
        b1(a, (uint8_t) (v >> (i * 8)));
}
static inline void b8(struct xasm *a, uint64_t v) {
    for (int i = 0; i < 8; i++)
        b1(a, (uint8_t) (v >> (i * 8)));
}
static inline void bn(struct xasm *a, const uint8_t *p, unsigned n) {
    for (unsigned i = 0; i < n; i++)
        b1(a, p[i]);
}

static void add_fix(struct xasm *a, uint8_t kind, uintptr_t target) {
    if (a->nfix == a->capfix) {
        unsigned cap = a->capfix ? a->capfix * 2 : 256;
        struct xfix *bigger = realloc(a->fix, cap * sizeof(*a->fix));
        if (bigger == NULL) {
            a->overflow = true;
            return;
        }
        a->fix = bigger;
        a->capfix = cap;
    }
    a->fix[a->nfix++] = (struct xfix) {a->cur, kind, xpos(a), target};
}

static unsigned new_label(struct xasm *a) {
    if (a->nlab == a->caplab) {
        unsigned cap = a->caplab ? a->caplab * 2 : 256;
        struct xlabel *bigger = realloc(a->lab, cap * sizeof(*a->lab));
        if (bigger == NULL) {
            a->overflow = true;
            return 0;
        }
        a->lab = bigger;
        a->caplab = cap;
    }
    a->lab[a->nlab] = (struct xlabel) {0xff, 0};
    return a->nlab++;
}
static void lbind(struct xasm *a, unsigned l) {
    if (l < a->nlab)
        a->lab[l] = (struct xlabel) {a->cur, xpos(a)};
}

// rel32 fields
static void rel_abs(struct xasm *a, const void *target) {
    add_fix(a, FIX_ABS, (uintptr_t) target);
    b4(a, 0);
}
static void rel_lab(struct xasm *a, unsigned l) {
    add_fix(a, FIX_LABEL, l);
    b4(a, 0);
}

enum { RAX, RCX, RDX, RBX, RSP, RBP, RSI, RDI, R8, R9, R10, R11, R12, R13, R14, R15 };
#define H_CPU R11
#define H_TLB R12
#define H_ADDR R13
#define H_IP R9

static inline uint8_t modrm_b(unsigned mod, unsigned reg, unsigned rm) {
    return (uint8_t) (mod << 6 | (reg & 7) << 3 | (rm & 7));
}

// REX for registers reg (modrm.reg), index (sib.index), base (modrm.rm or
// sib.base); -1 for none. force: emit 0x40 even with no bits set.
static void e_rex(struct xasm *a, bool w, int reg, int index, int base, bool force) {
    uint8_t v = 0x40 | (w ? 8 : 0);
    if (reg >= 8) v |= 4;
    if (index >= 8) v |= 2;
    if (base >= 8) v |= 1;
    if (v != 0x40 || force)
        b1(a, v);
}

// a memory operand: [base + index*scale + disp], base -1: none
struct xmem {
    int base, index, scale;
    int32_t disp;
};
#define XMEM(b, d) ((struct xmem) {(b), -1, 0, (d)})

static void e_modrm_mem(struct xasm *a, int reg, struct xmem m) {
    if (m.base < 0) {
        b1(a, modrm_b(0, reg, 4));
        b1(a, (uint8_t) (m.scale << 6 | ((m.index < 0 ? 4 : m.index) & 7) << 3 | 5));
        b4(a, (uint32_t) m.disp);
        return;
    }
    unsigned mod = (m.disp == 0 && (m.base & 7) != 5) ? 0 :
        (m.disp >= -128 && m.disp <= 127) ? 1 : 2;
    if (m.index >= 0 || (m.base & 7) == 4) {
        b1(a, modrm_b(mod, reg, 4));
        b1(a, (uint8_t) (m.scale << 6 | ((m.index < 0 ? 4 : m.index) & 7) << 3 | (m.base & 7)));
    } else {
        b1(a, modrm_b(mod, reg, m.base));
    }
    if (mod == 1)
        b1(a, (uint8_t) m.disp);
    else if (mod == 2)
        b4(a, (uint32_t) m.disp);
}

// op reg, [mem] (or op [mem], reg, as the opcode says)
static void e_rm(struct xasm *a, bool w, uint8_t op, int reg, struct xmem m) {
    e_rex(a, w, reg, m.index, m.base, false);
    b1(a, op);
    e_modrm_mem(a, reg, m);
}
static void e_rm2(struct xasm *a, bool w, uint8_t op1, uint8_t op2, int reg, struct xmem m) {
    e_rex(a, w, reg, m.index, m.base, false);
    b1(a, op1);
    b1(a, op2);
    e_modrm_mem(a, reg, m);
}
// op rm_reg, reg (register forms)
static void e_rr(struct xasm *a, bool w, uint8_t op, int reg, int rm) {
    e_rex(a, w, reg, -1, rm, false);
    b1(a, op);
    b1(a, modrm_b(3, reg, rm));
}

static void e_mov_load(struct xasm *a, int dst, struct xmem m) { e_rm(a, true, 0x8b, dst, m); }
static void e_mov_store(struct xasm *a, struct xmem m, int src) { e_rm(a, true, 0x89, src, m); }
static void e_mov_rr(struct xasm *a, int dst, int src) { e_rr(a, true, 0x89, src, dst); }
static void e_lea(struct xasm *a, int dst, struct xmem m) { e_rm(a, true, 0x8d, dst, m); }

static void e_movabs(struct xasm *a, int dst, uint64_t imm) {
    if ((int64_t) imm == (int32_t) imm) {
        // movq $imm32, dst (sign-extended)
        e_rex(a, true, -1, -1, dst, false);
        b1(a, 0xc7);
        b1(a, modrm_b(3, 0, dst));
        b4(a, (uint32_t) imm);
        return;
    }
    e_rex(a, true, -1, -1, dst, false);
    b1(a, (uint8_t) (0xb8 + (dst & 7)));
    b8(a, imm);
}

static void e_jmp_lab(struct xasm *a, unsigned l) { b1(a, 0xe9); rel_lab(a, l); }
static void e_jmp_abs(struct xasm *a, const void *t) { b1(a, 0xe9); rel_abs(a, t); }
static void e_call_abs(struct xasm *a, const void *t) { b1(a, 0xe8); rel_abs(a, t); }
static void e_jcc_lab(struct xasm *a, unsigned cc, unsigned l) { b1(a, 0x0f); b1(a, (uint8_t) (0x80 + cc)); rel_lab(a, l); }

// ---------------------------------------------------------------- runtime

// Gadget-engine entry points, by their assembler names
extern void handle_read_miss(void) __asm__("handle_read_miss");
extern void handle_write_miss(void) __asm__("handle_write_miss");
extern void crosspage_load(void) __asm__("crosspage_load");
extern void crosspage_store(void) __asm__("crosspage_store");
extern void fiber_ret_chain(void) __asm__("fiber_ret_chain");

#define CPU_OFF(f) ((int32_t) offsetof(struct cpu_state, f))
#define TLBREL(f) ((int32_t) offsetof(struct tlb, f) - (int32_t) offsetof(struct tlb, entries))
#define TLBENT(f) ((int32_t) offsetof(struct tlb_entry, f))

static bool have_bmi2;
static uint8_t *load_flags_stub;  // lazy flags -> host eflags; clobbers r14, r15

// ---------------------------------------------------------------- flag code

// Writes the flags in mask from the host's eflags to the lazy representation.
// Clobbers r14, r15 and eflags.
static void e_sync_flags(struct xasm *a, uint16_t mask) {
    if (mask == 0)
        return;
    b1(a, 0x9c);                         // pushfq
    b1(a, 0x41); b1(a, 0x5e);            // pop %r14
    // eflags bits (CF and OF too, harmless: collapse_flags rewrites them)
    // mov %r14d, %r15d; and $mask, %r15d
    e_rr(a, false, 0x89, R14, R15);
    e_rex(a, false, -1, -1, R15, false); b1(a, 0x81); b1(a, modrm_b(3, 4, R15)); b4(a, mask);
    // andl $~mask, eflags(%r11); orl %r15d, eflags(%r11)
    e_rm(a, false, 0x81, 4, XMEM(H_CPU, CPU_OFF(eflags))); b4(a, (uint32_t) ~mask);
    e_rm(a, false, 0x09, R15, XMEM(H_CPU, CPU_OFF(eflags)));
    uint8_t res = 0;
    if (mask & F_PF) res |= PF_RES;
    if (mask & F_ZF) res |= ZF_RES;
    if (mask & F_SF) res |= SF_RES;
    if (mask & F_AF) res |= AF_OPS;
    if (res) {
        // andb $~res, flags_res(%r11)
        e_rm(a, false, 0x80, 4, XMEM(H_CPU, CPU_OFF(flags_res))); b1(a, (uint8_t) ~res);
    }
    if (mask & F_CF) {
        // bt $0, %r14d; setc cf(%r11)
        e_rex(a, false, -1, -1, R14, false); b1(a, 0x0f); b1(a, 0xba); b1(a, modrm_b(3, 4, R14)); b1(a, 0);
        e_rm2(a, false, 0x0f, 0x92, 0, XMEM(H_CPU, CPU_OFF(cf)));
    }
    if (mask & F_OF) {
        e_rex(a, false, -1, -1, R14, false); b1(a, 0x0f); b1(a, 0xba); b1(a, modrm_b(3, 4, R14)); b1(a, 11);
        e_rm2(a, false, 0x0f, 0x92, 0, XMEM(H_CPU, CPU_OFF(of)));
    }
}

// Keeps the arithmetic flags in r10 across code that clobbers them (lahf
// form: ah = SF ZF AF PF CF, al = OF). Uses r14.
static void e_save_flags(struct xasm *a) {
    e_mov_rr(a, R14, RAX);
    b1(a, 0x9f);                                   // lahf
    b1(a, 0x0f); b1(a, 0x90); b1(a, 0xc0);         // seto %al
    e_mov_rr(a, R10, RAX);
    e_mov_rr(a, RAX, R14);
}
static void e_restore_flags(struct xasm *a) {
    e_mov_rr(a, R14, RAX);
    e_mov_rr(a, RAX, R10);
    b1(a, 0x04); b1(a, 0x7f);                      // add $0x7f, %al: OF
    b1(a, 0x9e);                                   // sahf
    e_mov_rr(a, RAX, R14);
}

// The lazy flags into the host's eflags. A shared routine: it ends in popfq
// and ret, which leaves the flags alone. Clobbers r14, r15.
static void e_load_flags_stub(struct xasm *a) {
    b1(a, 0x50);                       // push %rax
    b1(a, 0x51);                       // push %rcx
    // r14d = the stored eflags' status bits; rcx = which come from res
    e_rm(a, false, 0x8b, R14, XMEM(H_CPU, CPU_OFF(eflags)));
    // movzbl flags_res, %eax
    e_rm2(a, false, 0x0f, 0xb6, RAX, XMEM(H_CPU, CPU_OFF(flags_res)));
    // ecx = ((eax & PF_RES) << 2) | ((eax & (ZF_RES|SF_RES)) << 5)
    b1(a, 0x89); b1(a, modrm_b(3, RAX, RCX));                 // mov %eax, %ecx
    b1(a, 0x83); b1(a, modrm_b(3, 4, RCX)); b1(a, PF_RES);    // and $1, %ecx
    b1(a, 0xc1); b1(a, modrm_b(3, 4, RCX)); b1(a, 2);         // shl $2, %ecx
    b1(a, 0x83); b1(a, modrm_b(3, 4, RAX)); b1(a, ZF_RES | SF_RES | AF_OPS); // and $14, %eax
    // AF_OPS (8) << 1 = 0x10 = AF; ZF_RES (2) << 5 = 0x40; SF_RES (4) << 5 = 0x80
    e_rex(a, false, R15, -1, -1, false); b1(a, 0x8d);
    b1(a, modrm_b(0, R15, 4)); b1(a, (uint8_t) (1 << 6 | RAX << 3 | 5)); b4(a, 0); // lea 0(,%rax,2), %r15d
    e_rex(a, false, -1, -1, R15, false); b1(a, 0x83); b1(a, modrm_b(3, 4, R15)); b1(a, F_AF); // and $0x10, %r15d
    e_rr(a, false, 0x09, R15, RCX);                            // or %r15d, %ecx
    b1(a, 0xc1); b1(a, modrm_b(3, 4, RAX)); b1(a, 5);         // shl $5, %eax
    b1(a, 0x25); b4(a, F_ZF | F_SF);                           // and $0xc0, %eax
    b1(a, 0x09); b1(a, modrm_b(3, RAX, RCX));                 // or %eax, %ecx
    // eax = flags of res: test res; pushfq; pop
    e_mov_load(a, R15, XMEM(H_CPU, CPU_OFF(res)));
    e_rr(a, true, 0x85, R15, R15);                             // test %r15, %r15
    b1(a, 0x9c); b1(a, 0x58);                                  // pushfq; pop %rax
    // AF from op1 ^ op2 ^ res, bit 4
    e_rm(a, true, 0x33, R15, XMEM(H_CPU, CPU_OFF(op1)));       // xor op1, %r15
    e_rm(a, true, 0x33, R15, XMEM(H_CPU, CPU_OFF(op2)));       // xor op2, %r15
    e_rex(a, false, -1, -1, R15, false); b1(a, 0x83); b1(a, modrm_b(3, 4, R15)); b1(a, F_AF);
    b1(a, 0x25); b4(a, F_ZF | F_SF | F_PF);                    // and $0xc4, %eax
    e_rr(a, false, 0x09, R15, RAX);                            // or %r15d, %eax: res-derived ZF SF PF AF
    // r14 = (r14 & ~rcx) | (eax & rcx)
    e_rr(a, false, 0x21, RCX, RAX);                            // and %ecx, %eax
    b1(a, 0xf7); b1(a, modrm_b(3, 2, RCX));                    // not %ecx
    e_rr(a, false, 0x21, RCX, R14);                            // and %ecx, %r14d
    e_rr(a, false, 0x09, RAX, R14);                            // or %eax, %r14d
    e_rex(a, false, -1, -1, R14, false); b1(a, 0x81); b1(a, modrm_b(3, 4, R14));
    b4(a, F_PF | F_AF | F_ZF | F_SF);                          // keep those four
    // CF, OF from their bytes
    e_rm2(a, false, 0x0f, 0xb6, RAX, XMEM(H_CPU, CPU_OFF(cf)));
    b1(a, 0x83); b1(a, modrm_b(3, 4, RAX)); b1(a, 1);
    e_rr(a, false, 0x09, RAX, R14);
    e_rm2(a, false, 0x0f, 0xb6, RAX, XMEM(H_CPU, CPU_OFF(of)));
    b1(a, 0x83); b1(a, modrm_b(3, 4, RAX)); b1(a, 1);
    b1(a, 0xc1); b1(a, modrm_b(3, 4, RAX)); b1(a, 11);
    e_rr(a, false, 0x09, RAX, R14);
    // the host's other flags (DF, IF...) as they are
    b1(a, 0x9c); b1(a, 0x58);                                  // pushfq; pop %rax
    b1(a, 0x25); b4(a, (uint32_t) ~F_ALL);
    e_rr(a, false, 0x09, R14, RAX);                            // or %r14d, %eax
    b1(a, 0x59);                                               // pop %rcx
    // swap the image into place of the saved rax and popfq it
    b1(a, 0x48); b1(a, 0x87); b1(a, 0x04); b1(a, 0x24);       // xchg %rax, (%rsp)
    b1(a, 0x9d);                                               // popfq
    b1(a, 0xc3);                                               // ret
}

// ---------------------------------------------------------------- decoding

struct gi {
    uint64_t ip, next;
    uint8_t len;
    bool p66, pf2, pf3, lock, a32;
    uint8_t seg;       // 0, 1 fs, 2 gs
    uint8_t rex;
    uint8_t map;       // 0: one byte, 1: 0f, 2: 0f38, 3: 0f3a
    uint8_t op;
    uint8_t opoff;     // where the opcode (after prefixes and REX) starts
    bool has_modrm;
    uint8_t mod, reg, rm;   // reg/rm with the REX bits
    uint8_t rawreg;         // modrm.reg without REX.R (the /digit)
    // memory operand
    int base, index, scale;  // base 16: rip; -1: none
    int64_t disp;
    uint8_t immoff, immlen;
};

#define BASE_RIP 16

// The operand layout of an opcode, for the decoder
enum { IMM_NONE, IMM_B, IMM_Z, IMM_W, IMM_V };

static bool gdecode(const struct xinsn *x, struct gi *g) {
    const uint8_t *p = x->bytes;
    unsigned n = x->len, i = 0;
    memset(g, 0, sizeof(*g));
    g->ip = x->ip;
    g->next = x->ip + x->len;
    g->len = x->len;
    g->base = g->index = -1;
    for (;; i++) {
        if (i >= n) return false;
        uint8_t b = p[i];
        if (b == 0x66) g->p66 = true;
        else if (b == 0xf2) { g->pf2 = true; g->pf3 = false; }
        else if (b == 0xf3) { g->pf3 = true; g->pf2 = false; }
        else if (b == 0xf0) g->lock = true;
        else if (b == 0x67) g->a32 = true;
        else if (b == 0x64) g->seg = 1;
        else if (b == 0x65) g->seg = 2;
        else if (b == 0x2e || b == 0x3e || b == 0x26 || b == 0x36) ;
        else if ((b & 0xf0) == 0x40) {
            // REX counts only right before the opcode
            if (i + 1 >= n) return false;
            uint8_t nb = p[i + 1];
            if ((nb & 0xf0) == 0x40 || nb == 0x66 || nb == 0xf2 || nb == 0xf3 || nb == 0xf0 ||
                    nb == 0x67 || nb == 0x64 || nb == 0x65 || nb == 0x2e || nb == 0x3e ||
                    nb == 0x26 || nb == 0x36)
                continue;
            g->rex = b;
            i++;
            break;
        } else break;
    }
    g->opoff = (uint8_t) i;
    if (i >= n) return false;
    uint8_t op = p[i++];
    unsigned imm = IMM_NONE;
    bool modrm = false;
    if (op == 0x0f) {
        if (i >= n) return false;
        op = p[i++];
        g->map = 1;
        if (op == 0x38 || op == 0x3a) {
            g->map = op == 0x38 ? 2 : 3;
            if (i >= n) return false;
            op = p[i++];
            modrm = true;
            if (g->map == 3) imm = IMM_B;
        } else if ((op >= 0x80 && op <= 0x8f)) {
            imm = IMM_Z;  // rel32 (always 32 bits in long mode)
        } else if (op == 0x05 || op == 0x0b || op == 0x31 || op == 0xa2 || op == 0x77 ||
                   (op >= 0xc8 && op <= 0xcf) || op == 0xa0 || op == 0xa1 || op == 0xa8 || op == 0xa9) {
            // no modrm
        } else if (op == 0x70 || (op >= 0x71 && op <= 0x73) || op == 0xa4 || op == 0xac ||
                   op == 0xba || op == 0xc2 || op == 0xc4 || op == 0xc5 || op == 0xc6) {
            modrm = true;
            imm = IMM_B;
        } else {
            modrm = true;
        }
    } else {
        g->map = 0;
        if (op < 0x40) {
            unsigned lo = op & 7;
            if (lo <= 3) modrm = true;
            else if (lo == 4) imm = IMM_B;
            else if (lo == 5) imm = IMM_Z;
        } else if (op == 0x63 || (op >= 0x84 && op <= 0x8f) || (op >= 0xd0 && op <= 0xd3) ||
                   (op >= 0xd8 && op <= 0xdf) || op == 0xfe || op == 0xff || op == 0x62) {
            modrm = true;
        } else if (op == 0x68) imm = IMM_Z;
        else if (op == 0x69) { modrm = true; imm = IMM_Z; }
        else if (op == 0x6a) imm = IMM_B;
        else if (op == 0x6b) { modrm = true; imm = IMM_B; }
        else if (op >= 0x70 && op <= 0x7f) imm = IMM_B;
        else if (op == 0x80 || op == 0x82 || op == 0x83 || op == 0xc0 || op == 0xc1 || op == 0xc6) { modrm = true; imm = IMM_B; }
        else if (op == 0x81 || op == 0xc7) { modrm = true; imm = IMM_Z; }
        else if (op == 0xa8) imm = IMM_B;
        else if (op == 0xa9) imm = IMM_Z;
        else if (op >= 0xb0 && op <= 0xb7) imm = IMM_B;
        else if (op >= 0xb8 && op <= 0xbf) imm = IMM_V;
        else if (op == 0xc2) imm = IMM_W;
        else if (op == 0xcd || op == 0xeb || (op >= 0xe0 && op <= 0xe3)) imm = IMM_B;
        else if (op == 0xe8 || op == 0xe9) imm = IMM_Z;
        else if (op == 0xf6 || op == 0xf7) {
            modrm = true;
            if (i < n && ((p[i] >> 3) & 7) <= 1)
                imm = op == 0xf6 ? IMM_B : IMM_Z;
        }
        else if (op == 0xc8) imm = IMM_NONE;  // enter: unsupported below
    }
    g->op = op;
    g->has_modrm = modrm;
    if (modrm) {
        if (i >= n) return false;
        uint8_t m = p[i++];
        g->mod = m >> 6;
        g->rawreg = (m >> 3) & 7;
        g->reg = (uint8_t) (g->rawreg | ((g->rex & 4) ? 8 : 0));
        unsigned rm = m & 7;
        if (g->mod == 3) {
            g->rm = (uint8_t) (rm | ((g->rex & 1) ? 8 : 0));
        } else {
            unsigned dispsz = g->mod == 1 ? 1 : g->mod == 2 ? 4 : 0;
            if (rm == 4) {
                if (i >= n) return false;
                uint8_t sib = p[i++];
                g->scale = sib >> 6;
                unsigned idx = ((sib >> 3) & 7) | ((g->rex & 2) ? 8 : 0);
                g->index = idx == 4 ? -1 : (int) idx;
                unsigned base = sib & 7;
                if (base == 5 && g->mod == 0) {
                    g->base = -1;
                    dispsz = 4;
                } else {
                    g->base = (int) (base | ((g->rex & 1) ? 8 : 0));
                }
            } else if (rm == 5 && g->mod == 0) {
                g->base = BASE_RIP;
                dispsz = 4;
            } else {
                g->base = (int) (rm | ((g->rex & 1) ? 8 : 0));
            }
            if (i + dispsz > n) return false;
            if (dispsz == 1) g->disp = (int8_t) p[i];
            else if (dispsz == 4) { int32_t d; memcpy(&d, &p[i], 4); g->disp = d; }
            i += dispsz;
        }
    }
    unsigned isz = 0;
    switch (imm) {
        case IMM_B: isz = 1; break;
        case IMM_W: isz = 2; break;
        case IMM_Z: isz = g->p66 && !(g->rex & 8) && !(g->map == 1 && op >= 0x80 && op <= 0x8f) &&
                         op != 0xe8 && op != 0xe9 ? 2 : 4; break;
        case IMM_V: isz = (g->rex & 8) ? 8 : g->p66 ? 2 : 4; break;
    }
    g->immoff = (uint8_t) i;
    g->immlen = (uint8_t) isz;
    i += isz;
    if (i != n)
        return false;
    if (g->base == BASE_RIP)
        g->disp += (int64_t) g->next;  // the absolute address
    return true;
}

// ---------------------------------------------------------------- classify

enum {
    K_NONE,      // not translated
    K_GENERIC,   // modrm instruction: re-encode with operands mapped
    K_IMPLICIT,  // no modrm, implicit registers only (rax, rcx, rdx): copy
    K_NOP,       // emits nothing
    K_LEA,
    K_OPREG,     // register in the opcode: mov imm, xchg rax, bswap
    K_PUSH, K_POP, K_PUSHI, K_LEAVE,
    K_JCC, K_JMP,
};
enum { A_NONE, A_R, A_W, A_RW };

struct cls {
    uint8_t kind;
    uint8_t acc;       // what the memory operand is used for
    uint8_t msize;     // its size in bytes
    bool reg_gpr;      // modrm.reg names a general register
    bool reg_byte;     // ... at 8 bits
    bool rm_byte;      // the r/m operand is 8 bits
    uint16_t fr, fw;   // flags read / written
    uint8_t cc;        // jcc
};

static unsigned osize(const struct gi *g) {
    return (g->rex & 8) ? 8 : g->p66 ? 2 : 4;
}

static bool classify(const struct gi *g, struct cls *c) {
    memset(c, 0, sizeof(*c));
    unsigned op = g->op;
    unsigned os = osize(g);
    // prefixes this translator passes through only where it knows them
    if (g->a32)
        return false;
    bool reps = g->pf2 || g->pf3;

    if (g->map == 0) {
        if (op < 0x40 && (op & 7) <= 5) {
            unsigned alu = op >> 3;
            if (reps) return false;
            c->fw = F_ALL;
            c->fr = (alu == 2 || alu == 3) ? F_CF : 0;
            bool cmp = alu == 7;
            switch (op & 7) {
                case 0: case 1:
                    c->kind = K_GENERIC; c->reg_gpr = true;
                    c->acc = cmp ? A_R : A_RW;
                    c->reg_byte = c->rm_byte = (op & 1) == 0;
                    c->msize = (op & 1) ? os : 1;
                    break;
                case 2: case 3:
                    c->kind = K_GENERIC; c->reg_gpr = true; c->acc = A_R;
                    c->reg_byte = c->rm_byte = (op & 1) == 0;
                    c->msize = (op & 1) ? os : 1;
                    break;
                default:
                    c->kind = K_IMPLICIT;
            }
            if (g->lock && (c->kind != K_GENERIC || g->mod == 3 || cmp || (op & 7) >= 2))
                return false;
            return true;
        }
        if (g->lock)
            return false;
        if (op >= 0x50 && op <= 0x57) { c->kind = K_PUSH; return !reps && !g->p66; }
        if (op >= 0x58 && op <= 0x5f) { c->kind = K_POP; return !reps && !g->p66; }
        if (op == 0x68 || op == 0x6a) { c->kind = K_PUSHI; return !reps && !g->p66; }
        if (reps && !(op >= 0x70 && op <= 0x7f) && op != 0xeb && op != 0xe9)
            return false;
        switch (op) {
            case 0x63:
                c->kind = K_GENERIC; c->reg_gpr = true; c->acc = A_R;
                c->msize = os == 2 ? 2 : 4;
                return true;
            case 0x69: case 0x6b:
                c->kind = K_GENERIC; c->reg_gpr = true; c->acc = A_R; c->msize = os;
                c->fw = F_ALL;
                return true;
            case 0x80: case 0x81: case 0x83: {
                unsigned alu = g->rawreg;
                c->kind = K_GENERIC;
                c->acc = alu == 7 ? A_R : A_RW;
                c->rm_byte = op == 0x80;
                c->msize = op == 0x80 ? 1 : os;
                c->fw = F_ALL;
                c->fr = (alu == 2 || alu == 3) ? F_CF : 0;
                return true;
            }
            case 0x84: case 0x85:
                c->kind = K_GENERIC; c->reg_gpr = true; c->acc = A_R;
                c->reg_byte = c->rm_byte = op == 0x84;
                c->msize = op == 0x84 ? 1 : os;
                c->fw = F_ALL;
                return true;
            case 0x86: case 0x87:
                c->kind = K_GENERIC; c->reg_gpr = true; c->acc = A_RW;
                c->reg_byte = c->rm_byte = op == 0x86;
                c->msize = op == 0x86 ? 1 : os;
                return true;
            case 0x88: case 0x89: case 0x8a: case 0x8b:
                c->kind = K_GENERIC; c->reg_gpr = true;
                c->acc = op <= 0x89 ? A_W : A_R;
                c->reg_byte = c->rm_byte = (op & 1) == 0;
                c->msize = (op & 1) ? os : 1;
                return true;
            case 0x8d:
                if (g->mod == 3) return false;
                c->kind = K_LEA;
                return true;
            case 0x90:
                if (g->rex & 1) { c->kind = K_OPREG; return true; }  // xchg r8, rax
                c->kind = K_NOP;
                return true;
            case 0x91: case 0x92: case 0x93: case 0x94: case 0x95: case 0x96: case 0x97:
                c->kind = K_OPREG;
                return true;
            case 0x98: case 0x99:
                c->kind = K_IMPLICIT;
                return true;
            case 0xa8: case 0xa9:
                c->kind = K_IMPLICIT; c->fw = F_ALL;
                return true;
            case 0xb0: case 0xb1: case 0xb2: case 0xb3: case 0xb4: case 0xb5: case 0xb6: case 0xb7:
            case 0xb8: case 0xb9: case 0xba: case 0xbb: case 0xbc: case 0xbd: case 0xbe: case 0xbf:
                c->kind = K_OPREG;
                return true;
            case 0xc0: case 0xc1: case 0xd0: case 0xd1: case 0xd2: case 0xd3: {
                unsigned kind = g->rawreg;
                if (kind == 6) return false;  // sal alias: leave it to the gadgets
                c->kind = K_GENERIC; c->acc = A_RW;
                c->rm_byte = (op & 1) == 0;
                c->msize = (op & 1) ? os : 1;
                // by %cl it may write nothing, so it needs them all valid;
                // by a constant, shift_flags() knows
                c->fr = F_ALL;
                c->fw = F_ALL;
                return true;
            }
            case 0xc6: case 0xc7:
                if (g->rawreg != 0) return false;
                c->kind = K_GENERIC; c->acc = A_W;
                c->rm_byte = op == 0xc6;
                c->msize = op == 0xc6 ? 1 : os;
                return true;
            case 0xc9:
                c->kind = K_LEAVE;
                return !g->p66;
            case 0xf5: c->kind = K_IMPLICIT; c->fr = F_CF; c->fw = F_CF; return true;
            case 0xf8: case 0xf9: c->kind = K_IMPLICIT; c->fw = F_CF; return true;
            case 0xf6: case 0xf7: {
                unsigned kind = g->rawreg;
                c->kind = K_GENERIC;
                c->rm_byte = op == 0xf6;
                c->msize = op == 0xf6 ? 1 : os;
                switch (kind) {
                    case 0: case 1: c->acc = A_R; c->fw = F_ALL; return true;   // test
                    case 2: c->acc = A_RW; return true;                         // not
                    case 3: c->acc = A_RW; c->fw = F_ALL; return true;          // neg
                    case 4: case 5: c->acc = A_R; c->fw = F_ALL; return true;   // mul, imul
                    default: return false;                                      // div: #DE
                }
            }
            case 0xfe: case 0xff:
                if (g->rawreg > 1) return false;
                c->kind = K_GENERIC; c->acc = A_RW;
                c->rm_byte = op == 0xfe;
                c->msize = op == 0xfe ? 1 : os;
                c->fw = F_ALL & ~F_CF;
                return true;
            case 0x70: case 0x71: case 0x72: case 0x73: case 0x74: case 0x75: case 0x76: case 0x77:
            case 0x78: case 0x79: case 0x7a: case 0x7b: case 0x7c: case 0x7d: case 0x7e: case 0x7f:
                c->kind = K_JCC; c->cc = (uint8_t) (op & 15); c->fr = cond_flags(op & 15);
                return true;
            case 0xeb: case 0xe9:
                c->kind = K_JMP;
                return true;
        }
        return false;
    }

    if (g->map == 1) {
        if (g->lock) {
            // lock cmpxchg / xadd / bts... on memory
            if (g->mod == 3) return false;
            if (!(op == 0xb0 || op == 0xb1 || op == 0xc0 || op == 0xc1 ||
                  op == 0xab || op == 0xb3 || op == 0xbb || op == 0xba))
                return false;
        }
        if (op >= 0x80 && op <= 0x8f) {
            c->kind = K_JCC; c->cc = (uint8_t) (op & 15); c->fr = cond_flags(op & 15);
            return true;
        }
        if (op == 0x1f || op == 0x18 || op == 0x0d) {
            // multi-byte nop, prefetches: no memory access to make
            c->kind = K_NOP;
            return g->has_modrm;
        }
        if (op == 0xb8) {
            if (!g->pf3 || g->pf2) return false;
            c->kind = K_GENERIC; c->reg_gpr = true; c->acc = A_R; c->msize = osize(g);
            c->fw = F_ALL;
            return true;
        }
        if (reps)
            return false;
        if (op >= 0x40 && op <= 0x4f) {
            c->kind = K_GENERIC; c->reg_gpr = true; c->acc = A_R; c->msize = os;
            c->fr = cond_flags(op & 15);
            return true;
        }
        if (op >= 0x90 && op <= 0x9f) {
            c->kind = K_GENERIC; c->acc = A_W; c->msize = 1; c->rm_byte = true;
            c->fr = cond_flags(op & 15);
            return true;
        }
        if (op >= 0xc8 && op <= 0xcf) {
            c->kind = K_OPREG;
            return !g->p66;
        }
        switch (op) {
            case 0xa3: case 0xab: case 0xb3: case 0xbb:
                // with a register bit offset, the memory form reaches beyond the operand
                if (g->mod != 3) return false;
                c->kind = K_GENERIC; c->reg_gpr = true; c->acc = op == 0xa3 ? A_R : A_RW;
                c->msize = os; c->fw = F_ALL;
                return true;
            case 0xba:
                if (g->rawreg < 4) return false;
                c->kind = K_GENERIC; c->acc = g->rawreg == 4 ? A_R : A_RW;
                c->msize = os; c->fw = F_ALL;
                return true;
            case 0xa4: case 0xac:
                c->kind = K_GENERIC; c->reg_gpr = true; c->acc = A_RW; c->msize = os;
                c->fw = F_ALL;
                return true;
            case 0xa5: case 0xad:
                c->kind = K_GENERIC; c->reg_gpr = true; c->acc = A_RW; c->msize = os;
                c->fr = F_ALL; c->fw = F_ALL;
                return true;
            case 0xaf:
                c->kind = K_GENERIC; c->reg_gpr = true; c->acc = A_R; c->msize = os;
                c->fw = F_ALL;
                return true;
            case 0xb0: case 0xb1: case 0xc0: case 0xc1:
                c->kind = K_GENERIC; c->reg_gpr = true; c->acc = A_RW;
                c->reg_byte = c->rm_byte = (op & 1) == 0;
                c->msize = (op & 1) ? os : 1;
                c->fw = F_ALL;
                return true;
            case 0xb6: case 0xbe:
                c->kind = K_GENERIC; c->reg_gpr = true; c->acc = A_R; c->msize = 1;
                c->rm_byte = true;
                return true;
            case 0xb7: case 0xbf:
                c->kind = K_GENERIC; c->reg_gpr = true; c->acc = A_R; c->msize = 2;
                return true;
            case 0xbc: case 0xbd:
                c->kind = K_GENERIC; c->reg_gpr = true; c->acc = A_R; c->msize = os;
                c->fw = F_ALL;
                return true;
        }
        return false;
    }
    return false;
}

// Shifts and rotates by an immediate: which flags the count makes them touch.
static void shift_flags(const struct gi *g, const struct xinsn *x, struct cls *c) {
    unsigned op = g->op;
    bool shld = g->map == 1 && (op == 0xa4 || op == 0xac);
    if (!(g->map == 0 && (op == 0xc0 || op == 0xc1 || op == 0xd0 || op == 0xd1)) && !shld)
        return;
    unsigned count = (op == 0xd0 || op == 0xd1) ? 1 : x->bytes[g->immoff];
    count &= osize(g) == 8 && !(g->map == 0 && op == 0xc0) ? 63 : 31;
    if (g->map == 0 && op == 0xc0)
        count &= 31;
    if (count == 0) {
        c->fw = 0;
        c->fr = 0;
        return;
    }
    c->fr = 0;
    if (shld) {
        c->fw = F_ALL;
        return;
    }
    unsigned kind = g->rawreg;
    if (kind <= 1) {
        c->fw = F_CF | F_OF;
    } else if (kind <= 3) {
        c->fr = F_CF;
        c->fw = F_CF | F_OF;
    } else {
        c->fw = F_ALL;
    }
}

// ---------------------------------------------------------------- operands

// Where an operand lives on the host: a register, or a cpu slot it is
// loaded into r14/r15 from.
struct hop {
    int host;     // host register (for a slot: the scratch it is loaded into)
    int slot;     // guest r8-r15: cpu offset, else -1
};

struct plan {
    struct hop reg, rm;   // modrm.reg (if a GPR), modrm.rm (if a register)
    bool mem;
    bool norex;           // uses ah..bh: the encoding can't have a REX prefix
    bool rex;             // needs one
};

static int32_t slot_off(int g) {
    return CPU_OFF(regs) + 8 * g;
}

// Maps guest register gr (an operand at 8 bits if byte) to the host.
static void map_reg(const struct gi *g, int gr, bool byte, struct hop *h, struct plan *p, int *next_scratch, int *scratch_for) {
    h->slot = -1;
    if (byte && !g->rex && gr >= 4 && gr <= 7) {
        h->host = gr;  // ah, ch, dh, bh
        p->norex = true;
        return;
    }
    if (gr < 8) {
        h->host = gr == 4 ? R8 : gr;
        if (h->host >= 8 || (byte && h->host >= 4))
            p->rex = true;
        return;
    }
    if (scratch_for[gr - 8] < 0)
        scratch_for[gr - 8] = (*next_scratch)++;
    h->host = scratch_for[gr - 8];
    h->slot = slot_off(gr);
    p->rex = true;
}

static bool plan_generic(const struct gi *g, const struct cls *c, struct plan *p) {
    memset(p, 0, sizeof(*p));
    int scratch_for[8] = {-1, -1, -1, -1, -1, -1, -1, -1};
    int next = R14;
    p->reg.host = -1; p->reg.slot = -1;
    p->rm.host = -1; p->rm.slot = -1;
    if (c->reg_gpr)
        map_reg(g, g->reg, c->reg_byte, &p->reg, p, &next, scratch_for);
    if (g->mod == 3) {
        map_reg(g, g->rm, c->rm_byte, &p->rm, p, &next, scratch_for);
    } else {
        p->mem = true;
        p->rex = true;  // r13
    }
    if (g->rex & 8)
        p->rex = true;
    if (p->norex && p->rex)
        return false;
    return true;
}

// ---------------------------------------------------------------- context

struct xctx {
    struct xasm *a;
    uintptr_t *code;
    uint16_t valid, dirty;  // flags valid in the host's eflags / not yet in the lazy state
};

static void sync_dirty(struct xctx *x) {
    e_sync_flags(x->a, x->dirty);
    x->dirty = 0;
}

static uint64_t slot_addr(struct xctx *x, unsigned slot) {
    return (uint64_t) (uintptr_t) &x->code[slot];
}

// Hands over to the gadget stream at slot s.
static void e_to_stream(struct xctx *x, unsigned s) {
    e_movabs(x->a, H_IP, slot_addr(x, s + 1));
    // jmp *-8(%r9)
    b1(x->a, 0x41); b1(x->a, 0xff); b1(x->a, 0x61); b1(x->a, 0xf8);
}

// Leaves the block through the chainable stream slot s (jmp/jcc arguments).
static void e_exit_slot(struct xctx *x, unsigned s) {
    e_movabs(x->a, H_IP, slot_addr(x, s));
    e_mov_load(x->a, H_IP, XMEM(H_IP, 0));
    e_jmp_abs(x->a, (const void *) fiber_ret_chain);
}

// The guest address of a memory operand into dst. Slot registers go through
// r14 (base) and r15 (index). Doesn't touch the flags.
static void e_addr(struct xctx *x, const struct gi *g, int dst, bool seg) {
    struct xasm *a = x->a;
    if (g->base == BASE_RIP) {
        e_movabs(a, dst, (uint64_t) g->disp);
    } else {
        int base = -1, index = -1;
        if (g->base >= 0) {
            if (g->base >= 8) {
                e_mov_load(a, R14, XMEM(H_CPU, slot_off(g->base)));
                base = R14;
            } else {
                base = g->base == 4 ? R8 : g->base;
            }
        }
        if (g->index >= 0) {
            if (g->index >= 8) {
                e_mov_load(a, R15, XMEM(H_CPU, slot_off(g->index)));
                index = R15;
            } else {
                index = g->index == 4 ? R8 : g->index;
            }
        }
        if (base < 0 && index < 0) {
            e_movabs(a, dst, (uint64_t) g->disp);
        } else if (index < 0 && g->disp == 0) {
            if (base != dst)
                e_mov_rr(a, dst, base);
        } else {
            e_lea(a, dst, (struct xmem) {base, index, g->scale, (int32_t) g->disp});
        }
    }
    if (seg && g->seg) {
        e_mov_load(a, R14, XMEM(H_CPU, g->seg == 1 ? CPU_OFF(tls_ptr) : CPU_OFF(gs_base)));
        e_lea(a, dst, (struct xmem) {dst, R14, 0, 0});
    }
}

// The TLB index of the address in r13, times the entry size, into r14.
static void e_tlb_index(struct xasm *a) {
    unsigned lo = 12 - 5, hi = 12 + TLB_BITS - 5;
    if (have_bmi2) {
        // rorx $lo, %r13, %r14; rorx $hi, %r13, %r15
        b1(a, 0xc4); b1(a, 0x43); b1(a, 0xfb); b1(a, 0xf0); b1(a, modrm_b(3, R14, R13)); b1(a, (uint8_t) lo);
        b1(a, 0xc4); b1(a, 0x43); b1(a, 0xfb); b1(a, 0xf0); b1(a, modrm_b(3, R15, R13)); b1(a, (uint8_t) hi);
    } else {
        e_mov_rr(a, R14, R13);
        e_rex(a, true, -1, -1, R14, false); b1(a, 0xc1); b1(a, modrm_b(3, 5, R14)); b1(a, (uint8_t) lo);
        e_mov_rr(a, R15, R13);
        e_rex(a, true, -1, -1, R15, false); b1(a, 0xc1); b1(a, modrm_b(3, 5, R15)); b1(a, (uint8_t) hi);
    }
    e_rr(a, true, 0x31, R15, R14);  // xor %r15, %r14
    e_rex(a, false, -1, -1, R14, false); b1(a, 0x81); b1(a, modrm_b(3, 4, R14));
    b4(a, ((1u << TLB_BITS) - 1) << 5);
}

// ---------------------------------------------------------------- bodies

// What runs once the operand's host address is in r13 (or for a register
// operand, directly): loads of slot operands, the instruction, stores back.
// Emitted twice for memory operands: inline, and on the cross-page path.
struct body {
    const struct gi *g;
    const struct cls *c;
    const struct xinsn *in;
    const struct plan *p;
};

static void e_slot_loads(struct xasm *a, const struct plan *p) {
    if (p->reg.slot >= 0)
        e_mov_load(a, p->reg.host, XMEM(H_CPU, p->reg.slot));
    if (p->rm.slot >= 0 && p->rm.host != p->reg.host)
        e_mov_load(a, p->rm.host, XMEM(H_CPU, p->rm.slot));
}
static void e_slot_stores(struct xasm *a, const struct plan *p) {
    if (p->reg.slot >= 0)
        e_mov_store(a, XMEM(H_CPU, p->reg.slot), p->reg.host);
    if (p->rm.slot >= 0 && p->rm.host != p->reg.host)
        e_mov_store(a, XMEM(H_CPU, p->rm.slot), p->rm.host);
}

static void e_generic_insn(struct xasm *a, const struct body *bd) {
    const struct gi *g = bd->g;
    const struct cls *c = bd->c;
    const struct plan *p = bd->p;
    if (g->lock) b1(a, 0xf0);
    if (g->p66) b1(a, 0x66);
    if (g->pf3) b1(a, 0xf3);
    if (g->pf2) b1(a, 0xf2);
    int reg = c->reg_gpr ? p->reg.host : g->rawreg;
    int rm = p->mem ? R13 : p->rm.host;
    if (!p->norex)
        e_rex(a, g->rex & 8, c->reg_gpr ? reg : -1, -1, rm, g->rex != 0 || p->rex);
    if (g->map == 1) b1(a, 0x0f);
    b1(a, g->op);
    if (p->mem) {
        b1(a, modrm_b(1, reg, R13));
        b1(a, 0);
    } else {
        b1(a, modrm_b(3, reg, rm));
    }
    bn(a, &bd->in->bytes[g->immoff], g->immlen);
}

static void e_body(struct xasm *a, const struct body *bd) {
    const struct gi *g = bd->g;
    switch (bd->c->kind) {
        case K_GENERIC:
            e_slot_loads(a, bd->p);
            e_generic_insn(a, bd);
            e_slot_stores(a, bd->p);
            break;
        case K_PUSH: {
            int gr = (g->op & 7) | ((g->rex & 1) ? 8 : 0);
            int h = gr == 4 ? R8 : gr;
            if (gr >= 8) {
                e_mov_load(a, R14, XMEM(H_CPU, slot_off(gr)));
                h = R14;
            }
            e_mov_store(a, XMEM(R13, 0), h);
            break;
        }
        case K_PUSHI: {
            int32_t imm = g->immlen == 1 ? (int8_t) bd->in->bytes[g->immoff] :
                (int32_t) (bd->in->bytes[g->immoff] | bd->in->bytes[g->immoff + 1] << 8 |
                           bd->in->bytes[g->immoff + 2] << 16 | (uint32_t) bd->in->bytes[g->immoff + 3] << 24);
            e_rm(a, true, 0xc7, 0, XMEM(R13, 0));
            b4(a, (uint32_t) imm);
            break;
        }
        case K_POP: {
            int gr = (g->op & 7) | ((g->rex & 1) ? 8 : 0);
            if (gr >= 8) {
                e_mov_load(a, R14, XMEM(R13, 0));
                e_mov_store(a, XMEM(H_CPU, slot_off(gr)), R14);
            } else {
                e_mov_load(a, gr == 4 ? R8 : gr, XMEM(R13, 0));
            }
            break;
        }
        case K_LEAVE:
            e_mov_load(a, R14, XMEM(R13, 0));
            break;
    }
}

// After the body, on both paths
static void e_after(struct xasm *a, const struct body *bd) {
    const struct gi *g = bd->g;
    switch (bd->c->kind) {
        case K_PUSH: case K_PUSHI:
            e_lea(a, R8, XMEM(R8, -8));
            break;
        case K_POP: {
            int gr = (g->op & 7) | ((g->rex & 1) ? 8 : 0);
            if (gr != 4)
                e_lea(a, R8, XMEM(R8, 8));
            break;
        }
        case K_LEAVE:
            e_lea(a, R8, XMEM(RBP, 8));
            e_mov_rr(a, RBP, R14);
            break;
    }
}

// ---------------------------------------------------------------- memory

// An instruction with a memory operand whose guest address is in r13:
// translate it (keeping the flags in `keep` alive across the lookup), run
// the body on the host address. Flags in `sync` are written to the lazy
// state before anything here can fault.
static void e_mem_insn(struct xctx *x, const struct body *bd, unsigned size, bool write,
                       uint16_t keep, bool load_flags) {
    struct xasm *a = x->a;
    unsigned l_miss = new_label(a), l_back = new_label(a), l_cross = new_label(a),
             l_after = new_label(a), l_orig = new_label(a);
    uint16_t sync = keep & x->dirty;

    if (keep)
        e_save_flags(a);
    e_tlb_index(a);
    // the page of the operand's last byte, so that crossing a page misses
    e_lea(a, R15, XMEM(R13, (int32_t) size - 1));
    e_rex(a, true, -1, -1, R15, false); b1(a, 0x81); b1(a, modrm_b(3, 4, R15)); b4(a, (uint32_t) -4096);
    e_rm(a, true, 0x0b, R15, XMEM(H_TLB, TLBREL(stale)));
    e_rm(a, true, 0x3b, R15, (struct xmem) {H_TLB, R14, 0,
         write ? TLBENT(page_if_writable) : TLBENT(page)});
    e_jcc_lab(a, 5, l_miss);  // jne
    e_rm(a, true, 0x03, R13, (struct xmem) {H_TLB, R14, 0, TLBENT(data_minus_addr)});
    lbind(a, l_back);
    if (keep)
        e_restore_flags(a);
    if (load_flags)
        e_call_abs(a, load_flags_stub);
    e_body(a, bd);
    lbind(a, l_after);
    e_after(a, bd);

    // slow paths
    a->cur = SEC_COLD;
    lbind(a, l_miss);
    // mov %r13d, %r15d; and $0xfff, %r15d; cmp $(0x1000 - size), %r15d; ja cross
    e_rr(a, false, 0x89, R13, R15);
    e_rex(a, false, -1, -1, R15, false); b1(a, 0x81); b1(a, modrm_b(3, 4, R15)); b4(a, 0xfff);
    e_rex(a, false, -1, -1, R15, false); b1(a, 0x81); b1(a, modrm_b(3, 7, R15)); b4(a, 0x1000 - size);
    e_jcc_lab(a, 7, l_cross);  // ja
    if (sync) {
        e_restore_flags(a);
        e_sync_flags(a, sync);
    }
    // lea orig(%rip), %r9
    b1(a, 0x4c); b1(a, 0x8d); b1(a, 0x0d); rel_lab(a, l_orig);
    e_call_abs(a, write ? (const void *) handle_write_miss : (const void *) handle_read_miss);
    e_jmp_lab(a, l_back);

    lbind(a, l_cross);
    if (sync) {
        e_restore_flags(a);
        e_sync_flags(a, sync);
    }
    // movl $size, %r14d
    b1(a, 0x41); b1(a, (uint8_t) (0xb8 + (R14 & 7))); b4(a, size);
    b1(a, 0x4c); b1(a, 0x8d); b1(a, 0x0d); rel_lab(a, l_orig);
    e_call_abs(a, (const void *) crosspage_load);
    if (keep)
        e_restore_flags(a);
    if (load_flags)
        e_call_abs(a, load_flags_stub);
    e_body(a, bd);
    if (write) {
        e_lea(a, RSP, XMEM(RSP, -8));
        b1(a, 0x9c);  // pushfq
        b1(a, 0x41); b1(a, (uint8_t) (0xb8 + (R14 & 7))); b4(a, size);
        b1(a, 0x4c); b1(a, 0x8d); b1(a, 0x0d); rel_lab(a, l_orig);
        e_call_abs(a, (const void *) crosspage_store);
        b1(a, 0x9d);  // popfq
        e_lea(a, RSP, XMEM(RSP, 8));
    }
    e_jmp_lab(a, l_after);
    // the faulting instruction's address, where the fault path reads it
    while (xpos(a) & 7)
        b1(a, 0xcc);
    lbind(a, l_orig);
    b8(a, bd->g->ip);
    a->cur = SEC_MAIN;
}

// ---------------------------------------------------------------- insns

// Emits one translated instruction; live: flags live after it.
static void e_native(struct xctx *x, const struct xinsn *in, const struct gi *g,
                     const struct cls *c, const struct plan *p, uint16_t live) {
    struct xasm *a = x->a;
    struct body bd = {g, c, in, p};
    uint16_t live_in = c->fr | (live & ~c->fw);
    bool load_flags = (c->fr & ~x->valid) != 0;
    if (load_flags) {
        // the lazy state has some of what it reads: complete it, then load
        // everything from it (after any lookup, which would clobber them)
        sync_dirty(x);
        x->valid = 0;
    }

    unsigned size = 0;
    bool write = false, mem = false;
    switch (c->kind) {
        case K_GENERIC:
            if (p->mem) {
                mem = true;
                size = c->msize;
                write = c->acc == A_W || c->acc == A_RW;
                e_addr(x, g, R13, true);
            }
            break;
        case K_PUSH: case K_PUSHI:
            mem = true; size = 8; write = true;
            e_lea(a, R13, XMEM(R8, -8));
            break;
        case K_POP:
            mem = true; size = 8;
            e_mov_rr(a, R13, R8);
            break;
        case K_LEAVE:
            mem = true; size = 8;
            e_mov_rr(a, R13, RBP);
            break;
    }

    if (mem) {
        uint16_t keep = load_flags ? 0 : (x->valid & live_in);
        e_mem_insn(x, &bd, size, write, keep, load_flags);
        x->valid &= keep;
        x->dirty &= keep;
        if (load_flags)
            x->valid = F_ALL;
    } else {
        if (load_flags) {
            e_call_abs(a, load_flags_stub);
            x->valid = F_ALL;
        }
        switch (c->kind) {
            case K_GENERIC:
                e_body(a, &bd);
                break;
            case K_IMPLICIT:
                if (g->p66) b1(a, 0x66);
                if (g->rex) b1(a, g->rex);
                bn(a, &in->bytes[g->opoff], in->len - g->opoff);
                break;
            case K_NOP:
                break;
            case K_LEA: {
                unsigned os = osize(g);
                int gr = g->reg;
                bool direct = gr < 8 && g->base != BASE_RIP && g->base < 8 && g->index < 8 &&
                    (g->base >= 0 || g->index >= 0) && !g->a32;
                if (direct) {
                    int h = gr == 4 ? R8 : gr;
                    int base = g->base < 0 ? -1 : g->base == 4 ? R8 : g->base;
                    int index = g->index < 0 ? -1 : g->index == 4 ? R8 : g->index;
                    if (os == 2) b1(a, 0x66);
                    e_rm(a, os == 8, 0x8d, h, (struct xmem) {base, index, g->scale, (int32_t) g->disp});
                    break;
                }
                e_addr(x, g, R13, false);
                if (gr < 8) {
                    int h = gr == 4 ? R8 : gr;
                    if (os == 2) b1(a, 0x66);
                    e_rr(a, os == 8, 0x89, R13, h);
                } else {
                    if (os == 8) {
                        e_mov_store(a, XMEM(H_CPU, slot_off(gr)), R13);
                    } else if (os == 4) {
                        e_rr(a, false, 0x89, R13, R13);  // zero-extend
                        e_mov_store(a, XMEM(H_CPU, slot_off(gr)), R13);
                    } else {
                        e_mov_load(a, R14, XMEM(H_CPU, slot_off(gr)));
                        b1(a, 0x66); e_rr(a, false, 0x89, R13, R14);
                        e_mov_store(a, XMEM(H_CPU, slot_off(gr)), R14);
                    }
                }
                break;
            }
            case K_OPREG: {
                int gr = (g->op & 7) | ((g->rex & 1) ? 8 : 0);
                bool byte = g->map == 0 && g->op >= 0xb0 && g->op <= 0xb7;
                int h;
                bool norex = false;
                if (byte && !g->rex && gr >= 4)
                    h = gr, norex = true;  // ah..bh
                else
                    h = gr == 4 ? R8 : gr >= 8 ? R14 : gr;
                if (gr >= 8)
                    e_mov_load(a, R14, XMEM(H_CPU, slot_off(gr)));
                if (g->p66) b1(a, 0x66);
                if (!norex)
                    e_rex(a, g->rex & 8, -1, -1, h, g->rex != 0 || (byte && h >= 4));
                if (g->map == 1) b1(a, 0x0f);
                b1(a, (uint8_t) ((g->op & ~7) | (h & 7)));
                bn(a, &in->bytes[g->immoff], g->immlen);
                if (gr >= 8)
                    e_mov_store(a, XMEM(H_CPU, slot_off(gr)), R14);
                break;
            }
        }
    }
    x->valid |= c->fw;
    x->dirty |= c->fw;
}

// ---------------------------------------------------------------- templates

enum { FIX_MOVED = 2 };  // a template's rel32/disp32: keep its target as the copy moves

static void add_fix_at(struct xasm *a, uint32_t off, uint8_t kind, uintptr_t target) {
    uint32_t save = xpos(a);
    struct xbuf *b = xb(a);
    b->p = b->base + off;
    add_fix(a, kind, target);
    b->p = b->base + save;
}

static void e_template(struct xasm *a, const struct tpl *t) {
    uint32_t at = xpos(a);
    bn(a, t->code, t->len);
    if (a->overflow)
        return;
    uint8_t *out = xb(a)->base + at;
    for (unsigned j = 0; j < t->nrelocs; j++)
        add_fix_at(a, at + t->relocs[j], FIX_MOVED, (uintptr_t) (t->code + t->relocs[j]));
    for (unsigned j = 0; j < t->nends; j++) {
        unsigned off = t->ends[j] >> 1;
        if (t->ends[j] & 1) {
            int32_t v = rd32(t->code + off) - t->trim;
            memcpy(out + off, &v, 4);
        } else {
            out[off] = (uint8_t) ((int8_t) t->code[off] - t->trim);
        }
    }
}

// Whether slots [s0, s1) are gadgets with usable templates. *terminal: the
// last one leaves by itself (its arguments run to s1).
static bool tpl_range(const uintptr_t *code, unsigned s0, unsigned s1, bool *terminal) {
    *terminal = false;
    unsigned s = s0;
    if (s0 >= s1)
        return false;
    while (s < s1) {
        const struct tpl *t = tpl_lookup(code[s]);
        if (t == NULL || !t->usable)
            return false;
        if (t->pop < 0) {
            *terminal = true;
            return true;
        }
        s += 1 + (unsigned) t->pop;
    }
    return s == s1;
}

static void e_tpl_range(struct xctx *x, unsigned s0, unsigned s1) {
    e_movabs(x->a, H_IP, slot_addr(x, s0 + 1));
    unsigned s = s0;
    while (s < s1) {
        const struct tpl *t = tpl_lookup(x->code[s]);
        e_template(x->a, t);
        if (t->pop < 0)
            break;
        s += 1 + (unsigned) t->pop;
    }
}

// ---------------------------------------------------------------- stats

static _Atomic uint64_t st_native, st_tpl, st_stream;
static _Atomic uint32_t st_fallback_op[3 * 256];

static void count_fallback(const struct xinsn *in, const struct gi *g, bool decoded) {
    if (!stats_on)
        return;
    unsigned key = 2 * 256 + 0xff;
    if (decoded && g->map <= 1)
        key = g->map * 256 + g->op;
    else if (decoded)
        key = 2 * 256 + (g->map == 2 ? 0x38 : 0x3a);
    (void) in;
    atomic_fetch_add_explicit(&st_fallback_op[key], 1, memory_order_relaxed);
}

static void xlat_dump_stats(FILE *out) {
    fprintf(out, "[xlat] insns native=%llu template=%llu stream=%llu\n",
            (unsigned long long) st_native, (unsigned long long) st_tpl,
            (unsigned long long) st_stream);
    for (int round = 0; round < 25; round++) {
        unsigned best = 0, bestk = 0;
        for (unsigned k = 0; k < 3 * 256; k++) {
            if (st_fallback_op[k] > best) {
                best = st_fallback_op[k];
                bestk = k;
            }
        }
        if (best == 0)
            break;
        fprintf(out, "[xlat]   fallback %s%02x: %u\n",
                bestk >= 512 ? "other " : bestk >= 256 ? "0f " : "", bestk & 0xff, best);
        st_fallback_op[bestk] = 0;
    }
}

// ---------------------------------------------------------------- blocks

struct xi {
    struct gi g;
    struct cls c;
    struct plan p;
    uint8_t mode;
    bool terminal;
    unsigned taken, fall;   // jcc/jmp: the stream slots of the targets
};
enum { M_STREAM, M_TPL, M_NATIVE };

static __thread struct xi *xis;
static __thread unsigned xis_cap;
static bool xlat_on;

static bool plan_insn(struct xctx *x, const struct xinsn *in, struct xi *xi, unsigned n_left) {
    if (in->bad || !gdecode(in, &xi->g) || !classify(&xi->g, &xi->c))
        return false;
    shift_flags(&xi->g, in, &xi->c);
    const struct gi *g = &xi->g;
    switch (xi->c.kind) {
        case K_GENERIC:
            return plan_generic(g, &xi->c, &xi->p);
        case K_JCC: case K_JMP: {
            if (n_left != 1)
                return false;
            int64_t rel;
            if (g->immlen == 1)
                rel = (int8_t) in->bytes[g->immoff];
            else {
                int32_t r;
                memcpy(&r, &in->bytes[g->immoff], 4);
                rel = r;
            }
            uint64_t target = (g->next + (uint64_t) rel) | (1ull << 63);
            unsigned s0 = in->s0;
            if (xi->c.kind == K_JMP) {
                if (in->s1 != s0 + 2 || x->code[s0 + 1] != target)
                    return false;
                xi->taken = s0 + 1;
            } else {
                if (in->s1 != s0 + 3)
                    return false;
                if (x->code[s0 + 1] == target) {
                    xi->taken = s0 + 1;
                    xi->fall = s0 + 2;
                } else if (x->code[s0 + 2] == target) {
                    xi->taken = s0 + 2;
                    xi->fall = s0 + 1;
                } else {
                    return false;
                }
            }
            xi->terminal = true;
            return true;
        }
        default:
            return true;
    }
}

static bool xlat_finish(struct xctx *x, struct fiber_block *block, unsigned *entry_slot,
                        uint32_t *entry_off, unsigned nentries) {
    struct xasm *a = x->a;
    if (a->overflow)
        return false;
    size_t main_len = (size_t) (a->sec[SEC_MAIN].p - a->sec[SEC_MAIN].base);
    size_t cold_off = (main_len + 15) & ~(size_t) 15;
    size_t cold_len = (size_t) (a->sec[SEC_COLD].p - a->sec[SEC_COLD].base);
    size_t total = cold_off + cold_len;
    uint8_t *mem = code_alloc(total);
    if (mem == NULL) {
        atomic_fetch_add_explicit(&st_nomem, 1, memory_order_relaxed);
        return false;
    }
    uint8_t *sec_base[2] = {mem, mem + cold_off};
    memcpy(mem, a->sec[SEC_MAIN].base, main_len);
    memset(mem + main_len, 0xcc, cold_off - main_len);
    memcpy(mem + cold_off, a->sec[SEC_COLD].base, cold_len);
    for (unsigned i = 0; i < a->nfix; i++) {
        struct xfix *f = &a->fix[i];
        uint8_t *field = sec_base[f->sec] + f->off;
        int64_t v;
        if (f->kind == FIX_LABEL) {
            struct xlabel *l = &a->lab[f->target];
            if (l->sec > 1)
                goto fail;
            v = (int64_t) (intptr_t) (sec_base[l->sec] + l->off) - (int64_t) (intptr_t) (field + 4);
        } else if (f->kind == FIX_ABS) {
            v = (int64_t) f->target - (int64_t) (intptr_t) (field + 4);
        } else {
            const uint8_t *orig = (const uint8_t *) f->target;
            v = (int64_t) rd32(orig) - ((int64_t) (intptr_t) field - (int64_t) (intptr_t) orig);
        }
        if (v != (int32_t) v)
            goto fail;
        int32_t v32 = (int32_t) v;
        memcpy(field, &v32, 4);
    }
    for (unsigned i = 0; i < nentries; i++)
        block->code[entry_slot[i]] = (uintptr_t) (mem + entry_off[i]);
    block->xjit_code = mem;
    if (stats_on) {
        atomic_fetch_add_explicit(&st_blocks, 1, memory_order_relaxed);
        atomic_fetch_add_explicit(&st_runs, nentries, memory_order_relaxed);
        atomic_fetch_add_explicit(&st_bytes, total, memory_order_relaxed);
    }
    return true;
fail:
    code_free(mem);
    return false;
}

// Translates the block the instruction log describes. False: leave it to
// the template splicer.
static bool xlat_block(struct fiber_block *block) {
    if (!xlat_on || !xlog_ok || xlog_n == 0 || xlog[0].s0 != 0 || block->addr != xlog[0].ip)
        return false;
    unsigned n = xlog_n;
    if (xis_cap < n + 1) {
        free(xis);
        xis_cap = (n + 1) * 2;
        xis = malloc(xis_cap * sizeof(*xis));
        if (xis == NULL) {
            xis_cap = 0;
            return false;
        }
    }
    struct xasm *a = &xa_tls;
    if (!xa_init(a))
        return false;
    struct xctx x = {.a = a, .code = block->code};

    // the stream past the last instruction (gen_exit()), as one more
    struct xinsn tail = {.ip = 0, .s0 = xlog[n - 1].s1, .s1 = xlog_size, .bad = true};
    unsigned total = n;
    if (xlog_size > xlog[n - 1].s1)
        total = n + 1;

    // plan
    for (unsigned i = 0; i < total; i++) {
        const struct xinsn *in = i < n ? &xlog[i] : &tail;
        struct xi *xi = &xis[i];
        memset(&xi->c, 0, sizeof(xi->c));
        xi->terminal = false;
        if (i > 0 && in->s0 != (i < n ? xlog[i - 1].s1 : xlog[n - 1].s1))
            return false;
        if (plan_insn(&x, in, xi, total - i)) {
            xi->mode = M_NATIVE;
            continue;
        }
        bool term;
        if (tpl_range(block->code, in->s0, in->s1, &term)) {
            xi->mode = M_TPL;
            xi->terminal = term;
        } else {
            xi->mode = M_STREAM;
        }
        if (i < n)
            count_fallback(in, &xi->g, !in->bad && gdecode(in, &xi->g));
    }

    // flag liveness, backwards: what each native instruction must leave
    // valid behind it
    static __thread uint16_t *live;
    static __thread unsigned live_cap;
    if (live_cap < total) {
        free(live);
        live_cap = total * 2;
        live = malloc(live_cap * sizeof(*live));
        if (live == NULL) {
            live_cap = 0;
            return false;
        }
    }
    uint16_t after = F_ALL;
    for (unsigned i = total; i-- > 0;) {
        struct xi *xi = &xis[i];
        if (xi->mode != M_NATIVE || xi->terminal) {
            live[i] = F_ALL;
            after = F_ALL;
            continue;
        }
        live[i] = after;
        after = xi->c.fr | (after & ~xi->c.fw);
    }

    // emit
    static __thread unsigned *entry_slot;
    static __thread uint32_t *entry_off;
    static __thread unsigned entry_cap;
    if (entry_cap < total) {
        free(entry_slot);
        free(entry_off);
        entry_cap = total * 2;
        entry_slot = malloc(entry_cap * sizeof(*entry_slot));
        entry_off = malloc(entry_cap * sizeof(*entry_off));
        if (entry_slot == NULL || entry_off == NULL) {
            entry_cap = 0;
            return false;
        }
    }
    unsigned nentries = 0;
    bool in_run = false;
    unsigned native = 0, tpl = 0, stream = 0;
    for (unsigned i = 0; i < total; i++) {
        const struct xinsn *in = i < n ? &xlog[i] : &tail;
        struct xi *xi = &xis[i];
        if (xi->mode == M_STREAM) {
            stream++;
            if (in_run) {
                sync_dirty(&x);
                e_to_stream(&x, in->s0);
                in_run = false;
            }
            continue;
        }
        if (!in_run) {
            entry_slot[nentries] = in->s0;
            entry_off[nentries] = xpos(a);
            nentries++;
            in_run = true;
            x.valid = x.dirty = 0;
        }
        if (xi->mode == M_TPL) {
            tpl++;
            sync_dirty(&x);
            x.valid = 0;
            e_tpl_range(&x, in->s0, in->s1);
            if (xi->terminal)
                in_run = false;
            continue;
        }
        native++;
        struct gi *g = &xi->g;
        struct cls *c = &xi->c;
        if (c->kind == K_JCC || c->kind == K_JMP) {
            if ((c->fr & ~x.valid) != 0) {
                sync_dirty(&x);
                e_call_abs(a, load_flags_stub);
                x.valid = F_ALL;
            }
            uint16_t dirty = x.dirty;
            if (c->kind == K_JCC) {
                unsigned l_taken = new_label(a);
                e_jcc_lab(a, c->cc, l_taken);
                e_sync_flags(a, dirty);
                e_exit_slot(&x, xi->fall);
                lbind(a, l_taken);
            }
            e_sync_flags(a, dirty);
            e_exit_slot(&x, xi->taken);
            in_run = false;
            continue;
        }
        (void) g;
        e_native(&x, in, g, c, &xi->p, live[i]);
        if (a->overflow)
            return false;
    }
    if (in_run) {
        // the block ran off its end without leaving: shouldn't happen
        return false;
    }
    if (stats_on) {
        atomic_fetch_add_explicit(&st_native, native, memory_order_relaxed);
        atomic_fetch_add_explicit(&st_tpl, tpl, memory_order_relaxed);
        atomic_fetch_add_explicit(&st_stream, stream, memory_order_relaxed);
    }
    if (nentries == 0)
        return true;
    return xlat_finish(&x, block, entry_slot, entry_off, nentries);
}

static void xlat_init(void) {
    const char *e = getenv("ISH_XLAT");
    xlat_on = !(e && (!strcmp(e, "0") || !strcmp(e, "off")));
    if (!xlat_on)
        return;
#if defined(__x86_64__)
    unsigned ra, rb, rc, rd;
    if (__get_cpuid_count(7, 0, &ra, &rb, &rc, &rd))
        have_bmi2 = (rb >> 8) & 1;
    e = getenv("ISH_XLAT_BMI2");
    if (e && e[0] == '0')
        have_bmi2 = false;
#endif
    struct xasm *a = &xa_tls;
    if (!xa_init(a)) {
        xlat_on = false;
        return;
    }
    e_load_flags_stub(a);
    size_t len = (size_t) (a->sec[SEC_MAIN].p - a->sec[SEC_MAIN].base);
    uint8_t *mem = code_alloc(len);
    if (mem == NULL || a->overflow) {
        xlat_on = false;
        return;
    }
    memcpy(mem, a->sec[SEC_MAIN].base, len);
    load_flags_stub = mem;
}
