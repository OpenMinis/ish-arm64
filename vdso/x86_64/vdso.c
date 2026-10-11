#if !__x86_64__ || !__ELF__
#error "VDSO must be built for x86_64 elf"
#endif

typedef long time_t;
typedef int clockid_t;

static long syscall2(long nr, long a, long b) {
    long result;
    __asm__ volatile("syscall" : "=a" (result) : "0" (nr), "D" (a), "S" (b) : "rcx", "r11", "memory");
    return result;
}

time_t __vdso_time(time_t *t) {
    return syscall2(201 /* __NR_time */, (long) t, 0);
}

int __vdso_gettimeofday(void *timeval, void *timezone) {
    return syscall2(96 /* __NR_gettimeofday */, (long) timeval, (long) timezone);
}

int __vdso_clock_gettime(clockid_t clock, void *timespec) {
    return syscall2(228 /* __NR_clock_gettime */, clock, (long) timespec);
}

int __vdso_getcpu(unsigned *cpu, unsigned *node, void *unused) {
    (void) unused;
    return syscall2(309 /* __NR_getcpu */, (long) cpu, (long) node);
}
