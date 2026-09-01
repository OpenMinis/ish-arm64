#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <execinfo.h>
#include "util/signpost.h"
#include <termios.h>
#include <unistd.h>
#include <mach/mach.h>
#include <pthread.h>
#include <sys/sysctl.h>   // [T-ish-cli-memory-governor] hw.memsize
#include "kernel/calls.h"
#include "kernel/mm.h"    // [T-ish-cli-memory-governor] ish_set_memory_status
#include "kernel/task.h"
#include "emu/cpu.h"
#include "emu/tlb.h"
#include "xX_main_Xx.h"

// Thread-local JIT recovery state (defined in asbestos.c)
extern __thread volatile sig_atomic_t in_jit;
extern __thread volatile uint64_t jit_saved_pc;

// Diagnostic: JIT crash info (defined in calls.c)
extern __thread volatile uint64_t jit_last_host_fault;
extern __thread volatile uint64_t jit_last_x7;
extern __thread volatile uint64_t jit_last_x10;
extern __thread volatile int jit_crash_count;

#ifdef GUEST_ARM64
// Assembly trampoline: returns INT_JIT_CRASH via fiber_exit (defined in entry.S)
extern void jit_crash_trampoline(void);
#endif

// Field offsets needed by the JIT crash handler.
//
// These were previously hand-copied integer literals, and three of the four had
// drifted from the structs they describe (segfault_addr 832 vs 840,
// segfault_was_write 840 vs 848, jit_exit_sp 920 vs 936). The jit_exit_sp error
// is the damaging one: offset 920 lands on `fiber_frame.value[3]`, the crosspage
// scratch buffer, so recovery restored SP from leftover guest data (or 0) and
// `fiber_exit`'s first instruction — `ldr lr, [sp, #0x60]` — faulted. That is
// the `fiber_exit + 0` <- `handle_miss_… + 4` crash signature, including the
// literal `KERN_INVALID_ADDRESS at 0x60` seen when the buffer happened to be 0.
//
// Derive them from the actual types instead. main.c already includes both
// headers, so offsetof cannot drift; a struct-layout change now updates these
// automatically rather than silently corrupting recovery.
#include <stddef.h>
#include "asbestos/frame.h"
#define CRASH_CPU_pc                offsetof(struct cpu_state, pc)
#define CRASH_CPU_segfault_addr     offsetof(struct cpu_state, segfault_addr)
#define CRASH_CPU_segfault_was_write offsetof(struct cpu_state, segfault_was_write)
#define CRASH_LOCAL_jit_exit_sp     offsetof(struct fiber_frame, jit_exit_sp)

// The gadgets reach the same fields through cpu-offsets.h, which meson
// generates from these structs. Tie the two views together so a future layout
// change can never leave the C recovery path and the assembly disagreeing --
// silent disagreement is exactly how the literals above went stale.
#include "cpu-offsets.h"
static_assert(CRASH_CPU_pc                 == CPU_pc,                 "cpu-offsets drift: pc");
static_assert(CRASH_CPU_segfault_addr      == CPU_segfault_addr,      "cpu-offsets drift: segfault_addr");
static_assert(CRASH_CPU_segfault_was_write == CPU_segfault_was_write, "cpu-offsets drift: segfault_was_write");
static_assert(CRASH_LOCAL_jit_exit_sp      == LOCAL_jit_exit_sp,      "cpu-offsets drift: jit_exit_sp");

