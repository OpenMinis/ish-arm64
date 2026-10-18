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
// which opcodes fell back most. For bisecting a translation bug:
// ISH_XLAT_KINDS=<mask of K_*>, ISH_XLAT_OPS=lo[:hi] (map * 256 + opcode of
// generic instructions), ISH_XLAT_NTH=lo[:hi] (only those of them, in
// translation order; one prints it), ISH_XLAT_NOSUCC, ISH_XLAT_NOLINK.

#include <stddef.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include "emu/tlb.h"
#include "emu/cpu.h"
#include "asbestos/gen.h"
#include "asbestos/frame.h"
#include "emu/interrupt.h"
#include "kernel/memory.h"

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
static __thread struct tlb *xlog_tlb;

void xjit_insn_begin(struct gen_state *state) {
    if (state->size == 0) {
        xlog_n = 0;
        xlog_ok = true;
    }
    xlog_cur_s0 = state->size;
    xlog_cur_ip = state->ip;
}

static void xlat_note_thread(void);
void xjit_insn_end(struct gen_state *state, struct tlb *tlb) {
    xlog_tlb = tlb;
    if (!xlog_ok)
        return;
    if (xlog_n == xlog_cap) {
        xlat_note_thread();
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
// FIX_DATA: rel32 to word n of the block's data. FIX_SLOT64 / FIX_BLOCK64:
// an imm64 that is &block->code[n] / the block: everything that ties the
// code to one block is a fixup, so a translation can be cached and linked
// again for another block of the same guest code (xcache below).
enum { FIX_ABS, FIX_LABEL, FIX_DATA = 3, FIX_SLOT64 = 4, FIX_BLOCK64 = 5 };

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

static void xlat_note_thread(void);
static bool xa_init(struct xasm *a) {
    xlat_note_thread();
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
extern void fiber_ret_lookup(void) __asm__("fiber_ret_lookup");
extern void fiber_ret(void) __asm__("fiber_ret");
extern void fiber_exit(void) __asm__("fiber_exit");

#define CPU_OFF(f) ((int32_t) offsetof(struct cpu_state, f))
#define TLBREL(f) ((int32_t) offsetof(struct tlb, f) - (int32_t) offsetof(struct tlb, entries))
#define TLBENT(f) ((int32_t) offsetof(struct tlb_entry, f))

static bool have_bmi2;
static uint8_t *load_flags_stub;  // lazy flags -> host eflags; clobbers r14, r15

// ---------------------------------------------------------------- flag code

// Writes the flags in mask from the host's eflags to the lazy representation.
// Clobbers r14, r15 and eflags.
static bool dbg_slowsync;   // ISH_XLAT_SLOWSYNC: always the read-modify-write form
static void e_sync_flags(struct xasm *a, uint16_t mask, uint16_t valid) {
    if (mask == 0)
        return;
    // Stores only, when the host has all of PF AF ZF SF (or none is asked
    // for): eflags' low byte is exactly lahf's (its CF bit is nobody's: CF
    // lives in cf), and with all four in it none comes from res any more. The
    // form below reads and rewrites eflags and flags_res, every time the
    // same two bytes: a chain of store-to-load forwards from each block's
    // exit to the next one's.
    uint16_t four = F_PF | F_AF | F_ZF | F_SF;
    if (!dbg_slowsync && ((mask & four) == 0 || (valid & four) == four)) {
        if (mask & F_CF)
            e_rm2(a, false, 0x0f, 0x92, 0, XMEM(H_CPU, CPU_OFF(cf)));    // setc cf(%r11)
        if (mask & F_OF)
            e_rm2(a, false, 0x0f, 0x90, 0, XMEM(H_CPU, CPU_OFF(of)));    // seto of(%r11)
        if (mask & four) {
            e_mov_rr(a, R15, RAX);
            b1(a, 0x9f);                                                 // lahf
            b1(a, 0x0f); b1(a, 0xb6); b1(a, 0xc4);                       // movzbl %ah, %eax (ah has no encoding next to r11)
            e_rm(a, false, 0x88, RAX, XMEM(H_CPU, CPU_OFF(eflags)));     // mov %al, eflags(%r11)
            e_mov_rr(a, RAX, R15);
            e_rm(a, false, 0xc6, 0, XMEM(H_CPU, CPU_OFF(flags_res))); b1(a, 0);   // movb $0, flags_res(%r11)
        }
        return;
    }
    // lahf and seto, not pushfq (microcoded, the slowest part of this):
    // r14 = SF ZF - AF - PF - CF in bits 8-15, OF in bit 0
    e_mov_rr(a, R15, RAX);
    b1(a, 0x9f);                                   // lahf
    b1(a, 0x0f); b1(a, 0x90); b1(a, 0xc0);         // seto %al
    b1(a, 0x44); b1(a, 0x0f); b1(a, 0xb7); b1(a, modrm_b(3, R14, RAX));  // movzwl %ax, %r14d
    e_mov_rr(a, RAX, R15);
    // eflags bits (not OF: collapse_flags rewrites it from of, as CF from cf)
    uint16_t low = mask & (F_CF | F_PF | F_AF | F_ZF | F_SF);
    if (low) {
        // mov %r14d, %r15d; shr $8, %r15d; and $low, %r15d
        e_rr(a, false, 0x89, R14, R15);
        e_rex(a, false, -1, -1, R15, false); b1(a, 0xc1); b1(a, modrm_b(3, 5, R15)); b1(a, 8);
        e_rex(a, false, -1, -1, R15, false); b1(a, 0x81); b1(a, modrm_b(3, 4, R15)); b4(a, low);
        // andl $~low, eflags(%r11); orl %r15d, eflags(%r11)
        e_rm(a, false, 0x81, 4, XMEM(H_CPU, CPU_OFF(eflags))); b4(a, (uint32_t) ~low);
        e_rm(a, false, 0x09, R15, XMEM(H_CPU, CPU_OFF(eflags)));
    }
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
        // bt $8, %r14d; setc cf(%r11)
        e_rex(a, false, -1, -1, R14, false); b1(a, 0x0f); b1(a, 0xba); b1(a, modrm_b(3, 4, R14)); b1(a, 8);
        e_rm2(a, false, 0x0f, 0x92, 0, XMEM(H_CPU, CPU_OFF(cf)));
    }
    if (mask & F_OF)
        e_rm(a, false, 0x88, R14, XMEM(H_CPU, CPU_OFF(of)));   // mov %r14b, of(%r11)
}

// Keeps the arithmetic flags in r10 across code that clobbers them (lahf
// form: ah = SF ZF AF PF CF, al = OF). Uses r15.
static void e_save_flags(struct xasm *a) {
    e_mov_rr(a, R15, RAX);
    b1(a, 0x9f);                                   // lahf
    b1(a, 0x0f); b1(a, 0x90); b1(a, 0xc0);         // seto %al
    e_mov_rr(a, R10, RAX);
    e_mov_rr(a, RAX, R15);
}
static void e_restore_flags(struct xasm *a) {
    e_mov_rr(a, R15, RAX);
    e_mov_rr(a, RAX, R10);
    b1(a, 0x04); b1(a, 0x7f);                      // add $0x7f, %al: OF
    b1(a, 0x9e);                                   // sahf
    e_mov_rr(a, RAX, R15);
}

// The lazy flags into the host's eflags. A shared routine: it ends in sahf
// (and an add for OF) and ret, which leaves the flags alone. Preserves
// everything else.
static void e_load_flags_stub(struct xasm *a) {
    b1(a, 0x41); b1(a, 0x56);          // push %r14
    b1(a, 0x41); b1(a, 0x57);          // push %r15
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
    // eax = flags of res: test res; lahf
    e_mov_load(a, R15, XMEM(H_CPU, CPU_OFF(res)));
    e_rr(a, true, 0x85, R15, R15);                             // test %r15, %r15
    b1(a, 0x9f);                                               // lahf
    b1(a, 0x0f); b1(a, 0xb6); b1(a, 0xc4);                     // movzbl %ah, %eax
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
    // into the flags as e_restore_flags() does: ah for sahf, al = OF
    e_rr(a, false, 0x89, R14, RAX);                            // mov %r14d, %eax
    b1(a, 0x25); b4(a, F_SF | F_ZF | F_AF | F_PF | F_CF);      // and $0xd5, %eax
    b1(a, 0xc1); b1(a, modrm_b(3, 4, RAX)); b1(a, 8);         // shl $8, %eax
    e_rr(a, false, 0x89, R14, RCX);                            // mov %r14d, %ecx
    b1(a, 0xc1); b1(a, modrm_b(3, 5, RCX)); b1(a, 11);        // shr $11, %ecx
    b1(a, 0x83); b1(a, modrm_b(3, 4, RCX)); b1(a, 1);         // and $1, %ecx
    b1(a, 0x09); b1(a, modrm_b(3, RCX, RAX));                 // or %ecx, %eax
    b1(a, 0x04); b1(a, 0x7f);                                  // add $0x7f, %al: OF
    b1(a, 0x9e);                                               // sahf
    b1(a, 0x59);                                               // pop %rcx
    b1(a, 0x58);                                               // pop %rax
    b1(a, 0x41); b1(a, 0x5f);                                  // pop %r15
    b1(a, 0x41); b1(a, 0x5e);                                  // pop %r14
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

// exact: the instruction is all of x->len bytes; else it starts them
static bool gdecode_n(const struct xinsn *x, struct gi *g, bool exact) {
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
    if (exact ? i != n : i > n)
        return false;
    g->len = (uint8_t) i;
    g->next = x->ip + i;
    if (g->base == BASE_RIP)
        g->disp += (int64_t) g->next;  // the absolute address
    return true;
}

static bool gdecode(const struct xinsn *x, struct gi *g) {
    return gdecode_n(x, g, true);
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
    K_CALL, K_CALLI, K_JMPI, K_RET,       // call rel32, call/jmp through r/m, ret
    // bodies of the memory accesses those make (e_body)
    K_B_LOADT, K_B_PUSHRET, K_B_POPRET,
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
    // what the instruction does with its reg and r/m operands (RW_*; 0:
    // both): decides which slot registers are loaded and stored back
    uint8_t reg_rw, rm_rw;
    // SSE (classify_sse): modrm.reg / a register modrm.rm name xmm
    // registers, which live in the cpu struct and pass through the host's
    // xmm0 / xmm1. The host runs hpfx 0f hop: the guest's instruction, or
    // its unaligned form. memload: a 16-byte memory operand is loaded into
    // xmm1 first (legacy SSE arithmetic faults on an unaligned one).
    bool sse, reg_xmm, rm_xmm, memload;
    uint8_t hpfx, hop;
};
#define RW_R 1
#define RW_W 2
#define RW_RW 3

static unsigned osize(const struct gi *g) {
    return (g->rex & 8) ? 8 : g->p66 ? 2 : 4;
}

static bool dbg_nosse;     // ISH_XLAT_NOSSE: leave SSE to the gadgets

// The SSE instructions translated natively: moves, integer and logic
// operations, floating point arithmetic, conversions and compares. Only
// comis/ucomis touch the flags, all of them, as the guest's instruction
// does. The gadgets round those to nearest whatever the guest's
// MXCSR says and never set its exception bits: the host's own instructions,
// under the host's default MXCSR, do the same. cvtsd2si/cvtss2si, which
// honor the guest's rounding control, stay with the gadgets.
static bool classify_sse(const struct gi *g, struct cls *c) {
    unsigned op = g->op;
    if (dbg_nosse || g->lock || g->map != 1 || !g->has_modrm)
        return false;
    unsigned pfx = 0;
    if (g->p66 + g->pf2 + g->pf3 > 1)
        return false;
    if (g->p66) pfx = 0x66;
    if (g->pf3) pfx = 0xf3;
    if (g->pf2) pfx = 0xf2;
    bool mem = g->mod != 3;
    unsigned q = (g->rex & 8) ? 8 : 4;
    c->kind = K_GENERIC;
    c->sse = c->reg_xmm = c->rm_xmm = true;
    c->hpfx = (uint8_t) pfx;
    c->hop = (uint8_t) op;
    // loads: reg written, r/m read
#define LOAD(size, full) do { c->acc = A_R; c->msize = (size); c->rm_rw = RW_R; c->reg_rw = (full) ? RW_W : RW_RW; } while (0)
    // stores: reg read, r/m written
#define STORE(size, full) do { c->acc = A_W; c->msize = (size); c->reg_rw = RW_R; c->rm_rw = (full) ? RW_W : RW_RW; } while (0)
    switch (op) {
        case 0x10: case 0x28:  // movups/movupd/movaps/movapd (load), movss/movsd
            if (op == 0x28 && pfx >= 0xf2) return false;
            if (pfx == 0xf3) { LOAD(4, mem); return true; }   // a register merges
            if (pfx == 0xf2) { LOAD(8, mem); return true; }
            c->hop = 0x10;  // unaligned: the address can't fault on the host
            LOAD(16, true);
            return true;
        case 0x11: case 0x29:  // stores of the same
            if (op == 0x29 && pfx >= 0xf2) return false;
            if (pfx == 0xf3) { STORE(4, false); return true; }
            if (pfx == 0xf2) { STORE(8, false); return true; }
            c->hop = 0x11;
            STORE(16, true);
            return true;
        case 0x6f: case 0x7f:  // movdqa/movdqu
            if (pfx != 0x66 && pfx != 0xf3) return false;
            c->hpfx = 0xf3;
            if (op == 0x6f) LOAD(16, true); else STORE(16, true);
            return true;
        case 0x12: case 0x16:  // movlps/movlpd/movhps/movhpd load, movhlps/movlhps
            if (pfx != 0 && pfx != 0x66) return false;
            if (pfx == 0x66 && !mem) return false;
            LOAD(8, false);
            return true;
        case 0x13: case 0x17:  // their stores
            if ((pfx != 0 && pfx != 0x66) || !mem) return false;
            STORE(8, true);
            return true;
        case 0x6e:             // movd/movq xmm, r/m32/64
            if (pfx != 0x66) return false;
            c->rm_xmm = false;
            LOAD(q, true);
            return true;
        case 0x7e:
            if (pfx == 0xf3) { LOAD(8, true); return true; }   // movq xmm, xmm/m64
            if (pfx != 0x66) return false;
            c->rm_xmm = false;                                 // movd/movq r/m, xmm
            STORE(q, true);
            return true;
        case 0xd6:             // movq xmm/m64, xmm
            if (pfx != 0x66) return false;
            STORE(8, true);
            return true;
        case 0xd7:             // pmovmskb r32, xmm
            if (pfx != 0x66 || mem) return false;
            c->reg_xmm = false; c->reg_gpr = true;
            c->reg_rw = RW_W; c->rm_rw = RW_R;
            return true;
        case 0x70:             // pshufd/pshufhw/pshuflw
            if (pfx == 0) return false;
            c->acc = A_R; c->msize = 16; c->memload = true;
            c->reg_rw = RW_W; c->rm_rw = RW_R;
            return true;
        case 0x58: case 0x59: case 0x5c: case 0x5e: case 0x5d: case 0x5f: case 0x51:
            // add, mul, sub, div, min, max, sqrt
            if (pfx >= 0xf2) {   // scalar: the rest of the register stays
                c->acc = A_R; c->msize = pfx == 0xf2 ? 8 : 4;
            } else {             // packed
                c->acc = A_R; c->msize = 16; c->memload = true;
            }
            c->reg_rw = op == 0x51 && pfx < 0xf2 ? RW_W : RW_RW; c->rm_rw = RW_R;
            return true;
        case 0x5a:             // cvtsd2ss/cvtss2sd
            if (pfx < 0xf2) return false;
            c->acc = A_R; c->msize = pfx == 0xf2 ? 8 : 4;
            c->reg_rw = RW_RW; c->rm_rw = RW_R;
            return true;
        case 0x2a:             // cvtsi2sd/cvtsi2ss xmm, r/m32/64
            if (pfx < 0xf2) return false;
            c->rm_xmm = false;
            c->acc = A_R; c->msize = q;
            c->reg_rw = RW_RW; c->rm_rw = RW_R;
            return true;
        case 0x2c:             // cvttsd2si/cvttss2si r32/64, xmm/m64/m32
            if (pfx < 0xf2) return false;
            c->reg_xmm = false; c->reg_gpr = true;
            c->acc = A_R; c->msize = pfx == 0xf2 ? 8 : 4;
            c->reg_rw = RW_W; c->rm_rw = RW_R;
            return true;
        case 0x2e: case 0x2f:  // ucomiss/comiss, ucomisd/comisd
            // ZF PF CF from the compare, OF SF AF cleared: the host's
            // instruction leaves the guest's flags, as a cmp does
            if (pfx != 0 && pfx != 0x66) return false;
            c->acc = A_R; c->msize = pfx == 0x66 ? 8 : 4;
            c->reg_rw = RW_R; c->rm_rw = RW_R;
            c->fw = F_ALL;
            return true;
        case 0xc2:             // cmpps/cmppd/cmpss/cmpsd: a mask per lane
            if (pfx >= 0xf2) {
                c->acc = A_R; c->msize = pfx == 0xf2 ? 8 : 4;
            } else {
                c->acc = A_R; c->msize = 16; c->memload = true;
            }
            c->reg_rw = RW_RW; c->rm_rw = RW_R;
            return true;
        case 0x54: case 0x55: case 0x56: case 0x57:  // and/andn/or/xor ps/pd
        case 0x14: case 0x15:                        // unpckl/unpckh ps/pd
            if (pfx != 0 && pfx != 0x66) return false;
            c->acc = A_R; c->msize = 16; c->memload = true;
            c->reg_rw = RW_RW; c->rm_rw = RW_R;
            return true;
    }
#undef LOAD
#undef STORE
    // 66 0f: the integer operations on two xmm registers
    static const uint8_t ints[] = {
        0x60, 0x61, 0x62, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x6b, 0x6c, 0x6d,
        0x74, 0x75, 0x76, 0xd4, 0xd5, 0xd8, 0xd9, 0xda, 0xdb, 0xdc, 0xdd, 0xde, 0xdf,
        0xe0, 0xe3, 0xe4, 0xe5, 0xe8, 0xe9, 0xea, 0xeb, 0xec, 0xed, 0xee, 0xef,
        0xf4, 0xf5, 0xf6, 0xf8, 0xf9, 0xfa, 0xfb, 0xfc, 0xfd, 0xfe,
    };
    if (pfx == 0x66 && memchr(ints, (int) op, sizeof(ints))) {
        c->acc = A_R; c->msize = 16; c->memload = true;
        c->reg_rw = RW_RW; c->rm_rw = RW_R;
        return true;
    }
    memset(c, 0, sizeof(*c));
    return false;
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
        // rep/bnd prefixes mean nothing on branches (`repz ret`, `bnd jmp`)
        if (reps && !(op >= 0x70 && op <= 0x7f) && op != 0xeb && op != 0xe9 &&
                op != 0xe8 && op != 0xc3 && op != 0xc2 && op != 0xff)
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
            case 0xff:
                if (g->rawreg == 2 || g->rawreg == 4) {
                    if (g->p66) return false;
                    c->kind = g->rawreg == 2 ? K_CALLI : K_JMPI;
                    return true;
                }
                if (reps) return false;
                // fallthrough
            case 0xfe:
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
            case 0xe8:
                c->kind = K_CALL;
                return true;
            case 0xc3: case 0xc2:
                c->kind = K_RET;
                return true;
        }
        return false;
    }

    if (g->map == 1) {
        if (classify_sse(g, c))
            return true;
        memset(c, 0, sizeof(*c));
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

// Which of its operands an instruction reads and writes (cls.reg_rw/rm_rw).
// A write narrower than 32 bits merges into the register: that's a read too.
static void set_rw(const struct gi *g, struct cls *c) {
    unsigned op = g->op;
    bool full = osize(g) >= 4;  // a 32/64-bit destination
    uint8_t wr = full ? RW_W : RW_RW;
    if (c->kind != K_GENERIC || c->sse)
        return;
    if (g->map == 0) {
        if (op < 0x40 && (op & 7) <= 3) {
            bool cmp = (op >> 3) == 7;
            bool byte = (op & 1) == 0;
            if ((op & 7) <= 1) {
                c->rm_rw = cmp ? RW_R : RW_RW;
                c->reg_rw = RW_R;
            } else {
                c->reg_rw = cmp ? RW_R : RW_RW;
                c->rm_rw = RW_R;
            }
            (void) byte;
            return;
        }
        switch (op) {
            case 0x84: case 0x85: c->reg_rw = c->rm_rw = RW_R; return;
            case 0x88: c->reg_rw = RW_R; c->rm_rw = RW_RW; return;
            case 0x89: c->reg_rw = RW_R; c->rm_rw = wr; return;
            case 0x8a: c->rm_rw = RW_R; c->reg_rw = RW_RW; return;
            case 0x8b: case 0x63: case 0x69: case 0x6b: c->rm_rw = RW_R; c->reg_rw = wr; return;
            case 0x80: case 0x81: case 0x83: c->rm_rw = g->rawreg == 7 ? RW_R : RW_RW; return;
            case 0xc7: c->rm_rw = wr; return;
            case 0xf6: case 0xf7:
                if (g->rawreg <= 1 || g->rawreg == 4 || g->rawreg == 5)
                    c->rm_rw = RW_R;
                return;
        }
        return;
    }
    if (g->map == 1) {
        if (op >= 0x40 && op <= 0x4f) { c->rm_rw = RW_R; c->reg_rw = RW_RW; return; }
        switch (op) {
            case 0xb6: case 0xb7: case 0xbe: case 0xbf: case 0xb8:
                c->rm_rw = RW_R; c->reg_rw = wr; return;
            case 0xaf: case 0xbc: case 0xbd: c->rm_rw = RW_R; c->reg_rw = RW_RW; return;
            case 0xa3: c->reg_rw = c->rm_rw = RW_R; return;
            case 0xab: case 0xb3: case 0xbb: case 0xa4: case 0xa5: case 0xac: case 0xad:
            case 0xb0: case 0xb1:
                c->reg_rw = RW_R; c->rm_rw = RW_RW; return;
            case 0xba: c->rm_rw = g->rawreg == 4 ? RW_R : RW_RW; return;
        }
    }
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

// Flags the guest code at addr may read before it writes them: all a block
// exit to there has to leave in the lazy state. Interrupts in between are
// fine: the rest a signal handler sees is about to be overwritten anyway.
static bool dbg_nosucc;
static bool dbg_allpoke;   // ISH_XLAT_ALLPOKE: check for interrupts at every exit
// ISH_XLAT_SITEDUMP=<file>: every access site's checks and lookups, with what it accesses
struct sitedump { uint64_t checks, lookups, ip; int32_t disp; int8_t kind, base, index, size; bool write, lead; };
#define SITEDUMP_MAX (1u << 19)
static struct sitedump *sitedump;
static _Atomic unsigned sitedump_n;
static const char *sitedump_path;
static void xlat_sitedump_exit(void) {
    if (sitedump == NULL)
        return;
    FILE *f = fopen(sitedump_path, "w");
    if (f == NULL)
        return;
    unsigned n = sitedump_n < SITEDUMP_MAX ? sitedump_n : SITEDUMP_MAX;
    for (unsigned i = 0; i < n; i++) {
        struct sitedump *d = &sitedump[i];
        if (d->checks)
            fprintf(f, "%llx %d %d %d %d %d %d %d %llu %llu\n", (unsigned long long) d->ip, d->kind, d->base, d->index,
                    d->disp, d->size, d->write, d->lead, (unsigned long long) d->checks, (unsigned long long) d->lookups);
    }
    fclose(f);
}
// ISH_XLAT_TLBDIRECT (experimental): which accesses look their page up in the TLB instead of
// checking a site: 1 all, 2 those through a pointer (not rsp, rbp or a fixed address), 3 rbp too
static int dbg_tlbdirect;
static bool dbg_sitestats; // ISH_XLAT_SITESTATS: count site checks and lookups (tlb_stats_exit() prints)
static bool dbg_nogroup;   // ISH_XLAT_NOGROUP: check every memory access on its own
// Guest bytes outside the block a translation depended on (what succ_live()
// read): a cached translation is only valid while they are the same.
#define XDEPS_MAX 32
struct xdep {
    uint64_t addr;
    uint8_t len;
    uint8_t bytes[15];
};
static __thread struct xdep xdeps[XDEPS_MAX];
static __thread unsigned xdeps_n;
static __thread bool xdeps_overflow;

static void xdep_note(uint64_t addr, const uint8_t *bytes, unsigned len) {
    if (xdeps_n == XDEPS_MAX) {
        xdeps_overflow = true;
        return;
    }
    struct xdep *d = &xdeps[xdeps_n++];
    d->addr = addr;
    d->len = (uint8_t) len;
    memcpy(d->bytes, bytes, len);
}

static uint16_t succ_live(uint64_t addr) {
    if (xlog_tlb == NULL || dbg_nosucc)
        return F_ALL;
    uint16_t live = 0, written = 0;
    for (int k = 0; k < 16; k++) {
        struct xinsn in = {.ip = addr, .len = 15};
        unsigned room = 0x1000 - (unsigned) (addr & 0xfff);
        if (room < in.len)
            in.len = (uint8_t) room;
        if (!tlb_read(xlog_tlb, addr, in.bytes, in.len)) {
            xdeps_overflow = true;  // what it found depends on a fault: don't cache
            break;
        }
        xdep_note(addr, in.bytes, in.len);
        struct gi g;
        struct cls c;
        if (!gdecode_n(&in, &g, false) || !classify(&g, &c))
            break;
        shift_flags(&g, &in, &c);
        live |= c.fr & ~written;
        written |= c.fw;
        if (written == F_ALL)
            return live;
        if (c.kind == K_JCC || c.kind == K_JMP)
            break;
        addr = g.next;
    }
    return live | (F_ALL & ~written);
}

// ---------------------------------------------------------------- operands

// Where an operand lives on the host: a register, or a cpu slot it is
// loaded into r14/r15 from.
struct hop {
    int host;     // host register (for a slot: the scratch it is loaded into)
    int slot;     // guest r8-r15: cpu offset, else -1
    uint8_t rw;   // RW_*
};

struct plan {
    struct hop reg, rm;   // modrm.reg (if a GPR), modrm.rm (if a register)
    bool mem;             // the r/m operand is memory: (%r13,%r9), see e_mem_insn()
    bool norex;           // uses ah..bh: the encoding can't have a REX prefix
    bool rex;             // needs one
};

static int32_t slot_off(int g) {
    return CPU_OFF(regs) + 8 * g;
}

// Maps guest register gr (an operand at 8 bits if byte) to the host.
static void map_reg(const struct gi *g, int gr, bool byte, struct hop *h, struct plan *p,
                    const int *scratch, unsigned *nscratch, int *scratch_for) {
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
        scratch_for[gr - 8] = scratch[(*nscratch)++];
    h->host = scratch_for[gr - 8];
    h->slot = slot_off(gr);
    p->rex = true;
}

static bool plan_generic(const struct gi *g, const struct cls *c, struct plan *p) {
    memset(p, 0, sizeof(*p));
    int scratch_for[8] = {-1, -1, -1, -1, -1, -1, -1, -1};
    p->mem = g->mod != 3;
    static const int scratch[] = {R14, R15};
    unsigned nscratch = 0;
    p->reg.host = -1; p->reg.slot = -1;
    p->rm.host = -1; p->rm.slot = -1;
    p->reg.rw = c->reg_rw ? c->reg_rw : RW_RW;
    p->rm.rw = c->rm_rw ? c->rm_rw : RW_RW;
    if (c->reg_gpr)
        map_reg(g, g->reg, c->reg_byte, &p->reg, p, scratch, &nscratch, scratch_for);
    if (!p->mem && c->rm_xmm)
        p->rm.host = 1;  // xmm1
    else if (!p->mem)
        map_reg(g, g->rm, c->rm_byte, &p->rm, p, scratch, &nscratch, scratch_for);
    else
        p->rex = true;  // r13
    if (g->rex & 8)
        p->rex = true;
    if (p->norex && p->rex)
        return false;
    return true;
}

// ---------------------------------------------------------------- context

struct xctx {
    struct xasm *a;
    struct fiber_block *block;
    uintptr_t *code;          // the block's stream: what the translation points into
    const uintptr_t *plan;    // the stream as gen made it: what it reads (the same,
                              // but for a promotion, see xjit_promote())
    uint16_t valid, dirty;  // flags valid in the host's eflags / not yet in the lazy state
    // the patchable jumps of the block's exits: their stream slot, and where
    // the rel32 is in the main section
    struct { unsigned slot; uint32_t field; } links[2];
    unsigned nlinks;
    unsigned ndata;   // words of data (guesses), in the data region
};

static void sync_dirty(struct xctx *x) {
    e_sync_flags(x->a, x->dirty, x->valid);
    x->dirty = 0;
}


// movabs $&block->code[slot] / $block, %reg
static void e_movabs_fix(struct xasm *a, int reg, uint8_t kind, uintptr_t target) {
    e_rex(a, true, -1, -1, reg, false);
    b1(a, (uint8_t) (0xb8 + (reg & 7)));
    add_fix(a, kind, target);
    b8(a, 0);
}
static void e_movabs_slot(struct xctx *x, int reg, unsigned slot) {
    e_movabs_fix(x->a, reg, FIX_SLOT64, slot);
}

// LOCAL_last_block = this block: the gadgets that leave it (call, ret, an
// unchained jump) go to the run loop, which chains last_block's jumps --
// and a block entered through a link (xjit_link()) hasn't said it ran.
static void e_note_block(struct xctx *x) {
    e_movabs_fix(x->a, R14, FIX_BLOCK64, 0);
    e_mov_store(x->a, XMEM(H_CPU, (int32_t) offsetof(struct fiber_frame, last_block)), R14);
}

// Hands over to the gadget stream at slot s.
static void e_to_stream(struct xctx *x, unsigned s) {
    e_note_block(x);
    e_movabs_slot(x, H_IP, s + 1);
    // jmp *-8(%r9)
    b1(x->a, 0x41); b1(x->a, 0xff); b1(x->a, 0x61); b1(x->a, 0xf8);
}

// Leaves the block through the chainable stream slot s (jmp/jcc arguments).
static void e_exit_slot(struct xctx *x, unsigned s) {
    e_movabs_slot(x, H_IP, s);
    e_mov_load(x->a, H_IP, XMEM(H_IP, 0));
    e_jmp_abs(x->a, (const void *) fiber_ret_chain);
}

// Leaves the block for the target in stream slot s, live: the flags the
// target may read. A jmp chaining can point straight at the target's
// translation (xjit_link()); until then it goes the way of e_exit_slot().
// poke: check for a pending interrupt first. Every loop of chained blocks
// has an edge back to an address no higher, or an indirect jump or a ret
// (which check), so the forward edges needn't: the code between two checks
// is finite.
static void e_exit_chain(struct xctx *x, unsigned s, uint16_t live, bool poke) {
    struct xasm *a = x->a;
    e_sync_flags(a, x->dirty & live, x->valid);
    unsigned l_stub = new_label(a);
    // an interrupt pending: leave through the gadgets, which see to it
    if (poke) {
        e_mov_load(a, R14, XMEM(H_CPU, CPU_OFF(poked_ptr)));
        b1(a, 0x41); b1(a, 0x80); b1(a, 0x3e); b1(a, 0x00);  // cmpb $0, (%r14)
        e_jcc_lab(a, 5, l_stub);
    }
    // the rel32 aligned, so that patching it is one atomic store; the
    // padding one nop (it runs every time)
    switch (3 - ((xpos(a) + 4) & 3)) {
        case 1: b1(a, 0x90); break;
        case 2: b1(a, 0x66); b1(a, 0x90); break;
        case 3: b1(a, 0x0f); b1(a, 0x1f); b1(a, 0x00); break;
    }
    b1(a, 0xe9);
    if (x->nlinks < 2)
        x->links[x->nlinks++] = (typeof(x->links[0])) {s, xpos(a)};
    rel_lab(a, l_stub);
    a->cur = SEC_COLD;
    lbind(a, l_stub);
    e_note_block(x);
    e_exit_slot(x, s);
    a->cur = SEC_MAIN;
}

// An indirect jump to the guest address in r10: the block the run loop's
// cache has for it, looked up inline as fiber_ret_lookup does, so that each
// jump site has a jmp of its own for the branch predictor (an interpreter's
// dispatch is one site per opcode handler; through the shared lookup they
// all mispredicted). Misses, pokes and the timer tick go the shared way.
static void e_dispatch_r10(struct xctx *x) {
    struct xasm *a = x->a;
    unsigned l_slow = new_label(a);
    // pending interrupt
    e_mov_load(a, R14, XMEM(H_CPU, CPU_OFF(poked_ptr)));
    b1(a, 0x41); b1(a, 0x80); b1(a, 0x3e); b1(a, 0x00);              // cmpb $0, (%r14)
    e_jcc_lab(a, 5, l_slow);
    // the run loop's timer tick (fiber_ret_lookup does the same)
    e_mov_load(a, R14, XMEM(H_CPU, CPU_OFF(cycle)));
    e_rex(a, true, -1, -1, R14, false); b1(a, 0xff); b1(a, modrm_b(3, 0, R14));      // inc %r14
    e_rex(a, false, -1, -1, R14, false); b1(a, 0xf7); b1(a, modrm_b(3, 0, R14)); b4(a, 0x3ff);  // test
    e_jcc_lab(a, 4, l_slow);                                          // jz
    e_mov_store(a, XMEM(H_CPU, CPU_OFF(cycle)), R14);
    // the cache valid? tlb->block_cache_gen == mmu->asbestos->invalidate_gen
    e_mov_load(a, R14, XMEM(H_TLB, TLBREL(mmu)));
    e_mov_load(a, R14, XMEM(R14, (int32_t) offsetof(struct mmu, asbestos)));
    e_rm(a, false, 0x8b, R14, XMEM(R14, (int32_t) offsetof(struct asbestos, invalidate_gen)));
    e_rm(a, false, 0x3b, R14, XMEM(H_TLB, TLBREL(block_cache_gen)));
    e_jcc_lab(a, 5, l_slow);
    // block = block_cache[(ip ^ (ip >> 12)) & (FIBER_CACHE_SIZE - 1)]
    e_mov_rr(a, R14, R10);
    e_rex(a, true, -1, -1, R14, false); b1(a, 0xc1); b1(a, modrm_b(3, 5, R14)); b1(a, 12);  // shr $12
    e_rr(a, true, 0x31, R10, R14);                                   // xor %r10, %r14
    e_rex(a, false, -1, -1, R14, false); b1(a, 0x81); b1(a, modrm_b(3, 4, R14)); b4(a, FIBER_CACHE_SIZE - 1);
    e_mov_load(a, R14, (struct xmem) {H_TLB, R14, 3, TLBREL(block_cache)});
    e_rr(a, true, 0x85, R14, R14);                                   // test %r14, %r14
    e_jcc_lab(a, 4, l_slow);
    e_rm(a, true, 0x3b, R10, XMEM(R14, (int32_t) offsetof(struct fiber_block, addr)));
    e_jcc_lab(a, 5, l_slow);
    // enter it the gadget way: r9 = &code[1], jmp *code[0]
    e_lea(a, H_IP, XMEM(R14, (int32_t) offsetof(struct fiber_block, code) + 8));
    b1(a, 0x41); b1(a, 0xff); b1(a, 0x61); b1(a, 0xf8);              // jmp *-8(%r9)
    a->cur = SEC_COLD;
    lbind(a, l_slow);
    e_mov_rr(a, H_IP, R10);
    e_jmp_abs(a, (const void *) fiber_ret_lookup);
    a->cur = SEC_MAIN;
}

// A return to the guest address in r10, through the return cache the call
// gadgets (and translated calls) fill: the entry points at the calling
// block's call arguments, whose fourth word is its chainable return slot.
static void e_ret_dispatch(struct xctx *x) {
    struct xasm *a = x->a;
    unsigned l_generic = new_label(a), l_unchained = new_label(a);
    int32_t cache = (int32_t) offsetof(struct fiber_frame, ret_cache);
    // r14 = (ip & 0xffff) >> 4
    e_rex(a, false, R14, -1, R10, false); b1(a, 0x0f); b1(a, 0xb7); b1(a, modrm_b(3, R14, R10));  // movzwl %r10w, %r14d
    e_rex(a, false, -1, -1, R14, false); b1(a, 0xc1); b1(a, modrm_b(3, 5, R14)); b1(a, 4);
    e_mov_load(a, R15, (struct xmem) {H_CPU, R14, 3, cache});
    e_rr(a, true, 0x85, R15, R15);
    e_jcc_lab(a, 4, l_generic);
    // the call it returns from (the fake return address, marker bit off)?
    e_mov_load(a, R14, XMEM(R15, 16));
    e_rex(a, true, -1, -1, R14, false); b1(a, 0x0f); b1(a, 0xba); b1(a, modrm_b(3, 6, R14)); b1(a, 63);  // btr $63
    e_rr(a, true, 0x39, R14, R10);                                   // cmp %r14, %r10
    e_jcc_lab(a, 5, l_generic);
    e_mov_load(a, H_IP, XMEM(R15, 24));                              // the return slot
    e_rex(a, true, -1, -1, H_IP, false); b1(a, 0x0f); b1(a, 0xba); b1(a, modrm_b(3, 4, H_IP)); b1(a, 63);  // bt $63
    e_jcc_lab(a, 2, l_unchained);                                    // jc
    e_mov_load(a, R14, XMEM(H_CPU, CPU_OFF(poked_ptr)));
    b1(a, 0x41); b1(a, 0x80); b1(a, 0x3e); b1(a, 0x00);              // cmpb $0, (%r14)
    e_jcc_lab(a, 5, l_generic);
    e_lea(a, H_IP, XMEM(H_IP, 8));
    b1(a, 0x41); b1(a, 0xff); b1(a, 0x61); b1(a, 0xf8);              // jmp *-8(%r9)
    a->cur = SEC_COLD;
    // not chained yet: tell the run loop which block's slot to chain
    lbind(a, l_unchained);
    e_mov_load(a, R14, XMEM(R15, 8));
    e_mov_store(a, XMEM(H_CPU, (int32_t) offsetof(struct fiber_frame, last_block)), R14);
    e_mov_rr(a, H_IP, R10);
    e_jmp_abs(a, (const void *) fiber_ret);
    a->cur = SEC_MAIN;
    lbind(a, l_generic);
    e_dispatch_r10(x);
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

// The TLB index of the address in r13 (TLB_INDEX), times the entry size,
// into r14. Clobbers r15. Only the lookup after a site's own page missed
// computes it.
static void e_tlb_index(struct xasm *a) {
    static const uint8_t shifts[2][2] = {{R14, 12 + TLB_BITS - 5}, {R15, 12 - 5}};
    for (int i = 0; i < 2; i++) {
        unsigned r = shifts[i][0], shift = shifts[i][1];
        if (have_bmi2) {
            // rorx $shift, %r13, %rN: the bits it rotates in get masked off
            b1(a, 0xc4); b1(a, 0x43); b1(a, 0xfb); b1(a, 0xf0); b1(a, modrm_b(3, r, R13)); b1(a, (uint8_t) shift);
        } else {
            e_mov_rr(a, r, R13);
            e_rex(a, true, -1, -1, r, false); b1(a, 0xc1); b1(a, modrm_b(3, 5, r)); b1(a, (uint8_t) shift);
        }
    }
    e_rex(a, true, R15, -1, R14, false); b1(a, 0x31); b1(a, modrm_b(3, R15, R14));  // xor %r15, %r14
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
    uint64_t imm;   // K_B_PUSHRET: the return address; K_B_POPRET: how far rsp moves
};

// The memory operand in the host: guest address + data_minus_addr
static struct xmem hmem(const struct plan *p) {
    (void) p;
    return (struct xmem) {R13, R9, 0, 0};
}

static void e_slot_loads(struct xasm *a, const struct plan *p) {
    // one slot as both operands: one scratch, loaded if either reads it
    bool same = p->rm.slot >= 0 && p->rm.host == p->reg.host;
    if (p->reg.slot >= 0 && ((p->reg.rw & RW_R) || (same && (p->rm.rw & RW_R))))
        e_mov_load(a, p->reg.host, XMEM(H_CPU, p->reg.slot));
    if (p->rm.slot >= 0 && !same && (p->rm.rw & RW_R))
        e_mov_load(a, p->rm.host, XMEM(H_CPU, p->rm.slot));
}
static void e_slot_stores(struct xasm *a, const struct plan *p) {
    bool same = p->rm.slot >= 0 && p->rm.host == p->reg.host;
    if (p->reg.slot >= 0 && ((p->reg.rw & RW_W) || (same && (p->rm.rw & RW_W))))
        e_mov_store(a, XMEM(H_CPU, p->reg.slot), p->reg.host);
    if (p->rm.slot >= 0 && !same && (p->rm.rw & RW_W))
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
    struct xmem m = hmem(p);
    if (!p->norex) {
        if (p->mem)
            e_rex(a, g->rex & 8, c->reg_gpr ? reg : -1, m.index, m.base, g->rex != 0 || p->rex);
        else
            e_rex(a, g->rex & 8, c->reg_gpr ? reg : -1, -1, p->rm.host, g->rex != 0 || p->rex);
    }
    if (g->map == 1) b1(a, 0x0f);
    b1(a, g->op);
    if (p->mem)
        e_modrm_mem(a, reg, m);
    else
        b1(a, modrm_b(3, reg, p->rm.host));
    bn(a, &bd->in->bytes[g->immoff], g->immlen);
}

// movdqu between host xmm x and guest xmm n in the cpu struct
static void e_xmm_move(struct xasm *a, bool store, int x, unsigned n) {
    b1(a, 0xf3);
    e_rm2(a, false, 0x0f, store ? 0x7f : 0x6f, x, XMEM(H_CPU, CPU_OFF(xmm) + 16 * (int32_t) n));
}

static void e_sse_insn(struct xasm *a, const struct body *bd) {
    const struct gi *g = bd->g;
    const struct cls *c = bd->c;
    const struct plan *p = bd->p;
    struct xmem m = hmem(p);
    if (c->reg_xmm && (p->reg.rw & RW_R))
        e_xmm_move(a, false, 0, g->reg);
    if (c->rm_xmm && !p->mem && (p->rm.rw & RW_R))
        e_xmm_move(a, false, 1, g->rm);
    bool regform = !p->mem || c->memload;
    if (p->mem && c->memload) {
        // movdqu (%r13,%r9), %xmm1
        b1(a, 0xf3);
        e_rm2(a, false, 0x0f, 0x6f, 1, m);
    }
    e_slot_loads(a, p);
    int reg = c->reg_xmm ? 0 : p->reg.host;
    int rm = regform ? (c->rm_xmm || c->memload ? 1 : p->rm.host) : -1;
    if (c->hpfx) b1(a, c->hpfx);
    // REX.W only sizes a general register operand (movq, cvtsi2sd, cvttsd2si)
    bool w = (g->rex & 8) && (c->reg_gpr || !c->rm_xmm);
    if (regform)
        e_rex(a, w, c->reg_xmm ? -1 : reg, -1, (c->rm_xmm || c->memload) ? -1 : rm, false);
    else
        e_rex(a, w, c->reg_xmm ? -1 : reg, m.index, m.base, false);
    b1(a, 0x0f);
    b1(a, c->hop);
    if (regform)
        b1(a, modrm_b(3, reg, rm));
    else
        e_modrm_mem(a, reg, m);
    bn(a, &bd->in->bytes[g->immoff], g->immlen);
    e_slot_stores(a, p);
    if (c->reg_xmm && (p->reg.rw & RW_W))
        e_xmm_move(a, true, 0, g->reg);
    if (c->rm_xmm && !p->mem && (p->rm.rw & RW_W))
        e_xmm_move(a, true, 1, g->rm);
}

static void e_body(struct xasm *a, const struct body *bd) {
    const struct gi *g = bd->g;
    switch (bd->c->kind) {
        case K_GENERIC:
            if (bd->c->sse) {
                e_sse_insn(a, bd);
                break;
            }
            e_slot_loads(a, bd->p);
            e_generic_insn(a, bd);
            e_slot_stores(a, bd->p);
            break;
        case K_PUSH: {
            int gr = (g->op & 7) | ((g->rex & 1) ? 8 : 0);
            int h = gr == 4 ? R8 : gr;
            if (gr >= 8) {
                e_mov_load(a, R15, XMEM(H_CPU, slot_off(gr)));
                h = R15;
            }
            e_mov_store(a, hmem(NULL), h);
            break;
        }
        case K_PUSHI: {
            int32_t imm = g->immlen == 1 ? (int8_t) bd->in->bytes[g->immoff] :
                (int32_t) (bd->in->bytes[g->immoff] | bd->in->bytes[g->immoff + 1] << 8 |
                           bd->in->bytes[g->immoff + 2] << 16 | (uint32_t) bd->in->bytes[g->immoff + 3] << 24);
            e_rm(a, true, 0xc7, 0, hmem(NULL));
            b4(a, (uint32_t) imm);
            break;
        }
        case K_POP: {
            int gr = (g->op & 7) | ((g->rex & 1) ? 8 : 0);
            if (gr >= 8) {
                e_mov_load(a, R15, hmem(NULL));
                e_mov_store(a, XMEM(H_CPU, slot_off(gr)), R15);
            } else {
                e_mov_load(a, gr == 4 ? R8 : gr, hmem(NULL));
            }
            break;
        }
        case K_LEAVE:
            e_mov_load(a, R15, hmem(NULL));
            break;
        case K_B_LOADT: case K_B_POPRET:
            e_mov_load(a, R10, hmem(NULL));
            break;
        case K_B_PUSHRET:
            e_movabs(a, R15, bd->imm);
            e_mov_store(a, hmem(NULL), R15);
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
            e_mov_rr(a, RBP, R15);
            break;
        case K_B_PUSHRET:
            e_lea(a, R8, XMEM(R8, -8));
            break;
        case K_B_POPRET:
            e_lea(a, R8, XMEM(R8, (int32_t) bd->imm));
            break;
    }
}

// ---------------------------------------------------------------- memory

// An instruction with a memory operand whose guest address is in r13:
// translate it (keeping the flags in `keep` alive across the lookup), run
// the body on the host address. Flags in `sync` are written to the lazy
// state before anything here can fault.
//
// Each access site has an entry in the thread's tlb->xsite: the page it
// found last time and that page's data_minus_addr. The operand is
// (%r13,%r9) with r9 that data_minus_addr, when the operand is all on that
// page (and the TLB isn't stale): the check is a subtraction and a compare,
// not a lookup, and the access waits only for its address. Otherwise the
// TLB is looked up the usual way and the site learns the page.
// A group (struct mgrp): accesses of a run with the same base and index,
// which no instruction between them changes, all on one page. The first
// one (lead) checks [r13 + lo, r13 + hi) and needs the page writable if any
// of them writes; the others only load its data_minus_addr. If its page
// can't be had for the whole range, the lead runs in its gadgets, and the
// block goes on there.
struct mgrp {
    int lo, hi;      // lead: the range, from its own address
    bool write;      // lead: some access writes
    unsigned site;   // set by the lead's e_mem_insn()
};

static unsigned new_site(void) {
    return atomic_fetch_add_explicit(&tlb_xsites, 1, memory_order_relaxed) %
           array_size(((struct tlb *) 0)->xsite);
}
#define SITE_PAGE(n) XMEM(H_TLB, TLBREL(xsite) + (int32_t) (n) * 16)
#define SITE_DMA(n) XMEM(H_TLB, TLBREL(xsite) + (int32_t) (n) * 16 + 8)

// TLB lookup for [r13 + lo, r13 + hi), on one page: on a hit r9 = its
// data_minus_addr and the site learns the page; else to l_miss.
static void e_range_lookup(struct xasm *a, int lo, int hi, bool write, struct xmem site_page,
                           struct xmem site_dma, unsigned l_miss, int learn) {
    int32_t ent = write ? TLBENT(page_if_writable) : TLBENT(page);
    e_tlb_index(a);
    // the page of the range's first byte, when it isn't r13's
    if (lo < 0) {
        e_lea(a, R15, XMEM(R13, lo));
        e_rex(a, true, -1, -1, R15, false); b1(a, 0x81); b1(a, modrm_b(3, 4, R15)); b4(a, (uint32_t) -4096);
        e_rm(a, true, 0x3b, R15, (struct xmem) {H_TLB, R14, 0, ent});
        e_jcc_lab(a, 5, l_miss);  // jne
    }
    // the page of its last byte, so that crossing a page misses
    e_lea(a, R15, XMEM(R13, hi - 1));
    e_rex(a, true, -1, -1, R15, false); b1(a, 0x81); b1(a, modrm_b(3, 4, R15)); b4(a, (uint32_t) -4096);
    e_rm(a, true, 0x0b, R15, XMEM(H_TLB, TLBREL(stale)));
    e_rm(a, true, 0x3b, R15, (struct xmem) {H_TLB, R14, 0, ent});
    e_jcc_lab(a, 5, l_miss);  // jne
    // a hit: r15 is the page; the site learns it
    e_mov_load(a, R9, (struct xmem) {H_TLB, R14, 0, TLBENT(data_minus_addr)});
    if (learn & 1)
        e_mov_store(a, site_page, R15);
    if (learn & 2)
        e_mov_store(a, site_dma, R9);
}

// A group's access after the lead's: the lead checked its page.
static void e_mem_member(struct xctx *x, const struct body *bd, const struct mgrp *mg, bool load_flags) {
    struct xasm *a = x->a;
    if (dbg_sitestats) {
        // without touching the flags
        e_mov_load(a, R9, XMEM(H_TLB, TLBREL(xs_member)));
        e_lea(a, R9, XMEM(R9, 1));
        e_mov_store(a, XMEM(H_TLB, TLBREL(xs_member)), R9);
    }
    e_mov_load(a, R9, SITE_DMA(mg->site));
    if (load_flags)
        e_call_abs(a, load_flags_stub);
    e_body(a, bd);
    e_after(a, bd);
}

static void e_mem_insn(struct xctx *x, const struct body *bd, unsigned size, bool write,
                       uint16_t keep, bool load_flags, struct mgrp *mg) {
    struct xasm *a = x->a;
    unsigned l_miss = new_label(a), l_back = new_label(a), l_cross = new_label(a),
             l_after = new_label(a), l_orig = new_label(a), l_lookup = new_label(a);
    uint16_t sync = keep & x->dirty;
    int lo = mg ? mg->lo : 0, hi = mg ? mg->hi : (int) size;
    bool gwrite = mg ? mg->write : write;

    if (keep)
        e_save_flags(a);
    const struct gi *dg = bd->g;
    bool ptr = bd->c->kind == K_GENERIC && dg->base != BASE_RIP && dg->base != 4 &&
               (dg->base != 5 || dbg_tlbdirect == 3) && (dg->base >= 0 || dg->index >= 0);
    bool direct = dbg_tlbdirect == 1 || (dbg_tlbdirect >= 2 && ptr);
    // A group's members load the lead's data_minus_addr from its site; the page
    // goes with it, for a site check that shares the entry (sites wrap).
    int learn = direct ? (mg ? 3 : 0) : 3;
    unsigned n = learn ? new_site() : 0;
    if (mg)
        mg->site = n;
    struct xmem site_page = SITE_PAGE(n), site_dma = SITE_DMA(n);
    struct sitedump *sd = NULL;
    if (sitedump != NULL) {
        unsigned k = atomic_fetch_add(&sitedump_n, 1);
        if (k < SITEDUMP_MAX) {
            sd = &sitedump[k];
            *sd = (struct sitedump) {.ip = dg->ip, .disp = (int32_t) dg->disp, .kind = (int8_t) bd->c->kind,
                .base = (int8_t) (bd->c->kind == K_GENERIC ? dg->base : 4), .index = (int8_t) (bd->c->kind == K_GENERIC ? dg->index : -1),
                .size = (int8_t) size, .write = write, .lead = mg != NULL};
            e_movabs(a, R15, (uint64_t) (uintptr_t) &sd->checks);
            b1(a, 0x49); b1(a, 0xff); b1(a, 0x07);   // incq (%r15)
        }
    }
    if (dbg_sitestats) {
        e_rm(a, true, 0xff, 0, XMEM(H_TLB, TLBREL(xs_check)));  // incq
        if (bd->c->kind != K_GENERIC || dg->base == 4)
            e_rm(a, true, 0xff, 0, XMEM(H_TLB, TLBREL(xs_check_sp)));
    }
    if (direct) {
        e_range_lookup(a, lo, hi, gwrite, site_page, site_dma, l_miss, learn);
    } else {
        if (lo == 0)
            e_rr(a, true, 0x89, R13, R15);
        else
            e_lea(a, R15, XMEM(R13, lo));
        e_rm(a, true, 0x2b, R15, site_page);
        e_rm(a, true, 0x0b, R15, XMEM(H_TLB, TLBREL(stale)));
        e_rex(a, true, -1, -1, R15, false); b1(a, 0x81); b1(a, modrm_b(3, 7, R15)); b4(a, (uint32_t) (4096 - (hi - lo)));
        e_jcc_lab(a, 7, l_lookup);  // ja
        e_mov_load(a, R9, site_dma);
    }
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
    lbind(a, l_lookup);
    if (sd != NULL) {
        e_movabs(a, R14, (uint64_t) (uintptr_t) &sd->lookups);
        b1(a, 0x49); b1(a, 0xff); b1(a, 0x06);   // incq (%r14)
    }
    if (dbg_sitestats)
        e_rm(a, true, 0xff, 0, XMEM(H_TLB, TLBREL(xs_lookup)));
    if (!direct) {
        e_range_lookup(a, lo, hi, gwrite, site_page, site_dma, l_miss, learn);
        e_jmp_lab(a, l_back);
    }

    lbind(a, l_miss);
    // lea lo(%r13), %r14; lea hi-1(%r13), %r15; xor %r14, %r15; cmp $0xfff, %r15; ja cross
    e_lea(a, R14, XMEM(R13, lo));
    e_lea(a, R15, XMEM(R13, hi - 1));
    e_rr(a, true, 0x31, R14, R15);
    e_rex(a, true, -1, -1, R15, false); b1(a, 0x81); b1(a, modrm_b(3, 7, R15)); b4(a, 0xfff);
    e_jcc_lab(a, 7, l_cross);  // ja
    if (sync) {
        e_restore_flags(a);
        e_sync_flags(a, sync, x->valid);
    }
    // lea orig(%rip), %r9
    b1(a, 0x4c); b1(a, 0x8d); b1(a, 0x0d); rel_lab(a, l_orig);
    e_call_abs(a, write ? (const void *) handle_write_miss : (const void *) handle_read_miss);
    if (mg == NULL) {
        // r13 is the host address now
        b1(a, 0x41); b1(a, (uint8_t) (0xb8 + (R9 & 7))); b4(a, 0);  // movl $0, %r9d
        e_jmp_lab(a, l_back);
    } else {
        // The others need the page too: the address again, and the TLB
        // has its page now, if the page is enough for all of them.
        switch (bd->c->kind) {
            case K_PUSH: case K_PUSHI:
                e_lea(a, R13, XMEM(R8, -8));
                break;
            case K_POP:
                e_mov_rr(a, R13, R8);
                break;
            default:
                e_addr(x, bd->g, R13, true);
                break;
        }
        e_range_lookup(a, lo, hi, gwrite, site_page, site_dma, l_cross, learn);
        e_jmp_lab(a, l_back);
    }

    // Crossing a page: rare, so no second copy of the body for it. The
    // instruction runs in its gadgets instead, from its first stream slot
    // (the gadgets handle crossing pages), and the block goes on there.
    lbind(a, l_cross);
    if (keep)
        e_restore_flags(a);
    e_sync_flags(a, sync, x->valid);
    e_note_block(x);
    e_movabs_slot(x, H_IP, bd->in->s0 + 1);
    e_jmp_abs(a, (const void *) x->plan[bd->in->s0]);
    // the faulting instruction's address, where the fault path reads it
    while (xpos(a) & 7)
        b1(a, 0xcc);
    lbind(a, l_orig);
    b8(a, bd->g->ip);
    a->cur = SEC_MAIN;
}

// ---------------------------------------------------------------- insns

// Emits one translated instruction; live: flags live after it, inb: those
// of them the block's own native instructions read. mg: its memory access
// leads a group (member: or is one of the others).
static void e_native(struct xctx *x, const struct xinsn *in, const struct gi *g,
                     const struct cls *c, const struct plan *p, uint16_t live, uint16_t inb,
                     struct mgrp *mg, bool member) {
    struct xasm *a = x->a;
    struct body bd = {g, c, in, c->kind == K_GENERIC ? p : NULL};
    uint16_t live_in = c->fr | (live & ~c->fw);
    bool load_flags = (c->fr & ~x->valid) != 0;
    if (load_flags) {
        // the lazy state has some of what it reads: complete it, then load
        // everything from it (after any lookup, which would clobber them)
        sync_dirty(x);
        x->valid = 0;
    }

    bool has_mem = (c->kind == K_GENERIC && p->mem) || c->kind == K_PUSH || c->kind == K_PUSHI ||
                   c->kind == K_POP || c->kind == K_LEAVE;
    uint16_t held = x->valid & live_in;
    if (has_mem && !member && !load_flags && held != 0 && (held & (c->fr | (inb & ~c->fw))) == 0) {
        // Live only for where the block leaves: write them to the lazy state
        // now (leaving would) rather than keep them across this access and
        // the ones after it. Before the address, which may use r14/r15.
        e_sync_flags(a, x->dirty & held, x->valid);
        x->dirty = 0;
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

    if (mem && member) {
        // no check, so nothing to keep the flags from
        e_mem_member(x, &bd, mg, load_flags);
        if (load_flags)
            x->valid = F_ALL;
    } else if (mem) {
        uint16_t keep = load_flags ? 0 : (x->valid & live_in);
        e_mem_insn(x, &bd, size, write, keep, load_flags, mg);
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
    e_movabs_slot(x, H_IP, s0 + 1);
    unsigned s = s0;
    while (s < s1) {
        const struct tpl *t = tpl_lookup(x->plan[s]);
        e_template(x->a, t);
        if (t->pop < 0)
            break;
        s += 1 + (unsigned) t->pop;
    }
}

// ---------------------------------------------------------------- stats

static _Atomic uint64_t st_native, st_tpl, st_stream;
static size_t xc_limit = (size_t) 256 << 20;
static _Atomic uint64_t st_xc_hits, st_xc_misses, st_xc_inserts, st_xc_rejects;
static _Atomic uint64_t st_promote_ok, st_promote_differs, st_promote_failed, st_counted;

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

static uint64_t xs_used(bool count);
static void xlat_dump_stats(FILE *out) {
    fprintf(out, "[xlat] xcache: hits=%llu misses=%llu inserts=%llu rejected(full)=%llu used=%llu entries=%llu\n",
            (unsigned long long) st_xc_hits, (unsigned long long) st_xc_misses,
            (unsigned long long) st_xc_inserts, (unsigned long long) st_xc_rejects,
            (unsigned long long) xs_used(false), (unsigned long long) xs_used(true));
    fprintf(out, "[xlat] tiers: counted=%llu promoted=%llu differs=%llu failed=%llu\n",
            (unsigned long long) st_counted, (unsigned long long) st_promote_ok,
            (unsigned long long) st_promote_differs, (unsigned long long) st_promote_failed);
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

// ---------------------------------------------------------------- profiling

#ifdef _WIN32
// ISH_PROF (platform/windows/prof.c): which translated block a pc is in
void px_prof_hooks(void (*describe)(FILE *f, uintptr_t pc, unsigned count, uint64_t total),
                   void (*finish)(void));

struct prof_block {
    uint8_t *mem;
    uint32_t main_len, total;
    uint64_t addr;
};
static struct prof_block *prof_blocks;
static unsigned prof_nblocks, prof_capblocks;
static pthread_mutex_t prof_lock = PTHREAD_MUTEX_INITIALIZER;
static bool prof_on;

static void prof_note(uint8_t *mem, size_t main_len, size_t total, uint64_t addr) {
    if (!prof_on)
        return;
    pthread_mutex_lock(&prof_lock);
    if (prof_nblocks == prof_capblocks) {
        unsigned cap = prof_capblocks ? prof_capblocks * 2 : 1024;
        struct prof_block *bigger = realloc(prof_blocks, cap * sizeof(*bigger));
        if (bigger != NULL) {
            prof_blocks = bigger;
            prof_capblocks = cap;
        }
    }
    if (prof_nblocks < prof_capblocks)
        prof_blocks[prof_nblocks++] = (struct prof_block) {mem, (uint32_t) main_len, (uint32_t) total, addr};
    pthread_mutex_unlock(&prof_lock);
}

static struct prof_block *prof_find(uintptr_t pc) {
    for (unsigned i = prof_nblocks; i-- > 0;) {
        struct prof_block *b = &prof_blocks[i];
        if (pc >= (uintptr_t) b->mem && pc < (uintptr_t) b->mem + b->total)
            return b;
    }
    return NULL;
}

static void prof_describe(FILE *f, uintptr_t pc, unsigned count, uint64_t total) {
    double pct = total ? 100.0 * count / (double) total : 0;
    struct prof_block *b = prof_find(pc);
    if (b != NULL) {
        uint32_t off = (uint32_t) (pc - (uintptr_t) b->mem);
        fprintf(f, "%6u %5.2f%% X %llx +%x%s mem=%p\n", count, pct, (unsigned long long) b->addr,
                off, off >= b->main_len ? " cold" : "", (void *) b->mem);
    } else if (pc >= (uintptr_t) region && pc < (uintptr_t) region_end) {
        fprintf(f, "%6u %5.2f%% R %p\n", count, pct, (void *) pc);
    } else {
        fprintf(f, "%6u %5.2f%% C %p\n", count, pct, (void *) pc);
    }
}

// The code of every translated block, for disassembling.
static void prof_dump_code(void) {
    const char *e = getenv("PX_PROF");
    if (!prof_on || e == NULL || !strcmp(e, "1"))
        return;
    char path[1024];
    snprintf(path, sizeof(path), "%s.code", e);
    FILE *f = fopen(path, "w");
    if (f == NULL)
        return;
    for (unsigned i = 0; i < prof_nblocks; i++) {
        struct prof_block *b = &prof_blocks[i];
        fprintf(f, "BLOCK %llx %p %u %u\n", (unsigned long long) b->addr, (void *) b->mem,
                b->main_len, b->total);
        for (uint32_t j = 0; j < b->total; j++)
            fprintf(f, "%02x", b->mem[j]);
        fprintf(f, "\n");
    }
    fclose(f);
}

#else
static void prof_note(uint8_t *mem, size_t main_len, size_t total, uint64_t addr) {
    (void) mem; (void) main_len; (void) total; (void) addr;
}
#endif

// ---------------------------------------------------------------- blocks

struct xi {
    struct gi g;
    struct cls c;
    struct plan p;
    uint8_t mode;
    bool terminal;
    unsigned taken, fall;   // jcc/jmp: the stream slots of the targets
    uint16_t live_taken, live_fall;   // jcc/jmp: what the targets read (succ_live())
    int lead;          // memory access in a group: its lead's index (its own if it leads), else -1
    struct mgrp grp;   // the lead's
};
enum { M_STREAM, M_TPL, M_NATIVE };

// ---------------------------------------------------------------- groups

// A memory access that can be in a group: a plain base(+index) operand.
static bool grp_access(const struct xi *xi) {
    const struct gi *g = &xi->g;
    return xi->mode == M_NATIVE && !xi->terminal && xi->c.kind == K_GENERIC && xi->p.mem &&
           xi->c.msize > 0 && g->seg == 0 && !g->a32 && g->base >= 0 && g->base != BASE_RIP;
}

// Whether the instruction may change guest register r. Only what is known
// for sure says no.
// ah..bh (a byte operand 4..7 without REX) are in rax..rbx
static int grp_reg(const struct gi *g, int gr, bool byte) {
    return byte && !g->rex && gr >= 4 && gr <= 7 ? gr - 4 : gr;
}

static bool grp_writes(const struct xi *xi, int r) {
    const struct gi *g = &xi->g;
    const struct cls *c = &xi->c;
    if (r < 0)
        return false;
    switch (c->kind) {
        case K_NOP:
            return false;
        case K_LEA:
            return g->reg == r;
        case K_OPREG: {
            int gr = (g->op & 7) | ((g->rex & 1) ? 8 : 0);
            if (g->map == 0 && g->op >= 0x90 && g->op <= 0x97)   // xchg with rax
                return gr == r || r == 0;
            return grp_reg(g, gr, g->map == 0 && g->op >= 0xb0 && g->op <= 0xb7) == r;
        }
        case K_GENERIC:
            // implicit operands: mul/div (rax, rdx), cmpxchg (rax)
            if (g->map == 0 && (g->op == 0xf6 || g->op == 0xf7))
                return true;
            if (g->map == 1 && (g->op == 0xb0 || g->op == 0xb1))
                return true;
            if (g->map > 1)
                return true;
            if (c->reg_gpr && !(c->reg_rw == RW_R) && grp_reg(g, g->reg, c->reg_byte) == r)
                return true;
            if (!xi->p.mem && g->mod == 3 && !(c->rm_rw == RW_R) && grp_reg(g, g->rm, c->rm_byte) == r)
                return true;
            return false;
        default:
            return true;
    }
}

// What an instruction does to the stack, for a group of stack accesses: its
// access (addr relative to rsp before it, size) and how far it moves rsp.
enum { SP_NONE, SP_ACCESS, SP_MOVE, SP_STOP };
static int sp_effect(const struct xi *xi, const struct xinsn *in, int *addr, int *size, bool *write, int *move) {
    const struct gi *g = &xi->g;
    const struct cls *c = &xi->c;
    *move = 0;
    if (xi->mode != M_NATIVE)
        return SP_STOP;
    switch (c->kind) {
        case K_PUSH: case K_PUSHI:
            *addr = -8, *size = 8, *write = true, *move = -8;
            return SP_ACCESS;
        case K_POP:
            if (((g->op & 7) | ((g->rex & 1) ? 8 : 0)) == 4)
                return SP_STOP;
            *addr = 0, *size = 8, *write = false, *move = 8;
            return SP_ACCESS;
        case K_CALL:
            *addr = -8, *size = 8, *write = true;
            return SP_ACCESS;
        case K_CALLI:
            if (g->mod != 3)
                return SP_STOP;   // loads its target first
            *addr = -8, *size = 8, *write = true;
            return SP_ACCESS;
        case K_RET:
            *addr = 0, *size = 8, *write = false;
            return SP_ACCESS;
        case K_NOP:
            return SP_NONE;
        case K_LEA:
            if (g->reg != 4)
                return SP_NONE;
            if (g->base != 4 || g->index >= 0 || g->seg || g->a32 || !(g->rex & 8) || g->disp < -4096 || g->disp > 4096)
                return SP_STOP;
            *move = (int) g->disp;
            return SP_MOVE;
        case K_OPREG:
            return grp_writes(xi, 4) ? SP_STOP : SP_NONE;
        case K_GENERIC:
            if (xi->p.mem) {
                if (c->msize == 0 || g->seg || g->a32 || g->base != 4 || g->index >= 0 || grp_writes(xi, 4) ||
                        g->disp < -4096 || g->disp > 4096)
                    return SP_STOP;
                *addr = (int) g->disp, *size = c->msize, *write = c->acc == A_W || c->acc == A_RW;
                return SP_ACCESS;
            }
            // add/sub $imm, %rsp
            if (g->map == 0 && (g->op == 0x83 || g->op == 0x81) && g->mod == 3 && g->rm == 4 && (g->rex & 8) &&
                    (g->rawreg == 0 || g->rawreg == 5) && !g->lock) {
                int32_t imm = g->immlen == 1 ? (int8_t) in->bytes[g->immoff] :
                    (int32_t) (in->bytes[g->immoff] | in->bytes[g->immoff + 1] << 8 |
                               in->bytes[g->immoff + 2] << 16 | (uint32_t) in->bytes[g->immoff + 3] << 24);
                if (imm < -4096 || imm > 4096)
                    return SP_STOP;
                *move = g->rawreg == 0 ? imm : -imm;
                return SP_MOVE;
            }
            return grp_writes(xi, 4) ? SP_STOP : SP_NONE;
        default:
            return SP_STOP;
    }
}

static bool dbg_nospgroup;   // ISH_XLAT_NOSPGROUP: no groups of stack accesses
#define SP_GROUP_SPAN 128

// Groups of stack accesses: pushes, pops, the call's or the return's own, and
// operands off rsp, while rsp only moves by constants and nothing else touches
// memory. One check of the whole range (a function's prologue or epilogue).
static void find_sp_groups(struct xi *xis, unsigned total, const struct xinsn *xlog, unsigned n, const struct xinsn *tail) {
    for (unsigned i = 0; i < total; i++) {
        struct xi *l = &xis[i];
        int addr, size, move;
        bool write;
        if (l->lead >= 0 || l->terminal || sp_effect(l, i < n ? &xlog[i] : tail, &addr, &size, &write, &move) != SP_ACCESS)
            continue;
        // offsets from the lead's own address
        int base = addr, sp = move;
        int lo = 0, hi = size;
        unsigned members = 0;
        for (unsigned j = i + 1; j < total && j < i + 32; j++) {
            struct xi *m = &xis[j];
            bool mw;
            int e = sp_effect(m, j < n ? &xlog[j] : tail, &addr, &size, &mw, &move);
            if (e == SP_STOP || m->lead >= 0)
                break;
            if (e == SP_ACCESS) {
                int rel = sp + addr - base;
                int nlo = rel < lo ? rel : lo;
                int nhi = rel + size > hi ? rel + size : hi;
                if (nhi - nlo > SP_GROUP_SPAN)
                    break;
                lo = nlo, hi = nhi;
                m->lead = (int) i;
                members++;
                write |= mw;
            }
            sp += move;
            if (m->terminal)
                break;
        }
        if (members > 0) {
            l->lead = (int) i;
            l->grp = (struct mgrp) {lo, hi, write, 0};
        }
    }
}

// Finds the groups of xis[0..total).
static void find_groups(struct xi *xis, unsigned total, const struct xinsn *xlog, unsigned n, const struct xinsn *tail) {
    for (unsigned i = 0; i < total; i++)
        xis[i].lead = -1;
    if (!dbg_nospgroup)
        find_sp_groups(xis, total, xlog, n, tail);
    for (unsigned i = 0; i < total; i++) {
        struct xi *l = &xis[i];
        if (l->lead >= 0 || !grp_access(l))
            continue;
        int base = l->g.base, index = l->g.index;
        if (grp_writes(l, base) || grp_writes(l, index))
            continue;
        int lo = 0, hi = l->c.msize;
        bool write = l->c.acc == A_W || l->c.acc == A_RW;
        unsigned members = 0;
        for (unsigned j = i + 1; j < total && j < i + 32; j++) {
            struct xi *m = &xis[j];
            if (m->mode != M_NATIVE || m->terminal)
                break;
            if (m->lead >= 0)
                break;
            if (grp_access(m) && m->g.base == base && m->g.index == index &&
                    (index < 0 || m->g.scale == l->g.scale)) {
                int64_t rel = m->g.disp - l->g.disp;
                int nlo = rel < lo ? (int) rel : lo;
                int nhi = rel + m->c.msize > hi ? (int) (rel + m->c.msize) : hi;
                if (rel < -64 || rel > 64 || nhi - nlo > 64)
                    break;
                lo = nlo, hi = nhi;
                m->lead = (int) i;
                members++;
                write |= m->c.acc == A_W || m->c.acc == A_RW;
                if (grp_writes(m, base) || grp_writes(m, index))
                    break;
                continue;
            }
            // another access (its miss path may flush the TLB), or a branch
            bool other_mem = (m->c.kind == K_GENERIC && m->p.mem) || m->c.kind == K_PUSH ||
                             m->c.kind == K_PUSHI || m->c.kind == K_POP || m->c.kind == K_LEAVE;
            if (other_mem || !(m->c.kind == K_GENERIC || m->c.kind == K_LEA || m->c.kind == K_NOP ||
                               m->c.kind == K_OPREG))
                break;
            if (grp_writes(m, base) || grp_writes(m, index))
                break;
        }
        if (members > 0) {
            l->lead = (int) i;
            l->grp = (struct mgrp) {lo, hi, write, 0};
        }
    }
}

static __thread struct xi *xis;
static __thread unsigned xis_cap;
// Flags live after each instruction: to the end of the block (all), and
// read by the block's own native instructions before they are written (inb).
static __thread struct lv { uint16_t all, inb; } *live;
static __thread unsigned live_cap;
static __thread unsigned *entry_slot;
static __thread uint32_t *entry_off;
static __thread unsigned entry_cap;

// Every guest thread is a host thread, and a shell pipeline starts hundreds:
// what one allocated to translate goes when it exits (a key's destructor
// runs on the exiting thread, so it can see that thread's variables).
static pthread_key_t xlat_key;
static pthread_once_t xlat_key_once = PTHREAD_ONCE_INIT;
static __thread bool xlat_key_set;

static void xlat_thread_exit(void *unused) {
    (void) unused;
    struct xasm *a = &xa_tls;
    for (int i = 0; i < 2; i++) {
        free(a->sec[i].base);
        a->sec[i].base = a->sec[i].p = a->sec[i].end = NULL;
    }
    free(a->fix); a->fix = NULL; a->capfix = a->nfix = 0;
    free(a->lab); a->lab = NULL; a->caplab = a->nlab = 0;
    free(xlog); xlog = NULL; xlog_cap = xlog_n = 0;
    free(xis); xis = NULL; xis_cap = 0;
    free(live); live = NULL; live_cap = 0;
    free(entry_slot); entry_slot = NULL;
    free(entry_off); entry_off = NULL; entry_cap = 0;
    xlat_key_set = false;
}
static void xlat_key_init(void) {
    pthread_key_create(&xlat_key, xlat_thread_exit);
}
static void xlat_note_thread(void) {
    if (xlat_key_set)
        return;
    pthread_once(&xlat_key_once, xlat_key_init);
    pthread_setspecific(xlat_key, (void *) 1);
    xlat_key_set = true;
}
static bool xlat_on;

// call / call through r/m / jmp through r/m / ret. The successor is unknown
// (or a whole function away): every dirty flag goes to the lazy state first.
static void e_branch_insn(struct xctx *x, const struct xinsn *in, const struct xi *xi, const struct mgrp *mg) {
    struct xasm *a = x->a;
    const struct gi *g = &xi->g;
    unsigned kind = xi->c.kind;
    sync_dirty(x);
    x->valid = 0;
    struct cls bc = {0};
    struct body bd = {g, &bc, in, NULL, 0};

    if (kind == K_CALLI || kind == K_JMPI) {
        // the target, before the call's push moves rsp
        if (g->mod == 3) {
            int gr = g->rm;
            if (gr >= 8)
                e_mov_load(a, R10, XMEM(H_CPU, slot_off(gr)));
            else
                e_mov_rr(a, R10, gr == 4 ? R8 : gr);
        } else {
            e_addr(x, g, R13, true);
            bc.kind = K_B_LOADT;
            e_mem_insn(x, &bd, 8, false, 0, false, NULL);
        }
    }
    if (kind == K_CALL || kind == K_CALLI) {
        e_lea(a, R13, XMEM(R8, -8));
        bc.kind = K_B_PUSHRET;
        bd.imm = g->next;
        if (mg)
            e_mem_member(x, &bd, mg, false);
        else
            e_mem_insn(x, &bd, 8, true, 0, false, NULL);
        // ret_cache[(return address & 0xffff) >> 4] = the call's arguments
        e_movabs_slot(x, R14, xi->fall);
        e_mov_store(a, XMEM(H_CPU, (int32_t) offsetof(struct fiber_frame, ret_cache) +
                                   (int32_t) (((g->next & 0xffff) >> 4) * 8)), R14);
    }
    switch (kind) {
        case K_CALL:
            e_exit_chain(x, xi->taken, F_ALL, true);
            break;
        case K_CALLI: case K_JMPI:
            e_dispatch_r10(x);
            break;
        case K_RET: {
            e_mov_rr(a, R13, R8);
            bc.kind = K_B_POPRET;
            bd.imm = 8 + (g->immlen == 2 ? (uint64_t) (in->bytes[g->immoff] | in->bytes[g->immoff + 1] << 8) : 0);
            if (mg)
                e_mem_member(x, &bd, mg, false);
            else
                e_mem_insn(x, &bd, 8, false, 0, false, NULL);
            e_ret_dispatch(x);
            break;
        }
    }
}

static unsigned dbg_kinds = ~0u;  // ISH_XLAT_KINDS: bit per K_* translated
static unsigned dbg_op_lo = 0, dbg_op_hi = ~0u;  // ISH_XLAT_OPS=lo:hi
static bool dbg_nth_on;
static unsigned dbg_nth_lo, dbg_nth_hi;  // ISH_XLAT_NTH=lo:hi: only those of them
static bool plan_insn(struct xctx *x, const struct xinsn *in, struct xi *xi, unsigned n_left) {
    if (in->bad || !gdecode(in, &xi->g) || !classify(&xi->g, &xi->c))
        return false;
    if (!(dbg_kinds & (1u << xi->c.kind)))
        return false;
    if (xi->c.kind == K_GENERIC) {
        unsigned key = xi->g.map * 256u + xi->g.op;
        if (key < dbg_op_lo || key > dbg_op_hi)
            return false;
        if (dbg_nth_on) {
            static _Atomic unsigned nth;
            unsigned k = atomic_fetch_add(&nth, 1);
            if (k < dbg_nth_lo || k > dbg_nth_hi)
                return false;
            if (dbg_nth_lo == dbg_nth_hi) {
                fprintf(stderr, "[xlat] #%u ip=%llx:", k, (unsigned long long) in->ip);
                for (unsigned i = 0; i < in->len; i++)
                    fprintf(stderr, " %02x", in->bytes[i]);
                fprintf(stderr, "\n");
            }
        }
    }
    shift_flags(&xi->g, in, &xi->c);
    set_rw(&xi->g, &xi->c);
    const struct gi *g = &xi->g;
    switch (xi->c.kind) {
        case K_GENERIC:
            return plan_generic(g, &xi->c, &xi->p);
        case K_CALL: case K_CALLI: case K_JMPI: case K_RET: {
            extern void gadget_call(void), gadget_call_indir(void), gadget_jmp_indir(void), gadget_ret(void);
            if (n_left != 1)
                return false;
            unsigned s0 = in->s0, s1 = in->s1;
            switch (xi->c.kind) {
                case K_CALL:   // call, orig_ip, block, fake ret, return slot, target slot
                    if (s1 != s0 + 6 || x->plan[s0] != (uintptr_t) gadget_call)
                        return false;
                    xi->fall = s0 + 1;     // what the return cache points at
                    xi->taken = s0 + 5;
                    break;
                case K_CALLI:  // ... call_indir, orig_ip, block, fake ret, return slot
                    if (s1 < s0 + 5 || x->plan[s1 - 5] != (uintptr_t) gadget_call_indir)
                        return false;
                    xi->fall = s1 - 4;
                    break;
                case K_JMPI:
                    if (x->plan[s1 - 1] != (uintptr_t) gadget_jmp_indir)
                        return false;
                    break;
                case K_RET:    // ret, orig_ip, 8 + imm
                    if (s1 != s0 + 3 || x->plan[s0] != (uintptr_t) gadget_ret)
                        return false;
                    break;
            }
            xi->terminal = true;
            return true;
        }
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
                if (in->s1 != s0 + 2 || x->plan[s0 + 1] != target)
                    return false;
                xi->taken = s0 + 1;
            } else {
                if (in->s1 != s0 + 3)
                    return false;
                if (x->plan[s0 + 1] == target) {
                    xi->taken = s0 + 1;
                    xi->fall = s0 + 2;
                } else if (x->plan[s0 + 2] == target) {
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
    uint8_t *data = NULL;
    if (x->ndata > 0) {
        data = arena_alloc(&data_arena, x->ndata * 8u);
        if (data == NULL) {
            code_free(mem);
            atomic_fetch_add_explicit(&st_nomem, 1, memory_order_relaxed);
            return false;
        }
        memset(data, 0, x->ndata * 8u);
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
        } else if (f->kind == FIX_DATA) {
            v = (int64_t) (intptr_t) (data + 8 * f->target) - (int64_t) (intptr_t) (field + 4);
        } else if (f->kind == FIX_SLOT64 || f->kind == FIX_BLOCK64) {
            uint64_t p = f->kind == FIX_SLOT64 ? (uint64_t) (uintptr_t) &block->code[f->target]
                                               : (uint64_t) (uintptr_t) block;
            memcpy(field, &p, 8);
            continue;
        } else {
            const uint8_t *orig = (const uint8_t *) f->target;
            v = (int64_t) rd32(orig) - ((int64_t) (intptr_t) field - (int64_t) (intptr_t) orig);
        }
        if (v != (int32_t) v)
            goto fail;
        int32_t v32 = (int32_t) v;
        memcpy(field, &v32, 4);
    }
    for (unsigned i = 0; i < nentries; i++) {
        block->code[entry_slot[i]] = (uintptr_t) (mem + entry_off[i]);
        if (entry_slot[i] == 0)
            block->xjit_entry = (uintptr_t) (mem + entry_off[i]);
    }
    for (unsigned k = 0; k < x->nlinks; k++) {
        uint8_t *field = mem + x->links[k].field;
        for (int i = 0; i <= 1; i++) {
            if (block->jump_ip[i] == &block->code[x->links[k].slot]) {
                block->xjit_link[i] = field;
                block->xjit_link_orig[i] = (uintptr_t) (field + 4) + (intptr_t) rd32(field);
            }
        }
    }
    block->xjit_code = mem;
    block->xjit_data = data;
    prof_note(mem, main_len, total, block->addr);
    if (stats_on) {
        atomic_fetch_add_explicit(&st_blocks, 1, memory_order_relaxed);
        atomic_fetch_add_explicit(&st_runs, nentries, memory_order_relaxed);
        atomic_fetch_add_explicit(&st_bytes, total, memory_order_relaxed);
    }
    return true;
fail:
    code_free(mem);
    if (data != NULL)
        arena_free(&data_arena, data);
    return false;
}

// Translates the block the instruction log describes. False: leave it to
// the template splicer.
static void xc_insert(const struct xctx *x, const struct fiber_block *block,
                      const unsigned *eslot, const uint32_t *eoff, unsigned nent);

static bool xlat_translate(struct fiber_block *block, const uintptr_t *plan) {
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
    xdeps_n = 0;
    xdeps_overflow = false;
    if (!xa_init(a))
        return false;
    struct xctx x = {.a = a, .block = block, .code = block->code, .plan = plan};

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
        if (tpl_range(plan, in->s0, in->s1, &term)) {
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
    if (live_cap < total) {
        free(live);
        live_cap = total * 2;
        live = malloc(live_cap * sizeof(*live));
        if (live == NULL) {
            live_cap = 0;
            return false;
        }
    }
    uint16_t after = F_ALL, after_inb = 0;
    for (unsigned i = total; i-- > 0;) {
        struct xi *xi = &xis[i];
        live[i] = (struct lv) {F_ALL, 0};
        if (xi->mode == M_NATIVE && (xi->c.kind == K_JCC || xi->c.kind == K_JMP)) {
            // a block's end: what the targets read, and the jump itself
            xi->live_taken = succ_live(x.plan[xi->taken] & ~(1ull << 63));
            xi->live_fall = xi->c.kind == K_JCC ? succ_live(x.plan[xi->fall] & ~(1ull << 63)) : 0;
            after = xi->c.fr | xi->live_taken | xi->live_fall;
            after_inb = xi->c.fr;
            continue;
        }
        if (xi->mode != M_NATIVE || xi->terminal || xi->c.kind == K_CALL || xi->c.kind == K_CALLI ||
                xi->c.kind == K_JMPI || xi->c.kind == K_RET) {
            after = F_ALL;
            after_inb = 0;
            continue;
        }
        live[i] = (struct lv) {after, after_inb};
        after = xi->c.fr | (after & ~xi->c.fw);
        after_inb = xi->c.fr | (after_inb & ~xi->c.fw);
    }

    if (!dbg_nogroup)
        find_groups(xis, total, xlog, n, &tail);
    else
        for (unsigned i = 0; i < total; i++)
            xis[i].lead = -1;

    // emit
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
            if (xi->terminal)
                e_note_block(&x);
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
                e_exit_chain(&x, xi->fall, xi->live_fall, false);   // g->next: forward
                lbind(a, l_taken);
                x.dirty = dirty;
            }
            e_exit_chain(&x, xi->taken, xi->live_taken,
                         dbg_allpoke || (x.plan[xi->taken] & ~(1ull << 63)) <= g->ip);
            in_run = false;
            continue;
        }
        if (c->kind == K_CALL || c->kind == K_CALLI || c->kind == K_JMPI || c->kind == K_RET) {
            e_branch_insn(&x, in, xi, xi->lead >= 0 ? &xis[xi->lead].grp : NULL);
            in_run = false;
            continue;
        }
        (void) g;
        e_native(&x, in, g, c, &xi->p, live[i].all, live[i].inb,
                 xi->lead >= 0 ? &xis[xi->lead].grp : NULL, xi->lead >= 0 && xi->lead != (int) i);
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
    if (!xlat_finish(&x, block, entry_slot, entry_off, nentries))
        return false;
    xc_insert(&x, block, entry_slot, entry_off, nentries);
    return true;
}

// ---------------------------------------------------------------- xcache

// Translations kept for the next block of the same guest code. Every guest
// process re-translates the libraries it has in common with the last one
// (python3 twenty times in a shell loop translates libpython twenty times),
// and every ish process does it all again. An entry holds the assembler's
// output before linking (sections, fixups, labels, entries, links), so a
// hit costs a copy and the fixups.
//
// A hit must be what translating would give: the same guest address, the
// same instructions byte for byte (from the instruction log of the block
// just compiled), at the same stream slots, and the same bytes wherever else
// the translation looked (xdeps). Anything else is a miss and translates.
//
// The store is one region of offsets, no pointers: an anonymous mapping, or
// with ISH_XCACHE_FILE a file other ish processes map at the same time
// (sparse on Windows). Its header carries a hash of this executable, so
// another build never reads it (it gets a store of its own instead).
// Writers append with an atomic bump of `used` and publish an entry with a
// CAS on its bucket after its checksum; nothing is ever removed (a full
// store stops taking entries). Targets in this binary are kept relative to
// an anchor in it, as another process maps the binary elsewhere.
#define XS_MAGIC 0x3165686361637878ull   // "xxcache1"
#define XS_VERSION 3
#define XS_BUCKETS 65536
#define FIX_STUB 6   // in the store only: a call to load_flags_stub

struct xs_header {
    uint64_t magic, version, build, capacity;
    _Atomic uint64_t state;   // 0: new, 1: being set up, 2: ready
    _Atomic uint64_t used, count;
    uint64_t pad;
    _Atomic uint64_t buckets[XS_BUCKETS];
};

struct xc_insn {
    uint8_t len;
    bool bad;
    uint32_t s0, s1;
};

struct xs_entry {
    uint64_t next;       // offset of the next entry in the bucket, 0: none
    uint64_t checksum;   // of everything after it
    uint64_t addr;
    uint32_t size;
    uint32_t ninsn, guest_len, stream_size, ndeps, nfix, nlab, nent, nlinks, ndata, main_len, cold_len;
    uint32_t o_insn, o_guest, o_deps, o_fix, o_lab, o_eslot, o_eoff, o_main, o_cold;
    uint32_t link_slot[2], link_field[2];
};
#define XS_PART(e, o) ((void *) ((uint8_t *) (e) + (e)->o))

static struct xs_header *xs;
static bool xc_on;
// The process's own store fills only once a guest process has exited: an
// ish running one command (MinisApp's way) would pay for filling it and
// never read it. A shared file is used from the start.
static _Atomic bool xc_armed;
static bool xs_anon;            // the store is this process's own
static uint8_t *_Atomic xs_committed;  // Windows: committed up to here
static pthread_mutex_t xs_commit_lock = PTHREAD_MUTEX_INITIALIZER;
// Link cached translations at first compile, not only at promotion
// (ISH_XCACHE_EAGER=0: promotions only). Right for the process's own store;
// with a file every page an entry sits on has to come in first.
static bool xc_eager = true;

static uint64_t xs_used(bool count) {
    if (xs == NULL)
        return 0;
    return count ? atomic_load(&xs->count) : atomic_load(&xs->used);
}

void xjit_process_exited(void) {
    atomic_store_explicit(&xc_armed, true, memory_order_relaxed);
}

// Windows, the process's own store: commit [.., end) before writing there.
static bool xs_commit(uint64_t end) {
#ifdef _WIN32
    if (!xs_anon)
        return true;
    uint8_t *want = (uint8_t *) xs + end;
    if (want <= atomic_load(&xs_committed))
        return true;
    pthread_mutex_lock(&xs_commit_lock);
    bool ok = true;
    while (ok && want > xs_committed) {
        size_t step = 4u << 20;
        if (xs_committed + step > (uint8_t *) xs + xs->capacity)
            step = (size_t) ((uint8_t *) xs + xs->capacity - xs_committed);
        ok = step > 0 && px_jit_commit(xs_committed, step, false);
        if (ok)
            atomic_store(&xs_committed, xs_committed + step);
    }
    pthread_mutex_unlock(&xs_commit_lock);
    return ok;
#else
    (void) end;
    return true;
#endif
}

static uintptr_t xs_anchor(void) {
    return (uintptr_t) tpl_table_start;
}

static uint64_t xs_hash(const uint8_t *p, size_t n, uint64_t h) {
    h ^= 0x9e3779b97f4a7c15ull;
    size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        uint64_t v;
        memcpy(&v, p + i, 8);
        h = (h ^ v) * 0xff51afd7ed558ccdull;
        h ^= h >> 32;
    }
    for (; i < n; i++)
        h = (h ^ p[i]) * 0x100000001b3ull;
    return h ^ (h >> 29);
}

// This executable's contents (and what else decides a translation).
static uint64_t xs_build(void) {
    const char *path = NULL;
#ifdef _WIN32
    path = px_self_exe();
#elif defined(__APPLE__)
    extern int _NSGetExecutablePath(char *buf, uint32_t *size);
    static char buf[4096];
    uint32_t size = sizeof(buf);
    if (_NSGetExecutablePath(buf, &size) == 0)
        path = buf;
#else
    path = "/proc/self/exe";
#endif
    if (path == NULL)
        return 0;
    FILE *f = fopen(path, "rb");
    if (f == NULL)
        return 0;
    uint64_t h = XS_VERSION * 0x100 + have_bmi2;
    static uint8_t chunk[1 << 16];
    size_t n;
    while ((n = fread(chunk, 1, sizeof(chunk), f)) > 0)
        h = xs_hash(chunk, n, h);
    fclose(f);
    return h ? h : 1;
}

static void *xs_map_file(const char *path, size_t size) {
#ifdef _WIN32
    return px_shared_file_map(path, size);
#else
    int fd = open(path, O_RDWR | O_CREAT, 0644);
    if (fd < 0)
        return NULL;
    struct stat st;
    if (fstat(fd, &st) != 0 || ((size_t) st.st_size < size && ftruncate(fd, (off_t) size) != 0)) {
        close(fd);
        return NULL;
    }
    void *p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    return p == MAP_FAILED ? NULL : p;
#endif
}

// ISH_XCACHE_FILE is experimental and off unless set: measured on Windows
// (benchmark/daily/RESULTS.md, sections 10 and 14) a hit costs 4.2 us a
// block against 2.9 us to translate it, so a warm file makes a start slower
// (python3 -c pass 238 -> 301 ms). Kept for measuring; not for production.
static void xs_init(size_t capacity, bool persistent) {
    char path[1024] = "";
    const char *e = getenv("ISH_XCACHE_FILE");
    if (persistent && e != NULL)
        snprintf(path, sizeof(path), "%s", e);
    // a file's header names the build that wrote it (the process's own store doesn't need to)
    uint64_t build = path[0] != '\0' ? xs_build() : 1;
    if (build == 0)
        path[0] = '\0';
    // No file by default: measured on Windows, one ish per command gains
    // nothing from it (translating is about as fast as bringing the file's
    // pages in); the process's own store is what pays off.
    struct xs_header *h = NULL;
    if (path[0] != '\0')
        h = xs_map_file(path, capacity);
    if (h == NULL) {
        xs_anon = true;
#ifdef _WIN32
        // reserved; committed as it fills (xs_commit())
        h = px_jit_reserve(NULL, capacity);
        if (h != NULL && !px_jit_commit(h, 1u << 20, false))
            h = NULL;
        if (h != NULL)
            xs_committed = (uint8_t *) h + (1u << 20);
#else
        h = mmap(NULL, capacity, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        if (h == MAP_FAILED)
            h = NULL;
#endif
    }
    if (h == NULL)
        return;
    uint64_t expect = 0;
    if (atomic_compare_exchange_strong(&h->state, &expect, 1)) {
        h->magic = XS_MAGIC;
        h->version = XS_VERSION;
        h->build = build;
        h->capacity = capacity;
        atomic_store(&h->used, (sizeof(*h) + 63) & ~(uint64_t) 63);
        atomic_store(&h->state, 2);
    } else {
        // someone else sets it up
        for (int i = 0; i < 2000 && atomic_load(&h->state) != 2; i++)
            usleep(1000);
    }
    if (atomic_load(&h->state) != 2 || h->magic != XS_MAGIC || h->version != XS_VERSION ||
            h->build != build || h->capacity != capacity) {
        // another build's (a named file): keep this process's own instead
        if (path[0] != '\0')
            xs_init(capacity, false);
        return;
    }
    xs = h;
    if (!xs_anon)
        atomic_store(&xc_armed, true);
#ifdef _WIN32
    // what is there in one go: a fault per page of a mapped file costs more
    // than translating the block on it
    if (path[0] != '\0')
        px_prefetch(h, (size_t) atomic_load(&h->used));
#endif
}

// Only code from file mappings that aren't writable is kept: that is what
// the next process maps again (libraries, executables). A guest JIT's code
// (V8's) sits in anonymous memory it rewrites all the time: caching it only
// fills the cache and makes every lookup at those addresses longer. The
// lock may be held the other way round (munmap holds it and then takes the
// asbestos lock we hold): not getting it at once means not caching.
static bool xc_file_code(uint64_t addr, uint64_t end) {
    if (xlog_tlb == NULL || end <= addr)
        return false;
    struct mem *mem = container_of(xlog_tlb->mmu, struct mem, mmu);
    if (!read_wrtrylock(&mem->lock))
        return false;
    bool ok = true;
    page_t pages[2] = {PAGE(addr), PAGE(end - 1)};
    for (int i = 0; i < 2 && ok; i++) {
        struct pt_entry *pt = mem_pt(mem, pages[i]);
        ok = pt != NULL && pt->data != NULL && pt->data->fd != NULL && !(pt->flags & P_WRITE);
    }
    read_wrunlock(&mem->lock);
    return ok;
}

static uint64_t xlog_end(void) {
    const struct xinsn *last = &xlog[xlog_n - 1];
    return last->ip + (last->len ? last->len : 1);
}

static unsigned xc_hash(uint64_t addr) {
    return (unsigned) ((addr * 0x9e3779b97f4a7c15ull) >> 48) & (XS_BUCKETS - 1);
}

// The instructions of the block just compiled, against an entry.
static bool xc_matches(const struct xs_entry *e, uint64_t addr) {
    if (e->addr != addr || e->ninsn != xlog_n || e->stream_size != xlog_size)
        return false;
    const struct xc_insn *ins = XS_PART(e, o_insn);
    const uint8_t *guest = XS_PART(e, o_guest);
    uint32_t off = 0;
    for (unsigned i = 0; i < xlog_n; i++) {
        const struct xinsn *in = &xlog[i];
        const struct xc_insn *c = &ins[i];
        if (in->bad != c->bad || in->len != c->len || in->s0 != c->s0 || in->s1 != c->s1 ||
                in->ip != addr + off || off + in->len > e->guest_len)
            return false;
        if (in->len != 0 && memcmp(in->bytes, guest + off, in->len) != 0)
            return false;
        off += in->len;
    }
    const struct xdep *deps = XS_PART(e, o_deps);
    for (unsigned i = 0; i < e->ndeps; i++) {
        uint8_t now[15];
        if (deps[i].len > 15 || xlog_tlb == NULL || !tlb_read(xlog_tlb, deps[i].addr, now, deps[i].len) ||
                memcmp(now, deps[i].bytes, deps[i].len) != 0)
            return false;
    }
    return true;
}

static bool xs_entry_ok(const struct xs_entry *e) {
    if (e->size < sizeof(*e) || e->size > xs->capacity)
        return false;
    return xs_hash((const uint8_t *) e + 16, e->size - 16, 0) == e->checksum;
}

static bool xlat_finish(struct xctx *x, struct fiber_block *block, unsigned *entry_slot,
                        uint32_t *entry_off, unsigned nentries);

static bool xa_grow(struct xasm *a, unsigned nfix, unsigned nlab) {
    while (a->capfix < nfix) {
        unsigned cap = a->capfix ? a->capfix * 2 : 256;
        struct xfix *bigger = realloc(a->fix, cap * sizeof(*a->fix));
        if (bigger == NULL) return false;
        a->fix = bigger; a->capfix = cap;
    }
    while (a->caplab < nlab) {
        unsigned cap = a->caplab ? a->caplab * 2 : 256;
        struct xlabel *bigger = realloc(a->lab, cap * sizeof(*a->lab));
        if (bigger == NULL) return false;
        a->lab = bigger; a->caplab = cap;
    }
    return true;
}

// Links a cached translation into block. True: done.
static bool xc_link(struct fiber_block *block) {
    if (!xc_on || xs == NULL || !atomic_load_explicit(&xc_armed, memory_order_relaxed) ||
            !xlog_ok || xlog_n == 0 || !xc_file_code(block->addr, xlog_end()))
        return false;
    const struct xs_entry *e = NULL;
    uint64_t off = atomic_load_explicit(&xs->buckets[xc_hash(block->addr)], memory_order_acquire);
    for (int guard = 0; off != 0 && off < xs->capacity && guard < 4096; guard++) {
        const struct xs_entry *c = (const struct xs_entry *) ((uint8_t *) xs + off);
        if (xc_matches(c, block->addr) && xs_entry_ok(c)) {
            e = c;
            break;
        }
        off = c->next;
    }
    if (e == NULL || e->main_len > XBUF_SIZE || e->cold_len > XBUF_SIZE || e->nent == 0) {
        if (stats_on)
            atomic_fetch_add_explicit(&st_xc_misses, 1, memory_order_relaxed);
        return false;
    }
    struct xasm *a = &xa_tls;
    if (!xa_init(a) || !xa_grow(a, e->nfix, e->nlab))
        return false;
    static __thread unsigned *eslot;
    static __thread uint32_t *eoff;
    static __thread unsigned ecap;
    if (ecap < e->nent) {
        free(eslot);
        free(eoff);
        ecap = e->nent * 2;
        eslot = malloc(ecap * sizeof(*eslot));
        eoff = malloc(ecap * sizeof(*eoff));
        if (eslot == NULL || eoff == NULL) {
            ecap = 0;
            return false;
        }
    }
    // into the assembler's buffers, as if it had just emitted it
    memcpy(a->sec[SEC_MAIN].base, XS_PART(e, o_main), e->main_len);
    a->sec[SEC_MAIN].p = a->sec[SEC_MAIN].base + e->main_len;
    memcpy(a->sec[SEC_COLD].base, XS_PART(e, o_cold), e->cold_len);
    a->sec[SEC_COLD].p = a->sec[SEC_COLD].base + e->cold_len;
    const struct xfix *fix = XS_PART(e, o_fix);
    uintptr_t anchor = xs_anchor();
    for (unsigned i = 0; i < e->nfix; i++) {
        struct xfix f = fix[i];
        if (f.kind == FIX_STUB) {
            f.kind = FIX_ABS;
            f.target = (uintptr_t) load_flags_stub;
        } else if (f.kind == FIX_ABS || f.kind == FIX_MOVED) {
            f.target = anchor + f.target;
        }
        a->fix[i] = f;
    }
    a->nfix = e->nfix;
    memcpy(a->lab, XS_PART(e, o_lab), e->nlab * sizeof(struct xlabel));
    a->nlab = e->nlab;
    memcpy(eslot, XS_PART(e, o_eslot), e->nent * sizeof(*eslot));
    memcpy(eoff, XS_PART(e, o_eoff), e->nent * sizeof(*eoff));
    struct xctx x = {.a = a, .block = block, .code = block->code};
    x.nlinks = e->nlinks;
    for (int k = 0; k < 2; k++) {
        x.links[k].slot = e->link_slot[k];
        x.links[k].field = e->link_field[k];
    }
    x.ndata = e->ndata;
    if (!xlat_finish(&x, block, eslot, eoff, e->nent))
        return false;
    if (stats_on)
        atomic_fetch_add_explicit(&st_xc_hits, 1, memory_order_relaxed);
    return true;
}

// Keeps what xlat_translate() just linked into block.
static void xc_insert(const struct xctx *x, const struct fiber_block *block,
                      const unsigned *eslot, const uint32_t *eoff, unsigned nent) {
    const struct xasm *a = x->a;
    if (!xc_on || xs == NULL || !atomic_load_explicit(&xc_armed, memory_order_relaxed) ||
            xdeps_overflow || !xlog_ok || xlog_n == 0 || nent == 0 || !xc_file_code(block->addr, xlog_end()))
        return;
    uint32_t guest_len = 0;
    for (unsigned i = 0; i < xlog_n; i++)
        guest_len += xlog[i].len;
    uint32_t main_len = (uint32_t) (a->sec[SEC_MAIN].p - a->sec[SEC_MAIN].base);
    uint32_t cold_len = (uint32_t) (a->sec[SEC_COLD].p - a->sec[SEC_COLD].base);
    // the layout
    uint32_t size = sizeof(struct xs_entry);
#define XS_LAY(field, n) do { size = (size + 7) & ~7u; field = size; size += (uint32_t) (n); } while (0)
    uint32_t o_insn, o_guest, o_deps, o_fix, o_lab, o_eslot, o_eoff, o_main, o_cold;
    XS_LAY(o_insn, xlog_n * sizeof(struct xc_insn));
    XS_LAY(o_guest, guest_len);
    XS_LAY(o_deps, xdeps_n * sizeof(struct xdep));
    XS_LAY(o_fix, a->nfix * sizeof(struct xfix));
    XS_LAY(o_lab, a->nlab * sizeof(struct xlabel));
    XS_LAY(o_eslot, nent * 4);
    XS_LAY(o_eoff, nent * 4);
    XS_LAY(o_main, main_len);
    XS_LAY(o_cold, cold_len);
#undef XS_LAY
    size = (size + 63) & ~63u;
    uint64_t off = atomic_load_explicit(&xs->used, memory_order_relaxed);
    do {
        if (off + size > xs->capacity) {
            if (stats_on)
                atomic_fetch_add_explicit(&st_xc_rejects, 1, memory_order_relaxed);
            return;
        }
    } while (!atomic_compare_exchange_weak_explicit(&xs->used, &off, off + size,
                                                    memory_order_relaxed, memory_order_relaxed));
    if (!xs_commit(off + size))
        return;   // (the space is lost)
    struct xs_entry *e = (struct xs_entry *) ((uint8_t *) xs + off);
    memset(e, 0, sizeof(*e));
    e->addr = block->addr;
    e->size = size;
    e->ninsn = xlog_n;
    e->guest_len = guest_len;
    e->stream_size = xlog_size;
    e->ndeps = xdeps_n;
    e->nfix = a->nfix;
    e->nlab = a->nlab;
    e->nent = nent;
    e->nlinks = x->nlinks;
    e->ndata = x->ndata;
    e->main_len = main_len;
    e->cold_len = cold_len;
    e->o_insn = o_insn; e->o_guest = o_guest; e->o_deps = o_deps; e->o_fix = o_fix; e->o_lab = o_lab;
    e->o_eslot = o_eslot; e->o_eoff = o_eoff; e->o_main = o_main; e->o_cold = o_cold;
    for (int k = 0; k < 2; k++) {
        e->link_slot[k] = x->links[k].slot;
        e->link_field[k] = x->links[k].field;
    }
    struct xc_insn *ins = XS_PART(e, o_insn);
    uint8_t *guest = XS_PART(e, o_guest);
    uint32_t g = 0;
    for (unsigned i = 0; i < xlog_n; i++) {
        ins[i] = (struct xc_insn) {xlog[i].len, xlog[i].bad, xlog[i].s0, xlog[i].s1};
        memcpy(guest + g, xlog[i].bytes, xlog[i].len);
        g += xlog[i].len;
    }
    memcpy(XS_PART(e, o_deps), xdeps, xdeps_n * sizeof(struct xdep));
    // targets in the binary relative to the anchor; the shared stub by name
    struct xfix *fix = XS_PART(e, o_fix);
    uintptr_t anchor = xs_anchor();
    for (unsigned i = 0; i < a->nfix; i++) {
        struct xfix f = a->fix[i];
        if (f.kind == FIX_ABS && f.target == (uintptr_t) load_flags_stub) {
            f.kind = FIX_STUB;
            f.target = 0;
        } else if (f.kind == FIX_ABS || f.kind == FIX_MOVED) {
            intptr_t d = (intptr_t) (f.target - anchor);
            if (d > (intptr_t) 1 << 30 || d < -((intptr_t) 1 << 30))
                return;   // something outside the binary: not for keeping (the space is lost)
            f.target = (uintptr_t) d;
        }
        fix[i] = f;
    }
    memcpy(XS_PART(e, o_lab), a->lab, a->nlab * sizeof(struct xlabel));
    memcpy(XS_PART(e, o_eslot), eslot, nent * 4);
    memcpy(XS_PART(e, o_eoff), eoff, nent * 4);
    memcpy(XS_PART(e, o_main), a->sec[SEC_MAIN].base, main_len);
    memcpy(XS_PART(e, o_cold), a->sec[SEC_COLD].base, cold_len);
    // publish: checksum, then the bucket
    _Atomic uint64_t *bucket = &xs->buckets[xc_hash(block->addr)];
    uint64_t head = atomic_load_explicit(bucket, memory_order_acquire);
    do {
        e->next = head;
        e->checksum = xs_hash((const uint8_t *) e + 16, size - 16, 0);
    } while (!atomic_compare_exchange_weak_explicit(bucket, &head, off, memory_order_release, memory_order_acquire));
    atomic_fetch_add_explicit(&xs->count, 1, memory_order_relaxed);
    if (stats_on)
        atomic_fetch_add_explicit(&st_xc_inserts, 1, memory_order_relaxed);
}

// ---------------------------------------------------------------- tiers

// Most blocks of a starting process run once or a few times: translating
// them costs more than running their gadgets. So a new block first runs as
// gadgets behind a counting entry; after xlat_hot runs it asks the run loop
// (INT_XJIT_PROMOTE) to translate it. ISH_XLAT_HOT=1 translates at once.
static unsigned xlat_hot = 4;

static int32_t rel_to(const uint8_t *field_end, const void *target) {
    return (int32_t) ((intptr_t) target - (intptr_t) field_end);
}

static bool xlat_install_counter(struct fiber_block *block) {
    uint8_t *stub = code_alloc(64);
    uint8_t *counter = arena_alloc(&data_arena, 8);
    if (stub == NULL || counter == NULL) {
        if (stub) code_free(stub);
        if (counter) arena_free(&data_arena, counter);
        return false;
    }
    *counter = (uint8_t) (xlat_hot > 255 ? 255 : xlat_hot);
    uintptr_t gadget0 = block->code[0];
    uint8_t *p = stub;
    int32_t r;
    // decb counter(%rip); jz promote; jmp gadget0
    *p++ = 0xfe; *p++ = 0x0d; r = rel_to(p + 4, counter); memcpy(p, &r, 4); p += 4;
    *p++ = 0x0f; *p++ = 0x84; uint8_t *jz = p; p += 4;
    *p++ = 0xe9; r = rel_to(p + 4, (const void *) gadget0); memcpy(p, &r, 4); p += 4;
    r = rel_to(jz + 4, p); memcpy(jz, &r, 4);
    // promote: frame->xjit_promote = block; _ip = its address; leave with INT_XJIT_PROMOTE
    *p++ = 0x49; *p++ = 0xba; uint64_t v = (uint64_t) (uintptr_t) block; memcpy(p, &v, 8); p += 8;   // movabs $block, %r10
    int32_t off = (int32_t) offsetof(struct fiber_frame, xjit_promote);
    *p++ = 0x4d; *p++ = 0x89; *p++ = 0x93; memcpy(p, &off, 4); p += 4;                                // mov %r10, off(%r11)
    *p++ = 0x49; *p++ = 0xb9; v = block->addr; memcpy(p, &v, 8); p += 8;                                 // movabs $addr, %r9
    *p++ = 0x41; *p++ = 0xba; uint32_t code = INT_XJIT_PROMOTE; memcpy(p, &code, 4); p += 4;           // movl $INT, %r10d
    *p++ = 0xe9; r = rel_to(p + 4, (const void *) fiber_exit); memcpy(p, &r, 4); p += 4;
    block->xjit_stub = stub;
    block->xjit_counter = counter;
    block->xjit_gadget0 = gadget0;
    block->code[0] = (uintptr_t) stub;
    return true;
}

static bool xlat_block(struct fiber_block *block) {
    // With tiers, the cache is for promotions: linking a cached translation
    // is cheaper than translating, but dearer than running a block that
    // only runs once or twice as gadgets (most of a starting process's).
    if (xlat_hot <= 1 || xc_eager)
        if (xc_link(block))
            return true;
    if (xlat_hot <= 1)
        return xlat_translate(block, block->code);
    if (!xlat_on || !xlog_ok || xlog_n == 0)
        return false;
    // nothing to translate in a block whose start can't be: leave it be
    if (xlog[0].bad)
        return xlat_translate(block, block->code);
    if (!xlat_install_counter(block))
        return xlat_translate(block, block->code);
    if (stats_on)
        atomic_fetch_add_explicit(&st_counted, 1, memory_order_relaxed);
    return true;
}

void xjit_promote(struct fiber_block *block, struct fiber_block *tmp, const unsigned *skip, int nskip) {
    if (block->xjit_stub == NULL || block->code[0] != (uintptr_t) block->xjit_stub)
        return;
    // the counter stays at 0 from here: the stub won't ask twice
    bool same = xlog_ok && xlog_n > 0 && xlog[0].s0 == 0;
    unsigned n = xlog_size;
    for (unsigned i = 1; same && i < n; i++) {
        bool skipped = false;
        for (int k = 0; k < nskip; k++)
            skipped |= skip[k] == i;
        if (!skipped && tmp->code[i] != block->code[i])
            same = false;
    }
    if (same && tmp->code[0] != block->xjit_gadget0)
        same = false;
    if (!same && stats_on) {
        atomic_fetch_add_explicit(&st_promote_differs, 1, memory_order_relaxed);
        if (getenv("ISH_XLAT_PROMOTE_LOG")) {
            for (unsigned i = 0; i < n; i++)
                if (tmp->code[i] != block->code[i])
                    fprintf(stderr, "[promote] %llx slot %u: %llx vs %llx\n", (unsigned long long) block->addr, i,
                            (unsigned long long) tmp->code[i], (unsigned long long) block->code[i]);
        }
    }
    if (same) {
        block->code[0] = block->xjit_gadget0;
        bool ok = (xc_link(block) || xlat_translate(block, tmp->code)) && block->xjit_code != NULL;
        if (stats_on)
            atomic_fetch_add_explicit(ok ? &st_promote_ok : &st_promote_failed, 1, memory_order_relaxed);
        if (!ok) {
            // what a block the translator can't do gets at once
            block->code[0] = block->xjit_gadget0;
            if (block->xjit_code == NULL)
                xjit_splice(block);
        } else if (block->code[0] == block->xjit_gadget0) {
            // slot 0 not translated: the gadget again
            block->code[0] = block->xjit_gadget0;
        }
        if (ok) {
            // jumps chained while it ran as gadgets: link them natively too
            for (int i = 0; i <= 1; i++) {
                if (block->jump_ip[i] == NULL || (*block->jump_ip[i] >> 63))
                    continue;
                if (*block->jump_ip[i] == block->old_jump_ip[i])
                    continue;
                struct fiber_block *to = (struct fiber_block *)
                    (*block->jump_ip[i] - offsetof(struct fiber_block, code));
                xjit_link(block, i, to);
            }
        }
        return;
    }
    block->code[0] = block->xjit_gadget0;
}

static void xlat_init(void) {
    const char *e = getenv("ISH_XLAT");
    xlat_on = !(e && (!strcmp(e, "0") || !strcmp(e, "off")));
    if (!xlat_on)
        return;
#ifdef _WIN32
    e = getenv("PX_PROF");
    prof_on = e != NULL && e[0] != '\0';
    if (prof_on)
        px_prof_hooks(prof_describe, prof_dump_code);
#endif
#if defined(__x86_64__)
    unsigned ra, rb, rc, rd;
    if (__get_cpuid_count(7, 0, &ra, &rb, &rc, &rd))
        have_bmi2 = (rb >> 8) & 1;
    dbg_nosucc = getenv("ISH_XLAT_NOSUCC") != NULL;
    dbg_allpoke = getenv("ISH_XLAT_ALLPOKE") != NULL;
    dbg_nogroup = getenv("ISH_XLAT_NOGROUP") != NULL;
    dbg_slowsync = getenv("ISH_XLAT_SLOWSYNC") != NULL;
    dbg_nospgroup = getenv("ISH_XLAT_NOSPGROUP") != NULL;
    dbg_sitestats = getenv("ISH_XLAT_SITESTATS") != NULL;
    sitedump_path = getenv("ISH_XLAT_SITEDUMP");
    if (sitedump_path != NULL) {
        sitedump = calloc(SITEDUMP_MAX, sizeof(*sitedump));
        tlb_stats_hook = xlat_sitedump_exit;
    }
    dbg_tlbdirect = getenv("ISH_XLAT_TLBDIRECT") ? atoi(getenv("ISH_XLAT_TLBDIRECT")) : 0;
    dbg_nosse = getenv("ISH_XLAT_NOSSE") != NULL;
    if ((e = getenv("ISH_XLAT_OPS")) != NULL) {
        char *end;
        dbg_op_lo = (unsigned) strtoul(e, &end, 0);
        dbg_op_hi = *end == ':' ? (unsigned) strtoul(end + 1, NULL, 0) : dbg_op_lo;
    }
    if ((e = getenv("ISH_XLAT_NTH")) != NULL) {
        char *end;
        dbg_nth_on = true;
        dbg_nth_lo = (unsigned) strtoul(e, &end, 0);
        dbg_nth_hi = *end == ':' ? (unsigned) strtoul(end + 1, NULL, 0) : dbg_nth_lo;
    }
    if ((e = getenv("ISH_XLAT_KINDS")) != NULL)
        dbg_kinds = (unsigned) strtoul(e, NULL, 0);
    if ((e = getenv("ISH_XLAT_HOT")) != NULL)
        xlat_hot = (unsigned) strtoul(e, NULL, 0);
    e = getenv("ISH_XLAT_BMI2");
    if (e && e[0] == '0')
        have_bmi2 = false;
    e = getenv("ISH_XCACHE");
    xc_on = !(e && e[0] == '0');
    if ((e = getenv("ISH_XCACHE_EAGER")) != NULL)
        xc_eager = e[0] != '0';
    if ((e = getenv("ISH_XCACHE_MB")) != NULL)
        xc_limit = (size_t) strtoul(e, NULL, 0) << 20;
    if (xc_on) {
        // a translation made with a debugging switch on is not for keeping
        bool debug = getenv("ISH_XLAT_KINDS") || getenv("ISH_XLAT_OPS") || getenv("ISH_XLAT_NTH") ||
            getenv("ISH_XLAT_NOSUCC") || getenv("ISH_XLAT_NOLINK");
        xs_init(xc_limit, !debug);
        xc_on = xs != NULL;
    }
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
