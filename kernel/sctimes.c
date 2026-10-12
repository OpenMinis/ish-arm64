// ISH_SYSCALL_TIMES=<file>: where each guest thread's time goes. Per thread,
// per syscall number: calls, wall time and the host thread's CPU time inside
// it. A thread appends one line to the file when it exits:
//
//   T <tid> <pid> <comm> alive_us cpu_us | <nr>:<calls>:<wall_us>:<cpu_us> ...
//
// Wall time well above CPU time inside a syscall is waiting in the kernel
// (futex, poll, a child); CPU time outside syscalls is the guest computing
// (translated code, page faults, signal delivery). benchmark/daily/
// sctimes.py sums the lines up.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include "kernel/task.h"
#include "kernel/sctimes.h"

#define SCT_MAX 512

struct sctimes {
    uint64_t start_ns;
    uint64_t calls[SCT_MAX], wall_ns[SCT_MAX], cpu_ns[SCT_MAX];
};

static const char *sct_path;
int sctimes_on = -1;
static pthread_mutex_t sct_lock = PTHREAD_MUTEX_INITIALIZER;

bool sctimes_init(void) {
    sct_path = getenv("ISH_SYSCALL_TIMES");
    sctimes_on = sct_path != NULL && sct_path[0] != '\0';
    return sctimes_on;
}

static uint64_t wall_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t) ts.tv_sec * 1000000000 + ts.tv_nsec;
}

// CPU time of the calling host thread, user + kernel
static uint64_t cpu_ns(void) {
#ifdef _WIN32
    return px_thread_cpu_ns();
#else
    struct timespec ts;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return (uint64_t) ts.tv_sec * 1000000000 + ts.tv_nsec;
#endif
}

void sctimes_begin(struct sct_mark *m) {
    m->wall = wall_ns();
    m->cpu = cpu_ns();
}

void sctimes_end(const struct sct_mark *m, unsigned nr) {
    struct task *t = current;
    if (t == NULL || nr >= SCT_MAX)
        return;
    struct sctimes *s = t->sctimes;
    if (s == NULL) {
        s = t->sctimes = calloc(1, sizeof(*s));
        if (s == NULL)
            return;
        s->start_ns = m->wall;
    }
    s->calls[nr]++;
    s->wall_ns[nr] += wall_ns() - m->wall;
    s->cpu_ns[nr] += cpu_ns() - m->cpu;
}

void sctimes_task_exit(struct task *t) {
    struct sctimes *s = t->sctimes;
    if (s == NULL)
        return;
    t->sctimes = NULL;
    char line[16384], comm[sizeof(t->comm) + 1];
    snprintf(comm, sizeof(comm), "%.16s", t->comm[0] ? t->comm : "?");
    for (char *c = comm; *c; c++)
        if (*c == ' ' || *c == '\t' || *c == '\n')
            *c = '_';   // a program can rename itself ("npm install x")
    int n = snprintf(line, sizeof(line), "T %d %d %s %llu %llu |", t->pid, t->tgid, comm,
                     (unsigned long long) ((wall_ns() - s->start_ns) / 1000),
                     (unsigned long long) (cpu_ns() / 1000));
    for (unsigned i = 0; i < SCT_MAX && n < (int) sizeof(line) - 64; i++) {
        if (s->calls[i] == 0)
            continue;
        n += snprintf(line + n, sizeof(line) - n, " %u:%llu:%llu:%llu", i, (unsigned long long) s->calls[i],
                      (unsigned long long) (s->wall_ns[i] / 1000), (unsigned long long) (s->cpu_ns[i] / 1000));
    }
    free(s);
    pthread_mutex_lock(&sct_lock);
    FILE *f = fopen(sct_path, "a");
    if (f != NULL) {
        fprintf(f, "%s\n", line);
        fclose(f);
    }
    pthread_mutex_unlock(&sct_lock);
}
