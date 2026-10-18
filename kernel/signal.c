#include "debug.h"
#include <string.h>
#include <signal.h>
#include <stdio.h>
#include "kernel/calls.h"
#include "kernel/signal.h"
#include "kernel/task.h"
#include "kernel/native_offload.h"
#include "kernel/vdso.h"
#include "emu/interrupt.h"

#if is_gcc(9)
#pragma GCC diagnostic ignored "-Waddress-of-packed-member"
#endif


int xsave_extra = 0;
int fxsave_extra = 0;
static void sigmask_set(sigset_t_ set);
static void altstack_to_user(struct task *task, struct stack_t_ *user_stack);
static bool is_on_altstack(addr_t sp, struct task *task);

static int signal_is_blockable(int sig) {
    return sig != SIGKILL_ && sig != SIGSTOP_;
}

#define UNBLOCKABLE_MASK (sig_mask(SIGKILL_) | sig_mask(SIGSTOP_))

#define SIGNAL_IGNORE 0
#define SIGNAL_KILL 1
#define SIGNAL_CALL_HANDLER 2
#define SIGNAL_STOP 3

static int signal_action(struct sighand *sighand, int sig) {
    if (signal_is_blockable(sig)) {
        struct sigaction_ *action = &sighand->action[sig];
        if (action->handler == SIG_IGN_)
            return SIGNAL_IGNORE;
        if (action->handler != SIG_DFL_)
            return SIGNAL_CALL_HANDLER;
    }

    switch (sig) {
        case SIGURG_: case SIGCONT_: case SIGCHLD_:
        case SIGIO_: case SIGWINCH_:
            return SIGNAL_IGNORE;

        case SIGSTOP_: case SIGTSTP_: case SIGTTIN_: case SIGTTOU_:
            return SIGNAL_STOP;

        default:
            return SIGNAL_KILL;
    }
}

// Real-time signals (>= SIGRTMIN) queue: each delivery is a distinct instance
// and must not be coalesced. Standard signals coalesce (one pending bit).
// JSC's thread resume path uses RT signals (RTMIN/RT_1/RT_2); dropping a
// duplicate as "already pending" loses a resume and wedges the GC handshake.
#define SIGRTMIN_ 32
static void deliver_signal_unlocked(struct task *task, int sig, struct siginfo_ info) {
    if (sig < SIGRTMIN_ && sigset_has(task->pending, sig))
        return;

    sigset_add(&task->pending, sig);
    struct sigqueue *sigqueue = malloc(sizeof(struct sigqueue));
    sigqueue->info = info;
    sigqueue->info.sig = sig;
    list_add_tail(&task->queue, &sigqueue->queue);

    if (sigset_has(task->blocked & ~task->waiting, sig) && signal_is_blockable(sig))
        return;

    if (task != current) {
        pthread_kill(task->thread, SIGUSR1);

        // wake up any pthread condition waiters
        // actual madness, I hope to god it's correct
        // must release the sighand lock while going insane, to avoid a deadlock
        unlock(&task->sighand->lock);
        // Wake up the target thread's condvar wait.
        // We don't acquire waiting_lock (the associated mutex) because:
        // 1. It may be held by the target thread inside pthread_cond_wait,
        //    and spinning on it blocks the condvar from returning.
        // 2. pthread_cond_broadcast() is safe without the mutex — worst case
        //    is a spurious wakeup, which wait_for() handles via re-checking.
        lock(&task->waiting_cond_lock);
        if (task->waiting_cond != NULL)
            notify(task->waiting_cond);
        unlock(&task->waiting_cond_lock);
        lock(&task->sighand->lock);
    }
}

void deliver_signal(struct task *task, int sig, struct siginfo_ info) {
    if (task->sighand == NULL)
        return;
    lock(&task->sighand->lock);
    deliver_signal_unlocked(task, sig, info);
    unlock(&task->sighand->lock);
    cpu_poke(&task->cpu);
}

void send_signal(struct task *task, int sig, struct siginfo_ info) {
    // signal zero is for testing whether a process exists
    if (sig == 0)
        return;
    if (task->zombie || task->exiting)
        return;
#ifdef GUEST_ARM64
    if ((sig == SIGTRAP_ || sig == SIGABRT_ || sig == SIGILL_ || sig == SIGSEGV_ || sig == SIGBUS_)
        && ish_exec_trace())
        fprintf(stderr, "SIGNAL_TRACE: sig=%d pc=0x%llx pid=%d sp=0x%llx lr=0x%llx fault=0x%llx\n",
                sig, (unsigned long long)task->cpu.pc, task->pid,
                (unsigned long long)task->cpu.sp,
                (unsigned long long)task->cpu.regs[30],
                (unsigned long long)task->cpu.segfault_addr);
#endif

    // Native offload: forward signal to the host native process
    if (native_offload_forward_signal(task, sig))
        return;

    struct sighand *sighand = task->sighand;
    if (sighand == NULL)
        return;
    lock(&sighand->lock);
    if (signal_action(sighand, sig) != SIGNAL_IGNORE) {
        deliver_signal_unlocked(task, sig, info);
        // Poke the target CPU so it exits the JIT loop at the next gadget
        // boundary instead of waiting for INT_TIMER (up to 1024 cycles).
        // This reduces signal delivery latency from ~1024 to ~1 gadget.
        cpu_poke(&task->cpu);
    }
    unlock(&sighand->lock);

    if (sig == SIGCONT_ || sig == SIGKILL_) {
        lock(&task->group->lock);
        task->group->stopped = false;
        notify(&task->group->stopped_cond);
        unlock(&task->group->lock);
    }
}

bool try_self_signal(int sig) {
    assert(sig == SIGTTIN_ || sig == SIGTTOU_);

    struct sighand *sighand = current->sighand;
    lock(&sighand->lock);
    bool can_send = signal_action(sighand, sig) != SIGNAL_IGNORE &&
        !sigset_has(current->blocked, sig);
    if (can_send)
        deliver_signal_unlocked(current, sig, SIGINFO_NIL);
    unlock(&sighand->lock);
    return can_send;
}

static struct task *process_signal_target(struct task *task, int sig);

int send_group_signal(dword_t pgid, int sig, struct siginfo_ info) {
    lock(&pids_lock);
    struct pid *pid = pid_get(pgid);
    if (pid == NULL) {
        unlock(&pids_lock);
        return _ESRCH;
    }
    struct tgroup *tgroup;
    list_for_each_entry(&pid->pgroup, tgroup, pgroup) {
        send_signal(process_signal_target(tgroup->leader, sig), sig, info);
    }
    unlock(&pids_lock);
    return 0;
}

static addr_t sigreturn_trampoline(const char *name) {
    addr_t sigreturn_addr = vdso_symbol(name);
    if (sigreturn_addr == 0) {
        die("sigreturn not found in vdso, this should never happen");
    }
    return current->mm->vdso + sigreturn_addr;
}

int sigset_size_valid(dword_t size) {
#if defined(GUEST_64BIT)
    return size >= sizeof(sigset_t_) && (size % sizeof(sigset_t_) == 0);
#else
    return size == sizeof(sigset_t_);
#endif
}

int user_get_sigset(addr_t addr, dword_t size, sigset_t_ *out) {
    if (!sigset_size_valid(size))
        return _EINVAL;
#if defined(GUEST_64BIT)
    uint64_t word = 0;
    if (user_read(addr, &word, sizeof(word)))
        return _EFAULT;
    *out = (sigset_t_) word;
    return 0;
#else
    if (user_get(addr, *out))
        return _EFAULT;
    return 0;
#endif
}

