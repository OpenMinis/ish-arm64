// Reads and writes of a PROT_NONE page must raise SIGSEGV, also right after
// the page was readable (a stale TLB entry), as the JVM's implicit null
// checks expect: its heap starts after a PROT_NONE page.
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <sys/mman.h>

static sigjmp_buf env;
static volatile void *fault_addr;

static void on_segv(int sig, siginfo_t *info, void *uc) {
    (void) sig; (void) uc;
    fault_addr = info->si_addr;
    siglongjmp(env, 1);
}

static int faults(volatile char *p, int write) {
    fault_addr = NULL;
    if (sigsetjmp(env, 1))
        return fault_addr == p ? 1 : 2;
    if (write)
        *p = 1;
    else
        (void) *p;
    return 0;
}

int main(void) {
    struct sigaction sa = {0};
    sa.sa_sigaction = on_segv;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, NULL);

    char *m = mmap(NULL, 3 << 12, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (m == MAP_FAILED)
        return perror("mmap"), 1;
    mprotect(m + 4096, 4096, PROT_READ | PROT_WRITE);
    int bad = 0;
    const char *what[] = {"no fault", "fault", "fault at the wrong address"};
#define CHECK(p, w, want) do { \
        int r = faults(p, w); \
        printf("%-28s %s\n", #p " " #w, what[r]); \
        bad += r != want; \
    } while (0)
    CHECK(m + 0x28, 0, 1);
    CHECK(m + 0x28, 1, 1);
    CHECK(m + 4096 + 8, 0, 0);
    CHECK(m + 4096 + 8, 1, 0);
    mprotect(m + 4096, 4096, PROT_NONE);
    CHECK(m + 4096 + 8, 0, 1);
    CHECK(m + 4096 + 8, 1, 1);
    mprotect(m + 4096, 4096, PROT_READ);
    CHECK(m + 4096 + 8, 0, 0);
    CHECK(m + 4096 + 8, 1, 1);
    CHECK(m + 8192 + 0x28, 0, 1);
    printf("%d wrong\n", bad);
    return bad != 0;
}
