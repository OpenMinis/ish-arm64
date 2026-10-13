// Access sites of xlat (asbestos/guest-x86_64/xlat.c e_mem_insn): the same instructions on a page that
// stays and on pages that change with every access, in turn; across other mappings coming and going
// (TLB flushes), a remap and an mprotect of pages the sites learned; and from two threads.
// Prints checksums; compare with ISH_XLAT=0.  gcc -O1 -static sites.c -lpthread
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <pthread.h>
#include <sys/mman.h>

#define PAGES 64
struct node { uint64_t v; struct node *next; uint8_t pad[40]; };

__attribute__((noinline)) static uint64_t walk(struct node *n, int steps, uint64_t add) {
    uint64_t sum = 0;
    for (int i = 0; i < steps; i++) {
        sum = sum * 31 + n->v;       // a load through a pointer
        n->v += add + (uint64_t) i;  // a read-modify-write through it
        n = n->next;                 // the pointer itself
    }
    return sum;
}

__attribute__((noinline)) static void fill(uint8_t *p, size_t n, uint8_t v) {
    for (size_t i = 0; i < n; i++)
        p[i] = (uint8_t) (v + i);
}

static uint64_t run(uint64_t seed, int verbose) {
    uint8_t *mem = mmap(NULL, (PAGES + 1) * 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    // a ring through all the pages, and one that stays on a page
    struct node *far[PAGES], *near[PAGES];
    for (int i = 0; i < PAGES; i++) {
        far[i] = (struct node *) (mem + i * 4096 + (i * 56) % 3968);
        near[i] = (struct node *) (mem + 4096 * PAGES + i * 56 + 64);
    }
    for (int i = 0; i < PAGES; i++) {
        far[i]->v = seed + (uint64_t) i; far[i]->next = far[(i * 7 + 1) % PAGES];
    }
    for (int i = 0; i < PAGES; i++) {
        near[i]->v = seed * 3 + (uint64_t) i; near[i]->next = near[(i + 1) % PAGES];
    }
    uint64_t total = 0;
    for (int phase = 0; phase < 40; phase++) {
        // the same instructions of walk(): pages that change, a page that stays, in turn
        uint64_t a = walk(phase % 3 == 0 ? near[0] : far[phase % PAGES], 700 + phase * 13, seed + (uint64_t) phase);
        uint64_t b = walk(phase % 4 == 1 ? far[1] : near[phase % PAGES], 300, 1);
        total = total * 1099511628211ull + a + b;
        if (phase % 5 == 2) {
            // other mappings come and go: the TLB is flushed, the sites forget their pages
            uint8_t *t = mmap(NULL, 8 * 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            fill(t, 8 * 4096, (uint8_t) phase);
            total += t[4096 * 3 + 17];
            munmap(t, 8 * 4096);
        }
        if (phase % 7 == 3) {
            // a page the sites learned is replaced: they have to see the new one
            uint8_t *page = mem + 4096 * (PAGES / 2);
            uint8_t keep[4096];
            memcpy(keep, page, 4096);
            mmap(page, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
            total += far[PAGES / 2]->v;           // zero: the fresh page
            memcpy(page, keep, 4096);
            far[PAGES / 2]->v ^= 0x5555;
        }
        if (phase % 9 == 4) {
            // read-only and back: a write through a learned page has to notice
            mprotect(mem + 4096 * 5, 4096, PROT_READ);
            total += far[5]->v;
            mprotect(mem + 4096 * 5, 4096, PROT_READ | PROT_WRITE);
        }
        if (verbose)
            printf("phase %2d %016llx\n", phase, (unsigned long long) total);
    }
    for (int i = 0; i < PAGES; i++)
        total = total * 31 + far[i]->v + near[i]->v;
    munmap(mem, (PAGES + 1) * 4096);
    return total;
}

static void *thread(void *arg) {
    uint64_t *r = arg;
    *r = run(*r, 0);
    return NULL;
}

int main(void) {
    printf("single %016llx\n", (unsigned long long) run(0x1234, 1));
    // two threads on the same sites, each with its own pages (and its own TLB); their mmaps flush each other's
    uint64_t r[2] = {0x777, 0x999};
    pthread_t t[2];
    for (int i = 0; i < 2; i++)
        pthread_create(&t[i], NULL, thread, &r[i]);
    for (int i = 0; i < 2; i++)
        pthread_join(t[i], NULL);
    printf("threads %016llx %016llx\n", (unsigned long long) r[0], (unsigned long long) r[1]);
    printf("again %016llx\n", (unsigned long long) run(0x1234, 0));
    return 0;
}