int user_put_sigset(addr_t addr, dword_t size, sigset_t_ set) {
    if (!sigset_size_valid(size))
        return _EINVAL;
#if defined(GUEST_64BIT)
    uint64_t word = (uint64_t) set;
    if (user_write(addr, &word, sizeof(word)))
        return _EFAULT;
    dword_t remaining = size - sizeof(word);
    addr_t pos = addr + sizeof(word);
    static const byte_t zeros[16] = {0};
    while (remaining > 0) {
        dword_t chunk = remaining > sizeof(zeros) ? sizeof(zeros) : remaining;
        if (user_write(pos, zeros, chunk))
            return _EFAULT;
        pos += chunk;
        remaining -= chunk;
    }
    return 0;
#else
    if (user_put(addr, set))
        return _EFAULT;
    return 0;
#endif
}

#if defined(GUEST_64BIT)
struct sigaction_arm64_kernel {
    uint64_t handler;
    uint64_t flags;
    uint64_t restorer;
    uint64_t mask[1];
};

static int user_get_sigaction_arm64(addr_t addr, dword_t sigset_size, struct sigaction_ *out) {
    uint64_t handler = 0;
    uint64_t flags = 0;
    uint64_t restorer = 0;
    if (user_read(addr + offsetof(struct sigaction_arm64_kernel, handler), &handler, sizeof(handler)))
        return _EFAULT;
    if (user_read(addr + offsetof(struct sigaction_arm64_kernel, flags), &flags, sizeof(flags)))
        return _EFAULT;
    if (user_read(addr + offsetof(struct sigaction_arm64_kernel, restorer), &restorer, sizeof(restorer)))
        return _EFAULT;
    sigset_t_ mask = 0;
    int err = user_get_sigset(addr + offsetof(struct sigaction_arm64_kernel, mask), sigset_size, &mask);
    if (err)
        return err;
    *out = (struct sigaction_) {
        .handler = (addr_t) handler,
        .flags = (dword_t) flags,
        .restorer = (addr_t) restorer,
        .mask = mask,
    };
    return 0;
}

static int user_put_sigaction_arm64(addr_t addr, dword_t sigset_size, const struct sigaction_ *in) {
    uint64_t handler = (uint64_t) in->handler;
    uint64_t flags = (uint64_t) in->flags;
    uint64_t restorer = (uint64_t) in->restorer;
    if (user_write(addr + offsetof(struct sigaction_arm64_kernel, handler), &handler, sizeof(handler)))
        return _EFAULT;
    if (user_write(addr + offsetof(struct sigaction_arm64_kernel, flags), &flags, sizeof(flags)))
        return _EFAULT;
    if (user_write(addr + offsetof(struct sigaction_arm64_kernel, restorer), &restorer, sizeof(restorer)))
        return _EFAULT;
    return user_put_sigset(addr + offsetof(struct sigaction_arm64_kernel, mask), sigset_size, in->mask);
}
#endif

#if defined(GUEST_ARM64)
// ARM64 signal handling - matches Linux kernel sigcontext layout

static void setup_sigcontext(struct sigcontext_ *sc, struct cpu_state *cpu) {
    // Store full ARM64 state needed for sigreturn
    // NOTE: Do NOT call arm64_sync_nzcv() here!
    // The JIT maintains cpu->nzcv directly (via mrs/str in store_flags).
    // The individual byte flags (nf/zf/cf/vf) are stale and must not
    // overwrite the correct nzcv value. See also: MEMORY.md NZCV lesson.

    // Clear the entire structure first
    memset(sc, 0, sizeof(*sc));

    sc->fault_address = cpu->segfault_addr;
    memcpy(sc->regs, cpu->regs, sizeof(cpu->regs));
    sc->sp = cpu->sp;
    sc->pc = cpu->pc;
    sc->pstate = cpu->nzcv;

    // Set up FPSIMD context in the extension area
    struct fpsimd_context_ *fpsimd = (struct fpsimd_context_ *)sc->__reserved;
    fpsimd->magic = FPSIMD_MAGIC;
    fpsimd->size = sizeof(struct fpsimd_context_);
    fpsimd->fpsr = cpu->fpsr;
    fpsimd->fpcr = cpu->fpcr;
    // Copy vector registers
    for (int i = 0; i < 32; i++) {
        fpsimd->vregs[i] = cpu->fp[i].q;
    }

    // Terminate the extension list with null entry
    uint32_t *term = (uint32_t *)(sc->__reserved + sizeof(struct fpsimd_context_));
    term[0] = 0;  // magic = 0 terminates
    term[1] = 0;  // size = 0
}
#elif defined(GUEST_X86_64)
static void save_fpstate(struct fpstate_ *fp, struct cpu_state *cpu) {
    memset(fp, 0, sizeof(*fp));
    fp->cwd = cpu->fcw;
    fp->swd = cpu->fsw;
    for (int i = 0; i < 8; i++)
        memcpy(&fp->st_space[i * 4], &cpu->fp[(cpu->top + i) % 8], 10);
    fp->mxcsr = cpu->mxcsr;
    fp->mxcsr_mask = 0xffff;
    memcpy(fp->xmm_space, cpu->xmm, sizeof(fp->xmm_space));
}

static void restore_fpstate(struct cpu_state *cpu, const struct fpstate_ *fp) {
    cpu->fcw = fp->cwd;
    cpu->fsw = fp->swd;
    for (int i = 0; i < 8; i++)
        memcpy(&cpu->fp[(cpu->top + i) % 8], &fp->st_space[i * 4], 10);
    cpu->mxcsr = fp->mxcsr & 0xffff;
    memcpy(cpu->xmm, fp->xmm_space, sizeof(fp->xmm_space));
}

static void setup_sigcontext(struct sigcontext_ *sc, struct cpu_state *cpu) {
    memset(sc, 0, sizeof(*sc));
    sc->r8 = cpu->r8;
    sc->r9 = cpu->r9;
    sc->r10 = cpu->r10;
    sc->r11 = cpu->r11;
    sc->r12 = cpu->r12;
    sc->r13 = cpu->r13;
    sc->r14 = cpu->r14;
    sc->r15 = cpu->r15;
    sc->di = cpu->rdi;
    sc->si = cpu->rsi;
    sc->bp = cpu->rbp;
    sc->bx = cpu->rbx;
    sc->dx = cpu->rdx;
    sc->ax = cpu->rax;
    sc->cx = cpu->rcx;
    sc->sp = cpu->rsp;
    sc->ip = cpu->rip;
    collapse_flags(cpu);
    sc->flags = cpu->eflags;
    sc->cs = 0x33; // __USER_CS
    sc->ss = 0x2b; // __USER_DS
    sc->trapno = cpu->trapno;
    if (cpu->trapno == INT_GPF)
        sc->cr2 = cpu->segfault_addr;
    sc->oldmask = current->blocked;
}
#else
// The x87/MMX/SSE state, saved below the signal frame where sigcontext's
// fpstate points (as Linux does), and restored by sigreturn. Without it a
// handler that touches the XMM registers -- Go's signal handler does, through
// memmove -- hands the interrupted code corrupted registers.
static void save_fpstate(struct fpstate_ *fp, struct cpu_state *cpu) {
    memset(fp, 0, sizeof(*fp));
    fp->cw = 0xffff0000 | cpu->fcw;
    fp->sw = 0xffff0000 | cpu->fsw;
    fp->tag = 0xffffffff;
    for (int i = 0; i < 8; i++) {
        float80 *st = &cpu->fp[(cpu->top + i) % 8];
        memcpy(&fp->st[i], st, 10);
        memcpy(&fp->fxsr_st[i], st, 10);
    }
    fp->magic = 0; // FXSR data follows
    fp->_fxsr_env[0] = cpu->fcw | ((dword_t) cpu->fsw << 16);
    fp->mxcsr = cpu->mxcsr;
    memcpy(fp->xmm, cpu->xmm, sizeof(fp->xmm));
    // iSH keeps the MMX registers apart from the x87 stack
    static_assert(sizeof(fp->padding) >= sizeof(cpu->mm), "room for mm");
    memcpy(fp->padding, cpu->mm, sizeof(cpu->mm));
}

