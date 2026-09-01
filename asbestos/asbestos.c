#define DEFAULT_CHANNEL instr
#include "debug.h"
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include "asbestos/asbestos.h"
#include "asbestos/gen.h"
#include "asbestos/frame.h"
#include "emu/cpu.h"
#include "emu/interrupt.h"
#include "emu/tlb.h"
#include "kernel/memory.h"
#include "util/list.h"
#include "util/signpost.h"
#ifdef GUEST_ARM64
#include "kernel/native_offload.h"
#endif

// Thread-local recovery state for JIT crash handling.
// When a host SIGSEGV occurs inside JIT code (due to a stale TLB pointer
// from a concurrent CoW), the signal handler redirects PC to
// jit_crash_trampoline via ucontext, which returns INT_GPF to the
// dispatch loop. handle_interrupt resolves via mem_ptr (CoW/GROWSDOWN).
//
// This avoids the overhead of _setjmp on every block entry (~1.5% of
// total execution time). The signal handler writes crash info directly
// to cpu_state via the _cpu pointer (x1) from ucontext.
__thread volatile sig_atomic_t in_jit;
_Atomic uint64_t s_dispatch_iterations = 0;
_Atomic uint64_t st_block_cache_miss = 0, st_block_compile = 0, st_chain_attempts = 0;
_Atomic uint64_t st_int_poke = 0, st_int_poke_foreign = 0, st_int_cycle = 0, st_poke_calls = 0;
// Stage-trace profiling hooks were part of an in-progress instrumentation whose
// definitions lived in a scratch file; provide no-op stubs so the tree links.
// (Enable real staging by restoring that file.)
int stage_trace_on(void) { return 0; }
void dump_stage_trace(void) { }

__thread volatile addr_t jit_saved_pc;  // block start PC, read by signal handler

// PC dispatch trace: capture first N consecutive guest PCs hitting the
// dispatch loop. Used for V8-realistic micro-bench harness. Set
// ISH_PC_TRACE_FILE=/path to enable; trace is filtered to a hot range
// for size, written at exit.
#define PC_TRACE_MAX 4096
static uint64_t g_pc_trace[PC_TRACE_MAX];
static _Atomic int g_pc_trace_n;
static int g_pc_trace_enabled = -1;
static uint64_t g_pc_trace_lo, g_pc_trace_hi;

static inline void pc_trace_record(uint64_t pc) {
    if (g_pc_trace_enabled == -1) {
        const char *path = getenv("ISH_PC_TRACE_FILE");
        g_pc_trace_enabled = path ? 1 : 0;
        const char *r = getenv("ISH_PC_TRACE_RANGE");
        if (r) {
            unsigned long long lo, hi;
            if (sscanf(r, "%llx-%llx", &lo, &hi) == 2) {
                g_pc_trace_lo = lo; g_pc_trace_hi = hi;
            }
        }
        if (g_pc_trace_lo == 0) {
            g_pc_trace_lo = 0xee900000ULL;
            g_pc_trace_hi = 0xee940000ULL;
        }
    }
    if (g_pc_trace_enabled <= 0) return;
    if (pc < g_pc_trace_lo || pc >= g_pc_trace_hi) return;
    int n = atomic_fetch_add_explicit(&g_pc_trace_n, 1, memory_order_relaxed);
    if (n < PC_TRACE_MAX) g_pc_trace[n] = pc;
}

void dump_pc_trace(void) {
    if (g_pc_trace_enabled <= 0) return;
    const char *path = getenv("ISH_PC_TRACE_FILE");
    if (!path) return;
    FILE *f = fopen(path, "w");
    if (!f) return;
    int n = atomic_load_explicit(&g_pc_trace_n, memory_order_relaxed);
    if (n > PC_TRACE_MAX) n = PC_TRACE_MAX;
    for (int i = 0; i < n; i++) fprintf(f, "0x%llx\n", (unsigned long long)g_pc_trace[i]);
    fclose(f);
    fprintf(stderr, "[pc_trace] wrote %d PCs to %s\n", n, path);
}

