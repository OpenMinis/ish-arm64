// [FIX-261119-11] FUTEX_WAIT_BITSET reads its absolute deadline on the clock
// the caller asked for.
//
// FUTEX_WAIT_BITSET takes an absolute deadline: on CLOCK_MONOTONIC, or on
// CLOCK_REALTIME when FUTEX_CLOCK_REALTIME is set. iSH converted it against
// the wall clock in both cases, so a monotonic deadline (seconds since boot)
// looked decades in the past and the wait returned ETIMEDOUT at once. A
// caller that waits until a deadline (Rust std's park_timeout / recv_timeout,
// musl's pthread_cond_timedwait on CLOCK_MONOTONIC) then saw the deadline had
// not passed and waited again: a busy loop, one core at 100%.
//
// Raw syscalls, no libc wrapper in between, so each case names the exact op.
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#define FUTEX_WAIT_ 0
#define FUTEX_WAKE_ 1
#define FUTEX_WAIT_BITSET_ 9
#define FUTEX_PRIVATE_ 128
#define FUTEX_CLOCK_REALTIME_ 256
#define MATCH_ANY 0xffffffffu

#define WAIT_MS 300
// A wait that should block may come back a little early (the host's timed
// wait is not exact) but never almost at once, and never much later.
#define BLOCKED_MIN_MS 250
#define BLOCKED_MAX_MS 3000
#define AT_ONCE_MAX_MS 100

