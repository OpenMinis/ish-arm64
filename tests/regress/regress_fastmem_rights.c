// Pages of one 16KB host page with different rights: every store the guest
// may not make must fault, in hot code too, and the allowed accesses must
// work. fastmem (kernel/sba.h) protects host pages and sends such mixed ones
// the slow way (macOS); elsewhere this checks the TLB path the same way.
// (Loads from PROT_NONE pages read zeroes in iSH, and a store to a hole
// next to mappings maps it, on purpose: see the fault handling in
// kernel/calls.c. So: stores to PROT_NONE and read-only pages.)
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static sigjmp_buf env;
static void on_segv(int sig) { (void) sig; siglongjmp(env, 1); }

static int faults(volatile char *p, int write, int times) {
    int n = 0;
    for (int i = 0; i < times; i++) {
        if (sigsetjmp(env, 1)) {
            n++;
            continue;
        }
        if (write)
            p[i % 64] = (char) i;
        else
            (void) p[i % 64];
    }
    return n;
}

int main(void) {
    struct sigaction sa = {0};
    sa.sa_handler = on_segv;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    // 16KB-aligned run of 8 pages
    char *raw = mmap(NULL, 12 * 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    char *p = (char *) (((uintptr_t) raw + 16383) & ~(uintptr_t) 16383);
    memset(p, 7, 8 * 4096);
    int bad = 0;
    // warm the accesses up in hot code first
    bad += faults(p, 1, 2000) != 0;
    bad += faults(p + 4096, 0, 2000) != 0;
    mprotect(p + 4096, 4096, PROT_NONE);         // a guard page among RW pages
    bad += faults(p, 0, 2000) != 0;              // its neighbours still work
    bad += faults(p + 2 * 4096, 1, 2000) != 0;
    bad += faults(p + 4096, 1, 200) != 200;      // it faults, every time
    mprotect(p + 2 * 4096, 4096, PROT_READ);     // read-only next to RW
    bad += faults(p + 2 * 4096, 0, 2000) != 0;
    bad += faults(p + 2 * 4096, 1, 200) != 200;
    bad += faults(p + 3 * 4096, 1, 2000) != 0;
    mprotect(p + 4096, 2 * 4096, PROT_READ | PROT_WRITE);   // all open again
    bad += faults(p + 4096, 1, 2000) != 0;
    bad += faults(p + 2 * 4096, 1, 2000) != 0;
    bad += p[4096 + 1999 % 64] != (char) 1999;
    printf("%s (%d bad)\n", bad ? "FAIL" : "ok", bad);
    return bad != 0;
}