// PC histogram for trace-JIT feasibility study.
// Sampled at every INT_TIMER tick (every 1024 blocks). 1MB buckets covering
// low 4GB. Set ISH_PC_HIST=1 to enable. Dump via dump_pc_hist() at exit.
// ISH_PC_HIST_FINE: 256-byte buckets over a 16MB window based at 0xef900000
// (covers libpython + musl for function-level attribution). Default off keeps the
// original coarse 64KB/4GB histogram.
#define PC_HIST_FINE_BASE  0xeee00000ULL
#define PC_HIST_FINE_SHIFT 8            // 256-byte buckets
#define PC_HIST_FINE_N     (32*1024*1024 / 256)   // 16MB / 256 = 65536 buckets
static _Atomic uint64_t pc_hist_fine[PC_HIST_FINE_N];
static int pc_hist_fine_on = -1;
static inline int pc_fine_on(void) {
    if (pc_hist_fine_on == -1) { extern char *getenv(const char*);
        const char *e = getenv("ISH_PC_HIST_FINE"); pc_hist_fine_on = (e && e[0]=='1') ? 1 : 0; }
    return pc_hist_fine_on;
}
#define PC_HIST_BUCKETS 65536  // 64KB each, low 4GB
#define PC_HIST_SHIFT   16
static _Atomic uint64_t pc_hist[PC_HIST_BUCKETS];
static int pc_hist_enabled = -1;
static inline int pc_hist_on(void) {
    if (pc_hist_enabled == -1) {
        const char *e = getenv("ISH_PC_HIST");
        pc_hist_enabled = (e && e[0] == '1') ? 1 : 0;
    }
    return pc_hist_enabled;
}
void dump_pc_hist(void) {
    if (!pc_hist_on()) return;
    uint64_t total = 0;
    for (int i = 0; i < PC_HIST_BUCKETS; i++) total += pc_hist[i];
    if (total == 0) return;
    /* Per-process file dump: aggregate profiles across fork'd subprocesses (e.g.
     * pip's wheel-build children). Set ISH_PC_HIST_DIR=/dir; each exiting guest
     * process writes raw "bucket count" pairs to <dir>/hist.<pid>.<nonce>. */
    const char *dir = getenv("ISH_PC_HIST_DIR");
    if (dir) {
        char path[512];
        extern int current_pid(void);
        snprintf(path, sizeof(path), "%s/hist.%d.%llu", dir, current_pid(),
                 (unsigned long long)total);
        FILE *pf = fopen(path, "w");
        if (pf) {
            if (pc_fine_on()) {   /* 256-byte buckets: emit absolute addr + count */
                for (int i = 0; i < PC_HIST_FINE_N; i++)
                    if (pc_hist_fine[i])
                        fprintf(pf, "%llx %llu\n",
                                (unsigned long long)(PC_HIST_FINE_BASE + ((uint64_t)i << PC_HIST_FINE_SHIFT)),
                                (unsigned long long)pc_hist_fine[i]);
            } else {
                for (int i = 0; i < PC_HIST_BUCKETS; i++)
                    if (pc_hist[i]) fprintf(pf, "%d %llu\n", i, (unsigned long long)pc_hist[i]);
            }
            fclose(pf);
        }
    }
    fprintf(stderr, "=== PC histogram (insn-weighted, dispatched blocks, total=%llu) ===\n",
            (unsigned long long)total);
    for (int i = 0; i < PC_HIST_BUCKETS; i++) {
        uint64_t c = pc_hist[i];
        if (c == 0) continue;
        double pct = 100.0 * (double)c / (double)total;
        if (pct < 0.1) continue;
        fprintf(stderr, "  0x%08x-0x%08x  %8llu  %6.2f%%\n",
                i << PC_HIST_SHIFT, ((i + 1) << PC_HIST_SHIFT) - 1,
                (unsigned long long)c, pct);
    }
    fflush(stderr);
}
// --- Block execution profile (ISH_BLOCK_PROF_FILE=/path) ---
// Records exact per-block dispatch counts + insn length for trace-AOT
// feasibility study. Build with -DDISABLE_BLOCK_CHAINING for exact counts
// (otherwise chained transitions bypass the dispatch loop and are missed).
// Open-addressing hash, fixed size; dump at pid-1 exit.
#define BLK_PROF_SZ (1 << 21)   // 2M entries * 16B = 32MB
static struct { _Atomic uint64_t addr; _Atomic uint64_t cnt_len; } *blk_prof; // cnt_len = count<<8 | insns
static int blk_prof_enabled = -1;
static inline int blk_prof_on(void) {
    if (blk_prof_enabled == -1) {
        const char *e = getenv("ISH_BLOCK_PROF_FILE");
        if (e && !blk_prof) blk_prof = calloc(BLK_PROF_SZ, 16);
        blk_prof_enabled = (e && blk_prof) ? 1 : 0;
    }
    return blk_prof_enabled;
}
static inline void blk_prof_record(uint64_t addr, uint64_t insns) {
    if (insns > 255) insns = 255;
    uint64_t h = (addr * 0x9E3779B97F4A7C15ULL) >> 43;  // top 21 bits
    for (int probe = 0; probe < 64; probe++) {
        uint64_t i = (h + probe) & (BLK_PROF_SZ - 1);
        uint64_t cur = atomic_load_explicit(&blk_prof[i].addr, memory_order_relaxed);
        if (cur == addr) {
            atomic_fetch_add_explicit(&blk_prof[i].cnt_len, 256, memory_order_relaxed);
            return;
        }
        if (cur == 0) {
            uint64_t expect = 0;
            if (atomic_compare_exchange_strong(&blk_prof[i].addr, &expect, addr)) {
                atomic_fetch_add_explicit(&blk_prof[i].cnt_len, 256 + insns, memory_order_relaxed);
                return;
            }
            if (expect == addr) {
                atomic_fetch_add_explicit(&blk_prof[i].cnt_len, 256, memory_order_relaxed);
                return;
            }
        }
    }
}
void dump_block_prof(void) {
    const char *path = getenv("ISH_BLOCK_PROF_FILE");
    if (!path || !blk_prof) return;
    FILE *f = fopen(path, "w");
    if (!f) return;
    uint64_t n = 0;
    for (uint64_t i = 0; i < BLK_PROF_SZ; i++) {
        uint64_t a = blk_prof[i].addr;
        if (!a) continue;
        uint64_t cl = blk_prof[i].cnt_len;
        fprintf(f, "%llx %llu %llu\n", (unsigned long long)a,
                (unsigned long long)(cl & 0xff), (unsigned long long)(cl >> 8));
        n++;
    }
    fclose(f);
    fprintf(stderr, "[block_prof] wrote %llu blocks to %s\n", (unsigned long long)n, path);
}

// Marker set to 1 on iSH execution threads so the signal handler can distinguish
// iSH threads from app threads (Swift async, networking, UI).
__thread int ish_thread_marker;

// Architecture-specific instruction pointer access
#if defined(GUEST_ARM64)
#define CPU_IP(cpu) ((cpu)->pc)
#define CPU_HAS_SINGLE_STEP 0
#else
#define CPU_IP(cpu) ((cpu)->eip)
#define CPU_HAS_SINGLE_STEP ((cpu)->tf)
#endif

extern int current_pid(void);

// Stubs for debug hooks referenced from assembly/gen.c/tlb.c
volatile bool g_trace_highbits = false;
volatile addr_t g_watch_page_val = 0;
// Second watched page, contiguous with a shadow copy of the first so the asm
// fast path can ldp both. main.c keeps g_watch_pages[0] == g_watch_page_val.
volatile addr_t g_watch_pages[2] = {0, 0};

#ifdef ISH_GADGET_PROFILE
// Gadget call profile: ring buffer of next-gadget pointers, written by `gret`.
// 64K entries; reader (atexit handler) processes after run.
__attribute__((aligned(64))) uint64_t g_profile_buf[65536] = {0};
__attribute__((aligned(64))) uint64_t g_profile_idx = 0;
#endif

void jit_trace_regs(struct cpu_state *cpu) { (void)cpu; }

