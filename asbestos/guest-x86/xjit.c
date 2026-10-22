// Gadget-template JIT for the x86 guest -- see xjit.h.

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include "asbestos/guest-x86/xjit.h"
#include "asbestos/asbestos.h"

// ---------------------------------------------------------------- templates

// One entry per gadget, emitted by the .gadget macro in template mode.
struct tpl_entry {
    uintptr_t gadget;      // the real gadget
    const uint8_t *start;  // its template
    const uint8_t *end;
    int64_t pop;           // argument words it consumes; -1: never falls through
                           // (ends in a jump out); -2: varies
    int64_t noinline;      // moves _ip itself (skip): copying it would be wrong
};

#if defined(__APPLE__)
extern const struct tpl_entry tpl_table_start[] __asm("section$start$__DATA$__ish_xjit");
extern const struct tpl_entry tpl_table_end[] __asm("section$end$__DATA$__ish_xjit");
#elif defined(_WIN32)
// COFF has no __start_/__stop_ symbols. The linker orders the parts of a
// grouped section by what follows the '$', so these bracket the entries
// (.xjit$m, from gadgets.h).
__attribute__((section(".xjit$a"), used, aligned(8)))
static const struct tpl_entry tpl_table_head = {0};
__attribute__((section(".xjit$z"), used, aligned(8)))
static const struct tpl_entry tpl_table_tail = {0};
// Walking from one object to another is undefined behaviour the optimizer
// exploits (it folded the entry count to 0): launder the addresses.
static inline const struct tpl_entry *tpl_opaque(const struct tpl_entry *p) {
    uintptr_t v = (uintptr_t) p;
    __asm__("" : "+r"(v));
    return (const struct tpl_entry *) v;
}
#define tpl_table_start (tpl_opaque(&tpl_table_head) + 1)
#define tpl_table_end (tpl_opaque(&tpl_table_tail))
#else
extern const struct tpl_entry __start_ish_xjit[], __stop_ish_xjit[];
#define tpl_table_start __start_ish_xjit
#define tpl_table_end __stop_ish_xjit
#endif

#define MAX_RELOCS 24

// A template ready to copy: its bytes (without the trailing jump to its own
// end) and the rel32/disp32 fields that point outside it.
struct tpl {
    uintptr_t gadget;
    const uint8_t *code;
    uint16_t len;
    int8_t pop;
    bool noinline;
    bool usable;
    uint8_t nrelocs;
    uint16_t relocs[MAX_RELOCS];
    // Branches to the template's end (other grets): the copy drops the final
    // jump, so they land `trim` bytes earlier. Low bit of each: rel32.
    uint8_t nends;
    uint8_t trim;
    uint16_t ends[MAX_RELOCS];
};

static struct tpl *tpls;
static unsigned tpl_hash_size;  // power of two, open addressing on gadget address

static struct tpl *tpl_lookup(uintptr_t gadget) {
    if (tpls == NULL)
        return NULL;
    unsigned mask = tpl_hash_size - 1;
    for (unsigned i = (unsigned) (gadget >> 3) & mask; ; i = (i + 1) & mask) {
        if (tpls[i].gadget == gadget)
            return &tpls[i];
        if (tpls[i].gadget == 0)
            return NULL;
    }
}

// ---------------------------------------------------------------- decoder

// Instruction length decoder for x86-64, enough for what the gadgets contain.
// Reports the length, and for a branch with a relative target or an operand
// addressed relative to rip, where that 32-bit (or 8-bit) field is.
struct insn {
    unsigned len;
    int rel_off, rel_size;   // relative branch target field (0 size: none)
    int rip_off;             // rip-relative disp32 field (-1: none)
    bool uncond_exit;        // jmp/ret: control never falls through
};

