// [FIX-261031-04] POSIX timer syscalls: what the guest passes in is checked
// before the kernel indexes or stores anything with it.
//
// A 10-31 SIGBUS (send_signal <- posix_timer_callback <- timer_thread) had
// send_signal index sighand->action[] with sig=0x40814800. The timer
// syscalls took a guest timer id or signal number without the checks Linux
// does: timer_delete had no upper bound on the id (an out-of-range read and
// write of the host heap), timer_settime let id == TIMERS_MAX through and
// dereferenced an unallocated slot's NULL timer, timer_create stored any
// signal number for the callback to send, and its SIGEV_THREAD_ID branch
// returned with pids_lock held when the thread did not exist (the next
// fork / kill / wait then hung forever).
//
// One case per run (argv[1]) so a case that crashes or hangs iSH fails
// alone; run.sh runs each under a host timeout. `diag` reads the counters
// the kernel keeps for these rejections from /proc/ish/diag.
#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define TIMERS_MAX 16       // kernel/task.h
#define BAD_SIGNO 0x40814800 // the 10-31 crash's signal number

static int failures;
static void check(int ok, const char *what) {
    printf("  %s  %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok)
        failures++;
}

// The kernel's struct sigevent (no libc translation in between).
struct ksigevent {
    union { int sival_int; void *sival_ptr; } value;
    int signo;
    int notify;
    int tid;
    char pad[64 - 2 * sizeof(int) - sizeof(int) - sizeof(void *)];
};
#define SIGEV_SIGNAL_ 0
#define SIGEV_NONE_ 1
#define SIGEV_THREAD_ID_ 4

static long k_timer_create(clockid_t clock, struct ksigevent *sev, int *id) {
    return syscall(SYS_timer_create, clock, sev, id);
}
static long k_timer_settime(int id, int flags, const struct itimerspec *v, struct itimerspec *old) {
    return syscall(SYS_timer_settime, id, flags, v, old);
}
static long k_timer_gettime(int id, struct itimerspec *v) {
    return syscall(SYS_timer_gettime, id, v);
}
static long k_timer_getoverrun(int id) {
    return syscall(SYS_timer_getoverrun, id);
}
static long k_timer_delete(int id) {
    return syscall(SYS_timer_delete, id);
}
static int einval(long r) { return r == -1 && errno == EINVAL; }

static const struct itimerspec in_50ms = { .it_value = { 0, 50 * 1000 * 1000 } };

static volatile sig_atomic_t got_sig, got_code, got_value;
static void on_alrm(int sig, siginfo_t *si, void *uc) {
    (void) uc;
    got_sig = sig;
    got_code = si->si_code;
    got_value = si->si_value.sival_int;
}
static void catch_alrm(void) {
    struct sigaction sa = { .sa_sigaction = on_alrm, .sa_flags = SA_SIGINFO };
    sigemptyset(&sa.sa_mask);
    sigaction(SIGALRM, &sa, NULL);
    got_sig = got_code = got_value = 0;
}
// Waits up to 2 s for SIGALRM.
static int wait_alrm(void) {
    for (int i = 0; i < 200 && !got_sig; i++)
        usleep(10 * 1000);
    return got_sig == SIGALRM;
}

static void case_signo(void) {
    puts("timer_create: signal numbers");
    struct ksigevent sev = { .notify = SIGEV_SIGNAL_, .signo = BAD_SIGNO };
    int id = -1;
    check(einval(k_timer_create(CLOCK_MONOTONIC, &sev, &id)), "SIGEV_SIGNAL, signo 0x40814800: EINVAL");
    sev.signo = 0;
    check(einval(k_timer_create(CLOCK_MONOTONIC, &sev, &id)), "SIGEV_SIGNAL, signo 0: EINVAL");
    sev.signo = -1;
    check(einval(k_timer_create(CLOCK_MONOTONIC, &sev, &id)), "SIGEV_SIGNAL, signo -1: EINVAL");
    sev.signo = 64;
    check(einval(k_timer_create(CLOCK_MONOTONIC, &sev, &id)), "SIGEV_SIGNAL, signo 64 (no slot in iSH's 63): EINVAL");
    sev.notify = SIGEV_THREAD_ID_;
    sev.tid = (int) syscall(SYS_gettid);
    sev.signo = BAD_SIGNO;
    check(einval(k_timer_create(CLOCK_MONOTONIC, &sev, &id)), "SIGEV_THREAD_ID, signo 0x40814800: EINVAL");
    // SIGEV_NONE sends nothing: its signal number is not looked at (Linux).
    sev = (struct ksigevent) { .notify = SIGEV_NONE_, .signo = BAD_SIGNO };
    long r = k_timer_create(CLOCK_MONOTONIC, &sev, &id);
    check(r == 0, "SIGEV_NONE, any signo: accepted");
    if (r == 0)
        check(k_timer_delete(id) == 0, "...and deleted");
    sev = (struct ksigevent) { .notify = SIGEV_SIGNAL_, .signo = SIGRTMIN + 2 };
    r = k_timer_create(CLOCK_MONOTONIC, &sev, &id);
    check(r == 0, "SIGEV_SIGNAL, a real-time signal: accepted");
    if (r == 0)
        k_timer_delete(id);
}

static void case_ids(void) {
    puts("timer ids out of range or never created");
    struct itimerspec v;
    const int bad[] = { TIMERS_MAX, TIMERS_MAX + 1, 1000, -1, 0x7fffffff };
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        char what[96];
        snprintf(what, sizeof(what), "timer_delete(%d): EINVAL", bad[i]);
        check(einval(k_timer_delete(bad[i])), what);
        snprintf(what, sizeof(what), "timer_settime(%d): EINVAL", bad[i]);
        check(einval(k_timer_settime(bad[i], 0, &in_50ms, NULL)), what);
        snprintf(what, sizeof(what), "timer_gettime(%d): EINVAL", bad[i]);
        check(einval(k_timer_gettime(bad[i], &v)), what);
        snprintf(what, sizeof(what), "timer_getoverrun(%d): EINVAL", bad[i]);
        check(einval(k_timer_getoverrun(bad[i])), what);
    }
    // An in-range slot that was never created holds a NULL timer.
    check(einval(k_timer_settime(5, 0, &in_50ms, NULL)), "timer_settime(5), never created: EINVAL");
    check(einval(k_timer_delete(5)), "timer_delete(5), never created: EINVAL");
    check(einval(k_timer_gettime(5, &v)), "timer_gettime(5), never created: EINVAL");
    // ...and one that was created and deleted.
    struct ksigevent sev = { .notify = SIGEV_NONE_ };
    int id = -1;
    if (k_timer_create(CLOCK_MONOTONIC, &sev, &id) == 0 && k_timer_delete(id) == 0) {
        check(einval(k_timer_settime(id, 0, &in_50ms, NULL)), "timer_settime on a deleted id: EINVAL");
        check(einval(k_timer_delete(id)), "timer_delete twice: EINVAL");
    } else {
        check(0, "create + delete a SIGEV_NONE timer");
    }
}