static void crash_handler(int sig, siginfo_t *info, void *ctx) {
#if defined(__aarch64__) && defined(GUEST_ARM64)
    // If we're inside JIT code and got SIGSEGV/SIGBUS, recover by redirecting
    // execution to jit_crash_trampoline via ucontext PC manipulation.
    // This avoids the overhead of _setjmp on every block entry.
    if ((sig == SIGSEGV || sig == SIGBUS) && in_jit) {
        ucontext_t *uc = (ucontext_t *)ctx;

        // _cpu is in x1 — pointer to cpu_state within fiber_frame
        uint64_t cpu_ptr = uc->uc_mcontext->__ss.__x[1];

        // Reconstruct guest segfault_addr from registers.
        // x7 = _addr (host pointer = data_minus_addr + guest_addr)
        // x10 may hold data_minus_addr from TLB lookup (but only on TLB HIT path)
        uint64_t x7 = uc->uc_mcontext->__ss.__x[7];
        uint64_t x10 = uc->uc_mcontext->__ss.__x[10];
        uint64_t guest_addr = (x7 - x10) & 0xffffffffffffULL;

        // Store diagnostic info for handle_interrupt to read
        jit_last_host_fault = (uint64_t)info->si_addr;
        jit_last_x7 = x7;
        jit_last_x10 = x10;
        jit_crash_count++;

        // Determine read/write from host ESR. Bit 6 (WnR): 0=read, 1=write.
        uint64_t esr = uc->uc_mcontext->__es.__esr;
        int was_write = (esr & 0x40) != 0;

        // Write crash info directly to cpu_state via _cpu pointer.
        // Each store must match the field's real width: segfault_was_write is a
        // `bool`, and the previous `*(int *)` store wrote 4 bytes over it and the
        // padding before `trapno`. The generated gadgets use `strb` for the same
        // field, so 1 byte is the correct width on both sides.
        *(addr_t *)(cpu_ptr + CRASH_CPU_segfault_addr) = guest_addr;
        *(bool *)(cpu_ptr + CRASH_CPU_segfault_was_write) = (bool)was_write;
        // Restore guest PC to block start for re-execution
        *(uint64_t *)(cpu_ptr + CRASH_CPU_pc) = (uint64_t)jit_saved_pc;

        // Restore SP to the value saved by fiber_enter, so fiber_exit
        // can correctly pop the callee-saved register frame.
        uint64_t exit_sp = *(uint64_t *)(cpu_ptr + CRASH_LOCAL_jit_exit_sp);
        uc->uc_mcontext->__ss.__sp = exit_sp;

        // Redirect execution to crash trampoline (returns INT_JIT_CRASH)
        uc->uc_mcontext->__ss.__pc = (uint64_t)jit_crash_trampoline;

        // Unblock signal so it can fire again on next crash
        sigset_t unblock;
        sigemptyset(&unblock);
        sigaddset(&unblock, sig);
        sigprocmask(SIG_UNBLOCK, &unblock, NULL);

        // Signal handler returns; execution resumes at jit_crash_trampoline
        return;
    }
#endif

    // Non-JIT crash: dump state and exit
    char buf[512];
    int len;
    ucontext_t *uc = (ucontext_t *)ctx;
    len = snprintf(buf, sizeof(buf), "\n=== HOST CRASH: signal %d ===\nfault addr: %p\n", sig, info->si_addr);
    write(STDERR_FILENO, buf, len);
#ifdef __aarch64__
    len = snprintf(buf, sizeof(buf),
        "pc:  0x%llx\nlr:  0x%llx\nsp:  0x%llx\n"
        "x0:  0x%llx\nx1:  0x%llx\nx2:  0x%llx\n"
        "x7:  0x%llx\nx28: 0x%llx\n",
        uc->uc_mcontext->__ss.__pc, uc->uc_mcontext->__ss.__lr,
        uc->uc_mcontext->__ss.__sp,
        uc->uc_mcontext->__ss.__x[0], uc->uc_mcontext->__ss.__x[1],
        uc->uc_mcontext->__ss.__x[2],
        uc->uc_mcontext->__ss.__x[7], uc->uc_mcontext->__ss.__x[28]);
    write(STDERR_FILENO, buf, len);
#endif
    void *bt[20];
    int n = backtrace(bt, 20);
    backtrace_symbols_fd(bt, n, STDERR_FILENO);
    _exit(139);
}

static struct termios saved_termios;
static int saved_termios_valid;

void restore_termios(void) {
    if (saved_termios_valid)
        tcsetattr(STDIN_FILENO, TCSANOW, &saved_termios);
}

#ifdef ISH_GADGET_PROFILE
extern uint64_t g_profile_buf[65536];
extern uint64_t g_profile_idx;
#include <dlfcn.h>

// atexit-installed dumper: bucket the ring-buffer entries by symbol address,
// look up each via dladdr, print top-20 with name + count + percentage.
// Also reports the most common consecutive (gadget_a, gadget_b) pairs.
static const char *resolve_sym(uint64_t addr) {
    Dl_info info;
    if (dladdr((void *)addr, &info) && info.dli_sname)
        return info.dli_sname;
    return "?";
}