// Write-watchpoint recording (diagnostic). Arm with ISH_WATCH_PAGE=<hex page>;
// every guest store to that page (asm fast path via jit_watch_write_hit, C
// helpers via c_watch_write_hit) is recorded in a ring, dumped on fatal GPF
// (ISH_GPF_BT) and at exit. block_pc is the start PC of the translated block
// doing the store — symbolizable against the guest binary.
#define WATCH_RING 8192
// stored_lo/hi = a candidate stored value captured from cpu regs at the hit
// (for the numCalleeLocals block we snapshot x9, the value being str'd).
static struct watch_hit { uint64_t addr, block_pc, cpu_pc, stored; uint32_t field_val; int c_path; } watch_ring[WATCH_RING];
static _Atomic uint32_t watch_ring_n;
// Optional sub-page record filter (ISH_WATCH_LO/ISH_WATCH_HI, hex): keeps the
// ring from being flooded by unrelated writes to the same hot heap page.
// ISH_WATCH_FIELD: a u32 guest address snapshotted at every hit (pre-store),
// so the timeline shows exactly when the field changed and which recorded
// write (or unrecorded host-side write) it correlates with.
volatile uint64_t g_watch_lo, g_watch_hi, g_watch_lo2, g_watch_hi2, g_watch_field;
uint32_t mem_watch_peek32(uint64_t addr);  // kernel/memory.c
static inline void watch_record2(uint64_t addr, uint64_t block_pc, uint64_t cpu_pc, uint64_t stored, int c_path) {
    if (c_path < 2 && g_watch_hi &&
            (addr < g_watch_lo || addr >= g_watch_hi) &&
            !(g_watch_hi2 && addr >= g_watch_lo2 && addr < g_watch_hi2))
        return;
    uint32_t fv = g_watch_field ? mem_watch_peek32(g_watch_field) : 0;
    uint32_t i = atomic_fetch_add_explicit(&watch_ring_n, 1, memory_order_relaxed);
    watch_ring[i % WATCH_RING] = (struct watch_hit){addr, block_pc, cpu_pc, stored, fv, c_path};
}
static inline void watch_record(uint64_t addr, uint64_t block_pc, uint64_t cpu_pc, int c_path) {
    watch_record2(addr, block_pc, cpu_pc, 0, c_path);
}
void c_watch_write_hit(addr_t addr, const char *caller) {
    (void)caller;
    watch_record(addr, jit_saved_pc, 0, 1);
}
// Register snapshot at the moment the watched FIELD itself is stored: the
// store gadget reads its value from cpu->regs, so regs[] shows both the value
// about to be written and the source pointers the guest derived it from.
uint64_t g_watch_field_regs[32];
volatile int g_watch_field_hit_count;
// Snapshot the LAST store to the watched field (not just the first): the store
// that leaves the value wrong is what we want, and it's usually the last one.
uint64_t g_watch_field_last_regs[32];
void jit_watch_write_hit(struct cpu_state *cpu, addr_t store_addr, unsigned long *code_ptr) {
    (void)code_ptr;
    if (cpu && g_watch_field && store_addr == g_watch_field) {
        if (g_watch_field_hit_count++ == 0)
            for (int i = 0; i < 31; i++) g_watch_field_regs[i] = cpu->regs[i];
        for (int i = 0; i < 31; i++) g_watch_field_last_regs[i] = cpu->regs[i];
        g_watch_field_regs[31] = g_watch_field_last_regs[31] = cpu->pc;
    }
    // For the numCalleeLocals block (0x41eee2c), x9 is the value being stored
    // to [x0+0x10]. Capture it so the ring shows what actually got written.
    uint64_t stored = cpu ? cpu->regs[9] : 0;
    watch_record2(store_addr, jit_saved_pc, cpu ? cpu->pc : 0, stored, 0);
}
// Host-side write acquisition on the watched page (mem_ptr MEM_WRITE: syscall
// user_write, madvise zeroing, CoW copies...) — bypasses the guest store
// gadgets, so record it separately; not subject to the LO/HI window.
void watch_record_memptr(uint64_t addr) {
    watch_record(addr, jit_saved_pc, 0, 2);
}
void dump_watch_hits(uint64_t filter_lo, uint64_t filter_hi) {
    uint32_t total = atomic_load_explicit(&watch_ring_n, memory_order_relaxed);
    uint32_t kept = total < WATCH_RING ? total : WATCH_RING;
    fprintf(stderr, "[watch] %u total hits, showing last %u in [0x%llx,0x%llx)\n",
            total, kept, (unsigned long long)filter_lo, (unsigned long long)filter_hi);
    for (uint32_t k = 0; k < kept; k++) {
        uint32_t i = (total - kept + k) % WATCH_RING;
        struct watch_hit *h = &watch_ring[i];
        if (filter_hi && (h->addr < filter_lo || h->addr >= filter_hi))
            continue;
        fprintf(stderr, "[watch] #%u addr=0x%llx block_pc=0x%llx cpu_pc=0x%llx field=0x%x %s\n",
                total - kept + k, (unsigned long long)h->addr,
                (unsigned long long)h->block_pc, (unsigned long long)h->cpu_pc,
                h->field_val,
                h->c_path == 2 ? "(mem_ptr)" : h->c_path ? "(C)" : "(jit)");
    }
}
// Scan the whole ring for any store overlapping [addr, addr+8) — used at GPF to
// answer "was the crashing object's numCalleeLocals qword ever written?"
void dump_watch_hits_for(uint64_t addr) {
    uint32_t total = atomic_load_explicit(&watch_ring_n, memory_order_relaxed);
    uint32_t kept = total < WATCH_RING ? total : WATCH_RING;
    fprintf(stderr, "[watch-for] scanning %u hits for stores to [0x%llx,0x%llx)\n",
            kept, (unsigned long long)addr, (unsigned long long)(addr + 8));
    int found = 0;
    for (uint32_t k = 0; k < kept; k++) {
        uint32_t i = (total - kept + k) % WATCH_RING;
        struct watch_hit *h = &watch_ring[i];
        if (h->addr >= addr - 0x8 && h->addr < addr + 0x10) {
            fprintf(stderr, "[watch-for] #%u addr=0x%llx block_pc=0x%llx x9=0x%llx %s\n",
                    total - kept + k, (unsigned long long)h->addr,
                    (unsigned long long)h->block_pc, (unsigned long long)h->stored,
                    h->c_path == 2 ? "(mem_ptr)" : h->c_path ? "(C)" : "(jit)");
            found++;
        }
    }
    fprintf(stderr, "[watch-for] %d stores overlapping the qword\n", found);
}
void jit_highbit_alert(struct cpu_state *cpu) { (void)cpu; }

static void fiber_block_disconnect(struct asbestos *asbestos, struct fiber_block *block);
static void fiber_block_free(struct asbestos *asbestos, struct fiber_block *block);
static void fiber_free_jetsam(struct asbestos *asbestos);
static void fiber_resize_hash(struct asbestos *asbestos, size_t new_size);

struct asbestos *asbestos_new(struct mmu *mmu) {
    struct asbestos *asbestos = calloc(1, sizeof(struct asbestos));
    asbestos->mmu = mmu;
    fiber_resize_hash(asbestos, FIBER_INITIAL_HASH_SIZE);
    asbestos->page_hash = calloc(FIBER_PAGE_HASH_SIZE, sizeof(*asbestos->page_hash));
    list_init(&asbestos->jetsam);
    lock_init(&asbestos->lock);
    wrlock_init(&asbestos->jetsam_lock);
    atomic_init(&asbestos->jit_active_threads, 0);
    atomic_init(&asbestos->jetsam_gen, 0);
    return asbestos;
}

void asbestos_free(struct asbestos *asbestos) {
    for (size_t i = 0; i < asbestos->hash_size; i++) {
        struct fiber_block *block, *tmp;
        if (list_null(&asbestos->hash[i]))
            continue;
        list_for_each_entry_safe(&asbestos->hash[i], block, tmp, chain) {
            fiber_block_free(asbestos, block);
        }
    }
    fiber_free_jetsam(asbestos);
    free(asbestos->page_hash);
    free(asbestos->hash);
    free(asbestos);
}

static inline struct list *blocks_list(struct asbestos *asbestos, page_t page, int i) {
    // TODO is this a good hash function?
    return &asbestos->page_hash[page % FIBER_PAGE_HASH_SIZE].blocks[i];
}

void asbestos_invalidate_range(struct asbestos *absestos, page_t start, page_t end) {
    // [T-ish-mm-double-destroy-crash] Under CLONE_VM exit_group races the
    // second cleanup path may run pt_unmap_always after asbestos has already
    // been freed and nulled (see mem_destroy). Treat as no-op — the freed
    // asbestos had already been invalidated by the winning cleanup.
    if (absestos == NULL) return;
    lock(&absestos->lock);
    bool did_invalidate = false;
    struct fiber_block *block, *tmp;
    for (page_t page = start; page < end; page++) {
        for (int i = 0; i <= 1; i++) {
            struct list *blocks = blocks_list(absestos, page, i);
            if (list_null(blocks))
                continue;
            list_for_each_entry_safe(blocks, block, tmp, page[i]) {
                fiber_block_disconnect(absestos, block);
                block->is_jetsam = true;
                list_add(&absestos->jetsam, &block->jetsam);
                did_invalidate = true;
            }
        }
    }
    if (did_invalidate)
        absestos->invalidate_gen++;
    unlock(&absestos->lock);
}