static bool modrm_len(const uint8_t *p, unsigned *len, int *rip_off, unsigned at) {
    uint8_t modrm = p[0];
    unsigned mod = modrm >> 6, rm = modrm & 7;
    unsigned n = 1;
    *rip_off = -1;
    if (mod != 3 && rm == 4) {
        uint8_t sib = p[1];
        n++;
        if (mod == 0 && (sib & 7) == 5)
            n += 4;
    }
    if (mod == 0 && rm == 5) {
        *rip_off = (int) (at + 1);
        n += 4;
    } else if (mod == 1) {
        n += 1;
    } else if (mod == 2) {
        n += 4;
    }
    *len = n;
    return true;
}

static bool decode(const uint8_t *p, struct insn *in) {
    unsigned i = 0;
    bool opsize = false, rex_w = false;
    memset(in, 0, sizeof(*in));
    in->rip_off = -1;
    for (;; i++) {
        uint8_t b = p[i];
        if (b == 0x66) { opsize = true; continue; }
        if (b == 0x67 || b == 0xf0 || b == 0xf2 || b == 0xf3 || b == 0x2e ||
                b == 0x36 || b == 0x3e || b == 0x26 || b == 0x64 || b == 0x65)
            continue;
        break;
    }
    if (i > 4)
        return false;
    if ((p[i] & 0xf0) == 0x40) {
        rex_w = p[i] & 8;
        i++;
    }
    uint8_t op = p[i++];
    unsigned immz = opsize ? 2 : 4;
    bool has_modrm = false;
    unsigned imm = 0;

    if (op == 0x0f) {
        uint8_t op2 = p[i++];
        if (op2 == 0x38) {
            i++;
            has_modrm = true;
        } else if (op2 == 0x3a) {
            i++;
            has_modrm = true;
            imm = 1;
        } else if (op2 >= 0x80 && op2 <= 0x8f) {
            in->rel_off = (int) i;
            in->rel_size = 4;
            in->len = i + 4;
            return true;
        } else if (op2 == 0x05 || op2 == 0x0b || op2 == 0x31 || op2 == 0xa2 ||
                   op2 == 0x77 || (op2 >= 0xc8 && op2 <= 0xcf)) {
            // no operands
        } else if (op2 == 0x70 || (op2 >= 0x71 && op2 <= 0x73) || op2 == 0xa4 ||
                   op2 == 0xac || op2 == 0xba || op2 == 0xc2 || op2 == 0xc4 ||
                   op2 == 0xc5 || op2 == 0xc6) {
            has_modrm = true;
            imm = 1;
        } else if (op2 == 0x0d || (op2 >= 0x10 && op2 <= 0x17) ||
                   (op2 >= 0x18 && op2 <= 0x1f) || (op2 >= 0x28 && op2 <= 0x2f) ||
                   (op2 >= 0x40 && op2 <= 0x6f) || (op2 >= 0x74 && op2 <= 0x76) ||
                   (op2 >= 0x78 && op2 <= 0x7f) || (op2 >= 0x90 && op2 <= 0x9f) ||
                   op2 == 0xa3 || op2 == 0xa5 || op2 == 0xab || op2 == 0xad ||
                   op2 == 0xae || op2 == 0xaf || (op2 >= 0xb0 && op2 <= 0xb1) ||
                   (op2 >= 0xb3 && op2 <= 0xb8) || (op2 >= 0xbb && op2 <= 0xc1) ||
                   op2 == 0xc3 || op2 == 0xc7 || op2 >= 0xd0) {
            has_modrm = true;
        } else {
            return false;
        }
    } else if (op < 0x40) {
        unsigned lo = op & 7;
        if (lo <= 3)
            has_modrm = true;
        else if (lo == 4)
            imm = 1;
        else if (lo == 5)
            imm = immz;
        else
            return false;  // segment pushes/pops, prefixes, BCD
    } else if (op >= 0x50 && op <= 0x5f) {
        // push/pop reg
    } else if (op == 0x63) {
        has_modrm = true;
    } else if (op == 0x68) {
        imm = immz;
    } else if (op == 0x69) {
        has_modrm = true;
        imm = immz;
    } else if (op == 0x6a) {
        imm = 1;
    } else if (op == 0x6b) {
        has_modrm = true;
        imm = 1;
    } else if (op >= 0x70 && op <= 0x7f) {
        in->rel_off = (int) i;
        in->rel_size = 1;
        in->len = i + 1;
        return true;
    } else if (op == 0x80 || op == 0x83 || op == 0xc0 || op == 0xc1 || op == 0xc6) {
        has_modrm = true;
        imm = 1;
    } else if (op == 0x81 || op == 0xc7) {
        has_modrm = true;
        imm = immz;
    } else if (op >= 0x84 && op <= 0x8f) {
        has_modrm = true;
    } else if ((op >= 0x90 && op <= 0x99) || (op >= 0x9c && op <= 0x9f) ||
               (op >= 0xa4 && op <= 0xa7) || (op >= 0xaa && op <= 0xaf) ||
               op == 0xc9 || op == 0xcc || (op >= 0xf4 && op <= 0xf5) ||
               (op >= 0xf8 && op <= 0xfd)) {
        // no operands
    } else if (op == 0xa8) {
        imm = 1;
    } else if (op == 0xa9) {
        imm = immz;
    } else if (op >= 0xb0 && op <= 0xb7) {
        imm = 1;
    } else if (op >= 0xb8 && op <= 0xbf) {
        imm = rex_w ? 8 : immz;
    } else if (op == 0xc2) {
        imm = 2;
        in->uncond_exit = true;
    } else if (op == 0xc3) {
        in->uncond_exit = true;
    } else if (op == 0xcd) {
        imm = 1;
    } else if ((op >= 0xd0 && op <= 0xd3) || (op >= 0xd8 && op <= 0xdf) ||
               op == 0xfe) {
        has_modrm = true;
    } else if (op == 0xe8 || op == 0xe9) {
        in->rel_off = (int) i;
        in->rel_size = 4;
        in->len = i + 4;
        in->uncond_exit = op == 0xe9;
        return true;
    } else if (op == 0xeb || (op >= 0xe0 && op <= 0xe3)) {
        in->rel_off = (int) i;
        in->rel_size = 1;
        in->len = i + 1;
        in->uncond_exit = op == 0xeb;
        return true;
    } else if (op == 0xf6 || op == 0xf7) {
        has_modrm = true;
        unsigned reg = (p[i] >> 3) & 7;
        if (reg <= 1)
            imm = op == 0xf6 ? 1 : immz;
    } else if (op == 0xff) {
        has_modrm = true;
        unsigned reg = (p[i] >> 3) & 7;
        if (reg == 4 || reg == 5)
            in->uncond_exit = true;  // jmp *r/m
        if (reg == 7)
            return false;
    } else {
        return false;
    }

    if (has_modrm) {
        unsigned mlen;
        int rip;
        modrm_len(&p[i], &mlen, &rip, i);
        in->rip_off = rip;
        i += mlen;
    }
    in->len = i + imm;
    return in->len <= 15;
}