static void case_normal(void) {
    puts("normal path: SIGEV_SIGNAL SIGALRM, settime, delivery, delete");
    catch_alrm();
    struct ksigevent sev = { .notify = SIGEV_SIGNAL_, .signo = SIGALRM, .value.sival_int = 4242 };
    int id = -1;
    check(k_timer_create(CLOCK_MONOTONIC, &sev, &id) == 0, "timer_create");
    check(id >= 0 && id < TIMERS_MAX, "id in range");
    struct itimerspec v;
    check(k_timer_settime(id, 0, &in_50ms, NULL) == 0, "timer_settime 50 ms");
    check(k_timer_gettime(id, &v) == 0, "timer_gettime");
    check(k_timer_getoverrun(id) == 0, "timer_getoverrun");
    check(wait_alrm(), "SIGALRM delivered");
    check(got_code == SI_TIMER, "si_code SI_TIMER");
    check(got_value == 4242, "si_value from the sigevent");
    check(k_timer_delete(id) == 0, "timer_delete");
    check(einval(k_timer_delete(id)), "timer_delete again: EINVAL");

    puts("timeout(1)'s form: timer_create(clock, NULL, &id)");
    catch_alrm();
    id = -1;
    check(k_timer_create(CLOCK_REALTIME, NULL, &id) == 0, "timer_create(NULL sigevent)");
    check(k_timer_settime(id, 0, &in_50ms, NULL) == 0, "timer_settime 50 ms");
    check(wait_alrm(), "SIGALRM delivered");
    check(got_value == id, "si_value is the timer id");
    check(k_timer_delete(id) == 0, "timer_delete");

    puts("SIGEV_THREAD_ID to this thread");
    catch_alrm();
    sev = (struct ksigevent) { .notify = SIGEV_THREAD_ID_, .signo = SIGALRM, .tid = (int) syscall(SYS_gettid) };
    check(k_timer_create(CLOCK_MONOTONIC, &sev, &id) == 0, "timer_create");
    check(k_timer_settime(id, 0, &in_50ms, NULL) == 0, "timer_settime 50 ms");
    check(wait_alrm(), "SIGALRM delivered");
    check(k_timer_delete(id) == 0, "timer_delete");

    puts("all 16 slots, then ENOMEM, then reusable");
    int ids[TIMERS_MAX];
    int made = 0;
    sev = (struct ksigevent) { .notify = SIGEV_NONE_ };
    for (int i = 0; i < TIMERS_MAX; i++)
        if (k_timer_create(CLOCK_MONOTONIC, &sev, &ids[i]) == 0)
            made++;
    check(made == TIMERS_MAX, "16 timers");
    int extra;
    long r = k_timer_create(CLOCK_MONOTONIC, &sev, &extra);
    check(r == -1 && errno == ENOMEM, "17th: ENOMEM");
    for (int i = 0; i < made; i++)
        k_timer_delete(ids[i]);
    r = k_timer_create(CLOCK_MONOTONIC, &sev, &extra);
    check(r == 0, "a slot is free again");
    if (r == 0)
        k_timer_delete(extra);

    puts("setitimer / alarm");
    catch_alrm();
    struct itimerval it = { .it_value = { 0, 50 * 1000 } };
    check(setitimer(ITIMER_REAL, &it, NULL) == 0, "setitimer 50 ms");
    check(wait_alrm(), "SIGALRM delivered");
    catch_alrm();
    alarm(1);
    for (int i = 0; i < 300 && !got_sig; i++)
        usleep(10 * 1000);
    check(got_sig == SIGALRM, "alarm(1): SIGALRM delivered");
}

