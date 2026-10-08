// [T-ish-poll-spin-guard] poll()/ppoll() must not hang or spin when the host
// keeps waking it for an event the guest did not ask for.
//
// iSH backs a guest poll with a host kqueue. A regular file registered for
// EVFILT_READ fires whenever its offset is short of its size - even with
// NOTE_LOWAT=INT_MAX, which iSH uses to mean "only wake me on hangup" - while
// the guest-side check says nothing the caller asked for is ready. Before the
// fix every such wakeup was followed by another wait, all inside one syscall:
//   - with timeout 0 the loop never ended (the timeout only counted as
//     expired when the host returned no events), so the call hung forever;
//   - with any other timeout it spun at 100% of a host core.
// A pipe does the same when its buffer is full: xnu clamps the low water mark
// of a pipe to its buffer size.
// A Rust program's runtime polls fds 0-2 with events=0 and timeout 0 before
// main(), so `rg x < file` (or any Rust binary whose stdio is a regular file
// someone else is writing to) hung at 100% CPU. Field case 2026-10-07/08:
// ripgrep stuck in ppoll with a frozen syscall count until iOS killed the
// backgrounded app for exceeding its CPU budget.
//
// Each case runs in a child under a watchdog; a child that does not finish in
// time is killed and the case fails as "hung". CPU time spent inside the wait
// is checked too, so a fix that only makes the call return (but still spins
// until its timeout) fails as well.
//
// Build: aarch64-linux-musl-gcc -static -O0 -o pollsp regress_poll_spurious.c
// Run:   ish -f <rootfs> /bin/sh -c 'cat >/tmp/pollsp; chmod +x /tmp/pollsp; /tmp/pollsp' < pollsp
// Exit 0 when every case passes.
#define _GNU_SOURCE
#include <signal.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int pass, fail;
static void check(int ok, const char *what) {
    if (ok) pass++; else { fail++; printf("  FAIL %s\n", what); }
}
static int64_t now_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000000000LL + t.tv_nsec;
}
static int64_t cpu_ns(void) {
    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);
    return ((int64_t)ru.ru_utime.tv_sec + ru.ru_stime.tv_sec) * 1000000000LL
         + ((int64_t)ru.ru_utime.tv_usec + ru.ru_stime.tv_usec) * 1000LL;
}

static const char *TMP = "/tmp/regress_poll_spurious.dat";

static void make_file(void) {
    int fd = open(TMP, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    for (int i = 0; i < 64; i++)
        write(fd, "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcde\n", 64);
    close(fd);
}

// Child exit codes: 0 ok, 1 wrong result, 2 spun (too much CPU).
struct outcome { int code; int64_t wall_ms; };

// Runs `fn` in a child; kills it after `limit_ms`.
static struct outcome run_case(int (*fn)(void), int limit_ms) {
    int64_t t0 = now_ns();
    pid_t pid = fork();
    if (pid == 0) {
        alarm(10);   // belt and braces: never outlive the test
        _exit(fn());
    }
    struct outcome o = { -1, 0 };
    for (;;) {
        int status;
        pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid) {
            o.code = WIFEXITED(status) ? WEXITSTATUS(status) : 100 + WTERMSIG(status);
            break;
        }
        if ((now_ns() - t0) / 1000000 > limit_ms) {
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            o.code = -1;   // hung
            break;
        }
        struct timespec ts = { 0, 5000000 };
        nanosleep(&ts, NULL);
    }
    o.wall_ms = (now_ns() - t0) / 1000000;
    return o;
}

// 1. ppoll(timeout 0) on a regular file with events=0, the way the Rust
//    runtime probes its stdio. Must return 0 at once.
static int case_zero_timeout_file(void) {
    int fd = open(TMP, O_RDONLY);
    struct pollfd p = { fd, 0, 0 };
    struct timespec z = { 0, 0 };
    int r = ppoll(&p, 1, &z, NULL);
    return (r == 0 && p.revents == 0) ? 0 : 1;
}

// 2. The same probe on fds 0-2 with fd 0 redirected from a regular file
//    (`prog < file`): exactly what hung ripgrep.
static int case_rust_stdio_probe(void) {
    int fd = open(TMP, O_RDONLY);
    dup2(fd, 0);
    close(fd);
    struct pollfd p[3] = { {0, 0, 0}, {1, 0, 0}, {2, 0, 0} };
    int r = poll(p, 3, 0);
    for (int i = 0; i < 3; i++)
        if (p[i].revents & POLLNVAL) return 1;
    return r >= 0 ? 0 : 1;
}

// 3. A write-only append fd that has not written yet still has offset 0 < size,
//    so the host reports it readable: `prog >> existing.log`.
static int case_append_only_file(void) {
    int fd = open(TMP, O_WRONLY | O_APPEND);
    struct pollfd p = { fd, 0, 0 };
    int r = poll(&p, 1, 0);
    return r == 0 ? 0 : 1;
}

