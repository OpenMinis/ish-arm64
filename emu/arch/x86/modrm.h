#ifndef MODRM_H
#define MODRM_H

#include "debug.h"
#include "misc.h"
#include "emu/cpu.h"
#include "emu/tlb.h"

#undef DEFAULT_CHANNEL
#define DEFAULT_CHANNEL instr

struct modrm {
    union {
        enum reg32 reg;
        unsigned opcode;
    };
    enum {
        modrm_reg, modrm_mem, modrm_mem_si
    } type;
    union {
        enum reg32 base;
        unsigned rm_opcode;
    };
#ifdef GUEST_X86_64
    int64_t offset;
#else
    int32_t offset;
#endif
    enum reg32 index;
    enum {
        times_1 = 0,
        times_2 = 1,
        times_4 = 2,
    } shift;
};

static const unsigned rm_sib = reg_esp;
static const unsigned rm_none = reg_esp;
static const unsigned rm_disp32 = reg_ebp;
#define MOD(byte) ((byte & 0b11000000) >> 6)
#define REG(byte) ((byte & 0b00111000) >> 3)
#define RM(byte)  ((byte & 0b00000111) >> 0)

// read modrm and maybe sib, output information into *modrm, return false for segfault
static inline bool modrm_decode32(addr_t *ip, struct tlb *tlb, struct modrm *modrm) {
#define READ(thing) \
    *ip += sizeof(thing); \
    if (!tlb_read(tlb, *ip - sizeof(thing), &(thing), sizeof(thing))) \
        return false

    byte_t modrm_byte;
    READ(modrm_byte);

    enum {
        mode_disp0,
        mode_disp8,
        mode_disp32,
        mode_reg,
    } mode = MOD(modrm_byte);
    modrm->type = modrm_mem;
    modrm->reg = REG(modrm_byte);
    modrm->rm_opcode = RM(modrm_byte);
    if (mode == mode_reg) {
        modrm->type = modrm_reg;
    } else if (modrm->rm_opcode == rm_disp32 && mode == mode_disp0) {
        modrm->base = reg_none;
        mode = mode_disp32;
    } else if (modrm->rm_opcode == rm_sib && mode != mode_reg) {
        byte_t sib_byte;
        READ(sib_byte);
        modrm->base = RM(sib_byte);
        // wtf intel
        if (modrm->rm_opcode == rm_disp32) {
            if (mode == mode_disp0) {
                modrm->base = reg_none;
                mode = mode_disp32;
            } else {
                modrm->base = reg_ebp;
            }
        }
        modrm->index = REG(sib_byte);
        modrm->shift = MOD(sib_byte);
        if (modrm->index != rm_none)
            modrm->type = modrm_mem_si;
    }

    if (mode == mode_disp0) {
        modrm->offset = 0;
    } else if (mode == mode_disp8) {
        int8_t offset;
        READ(offset);
        modrm->offset = offset;
    } else if (mode == mode_disp32) {
        int32_t offset;
        READ(offset);
        modrm->offset = offset;
    }
#undef READ

    TRACE("reg=%s opcode=%d ", reg32_name(modrm->reg), modrm->opcode);
    TRACE("base=%s ", reg32_name(modrm->base));
    if (modrm->type != modrm_reg)
        TRACE("offset=%s0x%x ", modrm->offset < 0 ? "-" : "", modrm->offset);
    if (modrm->type == modrm_mem_si)
        TRACE("index=%s<<%d ", reg32_name(modrm->index), modrm->shift);

    return true;
}

#ifdef GUEST_X86_64
// base of a rip-relative operand: gen_addr adds the address of the next
// instruction, which is only known once the immediates have been read
#define reg_rip ((enum reg32) (reg_none + 1))

// modrm_decode32 extended by a REX prefix: R, X and B are the top bits of
// reg, index and base (rm). rm=100 still means a SIB byte and mod=00 rm=101
// means rip+disp32, whatever B is; SIB index 100 means none only without X.
static inline bool modrm_decode64(addr_t *ip, struct tlb *tlb, struct modrm *modrm, byte_t rex) {
#define READ(thing) \
    *ip += sizeof(thing); \
    if (!tlb_read(tlb, *ip - sizeof(thing), &(thing), sizeof(thing))) \
        return false

    unsigned r = rex & 4 ? 8 : 0, x = rex & 2 ? 8 : 0, b = rex & 1 ? 8 : 0;
    byte_t modrm_byte;
    READ(modrm_byte);

    enum {
        mode_disp0,
        mode_disp8,
        mode_disp32,
        mode_reg,
    } mode = MOD(modrm_byte);
    modrm->type = modrm_mem;
    modrm->reg = REG(modrm_byte) | r;
    modrm->rm_opcode = RM(modrm_byte);
    if (mode == mode_reg) {
        modrm->type = modrm_reg;
        modrm->base = modrm->rm_opcode | b;
    } else if (modrm->rm_opcode == rm_disp32 && mode == mode_disp0) {
        modrm->base = reg_rip;
        mode = mode_disp32;
    } else if (modrm->rm_opcode == rm_sib) {
        byte_t sib_byte;
        READ(sib_byte);
        modrm->base = RM(sib_byte) | b;
        if (RM(sib_byte) == rm_disp32 && mode == mode_disp0) {
            modrm->base = reg_none;
            mode = mode_disp32;
        }
        modrm->index = REG(sib_byte) | x;
        modrm->shift = MOD(sib_byte);
        if (modrm->index != rm_none)
            modrm->type = modrm_mem_si;
    } else {
        modrm->base = modrm->rm_opcode | b;
    }

    if (mode == mode_disp0) {
        modrm->offset = 0;
    } else if (mode == mode_disp8) {
        int8_t offset;
        READ(offset);
        modrm->offset = offset;
    } else if (mode == mode_disp32) {
        int32_t offset;
        READ(offset);
        modrm->offset = offset;
    }
#undef READ
    return true;
}
#endif

#endif
