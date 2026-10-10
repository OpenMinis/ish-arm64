// Interruptible waits: what pthread_kill(thread, SIGUSR1) does on POSIX hosts.
// Each thread that blocks in the px layer has a wake socket (a UDP socket on
// loopback) and an event. px_thread_interrupt() marks the thread, pokes both,
// and whatever it is blocked in -- WSAPoll, a sleep -- returns EINTR. A mark
// that arrives while the thread is not blocked is kept for its next wait,
// which closes the window POSIX hosts paper over with timeouts.
#include "px_impl.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct px_thread {
    pthread_t id;
    atomic_bool pending;
    SOCKET wake;
    struct sockaddr_in wake_addr;
    HANDLE event;
    HANDLE timer;
    struct px_thread *next;
};

static SRWLOCK threads_lock = SRWLOCK_INIT;
static struct px_thread *threads;
static SOCKET poker = INVALID_SOCKET;
static pthread_key_t self_key;
static INIT_ONCE key_once = INIT_ONCE_STATIC_INIT;

static void thread_free(void *param) {
    struct px_thread *t = param;
    AcquireSRWLockExclusive(&threads_lock);
    for (struct px_thread **p = &threads; *p; p = &(*p)->next) {
        if (*p == t) {
            *p = t->next;
            break;
        }
    }
    ReleaseSRWLockExclusive(&threads_lock);
    if (t->wake != INVALID_SOCKET)
        closesocket(t->wake);
    CloseHandle(t->event);
    if (t->timer)
        CloseHandle(t->timer);
    free(t);
}

static BOOL CALLBACK key_init(PINIT_ONCE once, PVOID param, PVOID *ctx) {
    (void) once; (void) param; (void) ctx;
    pthread_key_create(&self_key, thread_free);
    poker = socket(AF_INET, SOCK_DGRAM, 0);
    return TRUE;
}

// Finds the record for a thread, creating it if needed. Interrupting a
// thread that never waited yet creates its record, so the mark is not lost.
static struct px_thread *thread_find(pthread_t id, bool create) {
    AcquireSRWLockExclusive(&threads_lock);
    struct px_thread *t;
    for (t = threads; t; t = t->next)
        if (pthread_equal(t->id, id))
            break;
    if (t == NULL && create) {
        t = calloc(1, sizeof(*t));
        t->id = id;
        t->wake = INVALID_SOCKET;
        t->event = CreateEventW(NULL, FALSE, FALSE, NULL);
        t->next = threads;
        threads = t;
    }
    ReleaseSRWLockExclusive(&threads_lock);
    return t;
}

static struct px_thread *self(void) {
    px_init();
    InitOnceExecuteOnce(&key_once, key_init, NULL, NULL);
    struct px_thread *t = pthread_getspecific(self_key);
    if (t != NULL)
        return t;
    t = thread_find(pthread_self(), true);
    pthread_setspecific(self_key, t);
    if (t->wake == INVALID_SOCKET) {
        SOCKET s = socket(AF_INET, SOCK_DGRAM, 0);
        struct sockaddr_in a = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
        int len = sizeof(a);
        u_long on = 1;
        if (s != INVALID_SOCKET && bind(s, (struct sockaddr *) &a, sizeof(a)) == 0 &&
                getsockname(s, (struct sockaddr *) &a, &len) == 0 && ioctlsocket(s, FIONBIO, &on) == 0) {
            t->wake_addr = a;
            // publish the address before the socket
            atomic_thread_fence(memory_order_release);
            t->wake = s;
        } else if (s != INVALID_SOCKET) {
            closesocket(s);
        }
    }
    return t;
}

static void drain(struct px_thread *t) {
    char buf[16];
    while (recv(t->wake, buf, sizeof(buf), 0) > 0)
        ;
}

bool px_interrupted(void) {
    struct px_thread *t = self();
    if (!atomic_exchange(&t->pending, false))
        return false;
    drain(t);
    ResetEvent(t->event);
    return true;
}

void px_thread_interrupt(pthread_t id) {
    InitOnceExecuteOnce(&key_once, key_init, NULL, NULL);
    struct px_thread *t = thread_find(id, true);
    atomic_store(&t->pending, true);
    SetEvent(t->event);
    SOCKET wake = t->wake;
    atomic_thread_fence(memory_order_acquire);
    if (wake != INVALID_SOCKET)
        sendto(poker, "!", 1, 0, (struct sockaddr *) &t->wake_addr, sizeof(t->wake_addr));
}

int px_pthread_kill(pthread_t thread, int sig) {
    if (sig != 0)
        px_thread_interrupt(thread);
    return 0;
}

static int sleep_ns(int64_t ns, struct timespec *rem);