// ---------------------------------------------------------------- setup

static bool xjit_on;
static bool stats_on;
static const char *stats_file;
static FILE *stats_out(void) {
    FILE *f = stats_file ? fopen(stats_file, "a") : NULL;
    return f ? f : stderr;
}
static _Atomic uint64_t st_blocks, st_runs, st_gadgets, st_bytes, st_nomem;
static unsigned st_tpls, st_tpls_usable;

static int32_t rd32(const uint8_t *p) {
    int32_t v;
    memcpy(&v, p, 4);
    return v;
}

static FILE *check_file;  // ISH_XJIT_CHECK=<file>: every decoded instruction, for
                         // comparing the decoder with a disassembler

static void tpl_prepare(struct tpl *t, const struct tpl_entry *e) {
    t->gadget = e->gadget;
    t->code = e->start;
    t->pop = (int8_t) e->pop;
    t->noinline = e->noinline != 0;
    t->usable = false;
    size_t size = (size_t) (e->end - e->start);
    if (e->pop < -1 || e->noinline || size == 0 || size > 4096)
        return;

    // Walk the instructions: find the fields to relocate, and (for one that
    // falls through) the trailing jump to its own end, which gets dropped.
    size_t off = 0, last = 0;
    struct insn in = {0};
    bool last_exit = false;
    while (off < size) {
        if (!decode(e->start + off, &in)) {
            if (check_file)
                fprintf(check_file, "FAIL %p\n", (void *) (e->start + off));
            return;
        }
        if (check_file)
            fprintf(check_file, "%p %u\n", (void *) (e->start + off), in.len);
        if (off + in.len > size)
            return;
        if (in.rel_size != 0) {
            const uint8_t *field = e->start + off + in.rel_off;
            const uint8_t *target = e->start + off + in.len +
                (in.rel_size == 4 ? rd32(field) : (int8_t) *field);
            bool inside = target >= e->start && target <= e->end;
            if (target == e->end) {
                if (t->nends == MAX_RELOCS)
                    return;
                t->ends[t->nends++] = (uint16_t) ((off + in.rel_off) << 1 | (in.rel_size == 4));
            }
            if (!inside) {
                if (in.rel_size != 4 || t->nrelocs == MAX_RELOCS)
                    return;
                t->relocs[t->nrelocs++] = (uint16_t) (off + in.rel_off);
            }
        }
        if (in.rip_off >= 0) {
            const uint8_t *target = e->start + off + in.len + rd32(e->start + off + in.rip_off);
            if (target < e->start || target > e->end) {
                if (t->nrelocs == MAX_RELOCS)
                    return;
                t->relocs[t->nrelocs++] = (uint16_t) (off + in.rip_off);
            }
        }
        last = off;
        last_exit = in.uncond_exit;
        off += in.len;
    }
    if (off != size)
        return;

    size_t len = size;
    if (e->pop >= 0) {
        // gret is `addq; jmp <end>`: the last one jumps to where the next
        // template will be copied, so it can go.
        if (!last_exit || in.rel_size == 0)
            return;
        const uint8_t *field = e->start + last + in.rel_off;
        const uint8_t *target = e->start + last + in.len +
            (in.rel_size == 4 ? rd32(field) : (int8_t) *field);
        if (target != e->end)
            return;
        len = last;
        t->trim = (uint8_t) (size - len);
        t->nends--;  // the dropped jump itself (it was the last one recorded)
    } else if (!last_exit) {
        return;  // a terminal template must leave by itself
    }
    t->len = (uint16_t) len;
    t->usable = true;
}