void asbestos_invalidate_page(struct asbestos *asbestos, page_t page) {
    // [T-ish-mm-double-destroy-crash] See asbestos_invalidate_range —
    // a racing exit_group cleanup can reach this after asbestos was freed.
    if (asbestos == NULL) return;
    // Fast path: skip lock if no blocks exist on this page.
    // page_hash is only modified under asbestos->lock, and list_null is a
    // single pointer read, so a racy false-negative just means we take
    // the slow path unnecessarily (safe). A false-positive is impossible
    // because blocks are always added before being linked into page_hash.
    for (int i = 0; i <= 1; i++) {
        struct list *blocks = blocks_list(asbestos, page, i);
        if (!list_null(blocks))
            goto slow_path;
    }
    return;
slow_path:
    asbestos_invalidate_range(asbestos, page, page + 1);
}
void asbestos_invalidate_all(struct asbestos *asbestos) {
    // [T-ish-mm-double-destroy-crash] Safe no-op after cleanup race.
    if (asbestos == NULL) return;
    lock(&asbestos->lock);
    bool did_invalidate = false;
    struct fiber_block *block, *tmp;
    for (size_t bucket = 0; bucket < FIBER_PAGE_HASH_SIZE; bucket++) {
        for (int i = 0; i <= 1; i++) {
            struct list *blocks = &asbestos->page_hash[bucket].blocks[i];
            if (list_null(blocks))
                continue;
            list_for_each_entry_safe(blocks, block, tmp, page[i]) {
                fiber_block_disconnect(asbestos, block);
                block->is_jetsam = true;
                list_add(&asbestos->jetsam, &block->jetsam);
                did_invalidate = true;
            }
        }
    }
    if (did_invalidate)
        asbestos->invalidate_gen++;
    unlock(&asbestos->lock);
}

static void fiber_resize_hash(struct asbestos *asbestos, size_t new_size) {
    TRACE_(verbose, "%d resizing hash to %lu, using %lu bytes for gadgets\n", current_pid(), new_size, asbestos->mem_used);
    struct list *new_hash = calloc(new_size, sizeof(struct list));
    for (size_t i = 0; i < asbestos->hash_size; i++) {
        if (list_null(&asbestos->hash[i]))
            continue;
        struct fiber_block *block, *tmp;
        list_for_each_entry_safe(&asbestos->hash[i], block, tmp, chain) {
            list_remove(&block->chain);
            list_init_add(&new_hash[block->addr % new_size], &block->chain);
        }
    }
    free(asbestos->hash);
    asbestos->hash = new_hash;
    asbestos->hash_size = new_size;
}

static void fiber_insert(struct asbestos *asbestos, struct fiber_block *block) {
    asbestos->mem_used += block->used;
    asbestos->num_blocks++;
    // target an average hash chain length of 1-2
    if (asbestos->num_blocks >= asbestos->hash_size * 2)
        fiber_resize_hash(asbestos, asbestos->hash_size * 2);

    list_init_add(&asbestos->hash[block->addr % asbestos->hash_size], &block->chain);
    list_init_add(blocks_list(asbestos, PAGE(block->addr), 0), &block->page[0]);
    if (PAGE(block->addr) != PAGE(block->end_addr))
        list_init_add(blocks_list(asbestos, PAGE(block->end_addr), 1), &block->page[1]);
}

static struct fiber_block *fiber_lookup(struct asbestos *asbestos, addr_t addr) {
    struct list *bucket = &asbestos->hash[addr % asbestos->hash_size];
    if (list_null(bucket))
        return NULL;
    struct fiber_block *block;
    list_for_each_entry(bucket, block, chain) {
        if (block->addr == addr)
            return block;
    }
    return NULL;
}

static struct fiber_block *fiber_block_compile(addr_t ip, struct tlb *tlb) {
    ISH_SIGNPOST_SCOPE_BEGIN(jit, "block_compile", _bc_spid);
    struct gen_state state;
    TRACE("%d %08x --- compiling:\n", current_pid(), ip);
    if (!gen_start(ip, &state))
        return NULL;
#ifdef GUEST_ARM64
    // Pre-built gadget offload (ARM64 only): if a native spec_fn is registered for
    // this function entry, emit one prebuilt gadget covering the whole function
    // instead of the normal gadget stream. Lossless: no registration → normal
    // compile. State stays in guest semantics (spec_fn only touches cpu/tlb).
    if (native_offload_prebuilt_active()) {
        prebuilt_fn spec = native_offload_prebuilt_lookup(ip);
        if (spec != NULL) {
            gen_prebuilt_block(&state, (void *) spec);
            if (state.oom) {  // see the OOM note at the tail of this function
                free(state.block);
                ISH_SIGNPOST_SCOPE_END(jit, "block_compile", _bc_spid);
                return NULL;
            }
            gen_end(&state);
            state.block->used = state.capacity;
            ISH_SIGNPOST_SCOPE_END(jit, "block_compile", _bc_spid);
            return state.block;
        }
    }
#endif
    while (true) {
        if (!gen_step(&state, tlb))
            break;
        if (state.oom)  // buffer is stuck; decoding the rest of the page is wasted work
            break;
        // no block should span more than 2 pages
        // guarantee this by limiting total block size to 1 page
        // guarantee that by stopping as soon as there's less space left than
        // the maximum length of an x86 instruction
        // TODO refuse to decode instructions longer than 15 bytes
        if (state.ip - ip >= PAGE_SIZE - 15) {
            gen_exit(&state);
            break;
        }
    }
    // The gadget buffer could not grow: the emitted stream is truncated and must
    // never run. Discard it and report failure -- the caller raises INT_GPF,
    // which kills this guest process only. [T-ish-jit-oom-abort]
    //
    // Checked BEFORE gen_end(): that function writes through jump_ip[] and
    // block_patch_ip, which are offsets recorded on the assumption the buffer
    // would keep growing. After a failed realloc they can exceed the block's
    // real capacity, so running gen_end() here would overflow the heap.
    if (state.oom) {
        free(state.block);
        ISH_SIGNPOST_SCOPE_END(jit, "block_compile", _bc_spid);
        return NULL;
    }
    gen_end(&state);
    assert(state.ip - ip <= PAGE_SIZE);
    state.block->used = state.capacity;
    ISH_SIGNPOST_SCOPE_END(jit, "block_compile", _bc_spid);
    return state.block;
}

