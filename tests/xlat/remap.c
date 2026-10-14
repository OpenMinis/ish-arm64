#define _GNU_SOURCE
// One thread reads a region while others map it again in place (MAP_FIXED), as a garbage collector
// commits and uncommits its heap. Legal on Linux: the region is always mapped. gcc -O1 remap.c -lpthread
#include <stdio.h>
#include <stdint.h>
#include <pthread.h>
#include <sys/mman.h>
#include <unistd.h>
#include <stdlib.h>
#define PAGES 64
static volatile uint64_t *region;
static volatile int stop;
static int use_mmap = 1, use_madvise = 1, nreaders = 4, nwriters = 3;
static void *remapper(void *arg) {
    long n = 0;
    while (!stop) {
        unsigned p = (unsigned) (n * 7 + (long) arg) % PAGES;
        if (use_mmap) mmap((char *) region + p * 4096, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
        if (use_madvise && n % 3 == 0)
            madvise((char *) region + ((p + 5) % PAGES) * 4096, 4096, MADV_DONTNEED);
        n++;
    }
    return (void *) n;
}
static void *reader(void *arg) {
    uint64_t sum = 0;
    while (!stop)
        for (unsigned i = 0; i < PAGES * 512; i += 61)
            sum += region[i];
    return (void *) sum;
}
int main(int argc, char **argv) {
    int seconds = argc > 1 ? atoi(argv[1]) : 20;
    if (argc > 2) use_mmap = atoi(argv[2]);
    if (argc > 3) use_madvise = atoi(argv[3]);
    if (argc > 4) nreaders = atoi(argv[4]);
    if (argc > 5) nwriters = atoi(argv[5]);
    region = mmap(NULL, PAGES * 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    pthread_t r[16], w[16];
    for (long i = 0; i < nreaders; i++) pthread_create(&r[i], NULL, reader, (void *) i);
    for (long i = 0; i < nwriters; i++) pthread_create(&w[i], NULL, remapper, (void *) i);
    sleep(seconds);
    stop = 1;
    long maps = 0;
    for (int i = 0; i < nwriters; i++) { void *v; pthread_join(w[i], &v); maps += (long) v; }
    for (int i = 0; i < nreaders; i++) pthread_join(r[i], NULL);
    printf("survived %d s, %ld remaps\n", seconds, maps);
    return 0;
}
