// Signals against blocking calls, the way Linux does them. Pins da7162ba
// (a blocking host read/write gives up on EINTR when a guest signal is
// pending) and everything around it:
//   1  SA_RESTART handler: a blocked pipe read restarts and gets the data
//   2  handler without SA_RESTART: the read fails with EINTR
//   3  SIGCHLD with and without a handler, against read and waitpid
//   4  alarm()/SIGALRM against a blocked read
//   5  ^C on a pty against a read of the pty and of a pipe
//   6  SIGTERM and SIGKILL end a process blocked reading a pipe, threads too
//   7  a write blocked on a full pipe: EINTR, restart, and a partial count
//   8  poll/ppoll/select/pselect/epoll_wait/nanosleep/futex/sigsuspend/waitpid
//   9  signals that must not show as EINTR: default-ignored, SIG_IGN,
//      blocked, SIGSTOP/SIGCONT, a signal handled by another thread, and
//      (9f) none at all: run it with the host bombarding iSH with SIGUSR1,
//      the wake-up iSH uses for guest signals, so the host read sees EINTR
//      with no guest signal behind it and must retry without the caller
//      noticing (see run.sh)
//
// The output carries only portable facts (return values, errno names, data,
// handler counts, exit statuses, "took at least N ms"), so it must match a
// real arm64 Linux line for line:
//   aarch64-linux-musl-gcc -static -O1 -pthread -o sigeintr regress_signal_eintr.c
//   <run on arm64 Alpine> > native.txt; ish -r <rootfs> /tmp/sigeintr > ish.txt
//   diff native.txt ish.txt
// Every case runs in its own child under a watchdog, so a hang prints
// TIMEOUT instead of stopping the run. Exit status is 0 unless a case hung.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/futex.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define GAP_MS 300   // a helper waits this long so the caller is blocked first

static volatile sig_atomic_t hits;
static void on_signal(int sig) { (void) sig; hits++; }

static long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
static void sleep_ms(long ms) {
    struct timespec ts = {ms / 1000, (ms % 1000) * 1000000};
    while (nanosleep(&ts, &ts) < 0 && errno == EINTR) {}
}
static void handle(int sig, int flags) {
    struct sigaction sa = {0};
    sa.sa_handler = on_signal;
    sa.sa_flags = flags;
    sigemptyset(&sa.sa_mask);
    sigaction(sig, &sa, NULL);
}
static void set_disposition(int sig, void (*h)(int)) {
    struct sigaction sa = {0};
    sa.sa_handler = h;
    sigemptyset(&sa.sa_mask);
    sigaction(sig, &sa, NULL);
}
static const char *err(long r) {
    if (r >= 0) return "ok";
    switch (errno) {
        case EINTR: return "EINTR";
        case EAGAIN: return "EAGAIN";
        case ECHILD: return "ECHILD";
        case ETIMEDOUT: return "ETIMEDOUT";
        default: return strerror(errno);
    }
}
static void out(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    putchar('\n');
    fflush(stdout);
}
static const char *status_str(int st) {
    static char buf[32];
    if (WIFEXITED(st)) snprintf(buf, sizeof buf, "exit %d", WEXITSTATUS(st));
    else if (WIFSIGNALED(st)) snprintf(buf, sizeof buf, "signal %d", WTERMSIG(st));
    else snprintf(buf, sizeof buf, "status %#x", st);
    return buf;
}

// A helper process: after `signal_at` ms send `sig` to `target` (0: none),
// after `write_at` ms write "data" to `wfd` (-1: none), then exit.
static pid_t helper(pid_t target, int sig, long signal_at, int wfd, long write_at) {
    pid_t p = fork();
    if (p != 0) return p;
    long t0 = now_ms();
    if (sig) { sleep_ms(signal_at); kill(target, sig); }
    if (wfd >= 0) {
        long left = write_at - (now_ms() - t0);
        if (left > 0) sleep_ms(left);
        write(wfd, "data", 4);
    }
    _exit(0);
}
static void reap(pid_t p) { int st; while (waitpid(p, &st, 0) < 0 && errno == EINTR) {} }

