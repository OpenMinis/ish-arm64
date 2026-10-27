// Regression tests for the fs/wait syscall fixes landed on fix/ish-issue-batch.
//
// Each block below pins the observable behaviour of one bug so a future change
// that reintroduces it fails here instead of in a user's shell. The comment on
// each block records what the bug actually looked like before the fix.
//
// Build for the guest (static, so no rootfs libc is required):
//   aarch64-linux-musl-gcc -static -O0 -o regress_syscall regress_syscall.c
// Run inside iSH:
//   ish -r <rootfs> /tmp/regress_syscall
//
// Exit status is 0 when every case passes, 1 otherwise. See tests/regress/run.sh.

#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int pass, fail;

static void check(int ok, const char *what) {
    if (ok) {
        pass++;
        printf("  PASS  %s\n", what);
    } else {
        fail++;
        printf("  FAIL  %s   (errno=%d %s)\n", what, errno, strerror(errno));
    }
}

static void section(const char *title) {
    printf("\n=== %s ===\n", title);
}

static double now_sec(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

static void nap(double seconds) {
    struct timespec t = {(time_t)seconds, (long)((seconds - (int)seconds) * 1e9)};
    nanosleep(&t, NULL);
}

// Burn CPU rather than sleeping, so the child is genuinely running (this is
// what cc1 does, and it is the shape that triggered the waitpid bug).
static void burn(double seconds) {
    double t0 = now_sec();
    volatile unsigned x = 0;
    while (now_sec() - t0 < seconds)
        for (int i = 0; i < 200000; i++)
            x += i;
}

// ---------------------------------------------------------------------------
// #30 — O_DIRECTORY carried the x86 value (1<<16) instead of aarch64's 0x4000,
// so the ENOTDIR check in generic_openat() never fired for any guest, and
// 1<<16 (arm64's O_DIRECT) was misread as O_DIRECTORY. GNU cp probes its
// destination with open(dst, O_PATH|O_DIRECTORY); the bogus success made it
// treat an existing regular file as a directory and fail with
// "cannot create regular file '<dst>/<src>': Not a directory".
// ---------------------------------------------------------------------------
static void test_o_directory(const char *file, const char *dir) {
    section("#30 O_DIRECTORY flag value");

    errno = 0;
    int fd = open(file, O_RDONLY | O_DIRECTORY);
    check(fd < 0 && errno == ENOTDIR, "open(regular file, O_DIRECTORY) -> ENOTDIR");
    if (fd >= 0)
        close(fd);

    errno = 0;
    fd = open(file, O_PATH | O_DIRECTORY);
    check(fd < 0 && errno == ENOTDIR, "open(regular file, O_PATH|O_DIRECTORY) -> ENOTDIR");
    if (fd >= 0)
        close(fd);

    // Guard the other direction: the fix must not break real directories.
    fd = open(dir, O_RDONLY | O_DIRECTORY);
    check(fd >= 0, "open(directory, O_DIRECTORY) still succeeds");
    if (fd >= 0)
        close(fd);

    DIR *dp = opendir(dir);
    check(dp != NULL, "opendir(directory) still succeeds");
    if (dp != NULL) {
        int entries = 0;
        while (readdir(dp) != NULL)
            entries++;
        check(entries > 0, "readdir() returns entries");
        closedir(dp);
    }

    errno = 0;
    fd = open(dir, O_WRONLY);
    check(fd < 0 && errno == EISDIR, "open(directory, O_WRONLY) -> EISDIR");
    if (fd >= 0)
        close(fd);

    // 1<<16 is O_DIRECT on aarch64. It must not be mistaken for O_DIRECTORY.
    errno = 0;
    fd = open(file, O_RDONLY | O_DIRECT);
    check(fd >= 0, "open(regular file, O_DIRECT) succeeds (not read as O_DIRECTORY)");
    if (fd >= 0)
        close(fd);

    check(O_DIRECTORY == 0x4000, "guest O_DIRECTORY == 0x4000 (aarch64 ABI)");
    check((O_DIRECTORY & O_DIRECT) == 0, "O_DIRECTORY does not overlap O_DIRECT");
    check((O_DIRECTORY & O_NOFOLLOW) == 0, "O_DIRECTORY does not overlap O_NOFOLLOW");
    check((O_DIRECTORY & O_PATH) == 0, "O_DIRECTORY does not overlap O_PATH");
}

// ---------------------------------------------------------------------------
// #30 follow-up — O_NOFOLLOW was translated to the host open(), but
// generic_openat() resolves the final path component itself before the
// filesystem's open() runs, so the host flag was a no-op and
// open(symlink, O_NOFOLLOW) wrongly returned an fd to the link target.
// ---------------------------------------------------------------------------
static void test_o_nofollow(const char *file, const char *link) {
    section("#30 O_NOFOLLOW honored during path resolution");

    unlink(link);
    if (symlink(file, link) != 0) {
        printf("  SKIP  cannot create symlink %s: %s\n", link, strerror(errno));
        return;
    }

    errno = 0;
    int fd = open(link, O_RDONLY | O_NOFOLLOW);
    check(fd < 0 && errno == ELOOP, "open(symlink, O_NOFOLLOW) -> ELOOP");
    if (fd >= 0)
        close(fd);

    fd = open(file, O_RDONLY | O_NOFOLLOW);
    check(fd >= 0, "open(regular file, O_NOFOLLOW) still succeeds");
    if (fd >= 0)
        close(fd);

    // Without O_NOFOLLOW the symlink must still resolve.
    fd = open(link, O_RDONLY);
    check(fd >= 0, "open(symlink) without O_NOFOLLOW still follows");
    if (fd >= 0)
        close(fd);

    unlink(link);
}

// ---------------------------------------------------------------------------
// #31 — fakefs_stat()'s hook-routed branch memset the statbuf and then filled
// fields by hand, but never set dev (nor nlink/blksize). fstat() went through
// realfs/copy_stat and did set it, so stat() and fstat() on one file reported
// the same inode with different st_dev. GNU coreutils' psame_inode() reads
// that as "the file was replaced while being copied" and cp refuses to copy.
//
// Runs against whatever path it is given, so it is meaningful on a plain
// rootfs and on a fakefs mount point alike.
// ---------------------------------------------------------------------------
static void test_stat_fstat_agreement(const char *path) {
    section("#31 stat/fstat agreement (psame_inode)");
    printf("  path: %s\n", path);

    struct stat s, f;
    if (stat(path, &s) != 0) {
        printf("  SKIP  stat(%s): %s\n", path, strerror(errno));
        return;
    }
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        printf("  SKIP  open(%s): %s\n", path, strerror(errno));
        return;
    }
    if (fstat(fd, &f) != 0) {
        printf("  SKIP  fstat(%s): %s\n", path, strerror(errno));
        close(fd);
        return;
    }

    check(s.st_dev == f.st_dev, "stat.st_dev == fstat.st_dev");
    check(s.st_ino == f.st_ino, "stat.st_ino == fstat.st_ino");
    check(s.st_dev != 0, "stat.st_dev != 0");
    check(s.st_nlink > 0, "stat.st_nlink > 0 (0 reads as an unlinked file)");
    check(s.st_blksize > 0, "stat.st_blksize > 0 (0 breaks cp's buffer sizing)");
    // Fields that were already correct before the fix must stay correct.
    check(s.st_size == f.st_size, "stat.st_size == fstat.st_size");
    check(s.st_mode == f.st_mode, "stat.st_mode == fstat.st_mode");
    check(s.st_mtime == f.st_mtime, "stat.st_mtime == fstat.st_mtime");
    close(fd);
}

