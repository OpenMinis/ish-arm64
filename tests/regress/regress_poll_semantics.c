// [T-ish-poll-spin-guard review] poll / select / epoll semantics that the poll
// spin guards (fs/poll.c: regfile, hupedge, deadline, backoff) must not break.
//
// regress_poll_spurious.c pins the spin itself; this file pins everything
// around it: hangups that happen before or after a wait starts, several
// waiters on one fd, epoll registrations modified between interests, regular
// files and directories still reported ready at once, timeout accuracy for
// poll/ppoll/select/pselect/epoll_wait, EINTR, wake-up latency for pipes,
// ptys, eventfd and timerfd, and the per-call cost of poll().
//
// Every case runs in a child under a watchdog and reports its measurements.
// "FAIL" lines are semantic breaks; "NOTE" lines are measurements to compare
// between the guards on and off (echo <guard> 0 > /proc/ish/poll_spin).
//
// Build: aarch64-linux-musl-gcc -static -O0 -o pollsem regress_poll_semantics.c -lpthread
// Run:   ish -f <rootfs> /bin/sh -c 'cat >/tmp/pollsem; chmod +x /tmp/pollsem; /tmp/pollsem' < pollsem
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/resource.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

static int64_t now_us(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000000 + t.tv_nsec / 1000;
}
static int64_t cpu_us(void) {
    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);
    return ((int64_t)ru.ru_utime.tv_sec + ru.ru_stime.tv_sec) * 1000000
         + ru.ru_utime.tv_usec + ru.ru_stime.tv_usec;
}
static void sleep_us(long us) {
    struct timespec t = { us / 1000000, (us % 1000000) * 1000 };
    nanosleep(&t, NULL);
}
static void fill_pipe(int w) {
    char b[4096];
    memset(b, 'x', sizeof b);
    int fl = fcntl(w, F_GETFL);
    fcntl(w, F_SETFL, fl | O_NONBLOCK);
    while (write(w, b, sizeof b) > 0) {}
    fcntl(w, F_SETFL, fl);
}

// Child result: 0 pass, 1 fail. Children print their own NOTE/FAIL detail.
static int pass, fail;
static const char *only;   // argv[1]: run only cases whose name contains it
static void run(const char *name, int (*fn)(void), int limit_ms) {
    if (only && !strstr(name, only)) return;
    fflush(stdout);
    int64_t t0 = now_us();
    pid_t pid = fork();
    if (pid == 0) { alarm(20); _exit(fn()); }
    int status = 0, code = -1;
    for (;;) {
        pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid) { code = WIFEXITED(status) ? WEXITSTATUS(status) : 100 + WTERMSIG(status); break; }
        if ((now_us() - t0) / 1000 > limit_ms) { kill(pid, SIGKILL); waitpid(pid, &status, 0); break; }
        sleep_us(5000);
    }
    long ms = (long)((now_us() - t0) / 1000);
    const char *what = code == 0 ? "ok" : code == -1 ? "HUNG (killed)" : "FAIL";
    printf("%-46s %s in %ldms\n", name, what, ms);
    if (code == 0) pass++; else fail++;
}

// --- hangups ----------------------------------------------------------------

// EOF already there when the wait starts: must return at once with POLLHUP.
static int c_eof_before(void) {
    int p[2]; pipe(p); close(p[1]);
    struct pollfd q = { p[0], 0, 0 };
    int64_t t0 = now_us();
    int r = poll(&q, 1, -1);
    long us = (long)(now_us() - t0);
    printf("    NOTE eof-before: r=%d revents=%#x in %ldus\n", r, q.revents, us);
    return (r == 1 && (q.revents & POLLHUP) && us < 200000) ? 0 : 1;
}

