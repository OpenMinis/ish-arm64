#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <stdatomic.h>
#include "debug.h"
#include "kernel/task.h"
#include "fs/fd.h"
#include "kernel/calls.h"
#include "fs/tty.h"
#include "kernel/mm.h"
#include "kernel/ptrace.h"

// Architecture-specific register access for fork/clone
#if defined(GUEST_ARM64)
#define CPU_SP(cpu) ((cpu).sp)
#define CPU_RETVAL(cpu) ((cpu).regs[0])
#else
#define CPU_SP(cpu) ((cpu).esp)
#define CPU_RETVAL(cpu) ((cpu).eax)
#endif

static _Atomic(ish_fork_guard_t) g_fork_guard = NULL;

void ish_set_fork_guard(ish_fork_guard_t guard) {
    atomic_store_explicit(&g_fork_guard, guard, memory_order_release);
}

// [T-ish-cpu-top] Successful clone()/fork() count, read by the host [CPUTop] sampler.
_Atomic uint64_t ish_guest_forks;

// [T-ish-fork-rate] Fork-rate governor.
//
// Why: a 40-way `while :; do /bin/true; done` storm forks ~1200 times/s on an
// iPhone at ~4.2 ms CPU each (JIT recompilation dominates), i.e. it eats every
// core, drives the device to thermal "serious", and every other guest process
// (the agent's own shell_execute, tsproxy) crawls behind it. Bounding the
// aggregate fork rate bounds the storm's CPU; the per-task EWMA keeps a fresh
// shell that forks a few times (a tool call) out of the queue entirely.
//
// Token bucket: `tokens` refills at `rate`/s up to `burst`. A heavy forker
// that finds the bucket empty reserves its token in the future (tokens goes
// negative) and sleeps until then, which orders waiters fairly. Sleeps are
// capped so a misconfigured rate can never wedge fork().
#include <pthread.h>
#define FORK_RATE_LIGHT_EWMA 12.0     // decayed recent-fork count below which a task is "light"
#define FORK_RATE_MAX_SLEEP_NS 500000000LL
static pthread_mutex_t fork_rate_lock = PTHREAD_MUTEX_INITIALIZER;
static _Atomic unsigned fork_rate_per_sec;   // 0 = governor off
static _Atomic unsigned fork_rate_burst;
static double fork_rate_tokens;
static uint64_t fork_rate_last_ns;
static _Atomic uint64_t fork_rate_throttled, fork_rate_bypassed, fork_rate_sleep_ns;
static _Atomic int fork_rate_env_checked;

static uint64_t fork_rate_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t) ts.tv_sec * 1000000000ULL + (uint64_t) ts.tv_nsec;
}

void ish_set_fork_rate_limit(unsigned per_sec, unsigned burst) {
    if (burst == 0) burst = per_sec;
    pthread_mutex_lock(&fork_rate_lock);
    atomic_store(&fork_rate_per_sec, per_sec);
    atomic_store(&fork_rate_burst, burst);
    fork_rate_tokens = burst;
    fork_rate_last_ns = fork_rate_now_ns();
    pthread_mutex_unlock(&fork_rate_lock);
    atomic_store(&fork_rate_env_checked, 1);
    printk("[iSH][ForkRate] limit=%u/s burst=%u\n", per_sec, burst);
}

void ish_fork_rate_stats(uint64_t *throttled, uint64_t *bypassed, uint64_t *sleep_ns) {
    if (throttled) *throttled = atomic_load(&fork_rate_throttled);
    if (bypassed) *bypassed = atomic_load(&fork_rate_bypassed);
    if (sleep_ns) *sleep_ns = atomic_load(&fork_rate_sleep_ns);
}

// CLI: ISH_FORK_RATE=per_sec[,burst] in the HOST environment (the app sets
// the rate through ish_set_fork_rate_limit and never reaches this).
static void fork_rate_check_env(void) {
    if (atomic_exchange(&fork_rate_env_checked, 1)) return;
    const char *env = getenv("ISH_FORK_RATE");
    if (env == NULL || *env == '\0') return;
    unsigned per_sec = 0, burst = 0;
    if (sscanf(env, "%u,%u", &per_sec, &burst) >= 1)
        ish_set_fork_rate_limit(per_sec, burst);
}