static void xjit_init(void) {
    const char *e = getenv("ISH_XJIT");
    xjit_on = !(e && (!strcmp(e, "0") || !strcmp(e, "off")));
    e = getenv("ISH_XJIT_STATS");
    stats_on = e && e[0] != '\0' && e[0] != '0';
    // anything but 1 names a file to write them to
    if (stats_on && strcmp(e, "1") != 0)
        stats_file = e;
    if (!xjit_on)
        return;

    if ((e = getenv("ISH_XJIT_CHECK")) != NULL && (check_file = fopen(e, "w")) != NULL)
        fprintf(check_file, "TABLE %p\n", (void *) tpl_table_start);
    size_t n = (size_t) (tpl_table_end - tpl_table_start);
    unsigned size = 64;
    while (size < n * 2)
        size *= 2;
    struct tpl *table = calloc(size, sizeof(struct tpl));
    if (table == NULL) {
        xjit_on = false;
        return;
    }
    for (size_t i = 0; i < n; i++) {
        const struct tpl_entry *entry = &tpl_table_start[i];
        if (entry->gadget == 0)
            continue;  // a template whose gadget isn't built
        unsigned mask = size - 1;
        unsigned slot = (unsigned) (entry->gadget >> 3) & mask;
        while (table[slot].gadget != 0)
            slot = (slot + 1) & mask;
        tpl_prepare(&table[slot], entry);
        st_tpls++;
        if (table[slot].usable)
            st_tpls_usable++;
    }
    tpl_hash_size = size;
    tpls = table;
    if (check_file) {
        fclose(check_file);
        check_file = NULL;
    }
}

static pthread_once_t init_once = PTHREAD_ONCE_INIT;

// ---------------------------------------------------------------- code memory