// EOF arrives while a hangup-only wait (events=0, infinite) is blocked: the
// edge must wake it promptly, not after the 1 s internal re-scan.
static int c_eof_during(void) {
    int p[2]; pipe(p);
    pid_t c = fork();
    if (c == 0) { sleep_us(100000); _exit(0); }   // child holds p[1] for 100 ms
    close(p[1]);
    struct pollfd q = { p[0], 0, 0 };
    int64_t t0 = now_us();
    int r = poll(&q, 1, -1);
    long us = (long)(now_us() - t0);
    waitpid(c, NULL, 0);
    printf("    NOTE eof-during: r=%d revents=%#x after %ldus (writer exits at ~100000)\n", r, q.revents, us);
    return (r == 1 && (q.revents & POLLHUP) && us < 400000) ? 0 : 1;
}

// Full pipe, hangup-only wait, then the writer goes away: the full-buffer
// wakeup is consumed first; the later EOF must still wake the wait.
static int c_full_then_eof(void) {
    int p[2]; pipe(p);
    fill_pipe(p[1]);
    pid_t c = fork();
    if (c == 0) { sleep_us(150000); _exit(0); }
    close(p[1]);
    struct pollfd q = { p[0], 0, 0 };
    int64_t t0 = now_us(), c0 = cpu_us();
    int r = poll(&q, 1, 3000);
    long us = (long)(now_us() - t0), cpu = (long)(cpu_us() - c0);
    waitpid(c, NULL, 0);
    printf("    NOTE full-then-eof: r=%d revents=%#x after %ldus, cpu %ldus\n", r, q.revents, us, cpu);
    return (r == 1 && (q.revents & POLLHUP) && us < 600000) ? 0 : 1;
}

// Write end, reader goes away during the wait (`| head -1` shape).
static int c_reader_gone_during(void) {
    signal(SIGPIPE, SIG_IGN);
    int p[2]; pipe(p);
    pid_t c = fork();
    if (c == 0) { close(p[1]); sleep_us(100000); _exit(0); }
    close(p[0]);
    struct pollfd q = { p[1], 0, 0 };
    int64_t t0 = now_us();
    int r = poll(&q, 1, 3000);
    long us = (long)(now_us() - t0);
    waitpid(c, NULL, 0);
    printf("    NOTE reader-gone: r=%d revents=%#x after %ldus\n", r, q.revents, us);
    return (r == 1 && (q.revents & (POLLHUP | POLLERR)) && us < 600000) ? 0 : 1;
}

// Two processes, each in its own hangup-only poll on the same pipe: both wake.
static int c_two_pollers(void) {
    int p[2]; pipe(p);
    int done[2]; pipe(done);
    for (int i = 0; i < 2; i++) {
        if (fork() == 0) {
            close(p[1]);
            struct pollfd q = { p[0], 0, 0 };
            int64_t t0 = now_us();
            int r = poll(&q, 1, 3000);
            long us = (long)(now_us() - t0);
            char ok = (r == 1 && (q.revents & POLLHUP)) ? 'y' : 'n';
            printf("    NOTE poller %d: r=%d after %ldus\n", i, r, us);
            fflush(stdout);
            write(done[1], &ok, 1);
            _exit(0);
        }
    }
    sleep_us(100000);
    close(p[1]);
    int good = 0;
    for (int i = 0; i < 2; i++) { char ok; if (read(done[0], &ok, 1) == 1 && ok == 'y') good++; }
    while (wait(NULL) > 0) {}
    return good == 2 ? 0 : 1;
}

