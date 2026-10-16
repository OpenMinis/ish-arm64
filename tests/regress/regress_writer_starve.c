// mmap, munmap and mprotect must not starve while other threads spin.
//
// iSH runs guest code holding the address-space lock for reading and only lets
// go between quanta; a call that changes the address space takes it for
// writing. The writer used to retry a trylock every 100 us without telling
// anyone it was waiting, so two threads spinning in guest code - each
// releasing the lock for a moment between quanta - almost never let go at the
// same time and the writer waited seconds, once a full minute. .NET hit this
// in every few officecli runs: two threads spin on a runtime lock (yield
// loop, then sched_yield) whose owner sits in munmap, so the whole process
// stalled until the writer got lucky.
//
// Two and then four threads spin like that while the main thread maps,
// protects and unmaps a page 200 times. The slowest call must take under
// 50 ms (arm64 Linux: tens of microseconds).
//   aarch64-linux-musl-gcc -static -O1 -pthread -o wstarve regress_writer_starve.c
#define _GNU_SOURCE
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/mman.h>
#include <time.h>

static atomic_int stop;
static volatile uint64_t sink;

static int64_t now_us(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t) t.tv_sec * 1000000 + t.tv_nsec / 1000;
}

// Like a runtime spin lock: a burst of pure computation, then sched_yield.
static void *spinner(void *arg) {
    (void) arg;
    while (!atomic_load(&stop)) {
        uint64_t x = 1;
        for (int i = 0; i < 20000; i++)
            x = x * 6364136223846793005ULL + 1442695040888963407ULL;
        sink = x;
        sched_yield();
    }
    return NULL;
}

static int run(int spinners) {
    pthread_t t[8];
    atomic_store(&stop, 0);
    for (int i = 0; i < spinners; i++)
        pthread_create(&t[i], NULL, spinner, NULL);
    struct timespec settle = {0, 50 * 1000000};
    nanosleep(&settle, NULL);

    int64_t worst = 0, total = now_us();
    for (int i = 0; i < 200; i++) {
        int64_t t0 = now_us();
        char *p = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        p[0] = 1;
        mprotect(p, 4096, PROT_READ);
        munmap(p, 4096);
        int64_t d = now_us() - t0;
        if (d > worst)
            worst = d;
    }
    total = now_us() - total;
    atomic_store(&stop, 1);
    for (int i = 0; i < spinners; i++)
        pthread_join(t[i], NULL);

    int ok = worst < 50000;
    printf("%s %d spinning threads: slowest map/protect/unmap %s 50 ms\n",
           ok ? "PASS" : "FAIL", spinners, ok ? "under" : "over");
    if (!ok)
        printf("     slowest %lld us, 200 rounds in %lld ms\n", (long long) worst, (long long) total / 1000);
    return ok;
}

int main(void) {
    int ok = run(2);
    ok &= run(4);
    return ok ? 0 : 1;
}