static void fork_rate_wait(struct task *task) {
    fork_rate_check_env();
    unsigned rate = atomic_load(&fork_rate_per_sec);
    uint64_t now = fork_rate_now_ns();

    // Decayed count of the caller's own recent forks (roughly "forks in the
    // last second"), decayed by the time since its previous fork at the
    // task's OWN pace: the sleep the governor imposes below is excluded by
    // re-stamping fork_rate_last_ns after it, otherwise a throttled storm
    // worker looks light again as soon as it has been slowed down.
    double dt = task->fork_rate_last_ns ? (double) (now - task->fork_rate_last_ns) / 1e9 : 10.0;
    double decay = dt >= 8.0 ? 0.0 : 1.0 / (1.0 + 2.0 * dt);   // cheap stand-in for exp(-dt)
    task->fork_rate_ewma = task->fork_rate_ewma * decay + 1.0;
    task->fork_rate_last_ns = now;
    if (rate == 0)
        return;

    pthread_mutex_lock(&fork_rate_lock);
    double burst = atomic_load(&fork_rate_burst);
    fork_rate_tokens += (double) (now - fork_rate_last_ns) / 1e9 * rate;
    if (fork_rate_tokens > burst) fork_rate_tokens = burst;
    fork_rate_last_ns = now;
    if (fork_rate_tokens >= 1.0) {
        fork_rate_tokens -= 1.0;
        pthread_mutex_unlock(&fork_rate_lock);
        return;
    }
    if (task->fork_rate_ewma < FORK_RATE_LIGHT_EWMA) {
        // Light forker: let it through without charging the bucket, so the
        // storm's own deficit never delays it.
        pthread_mutex_unlock(&fork_rate_lock);
        atomic_fetch_add(&fork_rate_bypassed, 1);
        return;
    }
    fork_rate_tokens -= 1.0;   // reserve; may go negative = queue position
    int64_t wait_ns = (int64_t) (-fork_rate_tokens / rate * 1e9);   // when our token materialises
    pthread_mutex_unlock(&fork_rate_lock);
    if (wait_ns <= 0)
        return;
    if (wait_ns > FORK_RATE_MAX_SLEEP_NS) wait_ns = FORK_RATE_MAX_SLEEP_NS;
    struct timespec ts = { .tv_sec = wait_ns / 1000000000LL, .tv_nsec = wait_ns % 1000000000LL };
    nanosleep(&ts, NULL);
    task->fork_rate_last_ns = fork_rate_now_ns();   // governor delay is not the task's pace
    atomic_fetch_add(&fork_rate_throttled, 1);
    atomic_fetch_add(&fork_rate_sleep_ns, (uint64_t) wait_ns);
}

#define CSIGNAL_ 0x000000ff
#define CLONE_VM_ 0x00000100
#define CLONE_FS_ 0x00000200
#define CLONE_FILES_ 0x00000400
#define CLONE_SIGHAND_ 0x00000800
#define CLONE_PTRACE_ 0x00002000
#define CLONE_VFORK_ 0x00004000
#define CLONE_PARENT_ 0x00008000
#define CLONE_THREAD_ 0x00010000
#define CLONE_NEWNS_ 0x00020000
#define CLONE_SYSVSEM_ 0x00040000
#define CLONE_SETTLS_ 0x00080000
#define CLONE_PARENT_SETTID_ 0x00100000
#define CLONE_CHILD_CLEARTID_ 0x00200000
#define CLONE_DETACHED_ 0x00400000
#define CLONE_UNTRACED_ 0x00800000
#define CLONE_CHILD_SETTID_ 0x01000000
#define CLONE_NEWCGROUP_ 0x02000000
#define CLONE_NEWUTS_ 0x04000000
#define CLONE_NEWIPC_ 0x08000000
#define CLONE_NEWUSER_ 0x10000000
#define CLONE_NEWPID_ 0x20000000
#define CLONE_NEWNET_ 0x40000000
#define CLONE_IO_ 0x80000000
#define IMPLEMENTED_FLAGS (CLONE_VM_|CLONE_FILES_|CLONE_FS_|CLONE_SIGHAND_|CLONE_SYSVSEM_|CLONE_VFORK_|CLONE_THREAD_|\
        CLONE_SETTLS_|CLONE_CHILD_SETTID_|CLONE_PARENT_SETTID_|CLONE_CHILD_CLEARTID_|CLONE_DETACHED_)

static struct tgroup *tgroup_copy(struct tgroup *old_group) {
    struct tgroup *group = malloc(sizeof(struct tgroup));
    *group = *old_group;
    list_init(&group->threads);
    list_add(&old_group->pgroup, &group->pgroup);
    list_add(&old_group->session, &group->session);
    if (group->tty) {
        lock(&group->tty->lock);
        group->tty->refcount++;
        unlock(&group->tty->lock);
    }
    group->itimer = NULL;
    group->doing_group_exit = false;
    group->children_rusage = (struct rusage_) {};
    {
        struct timespec _ts;
        clock_gettime(CLOCK_MONOTONIC, &_ts);
        atomic_store_explicit(&group->last_progress_ns,
            (uint64_t)_ts.tv_sec * 1000000000ULL + _ts.tv_nsec,
            memory_order_relaxed);
    }
    cond_init(&group->child_exit);
    cond_init(&group->stopped_cond);
    lock_init(&group->lock);
    return group;
}