// read with a 4-byte buffer, report "read=<n> <err> <data>"
static const char *read_report(int fd) {
    static char line[64];
    char buf[8] = {0};
    ssize_t n = read(fd, buf, 4);
    snprintf(line, sizeof line, "read=%zd %s%s%s", n, err(n), n > 0 ? " " : "", n > 0 ? buf : "");
    return line;
}

// ---- 1, 2: a blocked read with and without SA_RESTART ----
static void case_read_handler(int restart) {
    int p[2]; pipe(p);
    hits = 0;
    handle(SIGUSR1, restart ? SA_RESTART : 0);
    pid_t h = helper(getpid(), SIGUSR1, GAP_MS, p[1], 2 * GAP_MS);
    const char *r = read_report(p[0]);   // before hits: argument order is unspecified
    out("%s handler: %s, handler ran %d", restart ? "SA_RESTART" : "plain", r, hits);
    if (!restart) out("  then %s", read_report(p[0]));
    reap(h);
}
static void c1(void) { case_read_handler(1); }
static void c2(void) { case_read_handler(0); }

// ---- 3: SIGCHLD ----
static void case_sigchld_read(int mode) {   // 0 default, 1 handler, 2 handler+SA_RESTART
    int p[2]; pipe(p);
    hits = 0;
    if (mode == 0) set_disposition(SIGCHLD, SIG_DFL);
    else handle(SIGCHLD, mode == 2 ? SA_RESTART : 0);
    pid_t c = fork();
    if (c == 0) { sleep_ms(GAP_MS); _exit(7); }
    pid_t w = helper(0, 0, 0, p[1], 2 * GAP_MS);
    const char *r = read_report(p[0]);
    out("SIGCHLD %s: %s", mode == 0 ? "default" : mode == 1 ? "handler" : "handler+SA_RESTART", r);
    if (mode == 1) out("  then %s", read_report(p[0]));
    int st;
    pid_t got;
    while ((got = waitpid(c, &st, 0)) < 0 && errno == EINTR) {}
    out("  waitpid child: %s, handler ran %s", got == c ? status_str(st) : err(got), hits ? ">=1" : "0");
    reap(w);
}
static void c3a(void) { case_sigchld_read(0); }
static void c3b(void) { case_sigchld_read(1); }
static void c3c(void) { case_sigchld_read(2); }
static void case_waitpid_signal(int restart) {
    hits = 0;
    handle(SIGUSR1, restart ? SA_RESTART : 0);
    pid_t c = fork();
    if (c == 0) { sleep_ms(2 * GAP_MS); _exit(3); }
    pid_t h = helper(getpid(), SIGUSR1, GAP_MS, -1, 0);
    int st;
    pid_t got = waitpid(c, &st, 0);
    out("waitpid with %s SIGUSR1: %s, handler ran %d", restart ? "SA_RESTART" : "plain",
        got == c ? status_str(st) : err(got), hits);
    if (got != c) { got = waitpid(c, &st, 0); out("  then %s", got == c ? status_str(st) : err(got)); }
    reap(h);
}
static void c3d(void) { case_waitpid_signal(0); }
static void c3e(void) { case_waitpid_signal(1); }

// ---- 4: alarm ----
static void case_alarm(int restart) {
    int p[2]; pipe(p);
    hits = 0;
    handle(SIGALRM, restart ? SA_RESTART : 0);
    pid_t w = helper(0, 0, 0, p[1], 1500);
    long t0 = now_ms();
    alarm(1);
    const char *r = read_report(p[0]);
    long took = now_ms() - t0;
    out("alarm(1) %s: %s, handler ran %d, took >=900ms %s", restart ? "SA_RESTART" : "plain", r, hits,
        took >= 900 ? "yes" : "no");
    if (!restart) out("  then %s", read_report(p[0]));
    reap(w);
}
static void c4a(void) { case_alarm(0); }
static void c4b(void) { case_alarm(1); }