// rel32 calls and rip-relative operands in the copies must still reach the
// binary, so the code region sits within +-2GB of the templates. So does a
// data region for what translated code writes (xlat.c's guesses): writing
// to a page that holds code is self-modifying code to the CPU, and makes
// Rosetta throw its translation of the page away.
// On Windows (the target) both are reserved up front and committed as they
// fill, straight on VirtualAlloc. Elsewhere (the Mac build is for checking
// translations) they are plain RWX / RW mappings the kernel backs lazily.
#define REGION_SIZE (1024u << 20)
#define DATA_REGION_SIZE (64u << 20)
#define COMMIT_STEP (16u << 20)
#define CHUNK 64u
// Free lists: one per size up to NEXACT chunks, then one per power of two
// (rounded up), so that everything freed can be handed out again.
#define NEXACT 64
#define NCLASSES (NEXACT + 32)

// The free pieces of one size, lowest address first (a binary heap of chunk
// numbers). Taking the lowest keeps what a new guest process translates
// packed at the low end, as in a fresh region: a process frees its blocks in
// hash order, and handing them out again last-in-first-out scattered each
// later process's code over everything ever freed (a long-lived ish ran
// the same command 40% slower after a few runs).
struct free_heap {
    uint32_t *v;
    uint32_t n, cap;
};

struct arena {
    uint8_t *base, *next, *end, *committed;
    int prot;
    struct free_heap free_lists[NCLASSES];
    pthread_mutex_t lock;
};

static bool heap_push(struct free_heap *h, uint32_t x) {
    if (h->n == h->cap) {
        uint32_t cap = h->cap ? h->cap * 2 : 64;
        uint32_t *v = realloc(h->v, cap * sizeof(*v));
        if (v == NULL)
            return false;
        h->v = v;
        h->cap = cap;
    }
    uint32_t i = h->n++;
    while (i > 0 && h->v[(i - 1) / 2] > x) {
        h->v[i] = h->v[(i - 1) / 2];
        i = (i - 1) / 2;
    }
    h->v[i] = x;
    return true;
}

static uint32_t heap_pop(struct free_heap *h) {
    uint32_t top = h->v[0], x = h->v[--h->n], i = 0;
    for (;;) {
        uint32_t c = 2 * i + 1;
        if (c >= h->n)
            break;
        if (c + 1 < h->n && h->v[c + 1] < h->v[c])
            c++;
        if (h->v[c] >= x)
            break;
        h->v[i] = h->v[c];
        i = c;
    }
    h->v[i] = x;
    return top;
}
static struct arena code_arena = {.lock = PTHREAD_MUTEX_INITIALIZER};
static struct arena data_arena = {.lock = PTHREAD_MUTEX_INITIALIZER};
static uint8_t *region, *region_end;  // the code region

struct code_hdr {
    uint32_t chunks;
    uint32_t pad[3];
};

// rel32 reach, less some room for the binary itself around the anchor
static bool reachable(uintptr_t a, uintptr_t b) {
    uintptr_t d = a > b ? a - b : b - a;
    return d < (1u << 31) - (64u << 20);
}

// A mapping of size whose both ends reach both ends of [lo, hi].
static void *map_near(uintptr_t lo, uintptr_t hi, size_t size, int prot) {
    for (int64_t delta = 0; delta < (1ll << 31); delta += 64ll << 20) {
        for (int sign = 1; sign >= -1; sign -= 2) {
            uintptr_t hint = (sign > 0 ? hi + delta : lo - delta - size) & ~(uintptr_t) 0xfffff;
#ifdef _WIN32
            (void) prot;
            void *p = px_jit_reserve((void *) hint, size);
            if (p == NULL)
                continue;
#else
            void *p = mmap((void *) hint, size, prot, MAP_PRIVATE | MAP_ANON, -1, 0);
            if (p == MAP_FAILED)
                continue;
#endif
            uintptr_t a = (uintptr_t) p, b = a + size;
            if (reachable(a, lo) && reachable(a, hi) && reachable(b, lo) && reachable(b, hi))
                return p;
#ifdef _WIN32
            px_jit_release(p);
#else
            munmap(p, size);
#endif
        }
    }
    return NULL;
}

