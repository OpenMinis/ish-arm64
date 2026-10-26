// cmp, then a load from a page native code reaches only through the TLB (a
// shared file mapping lives outside the shadow window: the access takes its
// slow path), then a flag read: the flags must survive the slow path.
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/mman.h>
#include <unistd.h>
int main() {
    int fd = open("/tmp/flagstk.dat", O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (fd < 0 || ftruncate(fd, 4096) < 0)
        return 2;
    volatile uint64_t *slot = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (slot == MAP_FAILED)
        return 2;
    slot[1] = 22; slot[2] = 33;
    int bad = 0;
    for (int i = 0; i < 2000; i++) {
        uint64_t a = i & 7, r, v;
        __asm__ volatile("cmp %2, #3\n ldr %1, [%3, #8]\n cset %0, eq" : "=r"(r), "=r"(v) : "r"(a), "r"(slot) : "cc");
        if (r != (a == 3) || v != 22) bad++;
        __asm__ volatile("cmp %2, #5\n ldr %1, [%3, #16]\n csinc %0, xzr, xzr, lt" : "=r"(r), "=r"(v) : "r"(a), "r"(slot) : "cc");
        if (r != (a >= 5) || v != 33) bad++;
    }
    unlink("/tmp/flagstk.dat");
    printf("%s (%d bad)\n", bad ? "FAIL" : "ok", bad);
    return bad != 0;
}