// ---- 5: ^C on a pty ----
// The child becomes a session leader with the pty as its controlling
// terminal, then reads the pty (what) = 0 or a pipe (what) = 1, with SIGINT
// at default (handler = 0) or caught without SA_RESTART (handler = 1).
static void case_pty_intr(int what, int handler) {
    int m = posix_openpt(O_RDWR | O_NOCTTY);
    if (m < 0 || grantpt(m) || unlockpt(m)) { out("pty: posix_openpt failed: %s", strerror(errno)); return; }
    char *name = ptsname(m);
    int p[2]; pipe(p);
    pid_t k = fork();
    if (k == 0) {
        setsid();
        int s = open(name, O_RDWR);
        ioctl(s, TIOCSCTTY, 0);
        if (handler) handle(SIGINT, 0); else set_disposition(SIGINT, SIG_DFL);
        char buf[8];
        ssize_t n = read(what ? p[0] : s, buf, sizeof buf);
        _exit(n < 0 && errno == EINTR ? 40 : n >= 0 ? 41 : 42);
    }
    sleep_ms(GAP_MS);
    write(m, "\x03", 1);
    int st;
    long t0 = now_ms();
    pid_t got;
    while ((got = waitpid(k, &st, WNOHANG)) == 0 && now_ms() - t0 < 5000) sleep_ms(20);
    if (got == 0) { kill(k, SIGKILL); waitpid(k, &st, 0); out("^C, %s read, %s: not interrupted", what ? "pipe" : "pty", handler ? "handler" : "default"); }
    else out("^C, %s read, SIGINT %s: child %s%s", what ? "pipe" : "pty", handler ? "handler" : "default", status_str(st),
             WIFEXITED(st) && WEXITSTATUS(st) == 40 ? " (read EINTR)" : "");
    close(m);
}
static void c5a(void) { case_pty_intr(0, 0); }
static void c5b(void) { case_pty_intr(0, 1); }
static void c5c(void) { case_pty_intr(1, 0); }
static void c5d(void) { case_pty_intr(1, 1); }

// ---- 6: SIGTERM / SIGKILL end a process blocked reading a pipe ----
static int kill_pipe[2];
static void *block_read(void *a) { char c; (void) a; read(kill_pipe[0], &c, 1); return NULL; }
static void case_kill(int sig, int threads) {
    pipe(kill_pipe);
    pid_t k = fork();
    if (k == 0) {
        for (int i = 1; i < threads; i++) { pthread_t t; pthread_create(&t, NULL, block_read, NULL); }
        block_read(NULL);
        _exit(0);
    }
    sleep_ms(GAP_MS);
    long t0 = now_ms();
    kill(k, sig);
    int st;
    pid_t got;
    while ((got = waitpid(k, &st, WNOHANG)) == 0 && now_ms() - t0 < 5000) sleep_ms(20);
    if (got == 0) { kill(k, SIGKILL); waitpid(k, &st, 0); out("kill -%d, %d thread(s) blocked in read: still alive after 5 s", sig, threads); }
    else out("kill -%d, %d thread(s) blocked in read: %s", sig, threads, status_str(st));
    close(kill_pipe[0]); close(kill_pipe[1]);
}
static void c6a(void) { case_kill(SIGTERM, 1); }
static void c6b(void) { case_kill(SIGKILL, 1); }
static void c6c(void) { case_kill(SIGTERM, 3); }
static void c6d(void) { case_kill(SIGKILL, 3); }