// ---------------------------------------------------------------------------
// #29 — do_wait() waits with a 1-second bounded timeout (a workaround for
// macOS condvars). It tested `if (wait_for(...))`, which is true for both
// _EINTR and _ETIMEDOUT, so every expiry of that internal timeout was
// reported to the guest as a signal and waitpid returned EINTR. Any child
// running longer than a second broke its parent; gcc died with
// "failed to get exit status: Interrupted system call" at ~1.01s.
// ---------------------------------------------------------------------------
static void test_waitpid_across_timeout(void) {
    section("#29 waitpid across the 1s internal timeout");

    // Straddle the boundary: the bug appears the moment a child outlives it.
    static const double points[] = {0.2, 0.9, 1.0, 1.1, 1.5, 2.0};
    int bad = 0;
    for (unsigned i = 0; i < sizeof(points) / sizeof(points[0]); i++) {
        pid_t p = fork();
        if (p == 0) {
            // Alternate sleeping and burning so both blocked and runnable
            // children are covered.
            if (i % 2)
                burn(points[i]);
            else
                nap(points[i]);
            _exit(40 + i);
        }
        int st = 0;
        errno = 0;
        pid_t r = waitpid(p, &st, 0);
        int ok = (r == p && WIFEXITED(st) && WEXITSTATUS(st) == (int)(40 + i));
        if (!ok) {
            bad++;
            printf("  child %.1fs -> waitpid=%d errno=%d(%s) exited=%d code=%d\n",
                   points[i], (int)r, errno, r < 0 ? strerror(errno) : "-",
                   WIFEXITED(st), WIFEXITED(st) ? WEXITSTATUS(st) : -1);
        }
    }
    check(bad == 0, "children at 0.2/0.9/1.0/1.1/1.5/2.0s all reaped, no EINTR");

    // The zombie must actually be gone once waitpid returns.
    pid_t p = fork();
    if (p == 0) {
        nap(1.2);
        _exit(44);
    }
    int st = 0;
    check(waitpid(p, &st, 0) == p && WEXITSTATUS(st) == 44, "slow child reaped");
    errno = 0;
    check(waitpid(p, &st, WNOHANG) < 0 && errno == ECHILD,
          "no leftover zombie (second wait -> ECHILD)");

    // Several concurrent slow children, reaped through wait().
    pid_t kids[3];
    for (int i = 0; i < 3; i++) {
        kids[i] = fork();
        if (kids[i] == 0) {
            burn(1.1 + 0.2 * i);
            _exit(50 + i);
        }
    }
    int reaped = 0;
    for (int i = 0; i < 3; i++) {
        int s2;
        pid_t r = wait(&s2);
        if (r > 0 && WIFEXITED(s2) && WEXITSTATUS(s2) >= 50 && WEXITSTATUS(s2) <= 52)
            reaped++;
    }
    check(reaped == 3, "3 concurrent slow children all reaped via wait()");
    errno = 0;
    check(wait(&st) < 0 && errno == ECHILD, "wait() -> ECHILD when no children remain");
}