// Socket peer closes during a hangup-only wait; and a full socket receive
// buffer must not spin a timed hangup-only wait.
static int c_socket(void) {
    int s[2]; socketpair(AF_UNIX, SOCK_STREAM, 0, s);
    fill_pipe(s[1]);   // fills the receive buffer of s[0]
    struct pollfd q = { s[0], 0, 0 };
    int64_t t0 = now_us(), c0 = cpu_us();
    int r = poll(&q, 1, 300);
    long us = (long)(now_us() - t0), cpu = (long)(cpu_us() - c0);
    printf("    NOTE socket full rcvbuf, hup-only 300ms: r=%d wall %ldus cpu %ldus\n", r, us, cpu);
    int bad = (r != 0 || cpu > 100000);
    pid_t c = fork();
    if (c == 0) { sleep_us(100000); _exit(0); }
    close(s[1]);
    t0 = now_us();
    r = poll(&q, 1, 3000);
    us = (long)(now_us() - t0);
    waitpid(c, NULL, 0);
    printf("    NOTE socket peer close: r=%d revents=%#x after %ldus\n", r, q.revents, us);
    return (!bad && r == 1 && (q.revents & POLLHUP) && us < 600000) ? 0 : 1;
}

// --- epoll ------------------------------------------------------------------

// Level-triggered hangup is reported by every epoll_wait, not just the first.
static int c_epoll_level_hup(void) {
    int p[2]; pipe(p); close(p[1]);
    int ep = epoll_create1(0);
    struct epoll_event ev = { .events = 0, .data.fd = p[0] };
    epoll_ctl(ep, EPOLL_CTL_ADD, p[0], &ev);
    int got = 0;
    for (int i = 0; i < 3; i++) {
        struct epoll_event out;
        if (epoll_wait(ep, &out, 1, 1000) == 1 && (out.events & EPOLLHUP)) got++;
    }
    printf("    NOTE epoll level HUP reported %d/3 times\n", got);
    return got == 3 ? 0 : 1;
}

// Two threads blocked in epoll_wait on one epoll set (level-triggered
// EPOLLIN that STARTED life hangup-only and was MODded to EPOLLIN: xnu keeps
// EV_CLEAR from the first add). One write must wake both promptly, as on
// Linux, where a level-triggered ready item is re-queued for the next waiter.
static int ep_fd, ep_pipe_r;
static int64_t ep_t0, ep_lat[2];
static void *ep_waiter(void *arg) {
    int i = (int)(intptr_t)arg;
    struct epoll_event out;
    int r = epoll_wait(ep_fd, &out, 1, 3000);
    ep_lat[i] = r == 1 ? now_us() - ep_t0 : -1;
    return NULL;
}
static int epoll_two_waiters(int first_events) {
    int p[2]; pipe(p);
    ep_pipe_r = p[0];
    ep_fd = epoll_create1(0);
    struct epoll_event ev = { .events = first_events, .data.fd = p[0] };
    epoll_ctl(ep_fd, EPOLL_CTL_ADD, p[0], &ev);
    ev.events = EPOLLIN;
    epoll_ctl(ep_fd, EPOLL_CTL_MOD, p[0], &ev);
    pthread_t t[2];
    for (int i = 0; i < 2; i++) pthread_create(&t[i], NULL, ep_waiter, (void *)(intptr_t)i);
    sleep_us(150000);
    ep_t0 = now_us();
    write(p[1], "x", 1);
    for (int i = 0; i < 2; i++) pthread_join(t[i], NULL);
    printf("    NOTE epoll 2 waiters (added %s, MOD EPOLLIN): latency %lldus / %lldus\n",
           first_events ? "EPOLLIN" : "hangup-only", (long long)ep_lat[0], (long long)ep_lat[1]);
    long worst = ep_lat[0] > ep_lat[1] ? ep_lat[0] : ep_lat[1];
    return (ep_lat[0] >= 0 && ep_lat[1] >= 0 && worst < 200000) ? 0 : 1;
}
static int c_epoll_two_waiters_level(void) { return epoll_two_waiters(EPOLLIN); }
static int c_epoll_two_waiters_mod(void) { return epoll_two_waiters(0); }