static int failures;
static void check(int ok, const char *what) {
    printf("  %s  %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok)
        failures++;
}

static uint32_t word;

static struct timespec now(clockid_t clock) {
    struct timespec ts;
    clock_gettime(clock, &ts);
    return ts;
}

static struct timespec plus_ms(struct timespec ts, long ms) {
    ts.tv_sec += ms / 1000;
    ts.tv_nsec += (ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000L;
    } else if (ts.tv_nsec < 0) {
        ts.tv_sec--;
        ts.tv_nsec += 1000000000L;
    }
    return ts;
}

static long elapsed_ms(struct timespec from) {
    struct timespec to = now(CLOCK_MONOTONIC);
    return (to.tv_sec - from.tv_sec) * 1000 + (to.tv_nsec - from.tv_nsec) / 1000000;
}

// One wait on `word` (expected value 0). Returns 0 or the errno, and how long
// the call took.
static int futex_wait(int op, const struct timespec *ts, long *took_ms) {
    struct timespec start = now(CLOCK_MONOTONIC);
    long r = syscall(SYS_futex, &word, op, 0, ts, NULL, MATCH_ANY);
    int err = r == 0 ? 0 : errno;
    *took_ms = elapsed_ms(start);
    return err;
}

static int blocked(long ms) { return ms >= BLOCKED_MIN_MS && ms <= BLOCKED_MAX_MS; }

static void *waker(void *arg) {
    (void) arg;
    usleep(WAIT_MS * 1000);
    __atomic_store_n(&word, 1, __ATOMIC_SEQ_CST);
    syscall(SYS_futex, &word, FUTEX_WAKE_ | FUTEX_PRIVATE_, 1, NULL, NULL, 0);
    return NULL;
}

int main(void) {
    struct timespec ts;
    long ms;
    int err;
    char what[160];

    printf("CLOCK_MONOTONIC=%lld s  CLOCK_REALTIME=%lld s\n",
            (long long) now(CLOCK_MONOTONIC).tv_sec, (long long) now(CLOCK_REALTIME).tv_sec);

    // (a) Absolute monotonic deadline: blocks until it passes.
    ts = plus_ms(now(CLOCK_MONOTONIC), WAIT_MS);
    err = futex_wait(FUTEX_WAIT_BITSET_ | FUTEX_PRIVATE_, &ts, &ms);
    snprintf(what, sizeof what, "WAIT_BITSET, monotonic deadline +%d ms: blocked %ld ms, errno %d (want ETIMEDOUT)", WAIT_MS, ms, err);
    check(err == ETIMEDOUT && blocked(ms), what);

    // The same without FUTEX_PRIVATE_FLAG.
    ts = plus_ms(now(CLOCK_MONOTONIC), WAIT_MS);
    err = futex_wait(FUTEX_WAIT_BITSET_, &ts, &ms);
    snprintf(what, sizeof what, "WAIT_BITSET shared, monotonic deadline +%d ms: blocked %ld ms, errno %d", WAIT_MS, ms, err);
    check(err == ETIMEDOUT && blocked(ms), what);

    // (b) FUTEX_CLOCK_REALTIME: the deadline is wall-clock time.
    ts = plus_ms(now(CLOCK_REALTIME), WAIT_MS);
    err = futex_wait(FUTEX_WAIT_BITSET_ | FUTEX_PRIVATE_ | FUTEX_CLOCK_REALTIME_, &ts, &ms);
    snprintf(what, sizeof what, "WAIT_BITSET|CLOCK_REALTIME, wall deadline +%d ms: blocked %ld ms, errno %d (want ETIMEDOUT)", WAIT_MS, ms, err);
    check(err == ETIMEDOUT && blocked(ms), what);

    // With the flag, a monotonic reading is a wall-clock time long past (the
    // machine has not been up since 1970): the flag is honoured, not ignored.
    ts = plus_ms(now(CLOCK_MONOTONIC), 5000);
    if (ts.tv_sec < now(CLOCK_REALTIME).tv_sec) {
        err = futex_wait(FUTEX_WAIT_BITSET_ | FUTEX_PRIVATE_ | FUTEX_CLOCK_REALTIME_, &ts, &ms);
        snprintf(what, sizeof what, "WAIT_BITSET|CLOCK_REALTIME, deadline read off the monotonic clock: returned in %ld ms, errno %d", ms, err);
        check(err == ETIMEDOUT && ms <= AT_ONCE_MAX_MS, what);
    } else {
        printf("  SKIP  monotonic clock is not behind the wall clock here\n");
    }

    // (c) A deadline already past returns at once, on either clock.
    ts = plus_ms(now(CLOCK_MONOTONIC), -1000);
    err = futex_wait(FUTEX_WAIT_BITSET_ | FUTEX_PRIVATE_, &ts, &ms);
    snprintf(what, sizeof what, "WAIT_BITSET, monotonic deadline 1 s ago: returned in %ld ms, errno %d", ms, err);
    check(err == ETIMEDOUT && ms <= AT_ONCE_MAX_MS, what);

    ts = plus_ms(now(CLOCK_REALTIME), -1000);
    err = futex_wait(FUTEX_WAIT_BITSET_ | FUTEX_PRIVATE_ | FUTEX_CLOCK_REALTIME_, &ts, &ms);
    snprintf(what, sizeof what, "WAIT_BITSET|CLOCK_REALTIME, wall deadline 1 s ago: returned in %ld ms, errno %d", ms, err);
    check(err == ETIMEDOUT && ms <= AT_ONCE_MAX_MS, what);

    ts = (struct timespec) {0, 0};
    err = futex_wait(FUTEX_WAIT_BITSET_ | FUTEX_PRIVATE_, &ts, &ms);
    snprintf(what, sizeof what, "WAIT_BITSET, deadline 0: returned in %ld ms, errno %d", ms, err);
    check(err == ETIMEDOUT && ms <= AT_ONCE_MAX_MS, what);

    // (d) FUTEX_WAIT's timeout is relative, as before.
    ts = (struct timespec) {0, WAIT_MS * 1000000L};
    err = futex_wait(FUTEX_WAIT_ | FUTEX_PRIVATE_, &ts, &ms);
    snprintf(what, sizeof what, "WAIT, relative %d ms: blocked %ld ms, errno %d (want ETIMEDOUT)", WAIT_MS, ms, err);
    check(err == ETIMEDOUT && blocked(ms), what);

    // What a "wait until this instant" caller does: wait, and wait again
    // while the deadline has not passed. One wait, give or take an early
    // return - thousands when each wait returns at once.
    struct timespec start = now(CLOCK_MONOTONIC);
    struct timespec deadline = plus_ms(start, 1000);
    int calls = 0;
    for (;;) {
        struct timespec cur = now(CLOCK_MONOTONIC);
        if (cur.tv_sec > deadline.tv_sec || (cur.tv_sec == deadline.tv_sec && cur.tv_nsec >= deadline.tv_nsec))
            break;
        futex_wait(FUTEX_WAIT_BITSET_ | FUTEX_PRIVATE_, &deadline, &ms);
        if (++calls >= 100000)
            break;
    }
    snprintf(what, sizeof what, "wait-until loop over 1 s: %d futex calls in %ld ms (want a handful)", calls, elapsed_ms(start));
    check(calls <= 10, what);

    // A wake still ends a deadline wait early, with 0.
    pthread_t th;
    pthread_create(&th, NULL, waker, NULL);
    ts = plus_ms(now(CLOCK_MONOTONIC), 5000);
    err = futex_wait(FUTEX_WAIT_BITSET_ | FUTEX_PRIVATE_, &ts, &ms);
    pthread_join(th, NULL);
    snprintf(what, sizeof what, "WAIT_BITSET, monotonic deadline +5 s, woken after %d ms: returned in %ld ms, errno %d (want 0)", WAIT_MS, ms, err);
    check(err == 0 && ms >= BLOCKED_MIN_MS && ms < 4000, what);

    // The word no longer holds the expected value: EAGAIN, no wait.
    ts = plus_ms(now(CLOCK_MONOTONIC), 5000);
    err = futex_wait(FUTEX_WAIT_BITSET_ | FUTEX_PRIVATE_, &ts, &ms);
    snprintf(what, sizeof what, "WAIT_BITSET, value changed: returned in %ld ms, errno %d (want EAGAIN)", ms, err);
    check(err == EAGAIN && ms <= AT_ONCE_MAX_MS, what);

    printf("%d failed\n", failures);
    return failures ? 1 : 0;
}
