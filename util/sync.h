#ifndef UTIL_SYNC_H
#define UTIL_SYNC_H

#include <stdatomic.h>
#include <pthread.h>
#include <stdbool.h>
#include <assert.h>
#include <setjmp.h>
#include <errno.h>
#include <time.h>
#include <stdio.h>
#include "misc.h"
#include "debug.h"

// locks, implemented using pthread

#define LOCK_DEBUG 0

// Slow-lock tracing: when enabled AND the ISH_LOCK_TRACE environment variable
// is set, lock() spins with trylock and reports any wait longer than 3s. Useful
// for diagnosing lock-order inversions and hangs.
//
// Compiled out by default. As a plain runtime `if` it could not be optimised
// away (the flag is a mutable global), so every lock() carried the branch and
// the inlined tracing body — a needless icache cost across the very many lock
// sites in the emulator. It also dragged current_pid() and the trace flag into
// anything that merely takes a lock, which broke linking for standalone tools
// such as tools/fakefsify that have no kernel task state to report.
//
// Enable for a debugging build with -DLOCK_SLOW_TRACE=1.
#ifndef LOCK_SLOW_TRACE
#define LOCK_SLOW_TRACE 0
#endif

typedef struct {
    pthread_mutex_t m;
    pthread_t owner;
#if LOCK_DEBUG
    struct lock_debug {
        const char *file; // doubles as locked
        int line;
        int pid;
        bool initialized;
    } debug;
#endif
} lock_t;

static inline void lock_init(lock_t *lock) {
    pthread_mutex_init(&lock->m, NULL);
#if LOCK_DEBUG
    lock->debug = (struct lock_debug) {
        .initialized = true,
    };
#endif
}

#if LOCK_DEBUG
#define LOCK_INITIALIZER {PTHREAD_MUTEX_INITIALIZER, 0, { .initialized = true }}
#else
#define LOCK_INITIALIZER {PTHREAD_MUTEX_INITIALIZER, 0}
#endif
#if LOCK_SLOW_TRACE
extern int g_lock_slow_trace; // set from ISH_LOCK_TRACE at startup
#endif
static inline void __lock(lock_t *lock, __attribute__((unused)) const char *file, __attribute__((unused)) int line) {
#if LOCK_SLOW_TRACE
    if (g_lock_slow_trace) {
        struct timespec _s; clock_gettime(CLOCK_MONOTONIC, &_s);
        int spun = 0;
        while (pthread_mutex_trylock(&lock->m) != 0) {
            struct timespec _b = {0, 200000}; nanosleep(&_b, NULL);
            struct timespec _n; clock_gettime(CLOCK_MONOTONIC, &_n);
            long ms = (_n.tv_sec-_s.tv_sec)*1000 + (_n.tv_nsec-_s.tv_nsec)/1000000;
            if (ms > 3000 && !spun) {
                spun = 1;
                extern int current_pid(void);
                fprintf(stderr, "[lockslow] pid=%d waiting >3s at %s:%d for lock=%p\n",
                        current_pid(), file, line, (void*)lock);
            }
        }
        lock->owner = pthread_self();
        goto done;
    }
#endif
    pthread_mutex_lock(&lock->m);
    lock->owner = pthread_self();
#if LOCK_SLOW_TRACE
done:;
#endif
#if LOCK_DEBUG
    assert(lock->debug.initialized);
    assert(!lock->debug.file && "Attempting to recursively lock");
    lock->debug.file = file;
    lock->debug.line = line;
    extern int current_pid(void);
    lock->debug.pid = current_pid();
#endif
}
#define lock(lock) __lock(lock, __FILE__, __LINE__)
static inline void unlock(lock_t *lock) {
#if LOCK_DEBUG
    assert(lock->debug.initialized);
    assert(lock->debug.file && "Attempting to unlock an unlocked lock");
    lock->debug = (struct lock_debug) { .initialized = true };
#endif
    lock->owner = zero_init(pthread_t);
    pthread_mutex_unlock(&lock->m);
}

static inline int trylock(lock_t *lock, __attribute__((unused)) const char *file, __attribute__((unused)) int line) {
    int status = pthread_mutex_trylock(&lock->m);
#if LOCK_DEBUG
    if (!status) {
        lock->debug.file = file;
        lock->debug.line = line;
        extern int current_pid(void);
        lock->debug.pid = current_pid();
    }
#endif
    return status;
}
#define trylock(lock) trylock(lock, __FILE__, __LINE__)

// conditions, implemented using pthread conditions but hacked so you can also
// be woken by a signal

typedef struct {
    pthread_cond_t cond;
} cond_t;
#define COND_INITIALIZER ((cond_t) {PTHREAD_COND_INITIALIZER})

// Must call before using the condition
void cond_init(cond_t *cond);
// Must call when finished with the condition (currently doesn't do much but might do something important eventually I guess)
void cond_destroy(cond_t *cond);
// Releases the lock, waits for the condition, and reacquires the lock.
// Returns _EINTR if waiting stopped because the thread received a signal,
// _ETIMEDOUT if waiting stopped because the timout expired, 0 otherwise.
// Will never return _ETIMEDOUT if timeout is NULL.
int must_check wait_for(cond_t *cond, lock_t *lock, struct timespec *timeout);
// Same as wait_for, except it will never return _EINTR
int wait_for_ignore_signals(cond_t *cond, lock_t *lock, struct timespec *timeout);
// Wake up all waiters.
void notify(cond_t *cond);
// Wake up one waiter.
void notify_once(cond_t *cond);