static int copy_task(struct task *task, dword_t flags, addr_t stack, addr_t ptid_addr, addr_t tls_addr, addr_t ctid_addr) {
    task->vfork = NULL;
    if (stack != 0)
        CPU_SP(task->cpu) = stack;

    int err;
    struct mm *mm = task->mm;
    if (flags & CLONE_VM_) {
        mm_retain(mm);
    } else {
        task_set_mm(task, mm_copy(mm));
    }

    if (flags & CLONE_FILES_) {
        task->files->refcount++;
    } else {
        task->files = fdtable_copy(task->files);
        if (IS_ERR(task->files)) {
            err = PTR_ERR(task->files);
            goto fail_free_mem;
        }
    }

    err = _ENOMEM;
    if (flags & CLONE_FS_) {
        task->fs->refcount++;
    } else {
        task->fs = fs_info_copy(task->fs);
        if (task->fs == NULL)
            goto fail_free_files;
    }

    if (flags & CLONE_SIGHAND_) {
        task->sighand->refcount++;
    } else {
        task->sighand = sighand_copy(task->sighand);
        if (task->sighand == NULL)
            goto fail_free_fs;
    }

    struct tgroup *old_group = task->group;
    lock(&pids_lock);
    lock(&old_group->lock);
    if (!(flags & CLONE_THREAD_)) {
        task->group = tgroup_copy(old_group);
        task->group->leader = task;
        task->tgid = task->pid;
    }
    list_add(&task->group->threads, &task->group_links);
    unlock(&old_group->lock);
    unlock(&pids_lock);

    if (flags & CLONE_SETTLS_) {
#if defined(GUEST_ARM64)
        // On ARM64, the TLS argument is the actual TLS pointer value (for TPIDR_EL0),
        // not a pointer to a descriptor structure like x86.
        task->cpu.tls_ptr = tls_addr;
#else
        err = task_set_thread_area(task, tls_addr);
        if (err < 0)
            goto fail_free_sighand;
#endif
    }

    err = _EFAULT;
    if (flags & CLONE_CHILD_SETTID_)
        if (user_put_task(task, ctid_addr, task->pid))
            goto fail_free_sighand;
    if (flags & CLONE_PARENT_SETTID_)
        if (user_put(ptid_addr, task->pid))
            goto fail_free_sighand;
    if (flags & CLONE_CHILD_CLEARTID_)
        task->clear_tid = ctid_addr;
    task->exit_signal = flags & CSIGNAL_;

    // remember to do CLONE_SYSVSEM
    return 0;

fail_free_sighand:
    sighand_release(task->sighand);
fail_free_fs:
    fs_info_release(task->fs);
fail_free_files:
    fdtable_release(task->files);
fail_free_mem:
    mm_release(task->mm);
    return err;
}

