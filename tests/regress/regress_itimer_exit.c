// [FIX-261120-04] An interval ITIMER_REAL must not take the kernel down when
// it fires while its process, or the thread that armed it, is exiting.
//
//   regress_itimer_exit [seconds] [parallel]
//
// Phase "proc": children arm a 100 us interval timer, spin briefly and exit.
//   Before the fix the timer thread dereferenced the exiting task's cleared
//   sighand: SIGSEGV in deliver_signal_unlocked <- send_signal <-
//   itimer_notify <- timer_thread, within seconds.
// Phase "thread": in each child a helper thread arms the timer and exits; the
//   main thread must still get SIGALRM (it is a process signal) and the timer
//   must not fire into the freed helper.
// Prints "OK ..." on success; a crash of the emulator is the failure.
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t ticks;
static void on_alarm(int s) { (void) s; ticks++; }

static void arm(void) {
    struct itimerval it = {{0, 100}, {0, 100}};
    setitimer(ITIMER_REAL, &it, NULL);
}

static void *arm_and_leave(void *arg) {
    (void) arg;
    arm();
    return NULL;
}

static void spin(int base) {
    for (volatile int k = 0; k < base + (getpid() % 7) * 3000; k++) {}
}

int main(int argc, char **argv) {
    int seconds = argc > 1 ? atoi(argv[1]) : 20;
    int par = argc > 2 ? atoi(argv[2]) : 6;
    long done = 0, bad = 0;
    for (int phase = 0; phase < 2; phase++) {
        time_t end = time(NULL) + seconds / 2;
        while (time(NULL) < end) {
            for (int i = 0; i < par; i++) {
                pid_t p = fork();
                if (p < 0) { perror("fork"); return 1; }
                if (p == 0) {
                    signal(SIGALRM, on_alarm);
                    if (phase == 0) {
                        arm();
                        spin(20000);
                        _exit(0);
                    }
                    pthread_t t;
                    if (pthread_create(&t, NULL, arm_and_leave, NULL) != 0) _exit(3);
                    pthread_join(t, NULL);
                    // The helper is gone; the timer must still reach us.
                    struct timespec nap = {0, 20 * 1000 * 1000};
                    nanosleep(&nap, NULL);
                    spin(20000);
                    _exit(ticks > 0 ? 0 : 4);
                }
            }
            for (int i = 0; i < par; i++) {
                int st = 0;
                if (wait(&st) > 0) {
                    done++;
                    if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) bad++;
                }
            }
        }
    }
    if (bad) { printf("FAIL children=%ld bad=%ld\n", done, bad); return 1; }
    printf("OK children=%ld\n", done);
    return 0;
}
