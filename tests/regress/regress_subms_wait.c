// [T-ish-futex-subms-timeout] Sub-millisecond waits must actually wait.
//
// Mimics what the Go runtime does when idle: futex-wait for "nanoseconds until
// the next timer" (usually sub-ms), check the clock, poll, sleep again. Before
// the fix futex_wait truncated the remaining time to whole milliseconds and
// returned ETIMEDOUT immediately for anything under 1 ms — so an idle tsproxy
// spun at ~98k syscalls/s ([CPUTop] 2026-09-19). nanosleep had the twin bug:
// every sleep <= 1 ms was turned into a yield.
//
// Build: aarch64-linux-musl-gcc -static -O0 -o subms regress_subms_wait.c
// Run:   ish -f <rootfs> /bin/sh -c 'cat >/tmp/subms; chmod +x /tmp/subms; /tmp/subms' < subms
// Exit 0 when every case passes.
#define _GNU_SOURCE
#include <errno.h>
#include <linux/futex.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

static int pass, fail;
static void check(int ok, const char *what) { if (ok) pass++; else { fail++; printf("  FAIL %s\n", what); } }
static int64_t now_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (int64_t)t.tv_sec * 1000000000LL + t.tv_nsec; }
static int futex_wait_ns(int *addr, int val, int64_t ns) {
    struct timespec ts = { .tv_sec = ns / 1000000000LL, .tv_nsec = ns % 1000000000LL };
    return syscall(SYS_futex, addr, FUTEX_WAIT_PRIVATE, val, &ts, NULL, 0);
}

int main(void) {
    int word = 0;   // never changes: every wait must run to its timeout

    // 1. 500us futex waits for 2 s: count them and measure the shortest one.
    //    Bug: ~millions of iterations, min ~0. Fixed: ~4000, min >= ~450us.
    int64_t start = now_ns(), min_wait = INT64_MAX; long iters = 0;
    while (now_ns() - start < 2000000000LL) {
        int64_t t0 = now_ns();
        int r = futex_wait_ns(&word, 0, 500000);
        int64_t d = now_ns() - t0;
        check(r == -1 && errno == ETIMEDOUT, "futex 500us returns ETIMEDOUT");
        if (d < min_wait) min_wait = d;
        iters++;
        if (iters > 200000) break;   // hopeless spin; do not burn the rest of the budget
    }
    printf("futex 500us: %ld waits in 2s, shortest %lld us\n", iters, (long long)min_wait / 1000);
    check(iters <= 5000, "futex 500us: not spinning (<= 5000 waits in 2 s)");
    check(min_wait >= 400000, "futex 500us: every wait lasted >= 400us");

    // 2. 1.5 ms futex waits: the fractional millisecond must not be dropped.
    //    Bug: returns at ~1.0 ms (polls 1 ms, then truncates the 0.5 ms remainder to 0).
    int64_t mn = INT64_MAX;
    for (int i = 0; i < 100; i++) { int64_t t0 = now_ns(); futex_wait_ns(&word, 0, 1500000); int64_t d = now_ns() - t0; if (d < mn) mn = d; }
    printf("futex 1.5ms: shortest %lld us\n", (long long)mn / 1000);
    check(mn >= 1400000, "futex 1.5ms: every wait lasted >= 1.4ms");

    // 3. nanosleep(200us) must sleep, not yield. Bug: mean ~0.
    struct timespec req = { 0, 200000 }; int64_t t0 = now_ns();
    for (int i = 0; i < 500; i++) nanosleep(&req, NULL);
    int64_t mean = (now_ns() - t0) / 500;
    printf("nanosleep 200us: mean %lld us\n", (long long)mean / 1000);
    check(mean >= 150000, "nanosleep 200us: mean >= 150us");

    // 4. A tiny sleep may still be a yield, but must not take forever (sanity).
    struct timespec tiny = { 0, 10000 }; t0 = now_ns();
    for (int i = 0; i < 1000; i++) nanosleep(&tiny, NULL);
    check((now_ns() - t0) < 2000000000LL, "nanosleep 10us x1000 completes in < 2s");

    printf("subms_wait: %d passed, %d failed\n", pass, fail);
    return fail ? 1 : 0;
}
