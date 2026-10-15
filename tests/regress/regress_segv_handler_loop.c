// Regression: a fault at the same pc and address, handled by the guest's own
// SIGSEGV handler every time, must be delivered every time.
//
// HotSpot's interpreter and C1 implement null checks as a read of
// [null + offset]; the SIGSEGV becomes a NullPointerException. A hot site that
// throws NPEs therefore faults at the same pc and address over and over. The
// kernel's "stuck at the same fault" guard (kernel/calls.c) counted those and
// killed the process with SIGSEGV on the 257th - `java -Xint` died after 257
// NPEs at one site. It now resets when the fault is delivered to a guest
// handler [T-ish-stuck-pc-guest-handler].
//
// Usage: regress_segv_handler_loop N [onstack] [thread]
// Prints "n=N handled=N OK"; exit status 0 on success.
#define _GNU_SOURCE
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ucontext.h>
static volatile long handled;
static void h(int sig, siginfo_t *si, void *ctx) {
    ucontext_t *uc = ctx;
    uc->uc_mcontext.pc += 4;          // skip the faulting ldr
    handled++;
}
static long run(long n) {
    volatile int *p = (volatile int *) 16;   // null page + offset, like [null + field]
    for (long i = 0; i < n; i++) { (void) *p; }
    return handled;
}
static void *thr(void *a) { return (void *) run((long) a); }
int main(int argc, char **argv) {
    long n = atol(argv[1]);
    int onstack = argc > 2 && !strcmp(argv[2], "onstack");
    int thread = argc > 3 && !strcmp(argv[3], "thread");
    struct sigaction sa = {0};
    sa.sa_sigaction = h; sa.sa_flags = SA_SIGINFO | (onstack ? SA_ONSTACK : 0);
    sigemptyset(&sa.sa_mask);
    if (onstack) {
        stack_t ss = { .ss_sp = malloc(1 << 16), .ss_size = 1 << 16, .ss_flags = 0 };
        sigaltstack(&ss, NULL);
    }
    sigaction(SIGSEGV, &sa, NULL);
    long got;
    if (thread) { pthread_t t; void *r; pthread_create(&t, NULL, thr, (void *) n); pthread_join(t, &r); got = (long) r; }
    else got = run(n);
    printf("n=%ld handled=%ld %s\n", n, got, got == n ? "OK" : "WRONG");
    return got == n ? 0 : 1;
}