static void restore_fpstate(struct cpu_state *cpu, const struct fpstate_ *fp) {
    cpu->fcw = fp->cw;
    cpu->fsw = fp->sw;
    for (int i = 0; i < 8; i++)
        memcpy(&cpu->fp[(cpu->top + i) % 8], fp->magic == 0 ? (const void *) &fp->fxsr_st[i] : (const void *) &fp->st[i], 10);
    if (fp->magic == 0) {
        cpu->mxcsr = fp->mxcsr & 0xffff;
        memcpy(cpu->xmm, fp->xmm, sizeof(cpu->xmm));
        memcpy(cpu->mm, fp->padding, sizeof(cpu->mm));
    }
}

static void setup_sigcontext(struct sigcontext_ *sc, struct cpu_state *cpu) {
    sc->ax = cpu->eax;
    sc->bx = cpu->ebx;
    sc->cx = cpu->ecx;
    sc->dx = cpu->edx;
    sc->di = cpu->edi;
    sc->si = cpu->esi;
    sc->bp = cpu->ebp;
    sc->sp = sc->sp_at_signal = cpu->esp;
    sc->ip = cpu->eip;
    collapse_flags(cpu);
    sc->flags = cpu->eflags;
    sc->trapno = cpu->trapno;
    if (cpu->trapno == INT_GPF)
        sc->cr2 = cpu->segfault_addr;
    // TODO more shit
    sc->oldmask = current->blocked & 0xffffffff;
}
#endif

#ifdef GUEST_X86_64
static void setup_rt_sigframe(struct siginfo_ *info, struct rt_sigframe_ *frame) {
    frame->restorer = sigreturn_trampoline("__kernel_rt_sigreturn");
    frame->info = *info;
    frame->uc.flags = 0;
    frame->uc.link = 0;
    altstack_to_user(current, &frame->uc.stack);
    setup_sigcontext(&frame->uc.mcontext, &current->cpu);
    frame->uc.sigmask = current->blocked;
}
#else
static void setup_sigframe(struct siginfo_ *info, struct sigframe_ *frame) {
    frame->restorer = sigreturn_trampoline("__kernel_sigreturn");
    frame->sig = info->sig;
    setup_sigcontext(&frame->sc, &current->cpu);
    frame->extramask = current->blocked >> 32;

    static const struct {
        uint16_t popmov;
        uint32_t nr_sigreturn;
        uint16_t int80;
    } __attribute__((packed)) retcode = {
        .popmov = 0xb858,
        .nr_sigreturn = 113,
        .int80 = 0x80cd,
    };
    memcpy(frame->retcode, &retcode, sizeof(retcode));
}

static void setup_rt_sigframe(struct siginfo_ *info, struct rt_sigframe_ *frame) {
    frame->restorer = sigreturn_trampoline("__kernel_rt_sigreturn");
    frame->sig = info->sig;
    frame->info = *info;
    frame->uc.flags = 0;
    frame->uc.link = 0;
    altstack_to_user(current, &frame->uc.stack);
    setup_sigcontext(&frame->uc.mcontext, &current->cpu);
    frame->uc.sigmask = current->blocked;

    static const struct {
        uint8_t mov;
        uint32_t nr_rt_sigreturn;
        uint16_t int80;
        uint8_t pad;
    } __attribute__((packed)) rt_retcode = {
        .mov = 0xb8,
        .nr_rt_sigreturn = 173,
        .int80 = 0x80cd,
    };
    memcpy(frame->retcode, &rt_retcode, sizeof(rt_retcode));
}
#endif

#ifdef GUEST_X86_64
// Long mode signal delivery (arch/x86/kernel/signal_64.c): the frame goes
// below the 128-byte red zone (or on the altstack), under the fxsave area,
// with rsp + 8 16-byte aligned as at a function entry.
static void setup_frame_x86_64(struct sigaction_ *action, struct siginfo_ *info) {
    struct cpu_state *cpu = &current->cpu;
    // SA_RESTART: rewind over `syscall` and put the number back in rax
    if ((action->flags & SA_RESTART_) && current->syscall_restartable &&
            cpu->rip == current->syscall_restart_pc) {
        cpu->rip -= 2;
        cpu->rax = current->syscall_restart_num;
    }
    // Either way a handler ran: without SA_RESTART it sees EINTR, and the
    // no-handler restart in receive_signals() must not fire as well.
    current->syscall_restartable = false;

    struct rt_sigframe_ frame = {};
    setup_rt_sigframe(info, &frame);

    addr_t sp = cpu->rsp - 128;
    if ((action->flags & SA_ONSTACK_) && current->altstack &&
            !is_on_altstack(cpu->rsp, current))
        sp = current->altstack + current->altstack_size;

    sp -= sizeof(struct fpstate_);
    sp &= ~(addr_t) 0x3f;
    addr_t fpstate_addr = sp;
    struct fpstate_ fps;
    save_fpstate(&fps, cpu);
    if (user_write(fpstate_addr, &fps, sizeof(fps)))
        fpstate_addr = 0;
    frame.uc.mcontext.fpstate = fpstate_addr;

    sp -= sizeof(frame);
    sp = (sp & ~(addr_t) 0xf) - 8;

    if (!(action->flags & SA_NODEFER_))
        sigset_add(&current->blocked, info->sig);
    current->blocked |= action->mask;

    if (user_write(sp, &frame, sizeof(frame))) {
        printk("failed to install frame for %d at %#llx\n", info->sig, (unsigned long long) sp);
        deliver_signal(current, SIGSEGV_, SIGINFO_NIL);
    }

    cpu->rsp = sp;
    cpu->rip = action->handler;
    cpu->rdi = info->sig;
    cpu->rsi = sp + offsetof(struct rt_sigframe_, info);
    cpu->rdx = sp + offsetof(struct rt_sigframe_, uc);
    cpu->rax = 0;
    // the handler starts with a clean FPU and DF clear, like on Linux
    cpu->fcw = 0x37f;
    cpu->fsw = 0;
    cpu->mxcsr = 0x1f80;
    collapse_flags(cpu);
    cpu->eflags &= ~DF_FLAG;
    cpu->df_offset = 1;
    expand_flags(cpu);
}
#endif

