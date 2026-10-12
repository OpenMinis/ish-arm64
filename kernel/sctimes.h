#ifndef KERNEL_SCTIMES_H
#define KERNEL_SCTIMES_H
#include <stdbool.h>
#include <stdint.h>

// ISH_SYSCALL_TIMES: per-thread syscall wall and CPU time (kernel/sctimes.c)
struct task;
struct sct_mark { uint64_t wall, cpu; };
// Spans inside (or outside) syscalls, counted like syscall numbers
#define SCT_FAKEFS_LOCK 500   // waiting for the fakefs meta.db mutex
#define SCT_DB_BEGIN 501      // sqlite BEGIN (takes the WAL locks)
#define SCT_DB_COMMIT 502     // sqlite COMMIT (may checkpoint and sync)
#define SCT_HOST_OPEN 503     // the host's open of a fakefs/realfs file
#define SCT_MEM_WRLOCK 504    // waiting for a wrlock as a writer (mem->lock: mmap/munmap/CoW)
// -1 until the environment was looked at; inline, so that a disabled
// ISH_SYSCALL_TIMES costs one load and a predicted branch per call site
extern int sctimes_on;
bool sctimes_init(void);
static inline bool sctimes_enabled(void) {
    return __builtin_expect(sctimes_on != 0, 0) && (sctimes_on > 0 || sctimes_init());
}
void sctimes_begin(struct sct_mark *m);
void sctimes_end(const struct sct_mark *m, unsigned nr);
void sctimes_task_exit(struct task *t);

#endif