static void case_thread_id(void) {
    puts("SIGEV_THREAD_ID naming a thread that does not exist");
    struct ksigevent sev = { .notify = SIGEV_THREAD_ID_, .signo = SIGALRM, .tid = 999999 };
    int id = -1;
    check(einval(k_timer_create(CLOCK_MONOTONIC, &sev, &id)), "timer_create: EINVAL");
    // pids_lock must have been released: fork, kill and wait all take it.
    // If it was not, this hangs and the runner's timeout fails the case.
    pid_t pid = fork();
    if (pid == 0)
        _exit(7);
    int status = 0;
    check(pid > 0 && waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 7,
          "fork + waitpid afterwards (pids_lock was released)");
    check(kill(getpid(), 0) == 0, "kill(self, 0) afterwards");
}

// Counters in /proc/ish/diag ("key=value ...").
static long diag_value(const char *text, const char *key) {
    char pat[64];
    snprintf(pat, sizeof(pat), " %s=", key);
    const char *p = strstr(text, pat);
    return p ? strtol(p + strlen(pat), NULL, 10) : -1;
}
static int read_diag(char *buf, size_t size) {
    FILE *f = fopen("/proc/ish/diag", "r");
    if (!f)
        return 0;
    size_t n = fread(buf, 1, size - 1, f);
    buf[n] = '\0';
    fclose(f);
    return 1;
}

static void case_diag(void) {
    puts("diag counters for the rejections");
    static char before[8192], after[8192];
    if (!read_diag(before, sizeof(before))) {
        check(0, "read /proc/ish/diag");
        return;
    }
    struct ksigevent sev = { .notify = SIGEV_SIGNAL_, .signo = BAD_SIGNO };
    int id;
    k_timer_create(CLOCK_MONOTONIC, &sev, &id);
    k_timer_delete(1000);
    k_timer_settime(TIMERS_MAX, 0, &in_50ms, NULL);
    sev = (struct ksigevent) { .notify = SIGEV_NONE_ };
    if (k_timer_create(CLOCK_MONOTONIC, &sev, &id) == 0)
        k_timer_delete(id);
    read_diag(after, sizeof(after));
    static const char *const keys[] = {
        "timer_create", "timer_delete", "timer_delete_oob", "timer_settime_oob",
        "timer_signo_rejected", "send_signal_oob", "timer_leaked_at_exit", "timer_threads_live",
    };
    for (unsigned i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        char what[96];
        snprintf(what, sizeof(what), "/proc/ish/diag has %s", keys[i]);
        check(diag_value(after, keys[i]) >= 0, what);
    }
    check(diag_value(after, "timer_signo_rejected") - diag_value(before, "timer_signo_rejected") == 1,
          "timer_signo_rejected +1");
    check(diag_value(after, "timer_delete_oob") - diag_value(before, "timer_delete_oob") == 1,
          "timer_delete_oob +1");
    check(diag_value(after, "timer_settime_oob") - diag_value(before, "timer_settime_oob") == 1,
          "timer_settime_oob +1");
    check(diag_value(after, "timer_create") - diag_value(before, "timer_create") == 1,
          "timer_create +1 (only the accepted one)");
    check(diag_value(after, "timer_delete") - diag_value(before, "timer_delete") == 1,
          "timer_delete +1 (only the accepted one)");
    // Existing keys keep their place: the line still starts the same way.
    check(strncmp(after, "procs=", 6) == 0, "the line still starts with procs=");
    check(diag_value(after, "unmap_calls") >= 0 && diag_value(after, "aot_rejected_images") >= 0,
          "older keys still there");
}

