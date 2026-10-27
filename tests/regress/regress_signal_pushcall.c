// What Go's asynchronous preemption does (runtime.sigctxt.pushCall): a
// signal handler that makes the interrupted code call a function it never
// called - pushes LR below SP, sets LR to the interrupted PC and PC to the
// function, which restores both and returns there. Another thread sends the
// signal as fast as it can while this one runs a loop whose registers and
// flags must survive being interrupted anywhere (Go does it only at safe
// points; here: anywhere in the loop), and makes system calls in between.
//
//   regress_signal_pushcall <seconds>
// prints "ok ..." or what went wrong.
#define _GNU_SOURCE
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>

atomic_long pushed;
static atomic_long sent, landed;
static volatile int stop;
static pid_t victim;

// spin(n, out): x9 = 1.., x10 = their sum, x11 = how often x9 < x10;
// out[0..2] = x9, x10, x11. preempted: called as if from where spin was
// interrupted ([sp]: the LR to put back, LR: where to go back to); uses x17
// as Go's asyncPreempt uses its temporary register, which spin does not.
extern void spin(uint64_t n, uint64_t *out);
extern char spin_lo[], spin_hi[];
extern void preempted(void);
__asm__(
    ".globl spin, spin_lo, spin_hi, preempted\n"
    "spin:\n"
    "    mov x9, #1\n"
    "    mov x10, #0\n"
    "    mov x11, #0\n"
    "spin_lo:\n"
    "1:  add x10, x10, x9\n"
    "    adds x9, x9, #1\n"
    "    cmp x9, x10\n"
    "    cinc x11, x11, lo\n"
    "    subs x0, x0, #1\n"
    "    b.ne 1b\n"
    "spin_hi:\n"
    "    stp x9, x10, [x1]\n"
    "    str x11, [x1, #16]\n"
    "    ret\n"
    "preempted:\n"
    "    stp x0, x1, [sp, #-16]!\n"
    "    mrs x1, nzcv\n"
    "    stp x1, x2, [sp, #-16]!\n"
    "    adrp x0, pushed\n"
    "    add x0, x0, :lo12:pushed\n"
    "1:  ldaxr x1, [x0]\n"
    "    add x1, x1, #1\n"
    "    stlxr w2, x1, [x0]\n"
    "    cbnz w2, 1b\n"
    "    ldp x1, x2, [sp], #16\n"
    "    msr nzcv, x1\n"
    "    ldp x0, x1, [sp], #16\n"
    "    mov x17, x30\n"
    "    ldr x30, [sp], #16\n"
    "    br x17\n");

static void handler(int sig, siginfo_t *info, void *uc_) {
    (void) sig, (void) info;
    mcontext_t *mc = &((ucontext_t *) uc_)->uc_mcontext;
    atomic_fetch_add(&landed, 1);
    if (mc->pc < (uint64_t) spin_lo || mc->pc >= (uint64_t) spin_hi)
        return;
    uint64_t sp = mc->sp - 16;
    *(uint64_t *) sp = mc->regs[30];
    mc->sp = sp;
    mc->regs[30] = mc->pc;
    mc->pc = (uint64_t) preempted;
}

static void *sender(void *arg) {
    (void) arg;
    while (!stop) {
        syscall(SYS_tgkill, getpid(), victim, SIGURG);
        atomic_fetch_add(&sent, 1);
        for (volatile int i = 0; i < 300; i++)
            ;
    }
    return NULL;
}

int main(int argc, char **argv) {
    int secs = argc > 1 ? atoi(argv[1]) : 3;
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = handler;
    sa.sa_flags = SA_SIGINFO | SA_RESTART | SA_ONSTACK;
    sigaction(SIGURG, &sa, NULL);
    stack_t ss = {.ss_sp = malloc(1 << 16), .ss_size = 1 << 16};
    sigaltstack(&ss, NULL);
    victim = (pid_t) syscall(SYS_gettid);
    pthread_t t;
    pthread_create(&t, NULL, sender, NULL);
    time_t end = time(NULL) + secs;
    long bad = 0, rounds = 0;
    while (time(NULL) < end && bad == 0) {
        uint64_t out[3];
        spin(100000, out);
        if (out[0] != 100001 || out[1] != 5000050000ull || out[2] != 99998) {
            printf("bad round %ld: x9=%llu x10=%llu x11=%llu\n", rounds, (unsigned long long) out[0],
                   (unsigned long long) out[1], (unsigned long long) out[2]);
            bad++;
        }
        rounds++;
        getppid();
    }
    stop = 1;
    pthread_join(t, NULL);
    if (bad == 0)
        printf("ok: %ld rounds, %ld signals sent, %ld handled, %ld calls pushed\n", rounds, (long) sent,
               (long) landed, (long) pushed);
    return bad != 0;
}