// ---- 7: a write blocked on a full pipe ----
static int fill_pipe(int fd) {
    int fl = fcntl(fd, F_GETFL);
    fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    static char chunk[4096];
    int total = 0;
    ssize_t n;
    while ((n = write(fd, chunk, sizeof chunk)) > 0) total += n;
    fcntl(fd, F_SETFL, fl);
    return total;
}
static void case_write_full(int restart) {
    int p[2]; pipe(p);
    hits = 0;
    handle(SIGUSR1, restart ? SA_RESTART : 0);
    fill_pipe(p[1]);
    pid_t me = getpid();
    pid_t d = fork();
    if (d == 0) {   // signal, then (SA_RESTART) drain so the restarted write can finish
        close(p[1]);
        sleep_ms(GAP_MS);
        kill(me, SIGUSR1);
        if (restart) { sleep_ms(GAP_MS); char buf[4096]; while (read(p[0], buf, sizeof buf) > 0) {} }
        _exit(0);
    }
    static char chunk[4096];
    ssize_t n = write(p[1], chunk, sizeof chunk);
    out("write 4096 to a full pipe, %s handler: write=%zd %s, handler ran %d", restart ? "SA_RESTART" : "plain", n, err(n), hits);
    close(p[1]);
    close(p[0]);
    reap(d);
}
static void c7a(void) { case_write_full(0); }
static void c7b(void) { case_write_full(1); }
static void c7c(void) {   // interrupted after part of a large write went through
    int p[2]; pipe(p);
    hits = 0;
    handle(SIGUSR1, 0);
    pid_t me = getpid();
    pid_t d = fork();
    if (d == 0) {   // take 64 KB, then signal while the writer is blocked again
        close(p[1]);
        sleep_ms(GAP_MS);
        char buf[4096];
        for (int got = 0; got < 65536;) { ssize_t r = read(p[0], buf, sizeof buf); if (r <= 0) break; got += r; }
        sleep_ms(GAP_MS);
        kill(me, SIGUSR1);
        while (read(p[0], buf, sizeof buf) > 0) {}
        _exit(0);
    }
    close(p[0]);
    size_t len = 4 << 20;
    char *big = calloc(1, len);
    ssize_t n = write(p[1], big, len);
    out("write 4 MB, interrupted midway: %s, handler ran %d",
        n > 0 && (size_t) n < len ? "partial count" : n == (ssize_t) len ? "all" : err(n), hits);
    close(p[1]);
    reap(d);
    free(big);
}