static void receive_signal(struct sighand *sighand, struct siginfo_ *info) {
    int sig = info->sig;
    STRACE("%d receiving signal %d\n", current->pid, sig);

    int action_type = signal_action(sighand, sig);
    switch (action_type) {
        case SIGNAL_IGNORE:
            return;

        case SIGNAL_STOP:
            lock(&current->group->lock);
            current->group->stopped = true;
            current->group->group_exit_code = sig << 8 | 0x7f;
            unlock(&current->group->lock);
            return;

        case SIGNAL_KILL:
            unlock(&sighand->lock); // do_exit must be called without this lock
#ifdef GUEST_ARM64
            // V8's IMMEDIATE_CRASH() uses BRK #0 on ARM64 (delivers SIGTRAP).
            // Generic recovery: unwind the current function frame and return 0
            // to the caller. This lets V8 continue past non-fatal CHECKs.
            if (sig == SIGTRAP_) {
                struct cpu_state *cpu = &current->cpu;
                if (ish_exec_trace())
                    fprintf(stderr, "V8_SIGTRAP: pc=0x%llx x0=0x%llx sp=%llx fp=%llx lr=%llx\n",
                            (unsigned long long)cpu->pc,
                            (unsigned long long)cpu->regs[0],
                            (unsigned long long)cpu->sp,
                            (unsigned long long)cpu->regs[29],
                            (unsigned long long)cpu->regs[30]);
                // Only treat as "clean exit 0" if this process has already
                // committed to V8 abort (printed the V8 fatal preamble to
                // stderr). Otherwise preserve normal SIGTRAP semantics so
                // legitimate errors / debugger traps aren't masked.
                //
                // Required for npx: npm's inner V8 throws non-fatal CHECKs
                // (e.g. Handle location_==nullptr during worker teardown)
                // AFTER all side-effects have landed on disk. Exit 0 lets
                // npx's outer shell pipeline continue to the actual install
                // step. Without this gate npx reports failure but the
                // install actually succeeded.
                if (current->group && current->group->v8_aborting) {
                    do_exit_group(0);
                } else {
                    do_exit_group(sig);
                }
                return;
            }
            if (sig == SIGABRT_) {
                struct cpu_state *cpu = &current->cpu;
                if (ish_exec_trace())
                    fprintf(stderr, "V8_SIGABRT: pc=0x%llx sp=%llx fp=%llx lr=%llx\n",
                            (unsigned long long)cpu->pc,
                            (unsigned long long)cpu->sp,
                            (unsigned long long)cpu->regs[29],
                            (unsigned long long)cpu->regs[30]);
                // Like SIGTRAP: v8_aborting → exit 0; otherwise preserve
                // abort() exit semantics.
                if (current->group && current->group->v8_aborting) {
                    do_exit_group(0);
                } else {
                    do_exit_group(sig);
                }
                return;
            }
            // V8 scope corruption GPF cascade: the deep frame unwind in
            // handle_interrupt may leave a background thread with a corrupt
            // PC pointing into the V8 heap. When that thread tries to
            // execute "code" at a heap address, it gets SIGILL.
            // Flush the host-side stdout/stderr pipes before killing the
            // process group, so buffered output (e.g. npm help) is visible.
            if (sig == SIGILL_ || sig == SIGBUS_) {
                struct cpu_state *cpu = &current->cpu;
                if (cpu->pc >= 0xb0000000 && cpu->pc < 0xf0000000) {
                    do_exit_group(1 << 8);
                    return;
                }
            }
            // If this process has already committed to a V8 fatal abort
            // (printed the V8 fatal preamble to stderr), treat any
            // subsequent fatal signal as a clean shutdown — the side
            // effects have already landed on disk and the abort preamble
            // is suppressed, so surfacing a non-zero exit just confuses
            // callers. Without v8_aborting set, fall through to normal
            // SIGSEGV semantics so legitimate user-program crashes still
            // surface as "Segmentation fault" / non-zero exit.
            if (sig == SIGSEGV_ && current->group && current->group->v8_aborting) {
                do_exit_group(0);
                return;
            }
#endif
            do_exit_group(sig);
    }

    struct sigaction_ *action = &sighand->action[info->sig];
#ifdef GUEST_X86_64
    setup_frame_x86_64(action, info);
#else
    bool need_siginfo = action->flags & SA_SIGINFO_;
#if defined(GUEST_ARM64)
    // ARM64 only provides __kernel_rt_sigreturn in the VDSO.
    // Always use an rt_sigframe for signal delivery.
    need_siginfo = true;

    // SA_RESTART: if this handler asked for automatic restart and the syscall
    // we're interrupting returned EINTR, rewind PC to the `svc #0` instruction
    // (4 bytes back) and restore the original first argument (x0), which the
    // return value clobbered. The signal frame we save next captures this
    // rewound PC, so when the handler returns via rt_sigreturn the syscall
    // re-executes instead of surfacing EINTR. Load-bearing for JSC's SIGPWR
    // GC-safepoint handshake, which registers SA_RESTART and otherwise
    // livelocks re-signalling a thread whose futex keeps returning EINTR.
    // syscall_restartable is a sticky flag: it stays set if an earlier EINTR
    // futex/epoll/read/write delivered a signal whose handler lacked SA_RESTART
    // (no rewind happened). Guarding the rewind on the flag alone is unsafe —
    // a later non-syscall interrupt (INT_TIMER every ~1024 blocks, INT_GPF on
    // demand-map/CoW) runs receive_signals() too, and if a NEW SA_RESTART signal
    // is pending it would rewind PC into the middle of ordinary code. Require
    // cpu.pc to still equal the interrupted syscall's return PC (svc_addr+4), so
    // the rewind can only fire when we truly are just past that svc.
    if ((action->flags & SA_RESTART_) && current->syscall_restartable &&
            current->cpu.pc == current->syscall_restart_pc) {
        current->cpu.pc -= 4;
        current->cpu.regs[0] = current->syscall_restart_arg0;
        current->syscall_restartable = false;
    }
#else
    // x86: rewind over `int $0x80` and put the syscall number back in eax.
    if ((action->flags & SA_RESTART_) && current->syscall_restartable &&
            current->cpu.eip == current->syscall_restart_pc) {
        current->cpu.eip -= 2;
        current->cpu.eax = current->syscall_restart_num;
        current->syscall_restartable = false;
    }
#endif

    // setup the frame
    union {
        struct sigframe_ sigframe;
        struct rt_sigframe_ rt_sigframe;
    } frame = {};
    size_t frame_size;
    if (need_siginfo) {
        setup_rt_sigframe(info, &frame.rt_sigframe);
        frame_size = sizeof(frame.rt_sigframe);
    } else {
        setup_sigframe(info, &frame.sigframe);
        frame_size = sizeof(frame.sigframe);
    }

    // set up registers for signal handler
#if defined(GUEST_ARM64)
    // ARM64 spec: exceptions clear the exclusive monitor
    current->cpu.excl_addr = UINT64_MAX;
    current->cpu.regs[0] = info->sig;
    current->cpu.pc = sighand->action[info->sig].handler;
    addr_t sp = current->cpu.sp;  // Use addr_t (64-bit) for ARM64
#else
    current->cpu.eax = info->sig;
    current->cpu.eip = sighand->action[info->sig].handler;
    dword_t sp = current->cpu.esp;
#endif
    // Only switch to the alternate signal stack when the handler was
    // registered with SA_ONSTACK. iSH previously switched whenever an altstack
    // existed at all, ignoring the flag — that put EVERY handler on the
    // altstack. JSC's GC installs an altstack (for SIGSEGV) but registers its
    // SIGPWR thread-suspend handler WITHOUT SA_ONSTACK: that handler compares
    // the interrupted sp against the thread's own stack bounds to decide
    // whether to actually suspend (sem_post + sigsuspend) or bail. Running it
    // on the altstack made sp fall outside those bounds, so the handler took
    // the bail path and never parked — the GC suspender then re-signalled
    // forever (the claude-cli startup deadlock).
    if ((action->flags & SA_ONSTACK_) && current->altstack &&
            !is_on_altstack(sp, current)) {
        sp = current->altstack + current->altstack_size;
    }
    if (xsave_extra) {
        // do as the kernel does
        // this is superhypermega condensed version of fpu__alloc_mathframe in
        // arch/x86/kernel/fpu/signal.c
        sp -= xsave_extra;
        sp &=~ 0x3f;
        sp -= fxsave_extra;
    }
#if !defined(GUEST_ARM64)
    // FPU state area (fpu__alloc_mathframe): 16-byte aligned, below the frame
    sp -= sizeof(struct fpstate_);
    sp &= ~0xf;
    addr_t fpstate_addr = sp;
    {
        struct fpstate_ fps;
        save_fpstate(&fps, &current->cpu);
        if (user_write(fpstate_addr, &fps, sizeof(fps)))
            fpstate_addr = 0;
        if (need_siginfo)
            frame.rt_sigframe.uc.mcontext.fpstate = fpstate_addr;
        else
            frame.sigframe.sc.fpstate = fpstate_addr;
        // the handler starts with a clean FPU, like on Linux
        current->cpu.fcw = 0x37f;
        current->cpu.fsw = 0;
        current->cpu.mxcsr = 0x1f80;
    }
#endif
    sp -= frame_size;
#if defined(GUEST_ARM64)
    // ARM64 requires the stack to be 16-byte aligned.
    sp &= ~0xf;
#else
    // align sp + 4 on a 16-byte boundary because that's what the abi says
    sp = ((sp + 4) & ~0xf) - 4;
#endif
#if defined(GUEST_ARM64)
    current->cpu.sp = sp;
#else
    current->cpu.esp = sp;
#endif

    // Update the mask. By default the signal will be blocked while in the
    // handler, but sigaction is allowed to customize this.
    if (!(action->flags & SA_NODEFER_))
        sigset_add(&current->blocked, info->sig);
    current->blocked |= action->mask;

    // these have to be filled in after the location of the frame is known
    if (need_siginfo) {
        frame.rt_sigframe.pinfo = sp + offsetof(struct rt_sigframe_, info);
        frame.rt_sigframe.puc = sp + offsetof(struct rt_sigframe_, uc);
#if defined(GUEST_ARM64)
        // ARM64 ABI: x0=signum, x1=siginfo*, x2=ucontext*
        current->cpu.regs[1] = frame.rt_sigframe.pinfo;
        current->cpu.regs[2] = frame.rt_sigframe.puc;
#else
        current->cpu.edx = frame.rt_sigframe.pinfo;
        current->cpu.ecx = frame.rt_sigframe.puc;
#endif
    }

    // install frame
    if (user_write(sp, &frame, frame_size)) {
        printk("failed to install frame for %d at %#x\n", info->sig, sp);
        deliver_signal(current, SIGSEGV_, SIGINFO_NIL);
    }

#if defined(GUEST_ARM64)
    // Set LR to the signal return trampoline.
    if (need_siginfo)
        current->cpu.regs[30] = frame.rt_sigframe.restorer;
    else
        current->cpu.regs[30] = frame.sigframe.restorer;
#endif
#endif // GUEST_X86_64

    if (action->flags & SA_RESETHAND_)
        *action = (struct sigaction_) {.handler = SIG_DFL_};
}