#ifdef GUEST_ARM64
// W^X compile wrapper. Guest JITs (bun/JSC) patch their code pages in place
// (inline caches, repatched branches). Without protection, a block compiled
// while such a patch is mid-flight caches torn bytes and keeps executing them
// (root cause of the claude-cli crashes that ISH_NO_BLOCK_CACHE=2 made
// disappear). Protocol:
//   1. Mark the page P_CODE + bump mmu->changes BEFORE reading guest bytes.
//      Every thread's cached writable TLB entry for the page goes stale, so
//      any store now takes the mem_ptr write-miss path, which invalidates the
//      page's blocks, clears P_CODE, and bumps mmu->changes again.
//   2. Compile.
//   3. Verify (seqlock): if mmu->changes moved or P_CODE was cleared, a store
//      may have landed mid-read — discard the block and retry.
// Called under asbestos->lock (compile+insert are already serialized there).
_Atomic uint64_t st_wx_marks = 0, st_wx_retries = 0, st_wx_giveups = 0;
extern _Atomic uint64_t st_wx_clears;  // kernel/memory.c: stores that unprotected a code page
void dump_wx_stats(void) {
    const char *e = getenv("ISH_WX_STATS");
    if (!e || e[0] != '1') return;
    fprintf(stderr, "[wx] marks=%llu clears=%llu retries=%llu giveups=%llu\n",
            (unsigned long long)st_wx_marks, (unsigned long long)st_wx_clears,
            (unsigned long long)st_wx_retries, (unsigned long long)st_wx_giveups);
}
static struct fiber_block *fiber_block_compile_protected(struct asbestos *asbestos, addr_t ip, struct tlb *tlb) {
    struct mmu *mmu = asbestos->mmu;
    for (int attempt = 0; ; attempt++) {
        int marked = mmu_mark_code_page(mmu, PAGE(ip));
        if (marked < 0)
            return fiber_block_compile(ip, tlb);  // page unmapped: nothing to protect
        if (marked > 0)
            atomic_fetch_add_explicit(&st_wx_marks, 1, memory_order_relaxed);
        uint64_t gen = __atomic_load_n(&mmu->changes, __ATOMIC_ACQUIRE);
        struct fiber_block *block = fiber_block_compile(ip, tlb);
        if (block == NULL)
            return NULL;
        bool crossed = PAGE(ip) != PAGE(block->end_addr);
        if (crossed && mmu_mark_code_page(mmu, PAGE(block->end_addr)) > 0) {
            // Tail page was unprotected while we read it; now that it's
            // marked, recompile so both pages are covered by the seqlock.
            fiber_block_free(NULL, block);
            continue;
        }
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        if (__atomic_load_n(&mmu->changes, __ATOMIC_ACQUIRE) == gen &&
                mmu_code_page_intact(mmu, PAGE(ip)) &&
                (!crossed || mmu_code_page_intact(mmu, PAGE(block->end_addr))))
            return block;
        if (attempt >= 16) {
            atomic_fetch_add_explicit(&st_wx_giveups, 1, memory_order_relaxed);
            return block;  // pathological churn; accept (pre-W^X behavior)
        }
        atomic_fetch_add_explicit(&st_wx_retries, 1, memory_order_relaxed);
        fiber_block_free(NULL, block);
    }
}
#endif

// Remove all pointers to the block. It can't be freed yet because another
// thread may be executing it.
static void fiber_block_disconnect(struct asbestos *asbestos, struct fiber_block *block) {
    if (asbestos != NULL) {
        asbestos->mem_used -= block->used;
        asbestos->num_blocks--;
    }
    list_remove(&block->chain);
    for (int i = 0; i <= 1; i++) {
        list_remove_safe(&block->page[i]);
        list_remove_safe(&block->jumps_from_links[i]);

        struct fiber_block *prev_block, *tmp;
        list_for_each_entry_safe(&block->jumps_from[i], prev_block, tmp, jumps_from_links[i]) {
            if (prev_block->jump_ip[i] != NULL)
                *prev_block->jump_ip[i] = prev_block->old_jump_ip[i];
            list_remove(&prev_block->jumps_from_links[i]);
        }
    }
}

static void fiber_block_free(struct asbestos *asbestos, struct fiber_block *block) {
    fiber_block_disconnect(asbestos, block);
    free(block);
}

static void fiber_free_jetsam(struct asbestos *asbestos) {
    struct fiber_block *block, *tmp;
    list_for_each_entry_safe(&asbestos->jetsam, block, tmp, jetsam) {
        list_remove(&block->jetsam);
        free(block);
    }
}

int fiber_enter(struct fiber_block *block, struct fiber_frame *frame, struct tlb *tlb);
static int cpu_single_step(struct cpu_state *cpu, struct tlb *tlb);

static inline size_t fiber_cache_hash(addr_t ip) {
    return (ip ^ (ip >> 12)) & (FIBER_CACHE_SIZE - 1);
}

