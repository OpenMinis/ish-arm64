// Regression: a process exiting while its other threads sit in epoll_wait
// must not wedge the kernel.
//
// The failure this pins (Minis 1.16(1), 2026-10-14): a Go program (gh)
// exited with its netpoller threads parked in epoll_pwait. do_exit_group's
// safety valve gave up waiting for them and "leaked" them - unlinked from the
// group, sighand/files released - but left the host threads running. Within a
// second each woke from poll_wait's bounded host wait and dereferenced the
// torn-down current->group; one did so inside the idle check that holds
// pids_lock, faulted, and jit_crash_handler parked it forever with the lock
// held. From then on every fork() blocked in task_create_.
//
// Each round forks PARALLEL children; every child starts THREADS threads that
// block in epoll_wait(-1) on a pipe nobody writes, then calls _exit() (exit_group).
// The parent then checks that the NEXT fork still completes. A wedged kernel
// shows up as a fork that never returns, so the parent arms alarm() and fails
// loudly instead of hanging the runner.
//
// Usage: regress_leaked_thread_exit [rounds] [parallel] [threads]
// Prints "PASS rounds=N" or "FAIL ...". Exit status 0 on pass.
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int epfd;
static pthread_barrier_t started;

static void *park(void *arg) {
    (void) arg;
    struct epoll_event ev;
    pthread_barrier_wait(&started);
    for (;;)
        epoll_wait(epfd, &ev, 1, -1);
    return NULL;
}

static void child(int threads) {
    int p[2];
    if (pipe(p) < 0)
        _exit(2);
    epfd = epoll_create1(0);
    struct epoll_event ev = {.events = EPOLLIN, .data.fd = p[0]};
    if (epfd < 0 || epoll_ctl(epfd, EPOLL_CTL_ADD, p[0], &ev) < 0)
        _exit(3);
    pthread_barrier_init(&started, NULL, threads + 1);
    for (int i = 0; i < threads; i++) {
        pthread_t t;
        if (pthread_create(&t, NULL, park, NULL) != 0)
            _exit(4);
    }
    pthread_barrier_wait(&started);
    // Let every thread get into the host wait before exit_group.
    struct timespec ts = {0, 50 * 1000000L};
    nanosleep(&ts, NULL);
    _exit(0);
}

static void on_alarm(int sig) {
    (void) sig;
    static const char msg[] = "FAIL fork or wait hung (kernel wedged)\n";
    write(1, msg, sizeof(msg) - 1);
    _exit(1);
}

int main(int argc, char **argv) {
    int rounds = argc > 1 ? atoi(argv[1]) : 20;
    int parallel = argc > 2 ? atoi(argv[2]) : 4;
    int threads = argc > 3 ? atoi(argv[3]) : 8;
    signal(SIGALRM, on_alarm);
    for (int r = 0; r < rounds; r++) {
        // A healthy round takes well under a second per child plus the
        // safety valve's own wait; 30 s means the kernel stopped forking.
        alarm(30);
        pid_t pids[64];
        int n = parallel > 64 ? 64 : parallel;
        for (int i = 0; i < n; i++) {
            pids[i] = fork();
            if (pids[i] < 0) {
                printf("FAIL fork: %s\n", strerror(errno));
                return 1;
            }
            if (pids[i] == 0)
                child(threads);
        }
        for (int i = 0; i < n; i++) {
            int st;
            if (waitpid(pids[i], &st, 0) < 0) {
                printf("FAIL waitpid: %s\n", strerror(errno));
                return 1;
            }
        }
        // Leaked threads wake on poll_wait's 1 s bounded host wait; give them
        // time to do so before the next round's forks need pids_lock.
        sleep(2);
        // The probe: a plain fork must still work.
        pid_t probe = fork();
        if (probe == 0)
            _exit(0);
        if (probe < 0 || waitpid(probe, NULL, 0) < 0) {
            printf("FAIL probe fork after round %d\n", r);
            return 1;
        }
        alarm(0);
        printf("round %d ok\n", r);
        fflush(stdout);
    }
    printf("PASS rounds=%d\n", rounds);
    return 0;
}
