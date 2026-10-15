// A process whose threads are blocked in host calls must die at once when it
// is killed or exits, and leave no thread behind.
//
// iSH runs a blocking read, write, accept or flock as the same host call and
// interrupts it with SIGUSR1 when a guest signal arrives. When the host
// thread had SIGUSR1 blocked - inherited from the app thread that started the
// guest process, a GCD worker in Minis - nothing interrupted it: kill -9 did
// nothing, exit_group's safety valve gave up after 1.5 s and leaked the
// threads with their address space (a crashed Gradle daemon left 4 threads
// that only an app restart removed). Run this both plainly and with SIGUSR1
// blocked in iSH's own mask (run.sh does both); the output must match arm64
// Linux line for line:
//   aarch64-linux-musl-gcc -static -O1 -pthread -o killblk regress_kill_blocked.c
//
// Each case forks a child whose main thread and two more threads block in
// one kind of call, then either the parent SIGKILLs it or a fourth thread
// calls _exit(3). The parent reports how the child ended, whether that took
// under 1.4 s (iSH's safety valve waits at least 1.5 s before leaking), and
// how many of the child's threads still exist in /proc afterwards.
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define LOCK_PATH "/tmp/regress_kill_blocked.lock"
#define BLOCKERS 3   // the main thread and two more

static long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
static void sleep_ms(long ms) {
    struct timespec ts = {ms / 1000, (ms % 1000) * 1000000};
    while (nanosleep(&ts, &ts) < 0 && errno == EINTR) {}
}

enum kind { K_WRITE_FULL, K_READ_PIPE, K_READ_SOCK, K_ACCEPT, K_FLOCK };
static const char *kind_name[] = {
    "write to a full pipe", "read an empty pipe", "read a quiet socketpair",
    "accept with no client", "flock a held lock",
};

static enum kind kind;
static int fd_a = -1, fd_b = -1;   // what the blockers block on
static int report_fd;

static void block_once(void) {
    char buf[65536];
    pid_t tid = (pid_t) syscall(SYS_gettid);
    write(report_fd, &tid, sizeof tid);
    for (;;) {
        switch (kind) {
        case K_WRITE_FULL: write(fd_b, buf, sizeof buf); break;
        case K_READ_PIPE: read(fd_a, buf, sizeof buf); break;
        case K_READ_SOCK: read(fd_a, buf, sizeof buf); break;
        case K_ACCEPT: accept(fd_a, NULL, NULL); break;
        case K_FLOCK: {
            // each blocker needs its own open file description
            int fd = open(LOCK_PATH, O_RDWR);
            flock(fd, LOCK_EX);
            break;
        }
        }
    }
}
static void *blocker(void *arg) { (void) arg; block_once(); return NULL; }
static void *exiter(void *arg) { (void) arg; sleep_ms(300); _exit(3); }

static void setup(void) {
    int p[2];
    switch (kind) {
    case K_WRITE_FULL:
        pipe(p);
        fd_a = p[0]; fd_b = p[1];   // nobody reads p[0]
        fcntl(fd_b, F_SETFL, O_NONBLOCK);
        {
            char buf[4096] = {0};
            while (write(fd_b, buf, sizeof buf) > 0) {}
        }
        fcntl(fd_b, F_SETFL, 0);
        break;
    case K_READ_PIPE:
        pipe(p);
        fd_a = p[0]; fd_b = p[1];   // nobody writes p[1]
        break;
    case K_READ_SOCK:
        socketpair(AF_UNIX, SOCK_STREAM, 0, p);
        fd_a = p[0]; fd_b = p[1];
        break;
    case K_ACCEPT: {
        struct sockaddr_in sin = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
        fd_a = socket(AF_INET, SOCK_STREAM, 0);
        bind(fd_a, (struct sockaddr *) &sin, sizeof sin);
        listen(fd_a, 1);
        break;
    }
    case K_FLOCK:
        break;   // the parent holds the lock
    }
}

static void run_case(enum kind k, int by_exit) {
    kind = k;
    int holder = -1;
    if (k == K_FLOCK) {
        holder = open(LOCK_PATH, O_RDWR | O_CREAT, 0600);
        flock(holder, LOCK_EX);
    }
    int rp[2];
    pipe(rp);
    pid_t c = fork();
    if (c == 0) {
        close(rp[0]);
        report_fd = rp[1];
        setup();
        pthread_t t;
        for (int i = 1; i < BLOCKERS; i++)
            pthread_create(&t, NULL, blocker, NULL);
        if (by_exit)
            pthread_create(&t, NULL, exiter, NULL);
        block_once();
    }
    close(rp[1]);
    pid_t tids[BLOCKERS];
    int got = 0;
    while (got < BLOCKERS && read(rp[0], &tids[got], sizeof tids[0]) == sizeof tids[0])
        got++;
    close(rp[0]);
    sleep_ms(300);   // let them all block

    long t0 = now_ms();
    if (!by_exit)
        kill(c, SIGKILL);
    int st = 0;
    pid_t w;
    while ((w = waitpid(c, &st, 0)) < 0 && errno == EINTR) {}
    long took = now_ms() - t0;
    if (by_exit)
        took -= 300;   // the exiter's own delay

    int left = 0;
    for (int i = 0; i < got; i++) {
        char path[32];
        snprintf(path, sizeof path, "/proc/%d", tids[i]);
        if (access(path, F_OK) == 0)
            left++;
    }
    char how[32];
    if (w != c) snprintf(how, sizeof how, "waitpid %s", strerror(errno));
    else if (WIFSIGNALED(st)) snprintf(how, sizeof how, "signal %d", WTERMSIG(st));
    else if (WIFEXITED(st)) snprintf(how, sizeof how, "exit %d", WEXITSTATUS(st));
    else snprintf(how, sizeof how, "status %#x", st);
    printf("%-24s %-8s %s, under 1.4s %s, %d of %d threads left\n", kind_name[k],
           by_exit ? "_exit" : "SIGKILL", how, took < 1400 ? "yes" : "no", left, got);
    fflush(stdout);
    if (holder >= 0) {
        close(holder);
        unlink(LOCK_PATH);
    }
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    alarm(120);   // a child that cannot be reaped must not hang the run
    for (enum kind k = K_WRITE_FULL; k <= K_FLOCK; k++) {
        run_case(k, 0);
        run_case(k, 1);
    }
    return 0;
}