// ---- 8: other blocking calls ----
static pthread_t main_thread;
static int *futex_word;
static void *poke_main(void *a) {   // signal the main thread, and maybe wake the futex later
    long wake = (long) a;
    sleep_ms(GAP_MS);
    pthread_kill(main_thread, SIGUSR1);
    if (wake) {
        sleep_ms(GAP_MS);
        __atomic_store_n(futex_word, 1, __ATOMIC_SEQ_CST);
        syscall(SYS_futex, futex_word, FUTEX_WAKE_PRIVATE, 1, NULL, NULL, 0);
    }
    return NULL;
}
static pthread_t start_poke(long wake) {
    pthread_t t;
    main_thread = pthread_self();
    pthread_create(&t, NULL, poke_main, (void *) wake);
    return t;
}
static void case_others(int restart) {
    const char *tag = restart ? "SA_RESTART" : "plain";
    handle(SIGUSR1, restart ? SA_RESTART : 0);
    int p[2]; pipe(p);
    pthread_t t;
    long r;

    t = start_poke(0); hits = 0;
    struct pollfd pfd = {p[0], POLLIN, 0};
    r = poll(&pfd, 1, -1);
    out("poll(-1) %s: %ld %s, handler ran %d", tag, r, err(r), hits); pthread_join(t, NULL);

    t = start_poke(0); hits = 0;
    struct timespec ten = {10, 0};
    r = ppoll(&pfd, 1, &ten, NULL);
    out("ppoll(10s) %s: %ld %s, handler ran %d", tag, r, err(r), hits); pthread_join(t, NULL);

    t = start_poke(0); hits = 0;
    fd_set rs; FD_ZERO(&rs); FD_SET(p[0], &rs);
    r = select(p[0] + 1, &rs, NULL, NULL, NULL);
    out("select(NULL) %s: %ld %s, handler ran %d", tag, r, err(r), hits); pthread_join(t, NULL);

    t = start_poke(0); hits = 0;
    FD_ZERO(&rs); FD_SET(p[0], &rs);
    r = pselect(p[0] + 1, &rs, NULL, NULL, &ten, NULL);
    out("pselect(10s) %s: %ld %s, handler ran %d", tag, r, err(r), hits); pthread_join(t, NULL);

    int ep = epoll_create1(0);
    struct epoll_event ev = {.events = EPOLLIN, .data.fd = p[0]};
    epoll_ctl(ep, EPOLL_CTL_ADD, p[0], &ev);
    t = start_poke(0); hits = 0;
    r = epoll_wait(ep, &ev, 1, -1);
    out("epoll_wait(-1) %s: %ld %s, handler ran %d", tag, r, err(r), hits); pthread_join(t, NULL);
    close(ep);

    t = start_poke(0); hits = 0;
    struct timespec two = {2, 0}, rem = {0, 0};
    r = nanosleep(&two, &rem);
    out("nanosleep(2s) %s: %ld %s, remaining > 0: %s, handler ran %d", tag, r, err(r),
        rem.tv_sec > 0 || rem.tv_nsec > 0 ? "yes" : "no", hits); pthread_join(t, NULL);

    t = start_poke(0); hits = 0;
    r = clock_nanosleep(CLOCK_MONOTONIC, 0, &two, &rem);
    out("clock_nanosleep(2s) %s: %s, handler ran %d", tag, r == 0 ? "ok" : r == EINTR ? "EINTR" : strerror(r), hits);
    pthread_join(t, NULL);

    futex_word = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    *futex_word = 0;
    t = start_poke(1); hits = 0;
    r = syscall(SYS_futex, futex_word, FUTEX_WAIT_PRIVATE, 0, NULL, NULL, 0);
    out("futex wait (no timeout) %s: %ld %s, handler ran %d", tag, r, err(r), hits); pthread_join(t, NULL);
    *futex_word = 0;
    t = start_poke(0); hits = 0;
    r = syscall(SYS_futex, futex_word, FUTEX_WAIT_PRIVATE, 0, &two, NULL, 0);
    out("futex wait (2s timeout) %s: %ld %s, handler ran %d", tag, r, err(r), hits); pthread_join(t, NULL);
    munmap(futex_word, 4096);

    t = start_poke(0); hits = 0;
    sigset_t none; sigemptyset(&none);
    r = sigsuspend(&none);
    out("sigsuspend %s: %ld %s, handler ran %d", tag, r, err(r), hits); pthread_join(t, NULL);

    close(p[0]); close(p[1]);
}
static void c8a(void) { case_others(0); }
static void c8b(void) { case_others(1); }