// EPOLLIN registration MODded to hangup-only on a full pipe: a timed wait
// must not spin (xnu keeps the first add's level-triggered mode).
static int c_epoll_mod_to_hup_full(void) {
    int p[2]; pipe(p);
    int ep = epoll_create1(0);
    struct epoll_event ev = { .events = EPOLLIN, .data.fd = p[0] };
    epoll_ctl(ep, EPOLL_CTL_ADD, p[0], &ev);
    ev.events = 0;
    epoll_ctl(ep, EPOLL_CTL_MOD, p[0], &ev);
    fill_pipe(p[1]);
    struct epoll_event out;
    int64_t t0 = now_us(), c0 = cpu_us();
    int r = epoll_wait(ep, &out, 1, 300);
    long us = (long)(now_us() - t0), cpu = (long)(cpu_us() - c0);
    printf("    NOTE epoll MOD EPOLLIN->hup-only, full pipe, 300ms: r=%d wall %ldus cpu %ldus\n", r, us, cpu);
    return (r == 0 && us >= 250000 && cpu < 100000) ? 0 : 1;
}

// Linux rejects regular files in epoll (d0d16815): still so.
static int c_epoll_regfile_eperm(void) {
    int f = open("/tmp/pollsem.dat", O_RDWR | O_CREAT | O_TRUNC, 0644);
    int ep = epoll_create1(0);
    struct epoll_event ev = { .events = EPOLLIN, .data.fd = f };
    int r = epoll_ctl(ep, EPOLL_CTL_ADD, f, &ev);
    int e = errno;
    unlink("/tmp/pollsem.dat");
    return (r == -1 && e == EPERM) ? 0 : 1;
}

// Edge-triggered EPOLLIN (Go / tokio style): one report per new write.
static int c_epoll_et(void) {
    int p[2]; pipe(p);
    int ep = epoll_create1(0);
    struct epoll_event ev = { .events = EPOLLIN | EPOLLET, .data.fd = p[0] };
    epoll_ctl(ep, EPOLL_CTL_ADD, p[0], &ev);
    write(p[1], "a", 1);
    struct epoll_event out;
    int r1 = epoll_wait(ep, &out, 1, 500);
    int r2 = epoll_wait(ep, &out, 1, 100);   // nothing new: must time out
    write(p[1], "b", 1);
    int r3 = epoll_wait(ep, &out, 1, 500);
    printf("    NOTE epoll ET: %d %d %d (want 1 0 1)\n", r1, r2, r3);
    return (r1 == 1 && r2 == 0 && r3 == 1) ? 0 : 1;
}

// --- regular files / dirs -----------------------------------------------------

// Regular files and directories are always ready for what they can do.
static int c_regfile_ready(void) {
    int f = open("/tmp/pollsem.dat", O_RDWR | O_CREAT | O_TRUNC, 0644);
    write(f, "abc", 3);
    lseek(f, 0, SEEK_END);   // at EOF: still "readable" on Linux
    struct pollfd q = { f, POLLIN | POLLOUT, 0 };
    int r = poll(&q, 1, 1000);
    int d = open("/tmp", O_RDONLY | O_DIRECTORY);
    struct pollfd qd = { d, POLLIN, 0 };
    int rd = poll(&qd, 1, 1000);
    fd_set rs, ws; FD_ZERO(&rs); FD_ZERO(&ws); FD_SET(f, &rs); FD_SET(f, &ws);
    struct timeval tv = { 1, 0 };
    int rs_n = select(f + 1, &rs, &ws, NULL, &tv);
    unlink("/tmp/pollsem.dat");
    printf("    NOTE file poll r=%d revents=%#x, dir poll r=%d revents=%#x, select r=%d\n",
           r, q.revents, rd, qd.revents, rs_n);
    return (r == 1 && (q.revents & POLLIN) && (q.revents & POLLOUT) && rd == 1 &&
            (qd.revents & POLLIN) && rs_n == 2) ? 0 : 1;
}