// ---------------------------------------------------------------------------
// A real signal must still interrupt waitpid. The #29 fix narrows got_signal
// to _EINTR only, so this guards against over-correcting it away.
// ---------------------------------------------------------------------------
static volatile sig_atomic_t handler_ran;
static void on_usr1(int sig) { (void)sig; handler_ran = 1; }

static void test_signal_still_interrupts(void) {
    section("#29 real signals still interrupt waitpid");

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_usr1;
    sigaction(SIGUSR1, &sa, NULL);
    handler_ran = 0;

    pid_t child = fork();
    if (child == 0) {
        nap(4);
        _exit(7);
    }
    pid_t killer = fork();
    if (killer == 0) {
        nap(0.3);
        kill(getppid(), SIGUSR1);
        _exit(0);
    }

    double t0 = now_sec();
    int st = 0;
    errno = 0;
    pid_t r = waitpid(child, &st, 0);
    double elapsed = now_sec() - t0;

    check(r < 0 && errno == EINTR, "SIGUSR1 during waitpid -> EINTR");
    check(handler_ran == 1, "signal handler ran");
    // If this fires at ~1s it means the internal timeout was misreported again.
    check(elapsed < 1.0, "interrupted at the signal, not at the 1s timeout");
    check(waitpid(child, &st, 0) == child && WEXITSTATUS(st) == 7,
          "child still reapable after EINTR");
    waitpid(killer, NULL, 0);
}

