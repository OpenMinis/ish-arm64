// FIX-261109-13: poll() / select() racing close() of the same fds.
//
//   regress_poll_close_race [seconds] [pollers]
//
// Poller threads poll() and select() pipe read ends picked from a shared
// table while closer threads replace those pipes and close the old ends.
// poll_common() used to take its fd reference after f_get() had dropped
// files->lock, and select_common() took none before poll_add_fd(): a close
// in that window freed the fd under the poller. On a macOS host that ended
// ish with SIGSEGV within a second; on Windows realfs_poll() asserted on
// POLLNVAL. The test passes when the emulator is still alive at the end and
// prints "PASS"; any poll() result is acceptable, the fds are meant to vanish.
#define _GNU_SOURCE
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/select.h>
#include <time.h>
#include <unistd.h>

#define SLOTS 32

static atomic_int stop;
static atomic_long polls, closes;
static int slots[SLOTS][2];
static pthread_mutex_t slot_lock = PTHREAD_MUTEX_INITIALIZER;

static unsigned rnd(unsigned *s) { *s = *s * 1103515245u + 12345u; return *s >> 8; }

static void *poller(void *arg) {
    unsigned s = (unsigned) (long) arg;
    while (!stop) {
        struct pollfd p[4];
        int n = 0;
        pthread_mutex_lock(&slot_lock);
        for (int i = 0; i < 4; i++) {
            int k = rnd(&s) % SLOTS;
            if (slots[k][0] >= 0) { p[n].fd = slots[k][0]; p[n].events = POLLIN; n++; }
        }
        pthread_mutex_unlock(&slot_lock);
        if (rnd(&s) % 2) {
            poll(p, n, rnd(&s) % 3);
        } else {
            fd_set r;
            FD_ZERO(&r);
            int maxfd = -1;
            for (int i = 0; i < n; i++) {
                if (p[i].fd < FD_SETSIZE) { FD_SET(p[i].fd, &r); if (p[i].fd > maxfd) maxfd = p[i].fd; }
            }
            struct timeval tv = { 0, (rnd(&s) % 3) * 1000 };
            if (maxfd >= 0) select(maxfd + 1, &r, NULL, NULL, &tv);
        }
        polls++;
    }
    return NULL;
}

static void *closer(void *arg) {
    unsigned s = (unsigned) (long) arg;
    while (!stop) {
        int k = rnd(&s) % SLOTS, nw[2];
        if (pipe2(nw, O_NONBLOCK | O_CLOEXEC) < 0) continue;
        pthread_mutex_lock(&slot_lock);
        int old0 = slots[k][0], old1 = slots[k][1];
        slots[k][0] = nw[0];
        slots[k][1] = nw[1];
        pthread_mutex_unlock(&slot_lock);
        // Closed outside the lock: a poller may be inside poll() on old0.
        if (old0 >= 0) close(old0);
        if (old1 >= 0) { if (rnd(&s) % 2) (void) write(old1, "x", 1); close(old1); }
        closes++;
    }
    return NULL;
}

int main(int argc, char **argv) {
    int seconds = argc > 1 ? atoi(argv[1]) : 5;
    int pollers = argc > 2 ? atoi(argv[2]) : 4;
    if (pollers < 1) pollers = 1;
    if (pollers > 16) pollers = 16;
    signal(SIGPIPE, SIG_IGN);
    for (int i = 0; i < SLOTS; i++) slots[i][0] = slots[i][1] = -1;

    pthread_t t[32];
    int n = 0;
    for (int i = 0; i < pollers; i++) {
        pthread_create(&t[n++], NULL, poller, (void *) (long) (i * 7 + 1));
        pthread_create(&t[n++], NULL, closer, (void *) (long) (i * 13 + 3));
    }
    struct timespec ts = { seconds, 0 };
    nanosleep(&ts, NULL);
    stop = 1;
    for (int i = 0; i < n; i++) pthread_join(t[i], NULL);
    printf("polls=%ld closes=%ld\n", polls, closes);
    printf(polls > 0 && closes > 0 ? "PASS\n" : "FAIL nothing ran\n");
    return polls > 0 && closes > 0 ? 0 : 1;
}