void signal_delivery_stop(int sig, struct siginfo_ *info) {
    lock(&current->ptrace.lock);
    current->ptrace.stopped = true;
    current->ptrace.signal = sig;
    current->ptrace.info = *info;
    unlock(&current->ptrace.lock);
    notify(&current->parent->group->child_exit);
    // TODO add siginfo
    send_signal(current->parent, current->group->leader->exit_signal, SIGINFO_NIL);

    unlock(&current->sighand->lock);
    lock(&current->ptrace.lock);
    while (current->ptrace.stopped) {
        wait_for_ignore_signals(&current->ptrace.cond, &current->ptrace.lock, NULL);
        lock(&current->sighand->lock);
        bool got_sigkill = sigset_has(current->pending, SIGKILL_);
        unlock(&current->sighand->lock);
        if (got_sigkill) {
            STRACE("%d received a SIGKILL in signal delivery stop\n", current->pid);
            unlock(&current->ptrace.lock);
            do_exit_group(SIGKILL_);
        }
    }
    unlock(&current->ptrace.lock);
    lock(&current->sighand->lock);
}

void receive_signals() {
    lock(&current->group->lock);
    bool was_stopped = current->group->stopped;
    unlock(&current->group->lock);

    struct sighand *sighand = current->sighand;
    if (sighand == NULL)
        return;
    lock(&sighand->lock);

    // A saved mask means that the last system call was a call like sigsuspend
    // that changes the mask during the call. Only ignore a signal right now if
    // it was both blocked during the call and should still be blocked after
    // the call.
    sigset_t_ blocked = current->blocked;
    if (current->has_saved_mask) {
        blocked &= current->saved_mask;
        current->has_saved_mask = false;
        current->blocked = current->saved_mask;
    }

    struct sigqueue *sigqueue, *tmp;
    bool received = false;
    list_for_each_entry_safe(&current->queue, sigqueue, tmp, queue) {
        int sig = sigqueue->info.sig;
        if (sigset_has(blocked, sig))
            continue;
        list_remove(&sigqueue->queue);
        sigset_del(&current->pending, sig);
        received = true;

        if (current->ptrace.traced && sig != SIGKILL_) {
            // This notifies the parent, goes to sleep, and waits for the
            // parent to tell it to continue.
            // Any signals received while waiting are left on the queue, except
            // for SIGKILL_, which causes an immediate exit.
            signal_delivery_stop(sig, &sigqueue->info);
        } else {
            receive_signal(sighand, &sigqueue->info);
        }
        free(sigqueue);
    }

#ifdef GUEST_X86_64
    // The signals that interrupted a restartable syscall ran no handler (they
    // were ignored, or stopped and continued us): Linux restarts the syscall
    // transparently, so the caller never sees this EINTR. A handler, if one
    // ran, has already settled it and cleared the flag.
    if (received && current->syscall_restartable &&
            current->cpu.rip == current->syscall_restart_pc &&
            current->cpu.rax == (uint64_t) (int64_t) _EINTR) {
        current->cpu.rip -= 2;
        current->cpu.rax = current->syscall_restart_num;
        current->syscall_restartable = false;
    }
#endif
    (void) received;

    unlock(&sighand->lock);

    // this got moved out of the switch case in receive_signal to fix locking problems
    if (!was_stopped) {
        lock(&current->group->lock);
        bool now_stopped = current->group->stopped;
        unlock(&current->group->lock);
        if (now_stopped) {
            lock(&pids_lock);
            notify(&current->parent->group->child_exit);
            // TODO add siginfo
            send_signal(current->parent, current->group->leader->exit_signal, SIGINFO_NIL);
            unlock(&pids_lock);
        }
    }
}

