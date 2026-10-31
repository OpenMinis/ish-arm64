// [FIX-261031-04] Host-side checks for what a guest cannot reach: send_signal
// given a signal number outside NUM_SIGS (the 10-31 SIGBUS indexed
// sighand->action[0x40814800]), the timer probes' records, and how the
// crash writer prints them. Linked against libish; see run_posix_timer.sh.
//
// -DBASELINE builds only the send_signal case, which uses no new API, so it
// can also be run against an iSH from before the change (it crashes there).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include "kernel/task.h"
#include "kernel/signal.h"
#include "kernel/diag.h"

static int failures;
static void check(int ok, const char *what) {
    printf("  %s  %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok)
        failures++;
}

static struct task *fake_task(int pid) {
    struct task *task = calloc(1, sizeof(*task));
    task->pid = pid;
    task->sighand = calloc(1, sizeof(*task->sighand));
    lock_init(&task->sighand->lock);
    return task;
}

// In a child: a crash there is a FAIL here, not the end of the run.
static int survives(void (*fn)(void)) {
    pid_t pid = fork();
    if (pid == 0) {
        fn();
        _exit(0);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

static void send_bad_signals(void) {
    struct task *task = fake_task(4242);
    send_signal(task, 0x40814800, SIGINFO_NIL);
    send_signal(task, NUM_SIGS, SIGINFO_NIL);
    send_signal(task, -5, SIGINFO_NIL);
}

#ifndef BASELINE
static uint64_t counter(enum ish_diag_counter c) {
    return atomic_load(&ish_diag_counters[c]);
}

static char dump[65536];
static void crash_dump(void) {
    char path[] = "/tmp/host_timer_diag.XXXXXX";
    int fd = mkstemp(path);
    unlink(path);
    ish_diag_write_crash(fd);
    lseek(fd, 0, SEEK_SET);
    ssize_t n = read(fd, dump, sizeof(dump) - 1);
    dump[n > 0 ? n : 0] = '\0';
    close(fd);
}
#endif

int main(void) {
    puts("send_signal with a signal number iSH does not have");
    check(survives(send_bad_signals), "0x40814800, NUM_SIGS and -5 do not crash");
#ifndef BASELINE
    uint64_t before = counter(DIAG_SEND_SIGNAL_OOB);
    send_bad_signals();
    check(counter(DIAG_SEND_SIGNAL_OOB) - before == 3, "send_signal_oob +3");

    puts("timer fires");
    struct tgroup *group = calloc(1, sizeof(*group));
    struct posix_timer live = {
        .timer_id = 3, .signal = 14, .thread_pid = 77, .tgroup = (struct tgroup *) 0x1234,
        .magic = DIAG_TIMER_MAGIC, .seq = 9,
    };
    ish_diag_timer_fire(&live);
    struct posix_timer gone = { .timer_id = 5, .signal = 0x40814800, .thread_pid = 12, .magic = 0, .seq = 4 };
    ish_diag_timer_fire(&gone);

    puts("timers left at exit");
    before = counter(DIAG_TIMER_LEAKED_AT_EXIT);
    struct tgroup *parent = calloc(1, sizeof(*parent));
    group->posix_timers[0] = (struct posix_timer) { .timer = (struct timer *) 1, .tgroup = group, .magic = DIAG_TIMER_MAGIC };
    group->posix_timers[1] = (struct posix_timer) { .timer = (struct timer *) 1, .tgroup = parent, .magic = DIAG_TIMER_MAGIC };
    group->posix_timers[2] = (struct posix_timer) { .timer = (struct timer *) 1, .tgroup = NULL, .magic = DIAG_TIMER_MAGIC };
    ish_diag_timer_leak(group);
    check(counter(DIAG_TIMER_LEAKED_AT_EXIT) - before == 3, "timer_leaked_at_exit +3");
    check(group->posix_timers[0].magic == 0 && group->posix_timers[1].magic == 0, "their magic is cleared");
    struct tgroup *empty = calloc(1, sizeof(*empty));
    before = counter(DIAG_TIMER_LEAKED_AT_EXIT);
    ish_diag_timer_leak(empty);
    check(counter(DIAG_TIMER_LEAKED_AT_EXIT) == before, "a group with none: nothing counted");

    puts("crash dump");
    crash_dump();
    fputs(dump, stdout);
    check(strstr(dump, " send_signal_oob=3") != NULL, "counter line: send_signal_oob=3");
    check(strstr(dump, " timer_leaked_at_exit=3") != NULL, "counter line: timer_leaked_at_exit=3");
    check(strstr(dump, "send_signal_oob pid=-1 thread=") && strstr(dump, " a=1082214400 b=4242"),
          "event: send_signal_oob a=sig b=target pid");
    check(strstr(dump, "timer_leak_exit") && strstr(dump, " a=3 b=1\n"), "event: timer_leak_exit a=3 b=1 (one inherited)");
    check(strstr(dump, "timer_stale") && strstr(dump, " a=1082214400 b=0\n"), "event: timer_stale for the gone timer");
    check(strstr(dump, "timer_fire pid=77 thread=\"timer\" a=14 b=3 seq=9 tgroup_lo=4660 tgroup_null=0 magic_ok=1\n") != NULL,
          "fire: the live timer");
    check(strstr(dump, "timer_fire pid=12 thread=\"timer\" a=1082214400 b=5 seq=4 tgroup_lo=0 tgroup_null=1 magic_ok=0\n") != NULL,
          "fire: the gone timer");

    puts("fire ring keeps its own 16");
    for (int i = 0; i < 40; i++)
        ish_diag_timer_fire(&live);
    crash_dump();
    int fires = 0;
    for (const char *p = dump; (p = strstr(p, " timer_fire ")) != NULL; p++)
        fires++;
    check(fires == 16, "16 fire lines after 42 fires");
    check(strstr(dump, "send_signal_oob pid=") != NULL, "...and the main ring still has the rare events");

    puts("/proc/ish/diag text");
    static char text[8192];
    ish_diag_describe(text, sizeof(text));
    const char *old_key = strstr(text, " aot_rejected_images=");
    const char *new_key = strstr(text, " timer_create=");
    check(strncmp(text, "procs=", 6) == 0, "starts with procs= as before");
    check(old_key && new_key && old_key < new_key, "new keys come after every older key");
    static const char *const keys[] = {
        " timer_create=", " timer_delete=", " timer_delete_oob=", " timer_settime_oob=",
        " timer_signo_rejected=", " send_signal_oob=3", " timer_leaked_at_exit=3", " timer_threads_live=",
    };
    for (unsigned i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        char what[80];
        snprintf(what, sizeof(what), "has%s", keys[i]);
        check(strstr(text, keys[i]) != NULL, what);
    }
#endif
    printf("host timer diag: %d failure(s)\n", failures);
    return failures ? 1 : 0;
}
