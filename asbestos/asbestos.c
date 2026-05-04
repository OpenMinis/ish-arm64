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
__thread volatile addr_t jit_saved_pc;  // block start PC, read by signal handler
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

#ifdef ISH_GADGET_PROFILE
// Gadget call profile: ring buffer of next-gadget pointers, written by `gret`.
// 64K entries; reader (atexit handler) processes after run.
__attribute__((aligned(64))) uint64_t g_profile_buf[65536] = {0};
__attribute__((aligned(64))) uint64_t g_profile_idx = 0;
#endif

void jit_trace_regs(struct cpu_state *cpu) { (void)cpu; }
void c_watch_write_hit(addr_t addr, const char *caller) { (void)addr; (void)caller; }
void jit_watch_write_hit(struct cpu_state *cpu, addr_t store_addr, unsigned long *code_ptr) {
    (void)cpu; (void)store_addr; (void)code_ptr;
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
    struct gen_state state;
    TRACE("%d %08x --- compiling:\n", current_pid(), ip);
    gen_start(ip, &state);
    while (true) {
        if (!gen_step(&state, tlb))
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
    gen_end(&state);
    assert(state.ip - ip <= PAGE_SIZE);
    state.block->used = state.capacity;
    return state.block;
}

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
            *cpu = frame->cpu;
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
            // Release asbestos jetsam_lock held by cpu_step_to_interrupt
            // before calling do_exit_group (which may synchronously reap).
            read_wrunlock(&asbestos->jetsam_lock);
            *cpu = frame->cpu;
            // Fall through to cpu_run_to_interrupt — return INT_GPF with
            // a canonical write=0 so handle_interrupt delivers SIGSEGV.
            cpu->segfault_addr = 0;
            cpu->segfault_was_write = 0;
            cpu->pc = 0;
            return INT_GPF;
        }
        size_t cache_index = fiber_cache_hash(ip);
        struct fiber_block *block = cache[cache_index];
        if (block == NULL || block->addr != ip) {
            lock(&asbestos->lock);
            block = fiber_lookup(asbestos, ip);
            if (block == NULL) {
                block = fiber_block_compile(ip, tlb);
                fiber_insert(asbestos, block);
            } else {
                TRACE("%d %08x --- missed cache\n", current_pid(), ip);
            }
            cache[cache_index] = block;
            unlock(&asbestos->lock);
        }
        struct fiber_block *last_block = frame->last_block;
        if (last_block != NULL &&
                !last_block->is_jetsam && !block->is_jetsam &&
                (last_block->jump_ip[0] != NULL ||
                 last_block->jump_ip[1] != NULL)) {
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
        frame->last_block = block;

        // block may be jetsam, but that's ok, because it can't be freed until
        // every thread on this asbestos is not executing anything

        TRACE("%d %08x --- cycle %ld\n", current_pid(), ip, frame->cpu.cycle);

        // Save block start PC to thread-local for crash recovery.
        // The signal handler reads this to restore cpu->pc on SIGSEGV.
        jit_saved_pc = frame->cpu.pc;

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

        if (interrupt == INT_NONE && __atomic_exchange_n(frame->cpu.poked_ptr, false, __ATOMIC_ACQUIRE))
            interrupt = INT_TIMER;
        if (interrupt == INT_NONE && (++frame->cpu.cycle & ((1 << 10) - 1)) == 0)
            interrupt = INT_TIMER;
    }
    *cpu = frame->cpu;

    // Release jetsam_lock read. Jetsam cleanup can now proceed.
    read_wrunlock(&asbestos->jetsam_lock);

    return interrupt;
}

static int cpu_single_step(struct cpu_state *cpu, struct tlb *tlb) {
    struct gen_state state;
    gen_start(CPU_IP(cpu), &state);
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
    __atomic_store_n(cpu->poked_ptr, true, __ATOMIC_SEQ_CST);
}