// ---- 9: signals that must not interrupt a blocked read ----
static void *other_thread_wait(void *a) { (void) a; sleep_ms(4 * GAP_MS); return NULL; }
static void case_quiet(int kind) {
    int p[2]; pipe(p);
    hits = 0;
    const char *what = "";
    pid_t me = getpid();
    pid_t h = -1;
    pthread_t other;
    switch (kind) {
    case 0:   // default-ignored signals
        what = "SIGWINCH+SIGURG+SIGCHLD at default";
        set_disposition(SIGCHLD, SIG_DFL);
        if (fork() == 0) { sleep_ms(GAP_MS); _exit(0); }   // our child: SIGCHLD to us
        h = fork();
        if (h == 0) { sleep_ms(GAP_MS); kill(me, SIGWINCH); kill(me, SIGURG); sleep_ms(GAP_MS); write(p[1], "data", 4); _exit(0); }
        break;
    case 1:
        what = "SIGUSR1 ignored (SIG_IGN)";
        set_disposition(SIGUSR1, SIG_IGN);
        h = helper(me, SIGUSR1, GAP_MS, p[1], 2 * GAP_MS);
        break;
    case 2: {
        what = "SIGUSR1 blocked";
        handle(SIGUSR1, 0);
        sigset_t s; sigemptyset(&s); sigaddset(&s, SIGUSR1);
        sigprocmask(SIG_BLOCK, &s, NULL);
        h = helper(me, SIGUSR1, GAP_MS, p[1], 2 * GAP_MS);
        break;
    }
    case 3:   // stopped and continued while blocked
        what = "SIGSTOP then SIGCONT";
        h = fork();
        if (h == 0) { sleep_ms(GAP_MS); kill(me, SIGSTOP); sleep_ms(GAP_MS); kill(me, SIGCONT); sleep_ms(GAP_MS); write(p[1], "data", 4); _exit(0); }
        break;
    case 4: {   // a handled signal that another thread takes
        what = "SIGUSR1 handled by another thread";
        handle(SIGUSR1, 0);
        sigset_t s; sigemptyset(&s); sigaddset(&s, SIGUSR1);
        pthread_create(&other, NULL, other_thread_wait, NULL);   // inherits the mask before blocking
        sigprocmask(SIG_BLOCK, &s, NULL);
        h = helper(me, SIGUSR1, GAP_MS, p[1], 2 * GAP_MS);
        break;
    }
    }
    const char *r = read_report(p[0]);
    if (kind == 2) {
        sigset_t pend; sigpending(&pend);
        out("%s: %s, still pending: %s", what, r, sigismember(&pend, SIGUSR1) ? "yes" : "no");
    } else if (kind == 4) {
        pthread_join(other, NULL);
        out("%s: %s, handler ran %d", what, r, hits);
    } else {
        out("%s: %s", what, r);
    }
    reap(h);
}
static void c9a(void) { case_quiet(0); }
static void c9b(void) { case_quiet(1); }
static void c9c(void) { case_quiet(2); }
static void c9d(void) { case_quiet(3); }
static void c9e(void) { case_quiet(4); }
static void c9f(void) {
    int p[2]; pipe(p);
    pid_t w = helper(0, 0, 0, p[1], 2000);
    long t0 = now_ms();
    const char *r = read_report(p[0]);
    out("no signal, data after 2 s: %s, took >=1900ms %s", r, now_ms() - t0 >= 1900 ? "yes" : "no");
    reap(w);
}

static const struct { const char *id; void (*fn)(void); } cases[] = {
    {"1", c1}, {"2", c2},
    {"3a", c3a}, {"3b", c3b}, {"3c", c3c}, {"3d", c3d}, {"3e", c3e},
    {"4a", c4a}, {"4b", c4b},
    {"5a", c5a}, {"5b", c5b}, {"5c", c5c}, {"5d", c5d},
    {"6a", c6a}, {"6b", c6b}, {"6c", c6c}, {"6d", c6d},
    {"7a", c7a}, {"7b", c7b}, {"7c", c7c},
    {"8a", c8a}, {"8b", c8b},
    {"9a", c9a}, {"9b", c9b}, {"9c", c9c}, {"9d", c9d}, {"9e", c9e}, {"9f", c9f},
};

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc > 2 && strcmp(argv[1], "-n") == 0) {   // one case, in this process (no watchdog)
        for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; i++)
            if (strcmp(argv[2], cases[i].id) == 0) { printf("%-3s ", cases[i].id); cases[i].fn(); }
        return 0;
    }
    int hung = 0;
    for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        if (argc > 1 && strcmp(argv[1], cases[i].id) != 0)
            continue;
        printf("%-3s ", cases[i].id);
        pid_t c = fork();
        if (c == 0) {
            setpgid(0, 0);
            cases[i].fn();
            _exit(0);
        }
        long t0 = now_ms();
        int st;
        pid_t got;
        while ((got = waitpid(c, &st, WNOHANG)) == 0 && now_ms() - t0 < 30000) sleep_ms(20);
        if (got == 0) {
            kill(-c, SIGKILL);
            waitpid(c, &st, 0);
            out("TIMEOUT");
            hung = 1;
        } else if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) {
            out("case died: %s", status_str(st));
        }
    }
    return hung;
}