void dump_gadget_profile(void) {
    uint64_t total_writes = g_profile_idx;
    uint64_t samples = total_writes < 65536 ? total_writes : 65536;
    if (samples == 0) {
        fprintf(stderr, "[gadget-prof] no samples collected\n");
        return;
    }

    typedef struct { uint64_t addr; uint64_t count; } slot_t;
    enum { MAX_DISTINCT = 4096 };
    slot_t *slots = calloc(MAX_DISTINCT, sizeof(slot_t));
    if (!slots) return;
    uint64_t distinct = 0;

    for (uint64_t i = 0; i < samples; i++) {
        uint64_t a = g_profile_buf[i];
        if (a == 0) continue;
        uint64_t j;
        for (j = 0; j < distinct; j++)
            if (slots[j].addr == a) { slots[j].count++; break; }
        if (j == distinct && distinct < MAX_DISTINCT) {
            slots[distinct].addr = a;
            slots[distinct].count = 1;
            distinct++;
        }
    }

    int n_top = distinct < 20 ? (int)distinct : 20;
    fprintf(stderr, "[gadget-prof] %llu total dispatches, %llu samples, %llu distinct gadgets\n",
            (unsigned long long)total_writes,
            (unsigned long long)samples,
            (unsigned long long)distinct);
    fprintf(stderr, "[gadget-prof] top %d singles:\n", n_top);
    for (int rank = 0; rank < n_top; rank++) {
        uint64_t best_count = 0;
        uint64_t best_idx = 0;
        for (uint64_t j = 0; j < distinct; j++) {
            if (slots[j].count > best_count) {
                best_count = slots[j].count;
                best_idx = j;
            }
        }
        if (best_count == 0) break;
        double pct = 100.0 * (double)best_count / (double)samples;
        fprintf(stderr, "[gadget-prof]  %2d. %-40s %8llu  %5.2f%%\n",
                rank + 1, resolve_sym(slots[best_idx].addr),
                (unsigned long long)best_count, pct);
        slots[best_idx].count = 0;
    }

    // Pair statistics: (a,b) consecutive-dispatch counts. Linear-probe hash table.
    typedef struct { uint64_t a, b, count; } pair_t;
    enum { PAIR_TBL = 16384 };
    pair_t *pairs = calloc(PAIR_TBL, sizeof(pair_t));
    if (!pairs) { free(slots); return; }
    uint64_t pair_distinct = 0;
    for (uint64_t i = 0; i + 1 < samples; i++) {
        uint64_t a = g_profile_buf[i], b = g_profile_buf[i + 1];
        if (a == 0 || b == 0) continue;
        uint64_t h = (a * 0x9e3779b97f4a7c15ULL) ^ ((b * 0xbf58476d1ce4e5b9ULL) >> 17);
        h &= (PAIR_TBL - 1);
        for (int probe = 0; probe < PAIR_TBL; probe++) {
            uint64_t k = (h + probe) & (PAIR_TBL - 1);
            if (pairs[k].count == 0) {
                pairs[k].a = a; pairs[k].b = b; pairs[k].count = 1;
                pair_distinct++;
                break;
            }
            if (pairs[k].a == a && pairs[k].b == b) {
                pairs[k].count++;
                break;
            }
        }
    }
    fprintf(stderr, "[gadget-prof] %llu distinct pairs; top 20:\n",
            (unsigned long long)pair_distinct);
    for (int rank = 0; rank < 20; rank++) {
        uint64_t best_count = 0;
        uint64_t best_idx = 0;
        for (uint64_t j = 0; j < PAIR_TBL; j++) {
            if (pairs[j].count > best_count) {
                best_count = pairs[j].count;
                best_idx = j;
            }
        }
        if (best_count == 0) break;
        double pct = 100.0 * (double)best_count / (double)samples;
        char ab[200];
        snprintf(ab, sizeof(ab), "%s + %s",
                 resolve_sym(pairs[best_idx].a),
                 resolve_sym(pairs[best_idx].b));
        fprintf(stderr, "[gadget-prof]  P%2d. %-72s %8llu  %5.2f%%\n",
                rank + 1, ab, (unsigned long long)best_count, pct);
        pairs[best_idx].count = 0;
    }
    free(pairs);
    free(slots);
}
#endif

extern void dump_pc_hist(void);
extern void dump_pc_trace(void);

static void microbench_signal_dump(int sig) {
    (void)sig;
    dump_pc_hist();
    dump_pc_trace();
#ifdef GUEST_ARM64
    // Walk all tasks, dump guest x21 if available (microbench counter reg).
    extern struct pid pids[];
    for (int i = 1; i < 8; i++) {
        struct task *t = pid_get_task(i);
        if (t && t->cpu.x21 != 0) {
            fprintf(stderr, "guest_pid=%d  x21=%llu\n", i, (unsigned long long)t->cpu.x21);
        }
    }
#endif
    _exit(0);
}