static void region_init(void) {
    uintptr_t anchor = (uintptr_t) tpl_table_start;
    uint8_t *code = map_near(anchor, anchor, REGION_SIZE, PROT_READ | PROT_WRITE | PROT_EXEC);
    if (code == NULL)
        return;
    // the code addresses it rip-relative
    uint8_t *data = map_near((uintptr_t) code, (uintptr_t) code + REGION_SIZE, DATA_REGION_SIZE,
                             PROT_READ | PROT_WRITE);
    if (data == NULL) {
#ifdef _WIN32
        px_jit_release(code);
#else
        munmap(code, REGION_SIZE);
#endif
        return;
    }
    code_arena.base = code_arena.next = region = code;
    code_arena.end = region_end = code + REGION_SIZE;
    code_arena.prot = PROT_READ | PROT_WRITE | PROT_EXEC;
    data_arena.base = data_arena.next = data;
    data_arena.end = data + DATA_REGION_SIZE;
    data_arena.prot = PROT_READ | PROT_WRITE;
#ifdef _WIN32
    code_arena.committed = code;   // committed as they fill
    data_arena.committed = data;
#else
    code_arena.committed = code_arena.end;
    data_arena.committed = data_arena.end;
#endif
}

// size in chunks -> free list, and the size that list's pieces have
static unsigned size_class(size_t *chunks) {
    if (*chunks <= NEXACT)
        return (unsigned) *chunks - 1;
    unsigned log = 0;
    while (((size_t) 1 << log) < *chunks)
        log++;
    *chunks = (size_t) 1 << log;
    return NEXACT + log;
}

static void *arena_alloc(struct arena *ar, size_t size) {
    size_t chunks = (size + sizeof(struct code_hdr) + CHUNK - 1) / CHUNK;
    unsigned cls = size_class(&chunks);
    if (cls >= NCLASSES)
        return NULL;
    pthread_mutex_lock(&ar->lock);
    struct code_hdr *h = NULL;
    if (ar->free_lists[cls].n > 0) {
        h = (struct code_hdr *) (ar->base + (size_t) heap_pop(&ar->free_lists[cls]) * CHUNK);
    } else if (ar->base != NULL && ar->next + chunks * CHUNK <= ar->end) {
        uint8_t *end = ar->next + chunks * CHUNK;
        while (end > ar->committed) {
            size_t step = (size_t) (ar->end - ar->committed);
            if (step > COMMIT_STEP)
                step = COMMIT_STEP;
#ifdef _WIN32
            if (!px_jit_commit(ar->committed, step, ar->prot & PROT_EXEC))
                break;
#else
            break;  // mapped whole
#endif
            ar->committed += step;
        }
        if (end <= ar->committed) {
            h = (struct code_hdr *) ar->next;
            ar->next = end;
        }
    }
    pthread_mutex_unlock(&ar->lock);
    if (h == NULL)
        return NULL;
    h->chunks = (uint32_t) chunks;
    return h + 1;
}

static void arena_free(struct arena *ar, void *p) {
    struct code_hdr *h = (struct code_hdr *) p - 1;
    size_t chunks = h->chunks;
    unsigned cls = size_class(&chunks);
    pthread_mutex_lock(&ar->lock);
    // (if the heap cannot grow the piece is lost, not the program)
    heap_push(&ar->free_lists[cls], (uint32_t) (((uint8_t *) h - ar->base) / CHUNK));
    pthread_mutex_unlock(&ar->lock);
}

static void *code_alloc(size_t size) { return arena_alloc(&code_arena, size); }
static void code_free(void *p) { arena_free(&code_arena, p); }

static void xjit_splice(struct fiber_block *block);
#ifdef GUEST_X86_64
#include "asbestos/guest-x86_64/xlat.c"
#endif

// ---------------------------------------------------------------- blocks

static void xjit_init_all(void) {
    xjit_init();
    if (xjit_on)
        region_init();
#ifdef GUEST_X86_64
    if (xjit_on && region != NULL)
        xlat_init();
#endif
}