static int cpu_step_to_interrupt(struct cpu_state *cpu, struct tlb *tlb) {
    struct asbestos *asbestos = cpu->mmu->asbestos;

    // Hold jetsam_lock read during JIT execution.
    // This prevents jetsam cleanup from freeing blocks while we're executing them.
    read_wrlock(&asbestos->jetsam_lock);

    // Use persistent block cache and frame from TLB; invalidate when blocks are jetsam'd
    bool caches_stale = (tlb->block_cache_gen != asbestos->invalidate_gen);
    struct fiber_block **cache = tlb->block_cache;
    if (caches_stale) {
        memset(cache, 0, sizeof(tlb->block_cache));
        tlb->block_cache_gen = asbestos->invalidate_gen;
    }

    // Use persistent frame from TLB (avoids malloc/free + ret_cache zeroing)
    struct fiber_frame *frame = tlb->frame;
    if (frame == NULL) {
        frame = calloc(1, sizeof(struct fiber_frame));
        if (frame == NULL) {
            // Out of memory. fiber_frame is ~48KB; under heavy Node/npm
            // workloads with many worker threads each needing their own
            // TLB+frame, allocation can fail. Release jetsam_lock and
            // surface this as INT_GPF so the guest sees a crash rather
            // than the host deref'ing a NULL frame pointer below.
            read_wrunlock(&asbestos->jetsam_lock);
            return INT_GPF;
        }
        tlb->frame = frame;
    } else if (caches_stale) {
        // ret_cache holds pointers into block->code; must clear on invalidation
        memset(frame->ret_cache, 0, sizeof(frame->ret_cache));
    }
    frame->last_block = NULL;
    frame->cpu = *cpu;
    assert(asbestos->mmu == cpu->mmu);

    int interrupt = INT_NONE;
    int crash_retry_count = 0;
    while (interrupt == INT_NONE) {
        // Check if blocks were invalidated since last check (e.g. CoW by another thread).
        // This must be inside the loop, not just at function entry, because invalidation
        // can happen while we're in the JIT cycle (between fiber_enter calls).
        if (tlb->block_cache_gen != asbestos->invalidate_gen) {
            memset(cache, 0, sizeof(tlb->block_cache));
            tlb->block_cache_gen = asbestos->invalidate_gen;
            memset(frame->ret_cache, 0, sizeof(frame->ret_cache));
        }

        addr_t ip = CPU_IP(&frame->cpu);
        pc_trace_record(ip);
        // Diagnostic: fake_ip leaked into cpu->pc (bit 63 set). This
        // indicates a gadget wrote a tagged pointer without masking.
        // Trace the first occurrence per task with the frame's LR /
        // previous block so we can locate the culprit gadget.
        // Guest PC with bit 63 set indicates corrupted state — BLR/RET
        // landed on a fake_ip tag or a sentinel pointer leaked through
        // guest memory (e.g. a zero-initialized V8 heap slot combined
        // with pointer tagging). Convert to an INT_GPF at a canonical
        // NULL fault so handle_interrupt's V8 zone recovery can try to
        // unwind instead of looping on fiber_block_compile at the
        // tagged address (which reads unmapped memory forever).
        if (ip & 0xffff000000000000ULL) {
            read_wrunlock(&asbestos->jetsam_lock);
            { bool live_poked = cpu->_poked; *cpu = frame->cpu; cpu->_poked = live_poked; }
            cpu->segfault_addr = ip;
            cpu->segfault_was_write = 0;
            cpu->pc = ip & 0xffffffffffffULL;
            return INT_GPF;
        }
        // Guard: null guest PC means corrupted state (e.g., RET with LR=0
        // after a BL return-address got clobbered, or BR to NULL). Native
        // Linux would deliver SIGSEGV and terminate. In iSH the fault
        // address resolves to the guard-page zeros we map at 0x0-0x1MB,
        // so no SIGSEGV fires from the JIT; instead handle_interrupt
        // re-enters the loop forever. Force-exit with 139 (128+SIGSEGV) so
        // the shell reports "Segmentation fault" and userspace sees a
        // non-zero exit status.
        if (ip == 0) {
            {
                extern char *getenv(const char *);
                static int ndbg = -1;
                if (ndbg == -1) { const char *e = getenv("ISH_NULLPC_DBG"); ndbg = (e && e[0]=='1') ? 1 : 0; }
                if (ndbg) {
                    struct cpu_state *c = &frame->cpu;
                    fprintf(stderr, "[NULLPC] lr(x30)=0x%llx sp=0x%llx x0=0x%llx x1=0x%llx x2=0x%llx x16=0x%llx x17=0x%llx\n",
                            (unsigned long long)c->regs[30], (unsigned long long)c->sp,
                            (unsigned long long)c->regs[0], (unsigned long long)c->regs[1],
                            (unsigned long long)c->regs[2], (unsigned long long)c->regs[16],
                            (unsigned long long)c->regs[17]);
                }
            }
            // Release asbestos jetsam_lock held by cpu_step_to_interrupt
            // before calling do_exit_group (which may synchronously reap).
            read_wrunlock(&asbestos->jetsam_lock);
            { bool live_poked = cpu->_poked; *cpu = frame->cpu; cpu->_poked = live_poked; }
            // Fall through to cpu_run_to_interrupt — return INT_GPF with
            // a canonical write=0 so handle_interrupt delivers SIGSEGV.
            cpu->segfault_addr = 0;
            cpu->segfault_was_write = 0;
            cpu->pc = 0;
            return INT_GPF;
        }
#ifdef GUEST_ARM64
        // --- Mixed execution: return from a nested prebuilt-gadget call ---
        // A prebuilt spec_fn that hit a guest bl/blr set LR to PREBUILT_SENTINEL
        // and re-entered the dispatch loop to run the callee. When the callee
        // returns (guest PC == sentinel), unwind back to prebuilt_call so the
        // spec_fn can continue from where the call was.
        if (ip == PREBUILT_SENTINEL) {
            read_wrunlock(&asbestos->jetsam_lock);
            { bool live_poked = cpu->_poked; *cpu = frame->cpu; cpu->_poked = live_poked; }
            return INT_PREBUILT_RET;
        }
        // --- Pluggable native offload (ARM64 guest only) ---
        // If a native handler is registered for this guest PC, run it instead
        // of compiling/dispatching the guest function. Lossless: when nothing
        // is registered, no target matches, or a handler declines, we fall
        // through to the normal gadget path below. No env gate — the scan is
        // skipped entirely unless a target is registered (native_offload_sym_active).
        if (native_offload_sym_active()) {
            // Refresh TLB before any bypass translation: after fork/mmap the
            // page table may have changed and stale entries point into the
            // parent's CoW pages. Handlers do value-copy that needs fresh TLB.
            if (tlb->mem_changes != __atomic_load_n(&tlb->mmu->changes, __ATOMIC_ACQUIRE)) {
                tlb_flush(tlb);
                memset(cache, 0, sizeof(tlb->block_cache));
                tlb->block_cache_gen = asbestos->invalidate_gen;
                memset(frame->ret_cache, 0, sizeof(frame->ret_cache));
                frame->last_block = NULL;
            }
            if (native_offload_sym_dispatch(ip, &frame->cpu, tlb)) {
                // Handler advanced cpu->pc. Keep dispatch-loop state consistent
                // (poke/timer checks the gadget path would do) and re-enter.
                frame->last_block = NULL;
                interrupt = INT_NONE;
                if (__atomic_exchange_n(frame->cpu.poked_ptr, false, __ATOMIC_ACQUIRE))
                    interrupt = INT_TIMER;
                else if ((++frame->cpu.cycle & ((1 << 10) - 1)) == 0)
                    interrupt = INT_TIMER;
                continue;
            }
        }
#endif
        // Trace-JIT bypass: if a native translation already exists for
        // this PC (in the dispatch table), skip the gadget block compile
        // and run native instead. If no translation exists yet, kick off
        // an async translation attempt — but DON'T call into native this
        // iteration; let the gadget run once, future iterations get the
        // native fast path.
        struct fiber_block *block = NULL;
        {
            // ISH_NO_BLOCK_CACHE=2: never reuse a translated block — compile a
            // fresh one on every dispatch (diagnostic tool for stale-translation
            // hypotheses; the old blocks pile up in the hash until jetsam).
            static int no_block_cache = -1;
            if (no_block_cache == -1) {
                const char *e = getenv("ISH_NO_BLOCK_CACHE");
                no_block_cache = e ? atoi(e) : 0;
            }
            // ISH_NO_JIT_REUSE=1: compile fresh for guest-JIT-pool addresses
            // only (diagnostic: localizes stale-translation suspects).
            static int no_jit_reuse = -1;
            if (no_jit_reuse == -1) {
                const char *e = getenv("ISH_NO_JIT_REUSE");
                no_jit_reuse = (e && e[0] == '1') ? 1 : 0;
            }
            size_t cache_index = fiber_cache_hash(ip);
            block = cache[cache_index];
            if (no_block_cache >= 2 ||
                    (no_jit_reuse && ip >= 0xc0000000ULL && ip < 0xe0000000ULL)) {
                lock(&asbestos->lock);
                block = fiber_block_compile(ip, tlb);
                if (block == NULL) {
                    unlock(&asbestos->lock);
                    interrupt = INT_GPF;
                    frame->cpu.segfault_addr = 0;
                    frame->cpu.segfault_was_write = 0;
                    break;
                }
                fiber_insert(asbestos, block);
                unlock(&asbestos->lock);
            } else
            if (block == NULL || block->addr != ip) {
                extern int stage_trace_on(void);
                extern _Atomic uint64_t st_block_cache_miss, st_block_compile;
                if (stage_trace_on())
                    atomic_fetch_add_explicit(&st_block_cache_miss, 1, memory_order_relaxed);
                lock(&asbestos->lock);
                block = fiber_lookup(asbestos, ip);
                if (block == NULL) {
#ifdef GUEST_ARM64
                    block = fiber_block_compile_protected(asbestos, ip, tlb);
#else
                    block = fiber_block_compile(ip, tlb);
#endif
                    if (block == NULL) {
                        unlock(&asbestos->lock);
                        interrupt = INT_GPF;
                        frame->cpu.segfault_addr = 0;
                        frame->cpu.segfault_was_write = 0;
                        break;
                    }
                    fiber_insert(asbestos, block);
                    if (stage_trace_on())
                        atomic_fetch_add_explicit(&st_block_compile, 1, memory_order_relaxed);
                } else {
                    TRACE("%d %08x --- missed cache\n", current_pid(), ip);
                }
                cache[cache_index] = block;
                unlock(&asbestos->lock);
            }
        }
        // ISH_NO_CHAIN=1: never patch direct-jump chains between blocks, so
        // every block exit returns to this dispatch loop (diagnostic).
        static int no_chain = -1;
        if (no_chain == -1) {
            const char *e = getenv("ISH_NO_CHAIN");
            no_chain = (e && e[0] == '1') ? 1 : 0;
        }
        // ISH_NO_RETCACHE=1: flush the per-frame return-target cache before
        // every dispatch so `ret` gadgets can never jump via a cached entry
        // (diagnostic).
        static int no_retcache = -1;
        if (no_retcache == -1) {
            const char *e = getenv("ISH_NO_RETCACHE");
            no_retcache = (e && e[0] == '1') ? 1 : 0;
        }
        if (no_retcache)
            memset(frame->ret_cache, 0, sizeof(frame->ret_cache));
        struct fiber_block *last_block = no_chain ? NULL : frame->last_block;
        if (block != NULL && last_block != NULL &&
                !last_block->is_jetsam && !block->is_jetsam &&
                (last_block->jump_ip[0] != NULL ||
                 last_block->jump_ip[1] != NULL)) {
            {
                extern int stage_trace_on(void);
                extern _Atomic uint64_t st_chain_attempts;
                if (stage_trace_on())
                    atomic_fetch_add_explicit(&st_chain_attempts, 1, memory_order_relaxed);
            }
            if (trylock(&asbestos->lock) == 0) {
                // can't mint new pointers to a block that has been marked jetsam
                // and is thus assumed to have no pointers left
                if (!last_block->is_jetsam && !block->is_jetsam) {
                    for (int i = 0; i <= 1; i++) {
                        if (last_block->jump_ip[i] != NULL &&
                                (*last_block->jump_ip[i] & 0xffffffff) == block->addr) {
                            *last_block->jump_ip[i] = (unsigned long) block->code;
                            list_add(&block->jumps_from[i], &last_block->jumps_from_links[i]);
                        }
                    }
                }
                unlock(&asbestos->lock);
            }
        }
        if (block != NULL) frame->last_block = block;

        // block may be jetsam, but that's ok, because it can't be freed until
        // every thread on this asbestos is not executing anything

        TRACE("%d %08x --- cycle %ld\n", current_pid(), ip, frame->cpu.cycle);

        // Save block start PC to thread-local for crash recovery.
        // The signal handler reads this to restore cpu->pc on SIGSEGV.
        jit_saved_pc = frame->cpu.pc;

        if (blk_prof_on() && block != NULL)
            blk_prof_record(block->addr, (block->end_addr - block->addr) >> 2);

        // Count dispatch-loop iterations (gated). Cached env check.
        {
            static int g_dispatch_count = -1;
            extern _Atomic uint64_t s_dispatch_iterations;
            if (g_dispatch_count == -1) {
                const char *e = getenv("ISH_DISPATCH_COUNT");
                extern int stage_trace_on(void);
                g_dispatch_count = ((e && e[0] == '1') || stage_trace_on()) ? 1 : 0;
            }
            if (g_dispatch_count)
                atomic_fetch_add_explicit(&s_dispatch_iterations, 1, memory_order_relaxed);
        }

        in_jit = 1;
        interrupt = fiber_enter(block, frame, tlb);
        in_jit = 0;


        // Check if fiber_enter returned due to a JIT crash (signal handler
        // redirected PC to jit_crash_trampoline which returns INT_JIT_CRASH).
        // The signal handler already set cpu->segfault_addr, cpu->pc, etc.
        if (interrupt == INT_JIT_CRASH) {
            // Flush all caches to get fresh host pointers.
            tlb_flush(tlb);
            memset(cache, 0, sizeof(tlb->block_cache));
            tlb->block_cache_gen = asbestos->invalidate_gen;
            memset(frame->ret_cache, 0, sizeof(frame->ret_cache));
            frame->last_block = NULL;

            crash_retry_count++;
            if (crash_retry_count >= 16) {
                // Too many consecutive crashes — escalate to INT_GPF for handle_interrupt
                interrupt = INT_GPF;
                crash_retry_count = 0;
            } else {
                // Retry: convert to INT_NONE so the loop continues
                interrupt = INT_NONE;
            }
        } else {
            crash_retry_count = 0;
        }

        // (debug trace removed)

        // Check if page table changed (mmap/munmap by another thread) EVERY BLOCK.
        if (tlb->mem_changes != __atomic_load_n(&tlb->mmu->changes, __ATOMIC_ACQUIRE)) {
            tlb_flush(tlb);
            memset(cache, 0, sizeof(tlb->block_cache));
            tlb->block_cache_gen = asbestos->invalidate_gen;
            memset(frame->ret_cache, 0, sizeof(frame->ret_cache));
            frame->last_block = NULL;
        }

        if (interrupt == INT_NONE && __atomic_exchange_n(frame->cpu.poked_ptr, false, __ATOMIC_ACQUIRE)) {
            interrupt = INT_TIMER;
            {
                extern int stage_trace_on(void);
                extern _Atomic uint64_t st_int_poke, st_int_poke_foreign;
                if (stage_trace_on()) {
                    atomic_fetch_add_explicit(&st_int_poke, 1, memory_order_relaxed);
                    if (frame->cpu.poked_ptr != &cpu->_poked)
                        atomic_fetch_add_explicit(&st_int_poke_foreign, 1, memory_order_relaxed);
                }
            }
        }
        if (interrupt == INT_NONE && (++frame->cpu.cycle & ((1 << 10) - 1)) == 0) {
            interrupt = INT_TIMER;
            {
                extern int stage_trace_on(void);
                extern _Atomic uint64_t st_int_cycle;
                if (stage_trace_on())
                    atomic_fetch_add_explicit(&st_int_cycle, 1, memory_order_relaxed);
            }
        }
        // PC histogram: sample on every block exit (not just timer ticks).
        // Weight by guest insn count of the block just executed; this gives
        // the per-insn share rather than per-block-dispatch share.
        // Chained blocks skip this loop entirely — but V8 jitless interp
        // dispatch ends in computed-goto (gret, unchainable) so V8 ranges
        // remain fully visible. Other code (loops with direct jumps) gets
        // chained and becomes invisible, biasing the histogram TOWARD V8.
        // Therefore the V8 share measured here is a lower bound on V8's
        // true insn-level share.
        if (pc_hist_on() && frame->last_block != NULL) {
            struct fiber_block *b = frame->last_block;
            uint64_t pc = b->addr;
            uint64_t weight = (b->end_addr - b->addr) >> 2;  // insns
            if (weight == 0) weight = 1;
            if (pc < ((uint64_t)PC_HIST_BUCKETS << PC_HIST_SHIFT))
                atomic_fetch_add_explicit(&pc_hist[pc >> PC_HIST_SHIFT], weight, memory_order_relaxed);
            if (pc_fine_on() && pc >= PC_HIST_FINE_BASE) {
                uint64_t fi = (pc - PC_HIST_FINE_BASE) >> PC_HIST_FINE_SHIFT;
                if (fi < PC_HIST_FINE_N)
                    atomic_fetch_add_explicit(&pc_hist_fine[fi], weight, memory_order_relaxed);
            }
        }
    }
    // _poked is owned by the poked_ptr channel (cpu_poke / the exchange above),
    // never by struct copy. Copying frame->cpu._poked back would resurrect a
    // stale 'true' snapshot taken at loop entry: the exchange cleared the live
    // byte and returned INT_TIMER, then the copy-back re-arms it, making every
    // block dispatch interrupt forever (and fork inherits the storm).
    { bool live_poked = cpu->_poked; *cpu = frame->cpu; cpu->_poked = live_poked; }

    // Release jetsam_lock read. Jetsam cleanup can now proceed.
    read_wrunlock(&asbestos->jetsam_lock);

    return interrupt;
}

