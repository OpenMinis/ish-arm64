#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <time.h>
#include "util/timer.h"
#include "misc.h"
#include "kernel/diag.h"
#include "kernel/errno.h"

// [INV-261109-03] Set by timer_thread around the callback.
static __thread struct timer *firing;
struct timer *timer_firing(void) {
    return firing;
}

// Fill the block before it goes back to malloc: a stale user then reads
// TIMER_DEAD_MAGIC (or whatever reused the block), never a live-looking timer.
static void timer_poison_free(struct timer *timer) {
    memset(timer, 0xde, sizeof(*timer));
    timer->magic = TIMER_DEAD_MAGIC;
    free(timer);
}

struct timer *timer_new(clockid_t clockid, timer_callback_t callback, void *data) {
//    assert(clockid == CLOCK_MONOTONIC || clockid == CLOCK_REALTIME);
    struct timer *timer = malloc(sizeof(struct timer));
    timer->clockid = clockid;
    timer->callback = callback;
    timer->data = data;
    timer->active = false;
    timer->thread_running = false;
    lock_init(&timer->lock);
    timer->dead = false;
    timer->magic = TIMER_LIVE_MAGIC;
    return timer;
}

void timer_free(struct timer *timer) {
    // [INV-261109-03] A freed (poisoned) or reused block: do not lock or free
    // it a second time, record who tried.
    uint32_t magic = __atomic_load_n(&timer->magic, __ATOMIC_RELAXED);
    if (magic != TIMER_LIVE_MAGIC) {
        ish_diag_timer_struct_bad(timer, DIAG_TIMER_CALL_DELETE, magic);
        return;
    }
    lock(&timer->lock);
    timer->active = false;
    if (timer->thread_running) {
        timer->dead = true;
        pthread_kill(timer->thread, SIGUSR1);
        unlock(&timer->lock);
    } else {
        unlock(&timer->lock);
        timer_poison_free(timer);
    }
}

static void *timer_thread(void *param) {
    struct timer *timer = param;
    // Rearming and freeing a timer interrupt its nanosleep with SIGUSR1,
    // which the creating thread may have had blocked (see task_thread).
    sigset_t wake;
    sigemptyset(&wake);
    sigaddset(&wake, SIGUSR1);
    pthread_sigmask(SIG_UNBLOCK, &wake, NULL);
    lock(&timer->lock);
    while (true) {
        struct timespec remaining = timespec_subtract(timer->end, timespec_now(timer->clockid));
        while (timer->active && timespec_positive(remaining)) {
            unlock(&timer->lock);
            nanosleep(&remaining, NULL);
            lock(&timer->lock);
            remaining = timespec_subtract(timer->end, timespec_now(timer->clockid));
        }
        if (timer->active) {
            firing = timer;
            timer->callback(timer->data);
            firing = NULL;
        }
        if (timer->active && timespec_positive(timer->interval)) {
            timer->start = timer->end;
            timer->end = timespec_add(timer->start, timer->interval);
        } else {
            break;
        }
    }
    timer->thread_running = false;
    ish_diag_add(DIAG_TIMER_THREADS_LIVE, (uint64_t) -1);   // [FIX-261031-04]
    if (timer->dead)
        timer_poison_free(timer);
    else
        unlock(&timer->lock);
    return NULL;
}

int timer_set(struct timer *timer, struct timer_spec spec, struct timer_spec *oldspec) {
    uint32_t magic = __atomic_load_n(&timer->magic, __ATOMIC_RELAXED);
    if (magic != TIMER_LIVE_MAGIC) {   // [INV-261109-03] see timer_free
        ish_diag_timer_struct_bad(timer, DIAG_TIMER_CALL_SETTIME, magic);
        return _EINVAL;
    }
    lock(&timer->lock);
    struct timespec now = timespec_now(timer->clockid);
    if (oldspec != NULL) {
        oldspec->value = timespec_subtract(timer->end, now);
        oldspec->interval = timer->interval;
    }

    timer->start = now;
    timer->end = timespec_add(timer->start, spec.value);
    timer->interval = spec.interval;
    timer->active = !timespec_is_zero(spec.value);
    if (timer->thread_running) {
        pthread_kill(timer->thread, SIGUSR1);
    } else if (timer->active) {
        timer->thread_running = true;
        ish_diag_add(DIAG_TIMER_THREADS_LIVE, 1);   // [FIX-261031-04] ended in timer_thread
        pthread_create(&timer->thread, NULL, timer_thread, timer);
        pthread_detach(timer->thread);
    }
    unlock(&timer->lock);
    return 0;
}