// A run: stream slots [first, end) holding `count` consecutive gadgets that
// all have usable templates. The last one may be terminal.
struct run {
    unsigned first;
    unsigned count;
    size_t size;
    bool terminal;
};
#define MAX_RUNS 512

// jmp *-8(%r9): the gadget dispatch, once at the end of a run.
static const uint8_t dispatch[] = {0x41, 0xff, 0x61, 0xf8};

static uint8_t *emit_run(uint8_t *out, const uintptr_t *code, const struct run *r) {
    unsigned slot = r->first;
    for (unsigned k = 0; k < r->count; k++) {
        const struct tpl *t = tpl_lookup(code[slot]);
        memcpy(out, t->code, t->len);
        // Fields that point outside the template keep their target: a rel32
        // is relative to its own end, which moved by (out - t->code).
        intptr_t moved = (intptr_t) (out - t->code);
        for (unsigned j = 0; j < t->nrelocs; j++) {
            int64_t v = (int64_t) rd32(t->code + t->relocs[j]) - moved;
            int32_t v32 = (int32_t) v;
            memcpy(out + t->relocs[j], &v32, 4);
        }
        for (unsigned j = 0; j < t->nends; j++) {
            unsigned at = t->ends[j] >> 1;
            if (t->ends[j] & 1) {
                int32_t v32 = rd32(t->code + at) - t->trim;
                memcpy(out + at, &v32, 4);
            } else {
                out[at] = (uint8_t) ((int8_t) t->code[at] - t->trim);
            }
        }
        out += t->len;
        slot += 1 + (unsigned) t->pop;
    }
    if (!r->terminal) {
        memcpy(out, dispatch, sizeof(dispatch));
        out += sizeof(dispatch);
    }
    return out;
}

void xjit_link(struct fiber_block *from, int i, struct fiber_block *to) {
    static int nolink = -1;
    if (nolink < 0)
        nolink = getenv("ISH_XLAT_NOLINK") != NULL;
    uint8_t *field = from->xjit_link[i];
    if (field == NULL || to->xjit_entry == 0 || nolink)
        return;
    int64_t v = (int64_t) to->xjit_entry - (int64_t) (uintptr_t) (field + 4);
    if (v != (int32_t) v)
        return;
    __atomic_store_n((int32_t *) field, (int32_t) v, __ATOMIC_RELEASE);
}

void xjit_unlink(struct fiber_block *from, int i) {
    uint8_t *field = from->xjit_link[i];
    if (field == NULL)
        return;
    int32_t v = (int32_t) ((intptr_t) from->xjit_link_orig[i] - (intptr_t) (field + 4));
    __atomic_store_n((int32_t *) field, v, __ATOMIC_RELEASE);
}

void xjit_block(struct fiber_block *block) {
    block->xjit_code = NULL;
    block->xjit_data = NULL;
    block->xjit_entry = 0;
    block->xjit_link[0] = block->xjit_link[1] = NULL;
    block->xjit_faults = NULL;
#ifdef GUEST_X86_64
    block->xjit_stub = NULL;
    block->xjit_counter = NULL;
#endif
    pthread_once(&init_once, xjit_init_all);
    if (!xjit_on || region == NULL)
        return;
#ifdef GUEST_X86_64
    if (xlat_block(block))
        return;
#endif
    xjit_splice(block);
}