// 4. A finite timeout that only wants hangups on a regular file: must last
//    about as long as asked and must sleep, not spin, while it waits.
static int case_timed_wait_does_not_spin(void) {
    int fd = open(TMP, O_RDONLY);
    struct pollfd p = { fd, POLLHUP, 0 };
    int64_t c0 = cpu_ns(), t0 = now_ns();
    int r = poll(&p, 1, 300);
    int64_t wall = (now_ns() - t0) / 1000000, cpu = (cpu_ns() - c0) / 1000000;
    printf("    timed wait: r=%d wall=%lldms cpu=%lldms\n", r, (long long)wall, (long long)cpu);
    if (r != 0 || wall < 250) return 1;
    return cpu > 100 ? 2 : 0;
}

// 5. Sanity: things that ARE ready still report at once.
static int case_ready_still_reported(void) {
    int fd = open(TMP, O_RDONLY);
    struct pollfd p = { fd, POLLIN, 0 };
    if (poll(&p, 1, 1000) != 1 || !(p.revents & POLLIN)) return 1;
    int pp[2];
    pipe(pp);
    close(pp[1]);
    struct pollfd q = { pp[0], 0, 0 };
    if (poll(&q, 1, 1000) != 1 || !(q.revents & POLLHUP)) return 1;
    return 0;
}

// 6. Pipes in the shapes a shell pipeline leaves behind: an upstream stage
//    that already exited (data + EOF) and a downstream stage that already
//    closed its end (`| head -1`). Both must answer a zero-timeout probe at
//    once - these pin the host's pipe semantics on each platform.
static int case_pipe_shapes(void) {
    signal(SIGPIPE, SIG_IGN);
    int a[2], b[2];
    pipe(a);
    write(a[1], "line\n", 5);
    close(a[1]);
    pipe(b);
    close(b[0]);
    struct pollfd p[2] = { {a[0], 0, 0}, {b[1], 0, 0} };
    struct timespec z = { 0, 0 };
    int r = ppoll(p, 2, &z, NULL);
    return r >= 0 ? 0 : 1;
}

// 7. A pipe whose upstream filled it before the reader started. xnu clamps
//    a pipe's low water mark to its buffer size, so the host reports a full
//    pipe as readable even to a "hangup only" registration. The reader's
//    probe (events=0, timeout 0) must still return at once - before the fix
//    it never did, so the reader never read and the writer stayed blocked:
//    `head -n 7604 big.py | rg ...` hung at 100% CPU in the field. A timed
//    hangup-only wait on the full pipe must also sleep, not spin.
static int case_full_pipe(void) {
    int p[2];
    pipe(p);
    fcntl(p[1], F_SETFL, O_NONBLOCK);
    char buf[4096];
    memset(buf, 'x', sizeof buf);
    while (write(p[1], buf, sizeof buf) > 0) {}
    struct pollfd q = { p[0], 0, 0 };
    struct timespec z = { 0, 0 };
    if (ppoll(&q, 1, &z, NULL) != 0)
        return 1;
    struct pollfd h = { p[0], POLLHUP, 0 };
    int64_t c0 = cpu_ns(), t0 = now_ns();
    int r = poll(&h, 1, 300);
    int64_t wall = (now_ns() - t0) / 1000000, cpu = (cpu_ns() - c0) / 1000000;
    printf("    full pipe timed wait: r=%d wall=%lldms cpu=%lldms\n", r, (long long)wall, (long long)cpu);
    if (r != 0 || wall < 250) return 1;
    return cpu > 100 ? 2 : 0;
}

static void run(const char *name, int (*fn)(void), int limit_ms) {
    struct outcome o = run_case(fn, limit_ms);
    const char *what = o.code == 0 ? "ok" : o.code == -1 ? "HUNG (killed)" :
                       o.code == 2 ? "SPUN (cpu)" : "wrong result";
    printf("%-34s %s in %lldms\n", name, what, (long long)o.wall_ms);
    char msg[128];
    snprintf(msg, sizeof msg, "%s: %s", name, what);
    check(o.code == 0, msg);
}

int main(void) {
    make_file();
    run("ppoll(file, 0, timeout 0)", case_zero_timeout_file, 3000);
    run("stdio probe with fd 0 = file", case_rust_stdio_probe, 3000);
    run("poll(append-only file, 0, 0)", case_append_only_file, 3000);
    run("poll(file, POLLHUP, 300ms)", case_timed_wait_does_not_spin, 3000);
    run("pipe EOF / reader-closed probe", case_pipe_shapes, 3000);
    run("full pipe probe + timed wait", case_full_pipe, 3000);
    run("ready fds still reported", case_ready_still_reported, 3000);
    unlink(TMP);
    printf("%d passed, %d failed\n", pass, fail);
    return fail ? 1 : 0;
}