dword_t sys_clone(dword_t flags, addr_t stack, addr_t ptid, addr_t tls, addr_t ctid) {
    STRACE("clone(0x%x, 0x%x, 0x%x, 0x%x, 0x%x)", flags, stack, ptid, tls, ctid);
    if (flags & ~CSIGNAL_ & ~IMPLEMENTED_FLAGS) {
        FIXME("unimplemented clone flags 0x%x", flags & ~CSIGNAL_ & ~IMPLEMENTED_FLAGS);
        return _EINVAL;
    }
    if (flags & CLONE_SIGHAND_ && !(flags & CLONE_VM_))
        return _EINVAL;
    if (flags & CLONE_THREAD_ && !(flags & CLONE_SIGHAND_))
        return _EINVAL;

    // [fork-guard] Let the host layer decide whether to allow this fork. A
    // negative errno (e.g. _EAGAIN) is returned to the guest verbatim and no
    // task is created. Threads (CLONE_THREAD_) are exempt: they share the
    // parent's address space, so they don't add the per-process memory cost
    // the guard exists to bound, and denying them would break pthread users.
    if (!(flags & CLONE_THREAD_)) {
        ish_fork_guard_t guard =
            atomic_load_explicit(&g_fork_guard, memory_order_acquire);
        if (guard != NULL) {
            int err = guard();
            if (err < 0)
                return (dword_t) err;
        }
        fork_rate_wait(current);   // [T-ish-fork-rate]
    }

    struct task *task = task_create_(current);
    if (task == NULL)
        return _ENOMEM;
    int err = copy_task(task, flags, stack, ptid, tls, ctid);
    if (err < 0) {
        // FIXME: there is a window between task_create_ and task_destroy where
        // some other thread could get a pointer to the task.
        // FIXME: task_destroy doesn't free all aspects of the task, which
        // could cause leaks
        lock(&pids_lock);
        task_destroy(task);
        unlock(&pids_lock);
        return err;
    }
    CPU_RETVAL(task->cpu) = 0;

    struct vfork_info vfork;
    if (flags & CLONE_VFORK_) {
        lock_init(&vfork.lock);
        cond_init(&vfork.cond);
        vfork.done = false;
        task->vfork = &vfork;
    }

    // task might be destroyed by the time we finish, so save the pid
    pid_t pid = task->pid;

    if (current->ptrace.traced) {
        current->ptrace.trap_event = PTRACE_EVENT_FORK_;
        send_signal(current, SIGTRAP_, SIGINFO_NIL);
    }

    // Starting the thread can fail when the host is out of thread resources
    // (each guest thread costs a host thread plus its stack). Report it to the
    // guest as clone() failing -- the alternative is a task that exists but
    // never runs, and a vfork parent that waits on it forever.
    // [T-ish-jit-oom-abort]
    int start_err = task_start(task);
    if (start_err >= 0)
        atomic_fetch_add(&ish_guest_forks, 1);   // [T-ish-cpu-top]
    if (start_err < 0) {
        if (flags & CLONE_VFORK_) {
            lock(&task->general_lock);
            task->vfork = NULL;
            unlock(&task->general_lock);
            cond_destroy(&vfork.cond);
        }
        lock(&pids_lock);
        task_destroy(task);
        unlock(&pids_lock);
        return start_err;
    }

    if (flags & CLONE_VFORK_) {
        lock(&vfork.lock);
        while (!vfork.done)
            // FIXME this should stop waiting if a fatal signal is received
            wait_for_ignore_signals(&vfork.cond, &vfork.lock, NULL);
        unlock(&vfork.lock);
        lock(&task->general_lock);
        task->vfork = NULL;
        unlock(&task->general_lock);
        cond_destroy(&vfork.cond);
    }

    return pid;
}

// Linux clone3 clone_args structure (uapi/linux/sched.h). All fields u64.
// We read only the subset iSH's sys_clone supports.
struct clone_args_ {
    uint64_t flags;         // CLONE_* flags (NOT combined with exit_signal)
    uint64_t pidfd;         // ptr: where to store pidfd (unsupported → ignored)
    uint64_t child_tid;     // ptr for CLONE_CHILD_SETTID/CLEARTID
    uint64_t parent_tid;    // ptr for CLONE_PARENT_SETTID
    uint64_t exit_signal;   // signal delivered to parent on exit
    uint64_t stack;         // lowest byte of the child stack region
    uint64_t stack_size;    // size of the child stack region
    uint64_t tls;           // new TLS value for CLONE_SETTLS
    uint64_t set_tid;       // ptr to pid array (unsupported)
    uint64_t set_tid_size;
    uint64_t cgroup;
};

dword_t sys_clone3(addr_t args_addr, dword_t size) {
    STRACE("clone3(0x%x, %d)", args_addr, size);
    // The first two u64 fields (flags, pidfd) were present since the initial
    // clone3 ABI; require at least up to `tls` for anything meaningful.
    if (size < offsetof(struct clone_args_, tls) + sizeof(uint64_t))
        return _EINVAL;
    if (size > sizeof(struct clone_args_))
        size = sizeof(struct clone_args_);

    struct clone_args_ args = {};
    if (user_read(args_addr, &args, size))
        return _EFAULT;

    // clone3 keeps exit_signal separate from flags; the legacy sys_clone
    // expects it OR'd into the low byte. Fold it back in.
    dword_t flags = (dword_t)args.flags | ((dword_t)args.exit_signal & CSIGNAL_);

    // clone3 passes the *base* of the stack region; the child SP is the top
    // (stack + stack_size). Legacy clone passes the top directly. A zero
    // stack means "share/copy the parent stack" (fork-like) — pass 0 through.
    addr_t child_sp = 0;
    if (args.stack != 0)
        child_sp = (addr_t)(args.stack + args.stack_size);

    return sys_clone(flags, child_sp, (addr_t)args.parent_tid,
                     (addr_t)args.tls, (addr_t)args.child_tid);
}

dword_t sys_fork() {
    return sys_clone(SIGCHLD_, 0, 0, 0, 0);
}

dword_t sys_vfork() {
    return sys_clone(CLONE_VFORK_ | CLONE_VM_ | SIGCHLD_, 0, 0, 0, 0);
}

void vfork_notify(struct task *task) {
    lock(&task->general_lock);
    if (task->vfork) {
        lock(&task->vfork->lock);
        task->vfork->done = true;
        notify(&task->vfork->cond);
        unlock(&task->vfork->lock);
    }
    unlock(&task->general_lock);
}