// ---------------------------------------------------------------------------
// waitid siginfo — do_wait() produces the raw wait(2) status word, which is
// what wait4 wants. sys_waitid handed that straight to userspace instead of
// decoding it, so si_status was 7936 for exit(31) (31<<8) and si_code was
// always 0, leaving a killed child indistinguishable from an exited one.
// si_uid was never assigned at all on the wait path.
// ---------------------------------------------------------------------------
static void test_waitid_siginfo(void) {
    section("waitid si_status / si_code / si_uid");

    uid_t me = getuid();

    // exit(N) -> CLD_EXITED / N, including both ends of the byte.
    static const int codes[] = {0, 31, 255};
    for (unsigned i = 0; i < sizeof(codes) / sizeof(codes[0]); i++) {
        pid_t p = fork();
        if (p == 0)
            _exit(codes[i]);
        siginfo_t si;
        memset(&si, 0, sizeof si);
        int r = waitid(P_PID, p, &si, WEXITED);
        char msg[96];
        snprintf(msg, sizeof msg, "exit(%d) -> CLD_EXITED, si_status=%d", codes[i], codes[i]);
        check(r == 0 && si.si_code == CLD_EXITED && si.si_status == codes[i], msg);
        if (i == 1) {
            check(si.si_signo == SIGCHLD, "si_signo == SIGCHLD");
            check(si.si_pid == p, "si_pid == child pid");
            check(si.si_uid == me, "si_uid == child uid");
        }
    }

    // Killed by a signal -> CLD_KILLED / signal number.
    static const int sigs[] = {SIGKILL, SIGTERM};
    for (unsigned i = 0; i < sizeof(sigs) / sizeof(sigs[0]); i++) {
        pid_t p = fork();
        if (p == 0) {
            nap(10);
            _exit(0);
        }
        nap(0.3);
        kill(p, sigs[i]);
        siginfo_t si;
        memset(&si, 0, sizeof si);
        int r = waitid(P_PID, p, &si, WEXITED);
        char msg[96];
        snprintf(msg, sizeof msg, "killed by %d -> CLD_KILLED, si_status=%d", sigs[i], sigs[i]);
        check(r == 0 && si.si_code == CLD_KILLED && si.si_status == sigs[i], msg);
        if (i == 0)
            check(si.si_uid == me, "si_uid set on CLD_KILLED too");
    }

    // Stopped -> CLD_STOPPED / stopping signal.
    {
        pid_t p = fork();
        if (p == 0) {
            nap(10);
            _exit(0);
        }
        nap(0.3);
        kill(p, SIGSTOP);
        siginfo_t si;
        memset(&si, 0, sizeof si);
        int r = waitid(P_PID, p, &si, WSTOPPED);
        check(r == 0 && si.si_code == CLD_STOPPED && si.si_status == SIGSTOP,
              "SIGSTOP -> CLD_STOPPED, si_status=SIGSTOP");
        kill(p, SIGCONT);
        kill(p, SIGKILL);
        siginfo_t reap;
        waitid(P_PID, p, &reap, WEXITED);
    }

    // A child that drops privileges must report its own uid, not the parent's.
    // This is the only case that can distinguish the si_uid bug, since
    // everything else here runs as a single uid.
    {
        pid_t p = fork();
        if (p == 0) {
            if (setuid(1000) != 0)
                _exit(99);
            _exit(12);
        }
        siginfo_t si;
        memset(&si, 0, sizeof si);
        int r = waitid(P_PID, p, &si, WEXITED);
        if (r == 0 && si.si_status == 99) {
            printf("  SKIP  setuid(1000) not permitted here; cannot test uid change\n");
        } else {
            check(r == 0 && si.si_status == 12, "child reached exit(12) after setuid(1000)");
            check(si.si_uid == 1000, "si_uid == 1000 (child's uid, not the parent's)");
        }
    }

    // WNOHANG with nothing ready must not report a scanned candidate's uid.
    {
        pid_t p = fork();
        if (p == 0) {
            nap(1.0);
            _exit(13);
        }
        nap(0.2);
        siginfo_t si;
        memset(&si, 0, sizeof si);
        int r = waitid(P_PID, p, &si, WEXITED | WNOHANG);
        check(r == 0 && si.si_pid == 0, "WNOHANG reports si_pid=0 while the child runs");
        check(si.si_uid == 0, "WNOHANG leaves si_uid=0 (no stale uid)");
        siginfo_t reap;
        waitid(P_PID, p, &reap, WEXITED);
    }

    // wait4/waitpid must keep seeing the *raw* encoding — only waitid decodes.
    {
        pid_t p = fork();
        if (p == 0)
            _exit(31);
        int st = 0;
        pid_t r = waitpid(p, &st, 0);
        check(r == p && WIFEXITED(st) && WEXITSTATUS(st) == 31,
              "waitpid still sees raw status (WIFEXITED/WEXITSTATUS)");

        p = fork();
        if (p == 0) {
            nap(10);
            _exit(0);
        }
        nap(0.3);
        kill(p, SIGKILL);
        st = 0;
        r = waitpid(p, &st, 0);
        check(r == p && WIFSIGNALED(st) && WTERMSIG(st) == SIGKILL,
              "waitpid still sees raw status (WIFSIGNALED/WTERMSIG)");
    }
}