// A process exits with timers still allocated: counted, not freed.
static void case_leak(void) {
    puts("timers left allocated at exit are counted");
    static char before[8192], after[8192];
    if (!read_diag(before, sizeof(before))) {
        check(0, "read /proc/ish/diag");
        return;
    }
    pid_t pid = fork();
    if (pid == 0) {
        struct ksigevent sev = { .notify = SIGEV_NONE_ };
        int id;
        k_timer_create(CLOCK_MONOTONIC, &sev, &id);
        k_timer_create(CLOCK_MONOTONIC, &sev, &id);
        _exit(0);
    }
    int status;
    waitpid(pid, &status, 0);
    read_diag(after, sizeof(after));
    check(diag_value(after, "timer_leaked_at_exit") - diag_value(before, "timer_leaked_at_exit") == 2,
          "timer_leaked_at_exit +2");
}

// [INV-261109-03] A process exits with a POSIX timer armed. Its group is
// freed at reap, but the timer thread still fires into the slot inside it.
// The next fork usually gets the same block back, and the new group's slot
// is a copy of ours: the stale fire then reads our pid and SIGALRM out of it
// and signals us, a process that has no timer at all (on Linux the dead
// process's timers die with it). Fixed: the fire sees the slot is no longer
// a live timer and does nothing.
static void case_stale(void) {
    puts("a timer of a process that exited never fires into another process");
    catch_alrm();
    // Leave this slot 0 holding our pid and SIGALRM, with no timer in it.
    struct ksigevent sev = { .signo = SIGALRM, .notify = SIGEV_SIGNAL_ };
    int id = -1;
    k_timer_create(CLOCK_MONOTONIC, &sev, &id);
    k_timer_delete(id);
    int fired = 0;
    for (int round = 0; round < 5 && !fired; round++) {
        pid_t a = fork();
        if (a == 0) {
            // Takes slot 0 (it is free), arms 200 ms, exits at once.
            int cid = -1;
            k_timer_create(CLOCK_MONOTONIC, &sev, &cid);
            struct itimerspec in_200ms = { .it_value = { 0, 200 * 1000 * 1000 } };
            k_timer_settime(cid, 0, &in_200ms, NULL);
            _exit(0);
        }
        int status;
        waitpid(a, &status, 0);
        // Reaped: a's group is freed. b is the next fork: its group is
        // likely a's block, its slot 0 a copy of ours.
        pid_t b = fork();
        if (b == 0) {
            usleep(600 * 1000);
            _exit(0);
        }
        for (int i = 0; i < 60 && !got_sig; i++)
            usleep(10 * 1000);
        fired = got_sig != 0;
        waitpid(b, &status, 0);
    }
    check(!fired, "no SIGALRM reached a process without a timer");
}

int main(int argc, char **argv) {
    const char *which = argc > 1 ? argv[1] : "";
    if (!strcmp(which, "signo")) case_signo();
    else if (!strcmp(which, "ids")) case_ids();
    else if (!strcmp(which, "normal")) case_normal();
    else if (!strcmp(which, "thread_id")) case_thread_id();
    else if (!strcmp(which, "diag")) case_diag();
    else if (!strcmp(which, "leak")) case_leak();
    else if (!strcmp(which, "stale")) case_stale();
    else {
        fprintf(stderr, "usage: %s signo|ids|normal|thread_id|diag|leak|stale\n", argv[0]);
        return 2;
    }
    printf("%s: %d failure(s)\n", which, failures);
    return failures ? 1 : 0;
}