static int cpu_single_step(struct cpu_state *cpu, struct tlb *tlb) {
    struct gen_state state;
    if (!gen_start(CPU_IP(cpu), &state))
        return INT_GPF;
    gen_step(&state, tlb);
    gen_exit(&state);
    gen_end(&state);

    struct fiber_block *block = state.block;
    struct fiber_frame frame = {.cpu = *cpu};
    int interrupt = fiber_enter(block, &frame, tlb);
    *cpu = frame.cpu;
    fiber_block_free(NULL, block);
    if (interrupt == INT_NONE)
        interrupt = INT_DEBUG;
    return interrupt;
}

#ifdef GUEST_ARM64
// Mixed execution: run guest function `target_pc` to completion as threaded-
// code and return its result (guest x0). Called from a prebuilt spec_fn at a
// bl/blr site. The callee runs through the normal dispatch loop; we point its
// return address at PREBUILT_SENTINEL so cpu_step_to_interrupt hands control
// back (INT_PREBUILT_RET) exactly when the callee returns. PC/LR are saved and
// restored so the spec_fn resumes cleanly.
uint64_t prebuilt_call(struct cpu_state *cpu, struct tlb *tlb, addr_t target_pc) {
    // The caller (a prebuilt spec_fn) runs on tlb->frame->cpu, driven by the
    // gadget inside an *outer* cpu_step_to_interrupt. We must run the callee
    // WITHOUT disturbing that outer frame: cpu_step_to_interrupt copies the
    // passed cpu into tlb->frame->cpu and mutates frame->last_block / ret_cache
    // / block_cache_gen as it runs. Reusing the same frame corrupts the outer
    // spec_fn's execution context (observed: PyUnicode_New re-enters itself
    // forever after calling PyMem_RawMalloc). So we swap in a private frame for
    // the duration of the nested call and restore the outer one afterwards.
    struct fiber_frame *outer_frame = tlb->frame;
    tlb->frame = NULL;                   // force a fresh frame to be allocated

    struct cpu_state nested = *cpu;      // callee runs on its own cpu copy
    nested.pc = target_pc;
    nested.regs[30] = PREBUILT_SENTINEL; // callee returns here

    for (;;) {
        int interrupt = cpu_step_to_interrupt(&nested, tlb);
        if (interrupt == INT_PREBUILT_RET)
            break;
        // A syscall/timer/etc. fired inside the callee. Service it the same way
        // the kernel's run loop would, then continue the nested call.
        extern void handle_interrupt(int interrupt);
        nested.trapno = interrupt;
        handle_interrupt(interrupt);
    }

    // Copy back the callee's observable effects (result reg + any caller-saved
    // state the ABI lets a callee clobber). The outer cpu keeps its own pc/lr.
    uint64_t ret = nested.regs[0];
    for (int i = 0; i < 18; i++) cpu->regs[i] = nested.regs[i];  // x0..x17 caller-saved
    cpu->sp = nested.sp;

    // Free the nested frame and restore the outer one.
    if (tlb->frame != NULL && tlb->frame != outer_frame)
        free(tlb->frame);
    tlb->frame = outer_frame;
    return ret;
}
#endif