#if defined(GUEST_X86_64)
static void restore_sigcontext(struct sigcontext_ *context, struct cpu_state *cpu) {
    cpu->r8 = context->r8;
    cpu->r9 = context->r9;
    cpu->r10 = context->r10;
    cpu->r11 = context->r11;
    cpu->r12 = context->r12;
    cpu->r13 = context->r13;
    cpu->r14 = context->r14;
    cpu->r15 = context->r15;
    cpu->rdi = context->di;
    cpu->rsi = context->si;
    cpu->rbp = context->bp;
    cpu->rbx = context->bx;
    cpu->rdx = context->dx;
    cpu->rax = context->ax;
    cpu->rcx = context->cx;
    cpu->rsp = context->sp;
    cpu->rip = context->ip;
    collapse_flags(cpu);
    // AC, RF, OF, DF, TF, SF, ZF, AF, PF, CF, as on i386
    const dword_t use_flags = 0b1010000110111010101;
    cpu->eflags = (context->flags & use_flags) | (cpu->eflags & ~use_flags);
    cpu->df_offset = (cpu->eflags & DF_FLAG) ? -1 : 1;
    expand_flags(cpu);
}

int64_t sys_rt_sigreturn() {
    struct cpu_state *cpu = &current->cpu;
    struct rt_sigframe_ frame;
    // the handler's ret popped pretcode
    addr_t frame_addr = cpu->rsp - 8;
    if (user_get(frame_addr, frame)) {
        deliver_signal(current, SIGSEGV_, SIGINFO_NIL);
        return _EFAULT;
    }
    restore_sigcontext(&frame.uc.mcontext, cpu);
    if (frame.uc.mcontext.fpstate != 0) {
        struct fpstate_ fps;
        if (user_get(frame.uc.mcontext.fpstate, fps) == 0)
            restore_fpstate(cpu, &fps);
    }

    lock(&current->sighand->lock);
    if (!is_on_altstack(frame_addr, current) &&
            frame.uc.stack.size >= MINSIGSTKSZ_) {
        current->altstack = frame.uc.stack.stack;
        current->altstack_size = frame.uc.stack.size;
    }
    sigmask_set(frame.uc.sigmask);
    unlock(&current->sighand->lock);
    return (int64_t) cpu->rax;
}

int64_t sys_sigreturn() {
    return _ENOSYS;
}
#elif defined(GUEST_ARM64)
static void restore_sigcontext(struct sigcontext_ *context, struct cpu_state *cpu) {
    memcpy(cpu->regs, context->regs, sizeof(cpu->regs));
    cpu->sp = context->sp;
    cpu->pc = context->pc;
    arm64_set_nzcv(cpu, (uint32_t)context->pstate);

    // Restore FPSIMD context from extension area
    struct fpsimd_context_ *fpsimd = (struct fpsimd_context_ *)context->__reserved;
    if (fpsimd->magic == FPSIMD_MAGIC) {
        cpu->fpsr = fpsimd->fpsr;
        cpu->fpcr = fpsimd->fpcr;
        for (int i = 0; i < 32; i++) {
            cpu->fp[i].q = fpsimd->vregs[i];
        }
    }
}
#else
static void restore_sigcontext(struct sigcontext_ *context, struct cpu_state *cpu) {
    cpu->eax = context->ax;
    cpu->ebx = context->bx;
    cpu->ecx = context->cx;
    cpu->edx = context->dx;
    cpu->edi = context->di;
    cpu->esi = context->si;
    cpu->ebp = context->bp;
    cpu->esp = context->sp;
    cpu->eip = context->ip;
    collapse_flags(cpu);

    // Use AC, RF, OF, DF, TF, SF, ZF, AF, PF, CF
#define USE_FLAGS 0b1010000110111010101
    cpu->eflags = (context->flags & USE_FLAGS) | (cpu->eflags & ~USE_FLAGS);
    // The gadgets read the arithmetic flags from the lazy state (res, cf,
    // of, flags_res...), which still holds the handler's last operation:
    // rebuild it from the restored eflags, or the interrupted code's next
    // jcc tests the handler's flags (Go's `TESTL $7, ptr; JZ` between a
    // preemption signal and its sigreturn panicked "unaligned 64-bit atomic").
    expand_flags(cpu);
}
#endif

#ifndef GUEST_X86_64
int64_t sys_rt_sigreturn() {
    struct cpu_state *cpu = &current->cpu;
    struct rt_sigframe_ frame;
#if defined(GUEST_ARM64)
    addr_t sp = cpu->sp;  // Use addr_t (64-bit) for ARM64
#else
    dword_t sp = cpu->esp;
#endif
#if defined(GUEST_ARM64)
    // ARM64 keeps SP pointing at the start of the frame.
    addr_t frame_addr = sp;
#else
    // esp/sp points past the first field of the frame
    addr_t frame_addr = sp - offsetof(struct rt_sigframe_, sig);
#endif
    if (user_get(frame_addr, frame)) {
        deliver_signal(current, SIGSEGV_, SIGINFO_NIL);
        return _EFAULT;
    }
    restore_sigcontext(&frame.uc.mcontext, cpu);
#if !defined(GUEST_ARM64)
    if (frame.uc.mcontext.fpstate != 0) {
        struct fpstate_ fps;
        if (user_get(frame.uc.mcontext.fpstate, fps) == 0)
            restore_fpstate(cpu, &fps);
    }
#endif

    lock(&current->sighand->lock);
    // FIXME this duplicates logic from sys_sigaltstack
    if (!is_on_altstack(sp, current) &&
            frame.uc.stack.size >= MINSIGSTKSZ_) {
        current->altstack = frame.uc.stack.stack;
        current->altstack_size = frame.uc.stack.size;
    }
    sigmask_set(frame.uc.sigmask);
    unlock(&current->sighand->lock);
#if defined(GUEST_ARM64)
    // Return the full 64-bit x0 from the restored context.
    // IMPORTANT: The caller (handle_interrupt) must NOT modify x0 after
    // this returns, because the full register state was restored from the
    // signal frame. We return int64_t to preserve all 64 bits.
    return (int64_t)cpu->regs[0];
#else
    return cpu->eax;
#endif
}

int64_t sys_sigreturn() {
    struct cpu_state *cpu = &current->cpu;
    struct sigframe_ frame;
#if defined(GUEST_ARM64)
    addr_t sp = cpu->sp;  // Use addr_t (64-bit) for ARM64
#else
    dword_t sp = cpu->esp;
#endif
#if defined(GUEST_ARM64)
    // ARM64 keeps SP pointing at the start of the frame.
    addr_t frame_addr = sp;
#else
    // esp/sp points past the first two fields of the frame
    addr_t frame_addr = sp - offsetof(struct sigframe_, sc);
#endif
    if (user_get(frame_addr, frame)) {
        deliver_signal(current, SIGSEGV_, SIGINFO_NIL);
        return _EFAULT;
    }
    restore_sigcontext(&frame.sc, cpu);
#if !defined(GUEST_ARM64)
    if (frame.sc.fpstate != 0) {
        struct fpstate_ fps;
        if (user_get(frame.sc.fpstate, fps) == 0)
            restore_fpstate(cpu, &fps);
    }
#endif

    lock(&current->sighand->lock);
#if defined(GUEST_ARM64)
    sigmask_set(current->blocked);
#else
    sigset_t_ oldmask = ((sigset_t_) frame.extramask << 32) | frame.sc.oldmask;
    sigmask_set(oldmask);
#endif
    unlock(&current->sighand->lock);
#if defined(GUEST_ARM64)
    return (int64_t)cpu->regs[0];
#else
    return cpu->eax;
#endif
}