// this is a read-write lock that prefers writers, i.e. if there are any
// writers waiting a read lock will block.
// on darwin pthread_rwlock_t is already like this, on linux you can configure
// it to prefer writers. not worrying about anything else right now.
typedef struct {
    pthread_rwlock_t l;
    // 0: unlocked
    // -1: write-locked
    // >0: read-locked with this many readers
    atomic_int val;
    const char *file;
    int line;
    int pid;
} wrlock_t;
static inline void wrlock_init(wrlock_t *lock) {
    pthread_rwlockattr_t *pattr = NULL;
#if defined(__GLIBC__)
    pthread_rwlockattr_t attr;
    pattr = &attr;
    pthread_rwlockattr_init(pattr);
    pthread_rwlockattr_setkind_np(pattr, PTHREAD_RWLOCK_PREFER_WRITER_NONRECURSIVE_NP);
#endif
    if (pthread_rwlock_init(&lock->l, pattr)) __builtin_trap();
    lock->val = lock->line = lock->pid = 0;
    lock->file = NULL;
}

extern int current_pid(void);
static inline void wrlock_destroy(wrlock_t *lock) {
    if (pthread_rwlock_destroy(&lock->l) != 0) __builtin_trap();
}
static inline void read_wrlock(wrlock_t *lock) {
    if (pthread_rwlock_rdlock(&lock->l) != 0) __builtin_trap();
    assert(lock->val >= 0);
    lock->val++;
}
static inline void read_wrunlock(wrlock_t *lock) {
    assert(lock->val > 0);
    lock->val--;
    if (pthread_rwlock_unlock(&lock->l) != 0) __builtin_trap();
}
static inline void __write_wrlock(wrlock_t *lock, const char *file, int line) {
    // Acquire the write lock without letting a *waiting* writer freeze out
    // readers (which macOS's writer-preferring pthread_rwlock does). We spin
    // on trywrlock instead of a blocking wrlock: while we're merely retrying
    // (not queued as a waiter), concurrent readers can still take the lock.
    //
    // This is load-bearing for the JIT: task_run_current holds mem->lock as a
    // READER for the duration of each JIT quantum and re-acquires it every
    // ~1024 blocks. Under a blocking writer-preferring wrlock, one thread
    // requesting the write lock (mmap/munmap/CoW, e.g. JSC's GC sweep) freezes
    // EVERY other JIT thread at its next read_wrlock — and if the writer is
    // itself waiting on those threads to reach a GC safepoint, the whole VM
    // deadlocks (observed intermittently in claude-cli startup). Spinning
    // keeps readers live so they can progress and release, letting the writer
    // eventually win a trylock.
    //
    // We must NEVER fall back to a blocking pthread_rwlock_wrlock here. That is
    // writer-preferring on macOS/iOS: the moment this thread queues as a waiter,
    // every subsequent reader blocks until we run — and if we (the writer, e.g.
    // JSC's GC doing an mmap/munmap sweep) are ourselves waiting for those
    // reader threads to reach a GC safepoint, the whole VM deadlocks. That is
    // exactly the intermittent claude/bun startup hang this trylock-spin exists
    // to prevent. It reproduces far more easily on-device (iOS): slower cores
    // mean JIT readers hold mem->lock across a longer quantum, so any bounded
    // spin budget is more likely to be exhausted and hit the blocking fallback.
    //
    // The original concern with an unbounded trylock-spin was burning a full
    // core (livelock) while readers are continuously present. We avoid that
    // WITHOUT ever blocking: back off to a steady sleep so the spinning writer
    // yields the CPU each iteration (it is not a busy-spin), and readers keep
    // making progress and releasing until the writer wins a trylock. This keeps
    // forward progress for readers guaranteed and the writer non-blocking.
    struct timespec _ts = {0, 1000}; // 1us initial backoff, grows to a steady sleep
    while (pthread_rwlock_trywrlock(&lock->l) != 0) {
        nanosleep(&_ts, NULL);
        if (_ts.tv_nsec < 100000) // ramp up to a 100us steady sleep (yields CPU)
            _ts.tv_nsec *= 2;
    }
    assert(lock->val == 0);
    lock->val = -1;
    lock->file = file;
    lock->line = line;
    lock->pid = current_pid();
}
#define write_wrlock(lock) __write_wrlock(lock, __FILE__, __LINE__)
static inline bool read_wrtrylock(wrlock_t *lock) {
    if (pthread_rwlock_tryrdlock(&lock->l) != 0)
        return false;
    assert(lock->val >= 0);
    lock->val++;
    return true;
}
static inline bool write_wrtrylock(wrlock_t *lock) {
    int err = pthread_rwlock_trywrlock(&lock->l);
    if (err == EBUSY) return false;
    if (err != 0) __builtin_trap();
    assert(lock->val == 0);
    lock->val = -1;
    return true;
}
static inline void write_wrunlock(wrlock_t *lock) {
    assert(lock->val == -1);
    lock->val = lock->line = lock->pid = 0;
    lock->file = NULL;
    if (pthread_rwlock_unlock(&lock->l) != 0) __builtin_trap();
}

extern __thread sigjmp_buf unwind_buf;
extern __thread bool should_unwind;
static inline int sigunwind_start(void) {
    if (sigsetjmp(unwind_buf, 1)) {
        should_unwind = false;
        return 1;
    } else {
        should_unwind = true;
        return 0;
    }
}
static inline void sigunwind_end(void) {
    should_unwind = false;
}

#endif