int cpu_run_to_interrupt(struct cpu_state *cpu, struct tlb *tlb) {
    ish_thread_marker = 1;
    if (cpu->poked_ptr == NULL)
        cpu->poked_ptr = &cpu->_poked;
#ifdef GUEST_ARM64
    // NOTE: Do NOT invalidate exclusive monitor here.
    // This function is called once, but the inner loop (cpu_step_to_interrupt)
    // calls fiber_enter repeatedly. The LDXR/STXR pair may span multiple
    // fiber_enter calls (unchained blocks). Invalidating here would break
    // LDXR/STXR atomicity across block boundaries.
    // The exclusive monitor is invalidated by STXR itself (success or fail)
    // and by context switches / signal delivery.
#endif
    struct asbestos *asbestos = cpu->mmu->asbestos;
    __atomic_add_fetch(&asbestos->active_threads, 1, __ATOMIC_RELAXED);
    tlb_refresh(tlb, cpu->mmu);
    int interrupt = (CPU_HAS_SINGLE_STEP ? cpu_single_step : cpu_step_to_interrupt)(cpu, tlb);
    cpu->trapno = interrupt;
    __atomic_sub_fetch(&asbestos->active_threads, 1, __ATOMIC_RELAXED);

    lock(&asbestos->lock);
    if (!list_empty(&asbestos->jetsam)) {
        unlock(&asbestos->lock);

        // Write lock ensures all JIT threads have exited (they hold read lock).
        // Use trylock so only ONE cleaner thread runs at a time; others skip
        // and let the winner handle the jetsam list. This avoids a
        // multi-writer contention pattern that can wedge macOS psynch rwlock
        // when many node/npm worker threads all try to clean jetsam at once.
        // (The jetsam list will still get drained by whichever thread wins.)
        if (write_wrtrylock(&asbestos->jetsam_lock)) {
            lock(&asbestos->lock);
            fiber_free_jetsam(asbestos);
            unlock(&asbestos->lock);
            write_wrunlock(&asbestos->jetsam_lock);
        }
    } else {
        unlock(&asbestos->lock);
    }

    return interrupt;
}

void cpu_poke(struct cpu_state *cpu) {
    {
        extern int stage_trace_on(void);
        extern _Atomic uint64_t st_poke_calls;
        if (stage_trace_on())
            atomic_fetch_add_explicit(&st_poke_calls, 1, memory_order_relaxed);
    }
    __atomic_store_n(cpu->poked_ptr, true, __ATOMIC_SEQ_CST);
}