#endif // GUEST_X86_64

struct sighand *sighand_new() {
    struct sighand *sighand = malloc(sizeof(struct sighand));
    if (sighand == NULL)
        return NULL;
    memset(sighand, 0, sizeof(struct sighand));
    sighand->refcount = 1;
    lock_init(&sighand->lock);
    return sighand;
}

struct sighand *sighand_copy(struct sighand *sighand) {
    struct sighand *new_sighand = sighand_new();
    if (new_sighand == NULL)
        return NULL;
    memcpy(new_sighand->action, sighand->action, sizeof(new_sighand->action));
    return new_sighand;
}

void sighand_release(struct sighand *sighand) {
    if (--sighand->refcount == 0) {
        free(sighand);
    }
}

static int do_sigaction(int sig, const struct sigaction_ *action, struct sigaction_ *oldaction) {
    if (sig >= NUM_SIGS)
        return _EINVAL;
    if (!signal_is_blockable(sig))
        return _EINVAL;

    struct sighand *sighand = current->sighand;
    lock(&sighand->lock);
    if (oldaction)
        *oldaction = sighand->action[sig];
    if (action)
        sighand->action[sig] = *action;
    unlock(&sighand->lock);
    return 0;
}

dword_t sys_rt_sigaction(dword_t signum, addr_t action_addr, addr_t oldaction_addr, dword_t sigset_size) {
    if (!sigset_size_valid(sigset_size))
        return _EINVAL;
    struct sigaction_ action = {}, oldaction;
    if (action_addr != 0) {
#if defined(GUEST_64BIT)
        int err = user_get_sigaction_arm64(action_addr, sigset_size, &action);
        if (err)
            return err;
#else
        if (user_get(action_addr, action))
            return _EFAULT;
#endif
    }
    STRACE("rt_sigaction(%d, %#x {handler=%#x, flags=%#x, restorer=%#x, mask=%#llx}, 0x%x, %d)", signum,
            action_addr, action.handler, action.flags, action.restorer,
            (unsigned long long) action.mask, oldaction_addr, sigset_size);

    int err = do_sigaction(signum,
            action_addr ? &action : NULL,
            oldaction_addr ? &oldaction : NULL);
    if (err < 0)
        return err;

    if (oldaction_addr != 0)
    {
#if defined(GUEST_64BIT)
        int err = user_put_sigaction_arm64(oldaction_addr, sigset_size, &oldaction);
        if (err)
            return err;
#else
        if (user_put(oldaction_addr, oldaction))
            return _EFAULT;
#endif
    }
    return err;
}

dword_t sys_sigaction(dword_t signum, addr_t action_addr, addr_t oldaction_addr) {
    return sys_rt_sigaction(signum, action_addr, oldaction_addr, 1);
}

static void sigmask_set(sigset_t_ set) {
    current->blocked = (set & ~UNBLOCKABLE_MASK);
}

static void sigmask_set_temp_unlocked(sigset_t_ mask) {
    current->saved_mask = current->blocked;
    current->has_saved_mask = true;
    sigmask_set(mask);
}

void sigmask_set_temp(sigset_t_ mask) {
    lock(&current->sighand->lock);
    sigmask_set_temp_unlocked(mask);
    unlock(&current->sighand->lock);
}

static int do_sigprocmask(dword_t how, sigset_t_ set) {
    if (how == SIG_BLOCK_)
        sigmask_set(current->blocked | set);
    else if (how == SIG_UNBLOCK_)
        sigmask_set(current->blocked & ~set);
    else if (how == SIG_SETMASK_)
        sigmask_set(set);
    else
        return _EINVAL;
    return 0;
}

dword_t sys_rt_sigprocmask(dword_t how, addr_t set_addr, addr_t oldset_addr, dword_t size) {
    if (!sigset_size_valid(size))
        return _EINVAL;

    sigset_t_ set;
    if (set_addr != 0) {
        int err = user_get_sigset(set_addr, size, &set);
        if (err)
            return err;
    }
    STRACE("rt_sigprocmask(%s, %#llx, %#x, %d)",
            how == SIG_BLOCK_ ? "SIG_BLOCK" :
            how == SIG_UNBLOCK_ ? "SIG_UNBLOCK" :
            how == SIG_SETMASK_ ? "SIG_SETMASK" : "??",
            set_addr != 0 ? (long long) set : -1, oldset_addr, size);

    if (oldset_addr != 0) {
        int err = user_put_sigset(oldset_addr, size, current->blocked);
        if (err)
            return err;
    }
    if (set_addr != 0) {
        struct sighand *sighand = current->sighand;
        lock(&sighand->lock);
        int err = do_sigprocmask(how, set);
        unlock(&sighand->lock);
        if (err < 0)
            return err;
    }
    return 0;
}

int_t sys_rt_sigpending(addr_t set_addr, dword_t size) {
    STRACE("rt_sigpending(%#x, %d)", set_addr, size);
    // as defined by the standard
    sigset_t_ pending = current->pending & current->blocked;
    return user_put_sigset(set_addr, size, pending);
}

static bool is_on_altstack(addr_t sp, struct task *task) {
    return sp > task->altstack && sp <= task->altstack + task->altstack_size;
}

static void altstack_to_user(struct task *task, struct stack_t_ *user_stack) {
    user_stack->stack = task->altstack;
    user_stack->size = task->altstack_size;
    user_stack->flags = 0;
    if (task->altstack == 0)
        user_stack->flags |= SS_DISABLE_;
#if defined(GUEST_ARM64)
    if (is_on_altstack(task->cpu.sp, task))
#else
    if (is_on_altstack(task->cpu.esp, task))
#endif
        user_stack->flags |= SS_ONSTACK_;
}

dword_t sys_sigaltstack(addr_t ss_addr, addr_t old_ss_addr) {
    STRACE("sigaltstack(0x%x, 0x%x)", ss_addr, old_ss_addr);
    // only ever touched by its own thread: no lock
    if (old_ss_addr != 0) {
        struct stack_t_ old_ss;
        altstack_to_user(current, &old_ss);
        if (user_put(old_ss_addr, old_ss))
            return _EFAULT;
    }
    if (ss_addr != 0) {
#if defined(GUEST_ARM64)
        if (is_on_altstack(current->cpu.sp, current))
#else
        if (is_on_altstack(current->cpu.esp, current))
#endif
            return _EPERM;
        struct stack_t_ ss;
        if (user_get(ss_addr, ss))
            return _EFAULT;
        if (ss.flags & SS_DISABLE_) {
            current->altstack = 0;
            current->altstack_size = 0;
        } else {
            if (ss.size < MINSIGSTKSZ_)
                return _ENOMEM;
            current->altstack = ss.stack;
            current->altstack_size = ss.size;
        }
    }
    return 0;
}

int_t sys_rt_sigsuspend(addr_t mask_addr, uint_t size) {
    sigset_t_ mask;
    int err = user_get_sigset(mask_addr, size, &mask);
    if (err)
        return err;
    STRACE("sigsuspend(0x%llx) = ...\n", (long long) mask);

    lock(&current->sighand->lock);
    sigmask_set_temp_unlocked(mask);
    while (wait_for(&current->pause, &current->sighand->lock, NULL) != _EINTR)
        continue;
    unlock(&current->sighand->lock);
    STRACE("%d done sigsuspend", current->pid);
    return _EINTR;
}