// Splices the templates of the block's gadget runs together (the i386
// guest's JIT; for x86_64, what a block the translator can't do gets).
static void xjit_splice(struct fiber_block *block) {
    uintptr_t *code = block->code;
    size_t n = block->used;
    // Slots something else jumps into (skip targets) must start a run: only
    // a run's first slot points at its native code.
    static __thread uint8_t *entry;
    static __thread size_t entry_size;
    if (entry_size < n) {
        free(entry);
        entry_size = n * 2;
        entry = malloc(entry_size);
        if (entry == NULL) {
            entry_size = 0;
            return;
        }
    }
    memset(entry, 0, n);

    struct run runs[MAX_RUNS];
    unsigned nruns = 0;
    size_t total = 0;
    for (int pass = 0; pass < 2; pass++) {
        // Pass 0 marks the entry slots, pass 1 collects the runs.
        struct run cur = {0};
        bool in_run = false;
        size_t i = 0;
        while (i < n) {
            const struct tpl *t = tpl_lookup(code[i]);
            if (t == NULL || t->pop < -1)
                break;  // unknown arguments: can't find the next gadget
            if (pass == 0) {
                if (t->noinline && t->pop == 1 && i + 1 < n) {
                    // skip_*: its argument is the size in bytes of what it skips
                    size_t target = i + 2 + code[i + 1] / sizeof(uintptr_t);
                    if (target < n)
                        entry[target] = 1;
                }
            } else {
                if (in_run && (!t->usable || entry[i])) {
                    if (cur.count >= 2 && nruns < MAX_RUNS) {
                        runs[nruns++] = cur;
                        total += cur.size + sizeof(dispatch) + 16;
                    }
                    in_run = false;
                }
                if (t->usable) {
                    if (!in_run) {
                        cur = (struct run) {.first = (unsigned) i};
                        in_run = true;
                    }
                    cur.count++;
                    cur.size += t->len;
                    cur.terminal = t->pop < 0;
                }
            }
            if (t->pop < 0)
                break;  // terminal: the block's code ends here
            i += 1 + (unsigned) t->pop;
        }
        if (pass == 1 && in_run && cur.count >= 2 && nruns < MAX_RUNS) {
            runs[nruns++] = cur;
            total += cur.size + sizeof(dispatch) + 16;
        }
    }
    if (nruns == 0)
        return;

    uint8_t *mem = code_alloc(total);
    if (mem == NULL) {
        atomic_fetch_add_explicit(&st_nomem, 1, memory_order_relaxed);
        return;
    }
    uint8_t *out = mem;
    unsigned gadgets = 0;
    for (unsigned r = 0; r < nruns; r++) {
        out = (uint8_t *) (((uintptr_t) out + 15) & ~(uintptr_t) 15);
        uint8_t *start = out;
        out = emit_run(out, code, &runs[r]);
        code[runs[r].first] = (uintptr_t) start;
        gadgets += runs[r].count;
    }
    block->xjit_code = mem;
    if (stats_on) {
        atomic_fetch_add_explicit(&st_blocks, 1, memory_order_relaxed);
        atomic_fetch_add_explicit(&st_runs, nruns, memory_order_relaxed);
        atomic_fetch_add_explicit(&st_gadgets, gadgets, memory_order_relaxed);
        atomic_fetch_add_explicit(&st_bytes, (uint64_t) (out - mem), memory_order_relaxed);
    }
}

void xjit_block_free(struct fiber_block *block) {
#ifdef GUEST_X86_64
    fm_forget(block);
#endif
    if (block->xjit_code != NULL)
        code_free(block->xjit_code);
#ifdef GUEST_X86_64
    if (block->xjit_stub != NULL)
        code_free(block->xjit_stub);
    if (block->xjit_counter != NULL)
        arena_free(&data_arena, block->xjit_counter);
    block->xjit_stub = NULL;
    block->xjit_counter = NULL;
#endif
    if (block->xjit_data != NULL)
        arena_free(&data_arena, block->xjit_data);
    block->xjit_data = NULL;
    block->xjit_code = NULL;
}

void xjit_dump_stats(void) {
    if (!stats_on)
        return;
    FILE *out = stats_out();
    fprintf(out, "[xjit] templates=%u usable=%u blocks=%llu runs=%llu gadgets=%llu bytes=%llu nomem=%llu region=%p\n",
            st_tpls, st_tpls_usable,
            (unsigned long long) st_blocks, (unsigned long long) st_runs,
            (unsigned long long) st_gadgets, (unsigned long long) st_bytes,
            (unsigned long long) st_nomem, (void *) region);
#ifdef GUEST_X86_64
    if (xlat_on)
        xlat_dump_stats(out);
#endif
    if (out != stderr)
        fclose(out);
}