int px_wsapoll(WSAPOLLFD *fds, ULONG n, int timeout_ms) {
    struct px_thread *t = self();
    if (n == 0 || t->wake == INVALID_SOCKET) {
        if (n == 0)
            return sleep_ns(timeout_ms < 0 ? INT64_MAX / 4 : timeout_ms * 1000000ll, NULL);
    }
    if (px_interrupted())
        return px_fail(EINTR);
    WSAPOLLFD stack[32];
    WSAPOLLFD *all = n + 1 <= 32 ? stack : malloc((n + 1) * sizeof(*all));
    if (n)
        memcpy(all, fds, n * sizeof(*all));
    all[n] = (WSAPOLLFD) {.fd = t->wake, .events = POLLRDNORM};
    int res = WSAPoll(all, t->wake != INVALID_SOCKET ? n + 1 : n, timeout_ms);
    if (res == SOCKET_ERROR) {
        res = px_fail_wsa();
    } else {
        if (n)
            memcpy(fds, all, n * sizeof(*all));
        if (all[n].revents)
            res--;
        if (px_interrupted())
            res = px_fail(EINTR);
    }
    if (all != stack)
        free(all);
    return res;
}

static int sleep_ns(int64_t ns, struct timespec *rem) {
    struct px_thread *t = self();
    if (px_interrupted())
        return px_fail(EINTR);
    if (ns <= 0)
        return 0;
    if (t->timer == NULL) {
        t->timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        if (t->timer == NULL)
            t->timer = CreateWaitableTimerW(NULL, TRUE, NULL);
    }
    LARGE_INTEGER due = {.QuadPart = -(ns / 100 ? ns / 100 : 1)};
    LARGE_INTEGER start, freq;
    QueryPerformanceCounter(&start);
    SetWaitableTimer(t->timer, &due, 0, NULL, NULL, FALSE);
    HANDLE hs[2] = {t->timer, t->event};
    DWORD w = WaitForMultipleObjects(2, hs, FALSE, INFINITE);
    if (w == WAIT_OBJECT_0 + 1 || px_interrupted()) {
        CancelWaitableTimer(t->timer);
        atomic_store(&t->pending, false);
        drain(t);
        if (rem != NULL) {
            LARGE_INTEGER now;
            QueryPerformanceCounter(&now);
            QueryPerformanceFrequency(&freq);
            int64_t spent = (now.QuadPart - start.QuadPart) * 1000000000 / freq.QuadPart;
            int64_t left = ns - spent > 0 ? ns - spent : 0;
            rem->tv_sec = left / 1000000000;
            rem->tv_nsec = left % 1000000000;
        }
        return px_fail(EINTR);
    }
    return 0;
}

int px_nanosleep(const struct timespec *req, struct timespec *rem) {
    if (req->tv_nsec < 0 || req->tv_nsec >= 1000000000 || req->tv_sec < 0)
        return px_fail(EINVAL);
    return sleep_ns((int64_t) req->tv_sec * 1000000000 + req->tv_nsec, rem);
}

int px_usleep(unsigned usec) {
    return sleep_ns((int64_t) usec * 1000, NULL);
}

unsigned px_sleep(unsigned sec) {
    struct timespec req = {sec, 0}, rem = {0, 0};
    if (px_nanosleep(&req, &rem) < 0)
        return (unsigned) rem.tv_sec;
    return 0;
}

// ---- SIGINT from the console, and kill()

static void (*sigint_handler)(int);

static BOOL WINAPI ctrl_handler(DWORD type) {
    if (type != CTRL_C_EVENT && type != CTRL_BREAK_EVENT)
        return FALSE;
    void (*h)(int) = sigint_handler;
    if (h == NULL) {
        extern void px_console_restore(void);
        px_console_restore();
        return FALSE; // default: terminate
    }
    h(2);
    return TRUE;
}

void px_set_sigint_handler(void (*handler)(int)) {
    static bool installed;
    sigint_handler = handler;
    if (!installed) {
        installed = true;
        SetConsoleCtrlHandler(ctrl_handler, TRUE);
    }
}

int px_spawn_kill(int pid, int sig);

int px_kill(int pid, int sig) {
    if (pid == (int) GetCurrentProcessId() || pid == 0) {
        if (sig == 0)
            return 0;
        if (sig == 2 /* SIGINT */ && sigint_handler != NULL) {
            sigint_handler(2);
            return 0;
        }
        if (sig == 10 /* SIGUSR1 */ || sig == 28 /* SIGWINCH */ || sig == 17 /* SIGCHLD */)
            return 0;
        extern void px_console_restore(void);
        px_console_restore();
        ExitProcess(128 + sig);
    }
    return px_spawn_kill(pid, sig);
}