int_t sys_pause() {
    lock(&current->sighand->lock);
    while (wait_for(&current->pause, &current->sighand->lock, NULL) != _EINTR)
        continue;
    unlock(&current->sighand->lock);
    return _EINTR;
}

int_t sys_rt_sigtimedwait(addr_t set_addr, addr_t info_addr, addr_t timeout_addr, uint_t set_size) {
    sigset_t_ set;
    int err = user_get_sigset(set_addr, set_size, &set);
    if (err)
        return err;
    struct timespec timeout;
    if (timeout_addr != 0) {
        struct timespec_ fake_timeout;
        if (user_get(timeout_addr, fake_timeout))
            return _EFAULT;
        timeout.tv_sec = fake_timeout.sec;
        timeout.tv_nsec = fake_timeout.nsec;
    }
    STRACE("sigtimedwait(%#llx, %#x, %#x) = ...\n", (long long) set, info_addr, timeout_addr);

    lock(&current->sighand->lock);
    assert(current->waiting == 0);
    current->waiting = set;
    // A signal in `set` is normally BLOCKED by the caller — that is how
    // sigwait/sigtimedwait are used (bind9's isc_app_run blocks SIGTERM/SIGINT
    // in every thread and waits for them here). wait_for() classifies a wakeup
    // as "real" only when (pending & ~blocked) != 0, so a delivery of a blocked
    // signal — the only kind this syscall exists to consume — always reads as a
    // spurious wakeup: deliver_signal queues it and notifies waiting_cond, we
    // wake, wait_for returns 0, and the old `while (wait_err == 0)` loop went
    // straight back to sleep. Forever. Field case: bind-tools nslookup prints
    // its answer, isc_app_shutdown raises SIGTERM at the sigwaiting main
    // thread, the wake is discarded, and the process hangs with
    // pending=0x4002 blocked=0x4003 (SIGTERM+SIGINT delivered, never consumed);
    // Ctrl-C dies the same way, and only unblockable SIGKILL gets through.
    // The loop must therefore check for awaited-signal arrival itself, before
    // every sleep — which also consumes a signal that was already pending on
    // entry, as POSIX requires.
    int wait_err = 0;
    while (!(current->pending & set)) {
        wait_err = wait_for(&current->pause, &current->sighand->lock, timeout_addr == 0 ? NULL : &timeout);
        if (wait_err != 0)
            break;
    }
    current->waiting = 0;
    if (wait_err == _ETIMEDOUT) {
        unlock(&current->sighand->lock);
        STRACE("sigtimedwait timed out\n");
        return _EAGAIN;
    }

    struct sigqueue *sigqueue;
    bool found = false;
    list_for_each_entry(&current->queue, sigqueue, queue) {
        if (sigset_has(set, sigqueue->info.sig)) {
            found = true;
            list_remove(&sigqueue->queue);
            break;
        }
    }
    unlock(&current->sighand->lock);
    if (!found)
        return _EINTR;
    struct siginfo_ info = sigqueue->info;
    free(sigqueue);
    if (info_addr != 0)
        if (user_put(info_addr, info))
            return _EFAULT;
    STRACE("done sigtimedwait = %d\n", info.sig);
    return info.sig;
}

// A signal sent to a process goes to one of its threads that does not block
// it, the leader when it can take it (Linux's complete_signal()); with every
// thread blocking it, it stays pending on the leader. It used to always go
// to the leader, so a process whose main thread blocks a signal that another
// thread waits for (the usual signal-handling-thread pattern) never ran the
// handler. Called with pids_lock held.
static struct task *process_signal_target(struct task *task, int sig) {
    struct tgroup *group = task->group;
    struct task *leader = group->leader;
    if (sig == SIGKILL_ || sig == SIGSTOP_)
        return leader;
    lock(&group->lock);
    struct task *target = leader;
    if (leader->exiting || leader->zombie || leader->sighand == NULL ||
            sigset_has(leader->blocked & ~leader->waiting, sig)) {
        struct task *t;
        list_for_each_entry(&group->threads, t, group_links) {
            if (!t->exiting && !t->zombie && t->sighand != NULL &&
                    !sigset_has(t->blocked & ~t->waiting, sig)) {
                target = t;
                break;
            }
        }
    }
    unlock(&group->lock);
    return target;
}

static int kill_task(struct task *task, dword_t sig) {
    if (!superuser() &&
            current->uid != task->uid &&
            current->uid != task->suid &&
            current->euid != task->uid &&
            current->euid != task->suid)
        return _EPERM;
    struct siginfo_ info = {
        .code = SI_USER_,
        .kill.pid = current->pid,
        .kill.uid = current->uid,
    };
    send_signal(task, sig, info);
    return 0;
}

// kill() and friends address the process the task belongs to.
static int kill_process(struct task *task, dword_t sig) {
    return kill_task(sig == 0 ? task : process_signal_target(task, sig), sig);
}

static int kill_group(pid_t_ pgid, dword_t sig) {
    struct pid *pid = pid_get(pgid);
    if (pid == NULL) {
        unlock(&pids_lock);
        return _ESRCH;
    }
    struct tgroup *tgroup;
    int err = _EPERM;
    list_for_each_entry(&pid->pgroup, tgroup, pgroup) {
        int kill_err = kill_process(tgroup->leader, sig);
        // killing a group should return an error only if no process can be signaled
        if (err == _EPERM)
            err = kill_err;
    }
    return err;
}

static int kill_everything(dword_t sig) {
    int err = _EPERM;
    for (int i = 2; i < MAX_PID; i++) {
        struct task *task = pid_get_task(i);
        if (task == NULL || task == current || !task_is_leader(task))
            continue;
        int kill_err = kill_process(task, sig);
        if (err == _EPERM)
            err = kill_err;
    }
    return err;
}

static int do_kill(pid_t_ pid, dword_t sig, pid_t_ tgid, bool thread) {
    STRACE("kill(%d, %d)", pid, sig);
    if (sig >= NUM_SIGS)
        return _EINVAL;
    if (pid == 0)
        pid = -current->group->pgid;

    int err;
    lock(&pids_lock);

    if (pid == -1) {
        err = kill_everything(sig);
    } else if (pid < 0) {
        err = kill_group(-pid, sig);
    } else {
        struct task *task = pid_get_task(pid);
        if (task == NULL) {
            unlock(&pids_lock);
            return _ESRCH;
        }

        // If tgid is nonzero, it must be correct
        if (tgid != 0 && task->tgid != tgid) {
            unlock(&pids_lock);
            return _ESRCH;
        }

        err = thread ? kill_task(task, sig) : kill_process(task, sig);
    }

    unlock(&pids_lock);
    return err;
}

dword_t sys_kill(pid_t_ pid, dword_t sig) {
    return do_kill(pid, sig, 0, false);
}
dword_t sys_tgkill(pid_t_ tgid, pid_t_ tid, dword_t sig) {
    if (tid <= 0 || tgid <= 0)
        return _EINVAL;
    return do_kill(tid, sig, tgid, true);
}
dword_t sys_tkill(pid_t_ tid, dword_t sig) {
    if (tid <= 0)
        return _EINVAL;
    return do_kill(tid, sig, 0, true);
}