// Poller thread for ISH_POLL_ADDR (see main): logs value transitions of a
// guest u32 with wall-clock timestamps, without perturbing guest stores.
void *poll_fn(void *arg) {
    uint64_t addr = *(uint64_t *)arg;
    uint32_t last = 0xdeadbeef;
    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (;;) {
        uint32_t v = 0xdead0003;
        struct task *t = pid_get_task(1);
        if (t && t->mem) {
            extern uint32_t mem_watch_peek32_mem(struct mem *mem, uint64_t addr);
            v = mem_watch_peek32_mem(t->mem, addr);
        }
        if (v != last) {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            double dt = (now.tv_sec - t0.tv_sec) + (now.tv_nsec - t0.tv_nsec) / 1e9;
            fprintf(stderr, "[poll] t=%.4fs [0x%llx] = 0x%x (was 0x%x)\n",
                    dt, (unsigned long long)addr, v, last);
            last = v;
        }
        usleep(1000);
    }
    return NULL;
}

// [T-ish-cli-memory-governor] Host memory feed for the CLI.
//
// Mirrors ISHKernel.m's ish_memory_governor_tick: report the process's physical
// footprint and the remaining allowance, and let the kernel's state machine own
// the BRAKE/OK decision. Two deliberate differences from iOS:
//
//   * there is no jetsam line on macOS, so the "limit" is host physical RAM
//     rather than footprint + os_proc_available_memory();
//   * a 250ms dispatch timer would drag libdispatch into a build that does not
//     otherwise need it, so this is a plain detached pthread on the same
//     cadence. The kernel's >2s staleness rule still fails closed if it dies.
static uint64_t ish_cli_phys_footprint(void) {
    task_vm_info_data_t info;
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&info, &count) != KERN_SUCCESS)
        return 0;
    return (uint64_t) info.phys_footprint;
}

static uint64_t ish_cli_host_ram(void) {
    // hw.memsize is the machine's physical RAM; on a CLI host that is the only
    // honest ceiling available (no per-process allowance exists).
    uint64_t bytes = 0;
    size_t len = sizeof(bytes);
    if (sysctlbyname("hw.memsize", &bytes, &len, NULL, 0) != 0)
        return 0;
    return bytes;
}

static void ish_cli_memory_governor_tick(void) {
    uint64_t ram = ish_cli_host_ram();
    uint64_t footprint = ish_cli_phys_footprint();
    if (ram == 0 || footprint == 0)
        return; // couldn't measure — staleness rule handles a dead feed
    // Leave a slice of RAM to the rest of the system rather than letting the
    // guest believe every byte is its own; the guest share matches the 0.8 the
    // iOS side uses for the same reason.
    uint64_t limit = (uint64_t)((double)ram * 0.8);
    uint64_t avail = limit > footprint ? limit - footprint : 0;
    ish_set_memory_status(limit, avail, false);
}

static void *ish_cli_memory_governor_thread(void *arg) {
    (void) arg;
    for (;;) {
        ish_cli_memory_governor_tick();
        usleep(250 * 1000);
    }
    return NULL;
}

static void ish_cli_memory_governor_start(void) {
    // Prime the feed BEFORE the guest boots. The first allocations happen
    // during exec of init, long before a 250ms tick would land, and until the
    // feed exists the kernel is still on the legacy ledger.
    ish_cli_memory_governor_tick();
    pthread_t t;
    if (pthread_create(&t, NULL, ish_cli_memory_governor_thread, NULL) == 0)
        pthread_detach(t);
}

