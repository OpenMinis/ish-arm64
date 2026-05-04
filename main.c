#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <execinfo.h>
#include <termios.h>
#include <unistd.h>
#include <mach/mach.h>
#include <pthread.h>
#include "kernel/calls.h"
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

// Assembly trampoline: returns INT_JIT_CRASH via fiber_exit (defined in entry.S)
extern void jit_crash_trampoline(void);

// cpu-offsets.h values needed by crash handler
#define CRASH_CPU_pc 272
#define CRASH_CPU_segfault_addr 832
#define CRASH_CPU_segfault_was_write 840
#define CRASH_LOCAL_jit_exit_sp 920

static void crash_handler(int sig, siginfo_t *info, void *ctx) {
#ifdef __aarch64__
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

        // Write crash info directly to cpu_state via _cpu pointer
        *(uint64_t *)(cpu_ptr + CRASH_CPU_segfault_addr) = guest_addr;
        *(int *)(cpu_ptr + CRASH_CPU_segfault_was_write) = was_write;
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

int main(int argc, char *const argv[]) {
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
    int err = xX_main_Xx(argc, argv, envp);
    if (err < 0) {
        fprintf(stderr, "xX_main_Xx: %s\n", strerror(-err));
        return err;
    }
    do_mount(&procfs, "proc", "/proc", "", 0);
    do_mount(&devptsfs, "devpts", "/dev/pts", "", 0);
    task_run_current();
}