// POSIX timer, SIGEV_SIGNAL. sys_timer_create's SIGEV_SIGNAL branch left
// timer->thread_pid at 0, and posix_timer_callback resolves its target with
// pid_get_task(thread_pid) -- pid 0 is never a live task, so the lookup
// returned NULL and the callback dropped the signal. Both syscalls reported
// success and the clock expired on time; nothing was ever delivered.
//
// The user-visible symptom was that GNU coreutils `timeout` never fired:
// `timeout 3 sleep 300` ran the full 300s and exited 0, which reads from the
// outside as "processes cannot be killed". busybox's timeout was unaffected
// because it polls with kill(pid, 0) instead of arming a timer.
static volatile sig_atomic_t timer_fired;
static void timer_sigalrm(int sig) { (void) sig; timer_fired = 1; }

static void test_posix_timer_sigev_signal(void) {
    section("posix timer SIGEV_SIGNAL delivery");

    struct sigaction sa, old;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = timer_sigalrm;
    sigaction(SIGALRM, &sa, &old);

    // The NULL-sigevent form, which is what coreutils `timeout` ends up using.
    timer_fired = 0;
    timer_t tid;
    if (timer_create(CLOCK_REALTIME, NULL, &tid) != 0) {
        check(0, "timer_create(CLOCK_REALTIME, NULL)");
    } else {
        check(1, "timer_create(CLOCK_REALTIME, NULL)");
        struct itimerspec its;
        memset(&its, 0, sizeof(its));
        its.it_value.tv_sec = 1;
        check(timer_settime(tid, 0, &its, NULL) == 0, "timer_settime(1s)");
        // Sleep well past the deadline. Before the fix this waited the full
        // duration with timer_fired still 0.
        for (int i = 0; i < 40 && !timer_fired; i++)
            usleep(100 * 1000);
        check(timer_fired == 1, "SIGEV_SIGNAL timer actually fires");
        timer_delete(tid);
    }

    // The explicit SIGEV_SIGNAL form must behave the same way.
    timer_fired = 0;
    struct sigevent sev;
    memset(&sev, 0, sizeof(sev));
    sev.sigev_notify = SIGEV_SIGNAL;
    sev.sigev_signo = SIGALRM;
    if (timer_create(CLOCK_REALTIME, &sev, &tid) != 0) {
        check(0, "timer_create(explicit SIGEV_SIGNAL)");
    } else {
        check(1, "timer_create(explicit SIGEV_SIGNAL)");
        struct itimerspec its;
        memset(&its, 0, sizeof(its));
        its.it_value.tv_sec = 1;
        timer_settime(tid, 0, &its, NULL);
        for (int i = 0; i < 40 && !timer_fired; i++)
            usleep(100 * 1000);
        check(timer_fired == 1, "explicit SIGEV_SIGNAL timer fires");
        timer_delete(tid);
    }

    sigaction(SIGALRM, &old, NULL);
}