int main(int argc, char *const argv[]) {
    ish_signpost_init();
    atexit(dump_pc_hist);
    { extern void dump_wx_stats(void); atexit(dump_wx_stats); }
    // Write watchpoint (diagnostic): ISH_WATCH_PAGE=<hex page addr> arms the
    // per-store page check in the write gadgets; ISH_WATCH_LO/HI narrow which
    // hits get recorded.
    {
        extern volatile addr_t g_watch_page_val;
        extern volatile addr_t g_watch_pages[2];
        extern volatile uint64_t g_watch_lo, g_watch_hi;
        const char *w = getenv("ISH_WATCH_PAGE");
        if (w) g_watch_page_val = g_watch_pages[0] = strtoull(w, NULL, 16) & ~0xfffULL;
        const char *w2 = getenv("ISH_WATCH_PAGE2");
        if (w2) g_watch_pages[1] = strtoull(w2, NULL, 16) & ~0xfffULL;
        const char *lo = getenv("ISH_WATCH_LO"), *hi = getenv("ISH_WATCH_HI");
        if (lo) g_watch_lo = strtoull(lo, NULL, 16);
        if (hi) g_watch_hi = strtoull(hi, NULL, 16);
        extern volatile uint64_t g_watch_lo2, g_watch_hi2;
        const char *lo2 = getenv("ISH_WATCH_LO2"), *hi2 = getenv("ISH_WATCH_HI2");
        if (lo2) g_watch_lo2 = strtoull(lo2, NULL, 16);
        if (hi2) g_watch_hi2 = strtoull(hi2, NULL, 16);
        extern volatile uint64_t g_watch_field;
        const char *fv = getenv("ISH_WATCH_FIELD");
        if (fv) g_watch_field = strtoull(fv, NULL, 16);
    }
    // Zero-perturbation host-side poller: ISH_POLL_ADDR=<hex> samples a guest
    // u32 in pid 1's address space every ~1ms and logs transitions. Unlike the
    // store watchpoint this doesn't slow guest stores at all, so it can't
    // scare a timing-sensitive race away.
    {
        const char *pa = getenv("ISH_POLL_ADDR");
        if (pa) {
            static uint64_t poll_addr;
            poll_addr = strtoull(pa, NULL, 16);
            pthread_t pt;
            void *poll_fn(void *arg);
            pthread_create(&pt, NULL, poll_fn, &poll_addr);
        }
    }

    // Microbench helper: on SIGTERM/SIGINT, dump stats then _exit. This
    // bypasses the normal halt_system path which doesn't trigger when
    // ish is killed externally during an infinite-loop guest workload.
    if (getenv("ISH_DUMP_ON_SIGTERM")) {
        struct sigaction sa = { .sa_handler = microbench_signal_dump };
        sigemptyset(&sa.sa_mask);
        sigaction(SIGTERM, &sa, NULL);
        sigaction(SIGINT,  &sa, NULL);
    }
#ifdef ISH_GADGET_PROFILE
    atexit(dump_gadget_profile);
#endif
    // Save host terminal settings so we can restore on exit
    if (isatty(STDIN_FILENO)) {
        if (tcgetattr(STDIN_FILENO, &saved_termios) == 0) {
            saved_termios_valid = 1;
            atexit(restore_termios);
        }
    }

    // Redirect printk output (fd 666) to stderr
    dup2(STDERR_FILENO, 666);

    static char altstack[SIGSTKSZ];
    stack_t ss = {.ss_sp = altstack, .ss_size = SIGSTKSZ};
    sigaltstack(&ss, NULL);
    struct sigaction sa = {0};
    sa.sa_sigaction = crash_handler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGABRT, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
    sigaction(SIGTRAP, &sa, NULL);
    char envp[512] = {0};
    size_t p = 0;
    if (getenv("TERM")) {
        const char *term = getenv("TERM");
        p += snprintf(envp + p, sizeof(envp) - p, "TERM=%s", term) + 1;
    }
    p += snprintf(envp + p, sizeof(envp) - p, "HOME=/root") + 1;
    p += snprintf(envp + p, sizeof(envp) - p, "PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin") + 1;
#ifdef GUEST_ARM64
    p += snprintf(envp + p, sizeof(envp) - p, "PYTHONMALLOC=malloc") + 1;
#endif
    // [T-ish-cli-memory-governor] Put the CLI on the same memory governor the
    // iOS app uses, instead of leaving it on the legacy compile-time ledger.
    //
    // mm.h's contract: "Builds whose host never calls the setter (tests, Linux
    // CLI) never enter footprint mode and keep the legacy ledger cap
    // unchanged." That ceiling is ANON_MMAP_LIMIT_PAGES = 131072 pages, and the
    // ledger charges COMMITMENT — every page an mmap reserves, whether or not
    // it is ever touched. Node commits a multi-hundred-MB V8 cage at startup
    // while dirtying single-digit MB (measured on device: heapUsed 3.4 MB,
    // heapTotal 4.6 MB), so it books ~164k pages against a 131k ceiling and
    // dies with "Fatal process out of memory: NewArray" before running a line
    // of JS. Four node cases in the 266-package suite fail for exactly this
    // reason, and they fail on the CLI only — the app installs a real limit and
    // enters footprint mode, where admission is decided by measured physical
    // footprint rather than by modelled commitment.
    //
    // So feed the same two numbers here. On macOS there is no
    // os_proc_available_memory(); the equivalent honest pair is the host's
    // physical RAM as the ceiling and (ceiling - our own footprint) as the
    // headroom, which is what a CLI process actually has to play with.
    ish_cli_memory_governor_start();

    int err = xX_main_Xx(argc, argv, envp);
    if (err < 0) {
        fprintf(stderr, "xX_main_Xx: %s\n", strerror(-err));
        return err;
    }
    do_mount(&procfs, "proc", "/proc", "", 0);
    do_mount(&devptsfs, "devpts", "/dev/pts", "", 0);
    task_run_current();
}