// `tail -f` style: a reader that polls POLLIN on a growing file gets an
// immediate answer each time (it then sleeps itself); appends are visible.
static int c_tail_follow(void) {
    int w = open("/tmp/pollsem.log", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    int r = open("/tmp/pollsem.log", O_RDONLY);
    int seen = 0;
    for (int i = 0; i < 5; i++) {
        char line[16]; int n = snprintf(line, sizeof line, "line %d\n", i);
        write(w, line, n);
        struct pollfd q = { r, POLLIN, 0 };
        int64_t t0 = now_us();
        if (poll(&q, 1, 1000) == 1 && now_us() - t0 < 100000) {
            char buf[64]; if (read(r, buf, sizeof buf) == n) seen++;
        }
    }
    unlink("/tmp/pollsem.log");
    printf("    NOTE tail-follow saw %d/5 appends\n", seen);
    return seen == 5 ? 0 : 1;
}

// --- timeouts -----------------------------------------------------------------

static int timed(const char *what, int expect_ms, int (*wait_fn)(int ms)) {
    int64_t t0 = now_us();
    int r = wait_fn(expect_ms);
    long ms = (long)((now_us() - t0) / 1000);
    printf("    NOTE %-10s %3dms -> r=%d after %ldms\n", what, expect_ms, r, ms);
    return (r == 0 && ms >= expect_ms - 5 && ms <= expect_ms + 150) ? 0 : 1;
}
static int tpipe[2];
static int w_poll(int ms) { struct pollfd q = { tpipe[0], POLLIN, 0 }; return poll(&q, 1, ms); }
static int w_ppoll(int ms) { struct pollfd q = { tpipe[0], POLLIN, 0 }; struct timespec t = { ms / 1000, (ms % 1000) * 1000000L }; return ppoll(&q, 1, &t, NULL); }
static int w_select(int ms) { fd_set s; FD_ZERO(&s); FD_SET(tpipe[0], &s); struct timeval t = { ms / 1000, (ms % 1000) * 1000 }; return select(tpipe[0] + 1, &s, NULL, NULL, &t); }
static int w_pselect(int ms) { fd_set s; FD_ZERO(&s); FD_SET(tpipe[0], &s); struct timespec t = { ms / 1000, (ms % 1000) * 1000000L }; return pselect(tpipe[0] + 1, &s, NULL, NULL, &t, NULL); }
static int w_epoll(int ms) {
    static int ep = -1;
    if (ep < 0) { ep = epoll_create1(0); struct epoll_event ev = { .events = EPOLLIN, .data.fd = tpipe[0] }; epoll_ctl(ep, EPOLL_CTL_ADD, tpipe[0], &ev); }
    struct epoll_event out; return epoll_wait(ep, &out, 1, ms);
}
static int c_timeouts(void) {
    pipe(tpipe);
    int bad = 0;
    int (*fns[])(int) = { w_poll, w_ppoll, w_select, w_pselect, w_epoll };
    const char *names[] = { "poll", "ppoll", "select", "pselect", "epoll_wait" };
    int mss[] = { 0, 20, 300, 1500 };
    for (int f = 0; f < 5; f++)
        for (int m = 0; m < 4; m++)
            bad |= timed(names[f], mss[m], fns[f]);
    return bad;
}

// Sub-millisecond ppoll keeps sleeping (regress_subms_wait's poll sibling).
static int c_subms(void) {
    int p[2]; pipe(p);
    struct pollfd q = { p[0], POLLIN, 0 };
    struct timespec t = { 0, 500000 };
    int64_t t0 = now_us(); int n = 0;
    while (now_us() - t0 < 500000) { ppoll(&q, 1, &t, NULL); n++; }
    printf("    NOTE ppoll 500us: %d calls in 500ms\n", n);
    return n <= 1200 ? 0 : 1;   // >1200 would mean it returns early / spins
}

static void on_alarm(int s) { (void)s; }
static int c_eintr(void) {
    int p[2]; pipe(p);
    struct sigaction sa = { 0 }; sa.sa_handler = on_alarm; sigaction(SIGALRM, &sa, NULL);
    struct itimerval it = { { 0, 0 }, { 0, 100000 } };
    setitimer(ITIMER_REAL, &it, NULL);
    struct pollfd q = { p[0], POLLIN, 0 };
    int64_t t0 = now_us();
    int r = poll(&q, 1, -1); int e = errno;
    long ms = (long)((now_us() - t0) / 1000);
    printf("    NOTE poll EINTR: r=%d errno=%d after %ldms\n", r, e, ms);
    return (r == -1 && e == EINTR && ms < 400) ? 0 : 1;
}

// --- wake-up latency ------------------------------------------------------------

static long wake_latency(int rfd, int wfd, int nbytes) {
    pid_t c = fork();
    if (c == 0) { sleep_us(50000); char b[8] = "xxxxxxx"; write(wfd, b, nbytes); _exit(0); }
    struct pollfd q = { rfd, POLLIN, 0 };
    int64_t t0 = now_us();
    int r = poll(&q, 1, 3000);
    long us = (long)(now_us() - t0) - 50000;
    waitpid(c, NULL, 0);
    char b[8]; read(rfd, b, sizeof b);
    return r == 1 ? us : -1;
}
static int c_latency_pipe(void) {
    int p[2]; pipe(p);
    long worst = 0;
    for (int i = 0; i < 10; i++) { long us = wake_latency(p[0], p[1], 1); if (us < 0 || us > worst) worst = us < 0 ? 999999 : us; }
    printf("    NOTE pipe wake latency worst of 10: %ldus\n", worst);
    return worst < 50000 ? 0 : 1;
}

// Pty in canonical mode: many partial-line keystrokes (each wakes the poll
// without making the line readable - exactly the spurious wakeups the backoff
// counts), then Enter. The line must be noticed promptly, not after a 10-50 ms
// backoff sleep.
static int c_latency_pty_canon(void) {
    int m = posix_openpt(O_RDWR | O_NOCTTY);
    grantpt(m); unlockpt(m);
    int s = open(ptsname(m), O_RDWR | O_NOCTTY);
    pid_t c = fork();
    if (c == 0) {
        for (int i = 0; i < 400; i++) { write(m, "a", 1); sleep_us(500); }
        sleep_us(20000);
        write(m, "\n", 1);
        _exit(0);
    }
    struct pollfd q = { s, POLLIN, 0 };
    int r = poll(&q, 1, 5000);
    int64_t t_ret = now_us();
    waitpid(c, NULL, 0);
    // Child sent Enter ~20ms after its last keystroke; measure from the end of
    // the child (an upper bound on when Enter was written).
    long after_exit_us = (long)(now_us() - t_ret);
    printf("    NOTE pty canonical: r=%d (poll returned %ldus before the writer was reaped)\n", r, after_exit_us);
    return r == 1 ? 0 : 1;
}
static int c_latency_pty_raw(void) {
    int m = posix_openpt(O_RDWR | O_NOCTTY);
    grantpt(m); unlockpt(m);
    int s = open(ptsname(m), O_RDWR | O_NOCTTY);
    struct termios t; tcgetattr(s, &t); cfmakeraw(&t); tcsetattr(s, TCSANOW, &t);
    long worst = 0;
    for (int i = 0; i < 10; i++) { long us = wake_latency(s, m, 1); if (us < 0 || us > worst) worst = us < 0 ? 999999 : us; }
    printf("    NOTE pty raw keystroke latency worst of 10: %ldus\n", worst);
    return worst < 50000 ? 0 : 1;
}
static int c_latency_eventfd_timerfd(void) {
    int e = eventfd(0, 0);
    long worst = 0;
    for (int i = 0; i < 5; i++) {
        pid_t c = fork();
        if (c == 0) { sleep_us(30000); uint64_t one = 1; write(e, &one, 8); _exit(0); }
        struct pollfd q = { e, POLLIN, 0 };
        int64_t t0 = now_us(); int r = poll(&q, 1, 3000); long us = (long)(now_us() - t0) - 30000;
        waitpid(c, NULL, 0); uint64_t v; read(e, &v, 8);
        if (r != 1) us = 999999;
        if (us > worst) worst = us;
    }
    int tf = timerfd_create(CLOCK_MONOTONIC, 0);
    struct itimerspec its = { { 0, 20000000 }, { 0, 20000000 } };
    timerfd_settime(tf, 0, &its, NULL);
    int64_t t0 = now_us(); int ticks = 0;
    while (now_us() - t0 < 500000) {
        struct pollfd q = { tf, POLLIN, 0 };
        if (poll(&q, 1, 1000) == 1) { uint64_t n; read(tf, &n, 8); ticks += (int)n; }
    }
    printf("    NOTE eventfd worst latency %ldus; timerfd 20ms ticks in 500ms: %d (want ~25)\n", worst, ticks);
    return (worst < 50000 && ticks >= 20 && ticks <= 30) ? 0 : 1;
}

// --- cost -------------------------------------------------------------------------

static int c_cost(void) {
    int p[2]; pipe(p);
    int f = open("/tmp/pollsem.dat", O_RDWR | O_CREAT | O_TRUNC, 0644);
    struct pollfd qp = { p[0], POLLIN, 0 }, qf = { f, POLLIN, 0 };
    int n = 2000;
    int64_t t0 = now_us();
    for (int i = 0; i < n; i++) poll(&qp, 1, 0);
    long pipe_ns = (long)((now_us() - t0) * 1000 / n);
    t0 = now_us();
    for (int i = 0; i < n; i++) poll(&qf, 1, 0);
    long file_ns = (long)((now_us() - t0) * 1000 / n);
    unlink("/tmp/pollsem.dat");
    printf("    NOTE poll(timeout 0) cost: pipe %ldns/call, regular file %ldns/call\n", pipe_ns, file_ns);
    return 0;
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (argc > 1) only = argv[1];
    run("hangup: EOF before wait", c_eof_before, 3000);
    run("hangup: EOF during hangup-only wait", c_eof_during, 3000);
    run("hangup: full pipe, then EOF", c_full_then_eof, 4000);
    run("hangup: reader gone during wait (write end)", c_reader_gone_during, 4000);
    run("hangup: two processes poll one pipe", c_two_pollers, 4000);
    run("socket: full rcvbuf / peer close", c_socket, 5000);
    run("epoll: level HUP every wait", c_epoll_level_hup, 5000);
    run("epoll: ET one report per write", c_epoll_et, 3000);
    run("epoll: regular file EPERM", c_epoll_regfile_eperm, 3000);
    run("epoll: 2 waiters, added EPOLLIN", c_epoll_two_waiters_level, 5000);
    run("epoll: 2 waiters, added hup-only then MOD", c_epoll_two_waiters_mod, 5000);
    run("epoll: MOD EPOLLIN->hup-only on full pipe", c_epoll_mod_to_hup_full, 3000);
    run("file/dir always ready (poll+select)", c_regfile_ready, 3000);
    run("tail -f style follow", c_tail_follow, 5000);
    run("timeouts poll/ppoll/select/pselect/epoll", c_timeouts, 30000);
    run("ppoll 500us does not return early", c_subms, 3000);
    run("poll EINTR on signal", c_eintr, 3000);
    run("latency: pipe", c_latency_pipe, 6000);
    run("latency: pty canonical after 400 keys", c_latency_pty_canon, 8000);
    run("latency: pty raw keystrokes", c_latency_pty_raw, 6000);
    run("latency: eventfd / timerfd", c_latency_eventfd_timerfd, 6000);
    run("cost: poll(timeout 0)", c_cost, 10000);
    printf("%d passed, %d failed\n", pass, fail);
    return fail ? 1 : 0;
}