// unlink() on a directory: Linux says EISDIR (the macOS host says EPERM).
// musl's remove() falls back to rmdir() only on EISDIR, so this is what
// decides whether remove() - and Java's File.delete() - can delete a directory.
static void test_unlink_directory(void) {
    const char *d = "/tmp/regress_unlink_dir";
    rmdir(d);
    if (mkdir(d, 0755) != 0) {
        check(0, "setup: mkdir for the unlink test");
        return;
    }
    errno = 0;
    int r = unlink(d);
    check(r < 0 && errno == EISDIR, "unlink(directory) -> EISDIR");
    check(remove(d) == 0, "remove(empty directory) succeeds");
    struct stat st;
    check(stat(d, &st) < 0 && errno == ENOENT, "the directory is gone after remove()");
    rmdir(d);
}

// TCP options BIND sets on every TCP query socket. iSH answered
// ENOPROTOOPT for both, and dig +tcp / nslookup -vc aborted with
// "netmgr/tcpdns.c:315: RUNTIME_CHECK(result == ISC_R_SUCCESS) failed".
static void test_tcp_query_socket_options(void) {
    section("TCP_USER_TIMEOUT / TCP_MAXSEG (dig +tcp)");
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) {
        check(0, "setup: socket(AF_INET, SOCK_STREAM)");
        return;
    }
    unsigned ms = 2500;
    check(setsockopt(s, IPPROTO_TCP, TCP_USER_TIMEOUT, &ms, sizeof(ms)) == 0,
          "setsockopt(TCP_USER_TIMEOUT, 2500 ms)");
    unsigned got = 0;
    socklen_t len = sizeof(got);
    // Darwin keeps whole seconds; Linux reads back 2500 exactly.
    check(getsockopt(s, IPPROTO_TCP, TCP_USER_TIMEOUT, &got, &len) == 0 && got >= 2500 && got <= 3000,
          "getsockopt(TCP_USER_TIMEOUT) reads back 2500..3000 ms");
    ms = 0;
    check(setsockopt(s, IPPROTO_TCP, TCP_USER_TIMEOUT, &ms, sizeof(ms)) == 0,
          "setsockopt(TCP_USER_TIMEOUT, 0) restores the default");
    int mss = 1200;
    check(setsockopt(s, IPPROTO_TCP, TCP_MAXSEG, &mss, sizeof(mss)) == 0, "setsockopt(TCP_MAXSEG, 1200)");
    close(s);
}

int main(int argc, char **argv) {
    // Optional extra path to check stat/fstat on, e.g. a fakefs mount point.
    const char *extra_path = argc > 1 ? argv[1] : NULL;

    const char *file = "/tmp/regress_file.txt";
    const char *dir = "/tmp/regress_dir";
    const char *link = "/tmp/regress_link";

    unlink(file);
    unlink(link);
    rmdir(dir);
    int fd = open(file, O_CREAT | O_RDWR, 0644);
    if (fd < 0) {
        fprintf(stderr, "setup: cannot create %s: %s\n", file, strerror(errno));
        return 2;
    }
    write(fd, "regression\n", 11);
    close(fd);
    if (mkdir(dir, 0755) != 0 && errno != EEXIST) {
        fprintf(stderr, "setup: cannot create %s: %s\n", dir, strerror(errno));
        return 2;
    }

    test_o_directory(file, dir);
    test_o_nofollow(file, link);
    test_stat_fstat_agreement(file);
    if (extra_path != NULL)
        test_stat_fstat_agreement(extra_path);
    test_waitpid_across_timeout();
    test_signal_still_interrupts();
    test_waitid_siginfo();
    test_posix_timer_sigev_signal();
    test_unlink_directory();
    test_tcp_query_socket_options();

    printf("\n================ RESULT ================\n");
    printf("  PASS: %d    FAIL: %d\n", pass, fail);

    unlink(file);
    unlink(link);
    rmdir(dir);
    return fail ? 1 : 0;
}
