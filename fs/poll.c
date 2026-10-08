#include "kernel/task.h"
#include <string.h>
#include <poll.h>
#include <fcntl.h>
#include <limits.h>
#include <time.h>
#include <sys/stat.h>
#include <stdio.h>
#include "misc.h"
#include "util/list.h"
#include "kernel/errno.h"
#include "kernel/fs.h"
#include "fs/fd.h"
#include "fs/poll.h"
#include "fs/real.h"

#include "fs/sockrestart.h"

// From kernel/calls.h - avoid circular include
extern _Noreturn void do_exit_group(int status);

#if defined(__linux__)
#include <sys/epoll.h>
#define HAVE_EPOLL 1
#elif defined(__APPLE__)
#include <sys/event.h>
#define HAVE_KQUEUE 1
#endif

static int real_poll_init(struct real_poll *real);
static void real_poll_close(struct real_poll *real);
struct real_poll_event {
#if HAVE_EPOLL
    struct epoll_event real;
#elif HAVE_KQUEUE
    struct kevent real;
#endif
};
static void *rpe_data(struct real_poll_event *rpe);
static int rpe_events(struct real_poll_event *rpe);
static int real_poll_wait(struct real_poll *real, struct real_poll_event *events, int max, struct timespec *timeout);
static int real_poll_update(struct real_poll *real, int fd, int types, void *data);

/// Safe close that skips fds 0-2 (stdin/stdout/stderr).
/// iOS guards these descriptors — closing them triggers EXC_GUARD and kills the app.
/// The iSH kernel operates on host fds that should never be 0/1/2, but race
/// conditions during fd reuse can occasionally produce them.
static inline void safe_close(int fd) {
    if (fd > 2)
        close(fd);
}

// lock order: fd, then poll

struct poll *poll_create() {
    struct poll *poll = malloc(sizeof(struct poll));
    if (poll == NULL)
        return ERR_PTR(_ENOMEM);
    int err = real_poll_init(&poll->real);
    if (err < 0)
        return ERR_PTR(errno_map());
    poll->waiters = 0;
    poll->notify_pipe[0] = -1;
    poll->notify_pipe[1] = -1;
    list_init(&poll->poll_fds);
    list_init(&poll->pollfd_freelist);
    lock_init(&poll->lock);
    return poll;
}

static inline bool poll_fd_is_real(struct poll_fd *pollfd) {
    return pollfd->fd->ops->poll == realfs_poll;
}

// does not do its own locking
// Match on the (struct fd, guest fd number) pair — see fd_no in fs/poll.h.
static struct poll_fd *poll_find_fd(struct poll *poll, struct fd *fd, int fd_no) {
    struct poll_fd *poll_fd, *tmp;
    list_for_each_entry_safe(&poll->poll_fds, poll_fd, tmp, fds) {
        if (poll_fd->fd == fd && poll_fd->fd_no == fd_no)
            return poll_fd;
    }
    return NULL;
}

// [T-ish-poll-spin-guard] Switches (see /proc/ish/poll_spin) and counters.
static _Atomic bool poll_guard_regfile = true;
static _Atomic bool poll_guard_hupedge = true;
static _Atomic bool poll_guard_deadline = true;
static _Atomic bool poll_guard_backoff = true;
static _Atomic uint64_t poll_stat_spurious;      // host wakeups that found nothing ready
static _Atomic uint64_t poll_stat_spins;         // waits that reached POLL_SPIN_GRACE
static _Atomic uint64_t poll_stat_backoffs;      // sleeps taken by the backoff
static _Atomic uint64_t poll_stat_deadline;      // waits ended by their deadline after spurious wakeups
static _Atomic uint64_t poll_stat_regfile_skips; // host registrations skipped for files/dirs

// Whether the host should watch this fd at all. A regular file or directory
// is always ready for reading and writing and never hangs up, so the guest
// check sees everything it can report without the host's help. kqueue,
// however, fires EVFILT_READ on a vnode whenever its offset is short of its
// size - NOTE_LOWAT (our "hangup only" marker) is ignored for vnodes - so a
// poll that wants only HUP/ERR on such an fd (events=0, as the Rust runtime
// probes fds 0-2 before main) would be woken for nothing, forever: `rg x <
// file`, or a log another process keeps appending to. Pipes, sockets and
// character devices keep their registration.
static bool poll_fd_wants_host(struct fd *fd) {
    if (!poll_guard_regfile)
        return true;
    struct stat st;
    if (fstat(fd->real_fd, &st) < 0)
        return true;
    if (S_ISREG(st.st_mode) || S_ISDIR(st.st_mode)) {
        poll_stat_regfile_skips++;
        return false;
    }
    return true;
}

// Recompute the host (real) registration for `fd` in this poll as the UNION
// of the event types of every registration referring to it. Several guest
// registrations (different fd numbers, same description after dup) share one
// host registration, since the host backend is keyed by real_fd. Passing 0
// types deletes the host registration. poll->lock must be held.
static int poll_real_refresh(struct poll *poll, struct fd *fd) {
    int types = 0;
    struct poll_fd *pf, *first = NULL;
    list_for_each_entry(&poll->poll_fds, pf, fds) {
        if (pf->fd == fd) {
            if (first == NULL)
                first = pf;
            if (!pf->oneshot_fired)
                types |= pf->types;
        }
    }
    if (types != 0 && !poll_fd_wants_host(fd))
        types = 0;
    return real_poll_update(&poll->real, fd->real_fd, types, first);
}

// See comment on pollfd_freelist for context
static void poll_fd_free(struct poll_fd *poll_fd) {
    struct poll *poll = poll_fd->poll;
    memset(poll_fd, 0xba, sizeof(*poll_fd));
    poll_fd->poll = NULL; // used to mark it as free
    list_add(&poll->pollfd_freelist, &poll_fd->fds);
}

bool poll_has_fd(struct poll *poll, struct fd *fd, int fd_no) {
    return poll_find_fd(poll, fd, fd_no) != NULL;
}

int poll_add_fd(struct poll *poll, struct fd *fd, int fd_no, int types, union poll_fd_info info) {
    int err;
    lock(&fd->poll_lock);
    lock(&poll->lock);

    struct poll_fd *poll_fd;
    if (!list_empty(&poll->pollfd_freelist)) {
        poll_fd = list_first_entry(&poll->pollfd_freelist, struct poll_fd, fds);
        list_remove(&poll_fd->fds);
    } else {
        poll_fd = malloc(sizeof(struct poll_fd));
        if (poll_fd == NULL) {
            err = _ENOMEM;
            goto out;
        }
    }
    poll_fd->fd = fd;
    poll_fd->fd_no = fd_no;
    poll_fd->poll = poll;
    poll_fd->types = types;
    poll_fd->info = info;
    poll_fd->triggered_types = 0;
    poll_fd->oneshot_fired = false;

    // Link first, then refresh the (shared) host registration to the union of
    // all registrations of this description in this poll.
    list_add(&fd->poll_fds, &poll_fd->polls);
    list_add(&poll->poll_fds, &poll_fd->fds);

    if (poll_fd_is_real(poll_fd)) {
        err = poll_real_refresh(poll, fd);
        if (err < 0) {
            list_remove(&poll_fd->polls);
            list_remove(&poll_fd->fds);
            poll_fd_free(poll_fd);
            err = errno_map();
            goto out;
        }
    }

    err = 0;
out:
    unlock(&poll->lock);
    unlock(&fd->poll_lock);
    return err;
}

int poll_del_fd(struct poll *poll, struct fd *fd, int fd_no) {
    int err;
    lock(&fd->poll_lock);
    lock(&poll->lock);
    struct poll_fd *poll_fd = poll_find_fd(poll, fd, fd_no);
    if (poll_fd == NULL) {
        err = _ENOENT;
        goto out;
    }

    bool is_real = poll_fd_is_real(poll_fd);
    list_remove(&poll_fd->polls);
    list_remove(&poll_fd->fds);
    poll_fd_free(poll_fd);

    // Refresh (or delete, if this was the last registration) the shared host
    // registration to the union of the remaining registrations.
    if (is_real) {
        err = poll_real_refresh(poll, fd);
        if (err < 0) {
            err = errno_map();
            goto out;
        }
    }

    err = 0;
out:
    unlock(&poll->lock);
    unlock(&fd->poll_lock);
    return err;
}

int poll_mod_fd(struct poll *poll, struct fd *fd, int fd_no, int types, union poll_fd_info info) {
    int err;
    lock(&fd->poll_lock);
    lock(&poll->lock);
    struct poll_fd *poll_fd = poll_find_fd(poll, fd, fd_no);
    if (poll_fd == NULL) {
        err = _ENOENT;
        goto out;
    }

    poll_fd->types = types;
    poll_fd->info = info;
    poll_fd->triggered_types &= types;
    poll_fd->oneshot_fired = false; // EPOLL_CTL_MOD re-arms a fired oneshot

    if (poll_fd_is_real(poll_fd)) {
        err = poll_real_refresh(poll, fd);
        if (err < 0) {
            err = errno_map();
            goto out;
        }
    }

    err = 0;
out:
    unlock(&poll->lock);
    unlock(&fd->poll_lock);
    return err;
}

void poll_cleanup_fd(struct fd *fd) {
    lock(&fd->poll_lock);
    struct poll_fd *poll_fd, *tmp;
    list_for_each_entry_safe(&fd->poll_fds, poll_fd, tmp, polls) {
        lock(&poll_fd->poll->lock);
        if (poll_fd_is_real(poll_fd))
            real_poll_update(&poll_fd->poll->real, fd->real_fd, 0, poll_fd);
        list_remove(&poll_fd->polls);
        list_remove(&poll_fd->fds);
        unlock(&poll_fd->poll->lock);
        poll_fd_free(poll_fd);
    }
    unlock(&fd->poll_lock);
}

// Diagnostic: dump each member fd of a poll set with its current poll state.
void poll_dump_members(struct poll *poll) {
    lock(&poll->lock);
    struct poll_fd *poll_fd;
    list_for_each_entry(&poll->poll_fds, poll_fd, fds) {
        struct fd *fd = poll_fd->fd;
        int ready = (fd->ops && fd->ops->poll) ? fd->ops->poll(fd) : -1;
        fprintf(stderr, "  [epoll-member] fd_no=%d type=%#x want=%#x ready=%#x triggered=%#x real_fd=%d\n",
                poll_fd->fd_no, fd->type, poll_fd->types, ready,
                poll_fd->triggered_types, fd->real_fd);
    }
    unlock(&poll->lock);
}

void poll_wakeup(struct fd *fd, int events) {
    struct poll_fd *poll_fd;
    lock(&fd->poll_lock);
    list_for_each_entry(&fd->poll_fds, poll_fd, polls) {
        struct poll *poll = poll_fd->poll;
        lock(&poll->lock);
        if (poll_fd->oneshot_fired) {
            unlock(&poll->lock);
            continue;
        }
        if (poll_fd->types & POLL_EDGETRIGGERED)
            poll_fd->triggered_types &= ~events;
        if (poll->notify_pipe[1] != -1)
            write(poll->notify_pipe[1], "", 1);
        unlock(&poll->lock);
        // oneshot?
    }
    unlock(&fd->poll_lock);
}

// [T-ish-poll-spin-guard] A wait that keeps waking without anything becoming
// ready must not turn into a host busy loop. The host poll can report an
// fd as ready while the guest fd's own poll() reports nothing the caller
// asked for (a level-triggered EOF/HUP on a pipe or socket is the usual
// shape), so every pass returns from kevent at once, re-scans, finds nothing
// and waits again — all inside ONE guest syscall. The thread then burns 100%
// of a core on the host side, and the background CPU governor cannot brake
// it: its throttle hook only runs between guest blocks, which this thread
// never reaches. Field case 2026-10-07/08: a ripgrep thread sat in ppoll at
// ~100% CPU with its syscall count frozen for minutes, the governor reached
// RED with 3% duty and no effect, and iOS killed the app for exceeding its
// background CPU budget ~50s after it went to the background.
//
// After POLL_SPIN_GRACE wakeups in a row that produced nothing, each further
// pass sleeps first (1ms, then 10ms, then 50ms). A wait that genuinely becomes
// ready breaks out on the next scan, so the cost is at most one sleep of
// latency, and only for a wait that was already spinning. Signals are still
// checked on every pass. The first stretch of a spin is logged once with
// what the host reported, so the misbehaving fd type can be found.
#define POLL_SPIN_GRACE 64
#define POLL_SPIN_LOG_AT 256

static void poll_spin_backoff(unsigned spurious) {
    if (!poll_guard_backoff)
        return;
    poll_stat_backoffs++;
    long ns = spurious < 256 ? 1000000L : (spurious < 2048 ? 10000000L : 50000000L);
    struct timespec ts = {.tv_sec = 0, .tv_nsec = ns};
    nanosleep(&ts, NULL);
}

// Called with poll_->lock held.
static void poll_spin_report(struct poll *poll_, unsigned spurious,
                             struct real_poll_event *last, int last_n) {
    char comm[17];
    memcpy(comm, current->comm, 16);
    comm[16] = '\0';
    printk("POLL-SPIN: pid=%d (%s) %u wakeups in a row with nothing ready; %s\n",
           current->pid, comm, spurious,
           poll_guard_backoff ? "backing off" : "backoff DISABLED, spinning");
    for (int i = 0; i < last_n; i++) {
        struct poll_fd *pfd = rpe_data(&last[i]);
        if (pfd == NULL) {
            printk("POLL-SPIN:   host event %#x on the notify pipe\n", rpe_events(&last[i]));
            continue;
        }
        // A registration removed since the event was queued sits on the
        // freelist with a stale fd pointer (see struct poll): do not touch it.
        if (pfd->poll == NULL) {
            printk("POLL-SPIN:   host event %#x on a removed registration\n", rpe_events(&last[i]));
            continue;
        }
        int ready = (pfd->fd != NULL && pfd->fd->ops->poll) ? pfd->fd->ops->poll(pfd->fd) : -1;
        printk("POLL-SPIN:   host event %#x on guest fd %d: wants %#x, fd poll says %#x, edge-fired %#x, oneshot %d\n",
               rpe_events(&last[i]), pfd->fd_no, pfd->types, ready,
               pfd->triggered_types, pfd->oneshot_fired);
    }
    struct poll_fd *pfd;
    int listed = 0;
    list_for_each_entry(&poll_->poll_fds, pfd, fds) {
        if (++listed > 8) break;
        int ready = (pfd->fd != NULL && pfd->fd->ops->poll) ? pfd->fd->ops->poll(pfd->fd) : -1;
        printk("POLL-SPIN:   registered guest fd %d: wants %#x, fd poll says %#x\n",
               pfd->fd_no, pfd->types, ready);
    }
}

int poll_wait(struct poll *poll_, poll_callback_t callback, void *context, struct timespec *timeout) {
    lock(&poll_->lock);

    // acquire the pipe
    if (poll_->waiters++ == 0) {
        assert(poll_->notify_pipe[0] == -1 && poll_->notify_pipe[1] == -1);
        if (pipe(poll_->notify_pipe) < 0) {
            unlock(&poll_->lock);
            return errno_map();
        }
        fcntl(poll_->notify_pipe[0], F_SETFL, O_NONBLOCK);
        fcntl(poll_->notify_pipe[1], F_SETFL, O_NONBLOCK);
        real_poll_update(&poll_->real, poll_->notify_pipe[0], POLL_READ, NULL);
    }

    // TODO this is pretty broken with regards to timeouts
    int res = 0;
    // [T-ish-poll-spin-guard] Consecutive host wakeups that left nothing ready.
    unsigned spurious = 0;
    // A finite wait ends when its time is up. The loop below only treats a
    // host wait that returned NO events as a timeout, so a host that keeps
    // reporting something the guest check then rejects never let a finite
    // wait end - with timeout 0 not even after the first pass.
    uint64_t deadline_ns = 0;
    if (timeout != NULL) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        deadline_ns = (uint64_t) now.tv_sec * 1000000000ULL + now.tv_nsec
            + (uint64_t) timeout->tv_sec * 1000000000ULL + timeout->tv_nsec;
    }
    bool woke = false;
    struct real_poll_event last_events[4];
    int last_n = 0;
    while (true) {
        // check if any fds are ready
        struct poll_fd *poll_fd, *tmp;
        list_for_each_entry_safe(&poll_->poll_fds, poll_fd, tmp, fds) {
            if (poll_fd->oneshot_fired)
                continue;
            struct fd *fd = poll_fd->fd;
            int poll_types = 0;
            if (fd->ops->poll)
                poll_types = fd->ops->poll(fd);
            poll_types &= poll_fd->types | POLL_HUP | POLL_ERR;
            if (poll_fd->types & POLL_EDGETRIGGERED) {
                poll_types &= ~poll_fd->triggered_types;
            }
            if (poll_types) {
                if (callback(context, poll_types, poll_fd->info) == 1)
                    res++;

                // The real poll does not actually get the FDs set as oneshot.
                // But this loop is done while holding the lock, so only one
                // thread can get each oneshot event. This doesn't solve the
                // thundering herd problem at all, but at least the semantics
                // are right. I'll just leave that as a TODO.
                if (poll_fd->types & POLL_ONESHOT) {
                    // Disable in place; do NOT unlink. We hold only
                    // poll->lock here, but poll_fd->polls is walked by
                    // poll_wakeup under fd->poll_lock — unlinking/freeing
                    // raced it and crashed the tty input thread (SIGSEGV in
                    // poll_wakeup when bun registers stdin with
                    // EPOLLONESHOT). Linux semantics also keep a fired
                    // oneshot registered, disabled until EPOLL_CTL_MOD.
                    poll_fd->oneshot_fired = true;
                    if (poll_fd_is_real(poll_fd)) {
                        // Refresh the shared host registration to the union
                        // of the still-armed registrations.
                        poll_real_refresh(poll_, fd);
                    }
                }

                if (poll_fd->types & POLL_EDGETRIGGERED) {
                    poll_fd->triggered_types |= poll_types;
                }
            }
        }
        if (res > 0)
            break;

        bool signal_pending = false;
        if (current->sighand != NULL) {
            lock(&current->sighand->lock);
            signal_pending = !!(current->pending & ~current->blocked);
            unlock(&current->sighand->lock);
        }
        if (signal_pending) {
            res = _EINTR;
            break;
        }

        // [T-ish-poll-spin-guard] The last wakeup produced nothing ready.
        if (woke) {
            woke = false;
            poll_stat_spurious++;
            if (++spurious == POLL_SPIN_GRACE)
                poll_stat_spins++;
            if (spurious == POLL_SPIN_LOG_AT)
                poll_spin_report(poll_, spurious, last_events, last_n);
            if (timeout != NULL && poll_guard_deadline) {
                struct timespec now;
                clock_gettime(CLOCK_MONOTONIC, &now);
                if ((uint64_t) now.tv_sec * 1000000000ULL + now.tv_nsec >= deadline_ns) {
                    poll_stat_deadline++;
                    break; // res is 0: timed out
                }
            }
        }

        // wait for a ready notification
        list_for_each_entry(&poll_->poll_fds, poll_fd, fds) {
            sockrestart_begin_listen_wait(poll_fd->fd);
        }
        unlock(&poll_->lock);
        if (spurious >= POLL_SPIN_GRACE)
            poll_spin_backoff(spurious);
        int err;
        int saved_errno;
        struct real_poll_event e[4];
        // Use a bounded timeout to avoid indefinite blocks. Guest signal
        // delivery via SIGUSR1 wakes kevent, but if the signal arrives
        // between our pending check and kevent entry, we'd block forever.
        // A 5-second cap ensures we re-check signals periodically.
        // Cap all waits to 1 second to avoid macOS condvar issues.
        // pthread_cond_timedwait_relative_np can block forever under
        // thread contention. Short caps ensure we re-check periodically.
        struct timespec bounded_timeout = {.tv_sec = 1, .tv_nsec = 0};
        struct timespec *wait_timeout;
        if (timeout == NULL) {
            wait_timeout = &bounded_timeout;
        } else if (timeout->tv_sec >= 1) {
            wait_timeout = &bounded_timeout; // Cap long timeouts
        } else {
            wait_timeout = timeout; // Short/zero timeouts pass through
        }
        current->blocking = true;
        do {
            err = real_poll_wait(&poll_->real, e, sizeof(e)/sizeof(e[0]), wait_timeout);
            saved_errno = errno;  // save immediately before anything clobbers it
        } while (saved_errno == EINTR && sockrestart_should_restart_listen_wait());
        current->blocking = false;
        // Only update last_unblocked_ns when actual events were received.
        // Timeout returns (err==0) don't count as real progress — the poll_wait
        // loop is just cycling. This prevents the deadlock detector from being
        // fooled by idle poll_wait loops during exit cleanup.
        if (err > 0) {
            struct timespec _ts;
            clock_gettime(CLOCK_MONOTONIC, &_ts);
            uint64_t now = (uint64_t)_ts.tv_sec * 1000000000ULL + _ts.tv_nsec;
            current->last_unblocked_ns = now;
            atomic_store_explicit(&current->group->last_progress_ns, now,
                                  memory_order_relaxed);
        }
        // If we timed out from our bounded timeout, loop back to re-check
        // fd readiness and signal pending.
        if (err == 0 && wait_timeout == &bounded_timeout) {
            spurious = 0;
            lock(&poll_->lock);
            list_for_each_entry(&poll_->poll_fds, poll_fd, fds) {
                sockrestart_end_listen_wait(poll_fd->fd);
            }
            // Check for group exit so blocking threads unblock promptly
            if (current->group->doing_group_exit) {
                res = _EINTR;
                break;
            }
            // Safety valve: if no thread in this process group has
            // done real work for >60s and there are no live child
            // processes, force exit. Catches V8/libuv exit cleanup
            // hangs where the event loop spins idle forever.
            //
            // Exceptions:
            //   - pid 1 (init): legitimately idles, killing halts the system
            //   - processes with a controlling TTY: interactive shells idle
            //     waiting for user input and must not be killed
            if (current->pid != 1 && current->group->tty == NULL) {
                struct timespec _now;
                clock_gettime(CLOCK_MONOTONIC, &_now);
                uint64_t now_ns = (uint64_t)_now.tv_sec * 1000000000ULL + _now.tv_nsec;
                uint64_t last = atomic_load_explicit(
                    &current->group->last_progress_ns, memory_order_relaxed);
                int64_t idle_s = (int64_t)(now_ns - last) / 1000000000LL;
                if (idle_s >= 60) {
                    bool has_live_children = false;
                    int thread_count = 0;
                    lock(&pids_lock);
                    lock(&current->group->lock);
                    struct task *t_iter;
                    list_for_each_entry(&current->group->threads, t_iter, group_links) {
                        thread_count++;
                        struct task *child;
                        list_for_each_entry(&t_iter->children, child, siblings) {
                            if (child->group != current->group && !child->zombie)
                                has_live_children = true;
                        }
                    }
                    unlock(&current->group->lock);
                    unlock(&pids_lock);
                    if (!has_live_children) {
                        if (ish_exec_trace())
                            printk("SAFETY-VALVE[poll]: pid=%d idle %llds, %d threads → exit_group\n",
                                   current->pid, (long long)idle_s, thread_count);
                        do_exit_group(0);
                    }
                }
            }
            if (timeout != NULL) {
                // Timed wait: subtract elapsed time
                timeout->tv_sec -= 1;
                if (timeout->tv_sec < 0) {
                    // Original timeout expired
                    break;
                }
            }
            continue;
        }
        lock(&poll_->lock);
        list_for_each_entry(&poll_->poll_fds, poll_fd, fds) {
            sockrestart_end_listen_wait(poll_fd->fd);
        }

        if (err < 0) {
            errno = saved_errno;
            res = errno_map();
            break;
        }
        if (err == 0) {
            // timed out and still nobody is ready
            break;
        }

        // dead with any edge-triggered notifications
        for (int i = 0; i < err; i++) {
            struct poll_fd *triggered_poll_fd = rpe_data(&e[i]);
            if (triggered_poll_fd != NULL && triggered_poll_fd->poll != NULL &&
                    triggered_poll_fd->types & POLL_EDGETRIGGERED) {
                triggered_poll_fd->triggered_types &= ~rpe_events(&e[i]);
            }
        }

        woke = true;
        last_n = err < 4 ? err : 4;
        memcpy(last_events, e, sizeof(e[0]) * last_n);

        char fuck;
        if (read(poll_->notify_pipe[0], &fuck, 1) < 0 && errno != EAGAIN) {
            res = errno_map();
            break;
        }
    }

    // release the pipe
    if (--poll_->waiters == 0) {
        real_poll_update(&poll_->real, poll_->notify_pipe[0], 0, NULL);
        safe_close(poll_->notify_pipe[0]);
        safe_close(poll_->notify_pipe[1]);
        poll_->notify_pipe[0] = -1;
        poll_->notify_pipe[1] = -1;
    }

    unlock(&poll_->lock);
    return res;
}

void poll_spin_describe(char *out, size_t size) {
    snprintf(out, size,
             "regfile %d\n"
             "hupedge %d\n"
             "deadline %d\n"
             "backoff %d\n"
             "spurious_wakeups %llu\n"
             "spinning_waits %llu\n"
             "backoff_sleeps %llu\n"
             "deadline_returns %llu\n"
             "regfile_skips %llu\n",
             (int) poll_guard_regfile, (int) poll_guard_hupedge, (int) poll_guard_deadline, (int) poll_guard_backoff,
             (unsigned long long) poll_stat_spurious, (unsigned long long) poll_stat_spins,
             (unsigned long long) poll_stat_backoffs, (unsigned long long) poll_stat_deadline,
             (unsigned long long) poll_stat_regfile_skips);
}

int poll_spin_control(const char *cmd, size_t len) {
    char buf[64];
    if (len >= sizeof(buf))
        return -1;
    memcpy(buf, cmd, len);
    buf[len] = '\0';
    char name[16];
    int value;
    if (sscanf(buf, " %15[a-z] = %d", name, &value) != 2 &&
            sscanf(buf, " %15[a-z] %d", name, &value) != 2)
        return -1;
    if (value != 0 && value != 1)
        return -1;
    if (strcmp(name, "regfile") == 0)
        poll_guard_regfile = value;
    else if (strcmp(name, "hupedge") == 0)
        poll_guard_hupedge = value;
    else if (strcmp(name, "deadline") == 0)
        poll_guard_deadline = value;
    else if (strcmp(name, "backoff") == 0)
        poll_guard_backoff = value;
    else
        return -1;
    printk("POLL-SPIN: guard %s set to %d\n", name, value);
    return 0;
}

void poll_destroy(struct poll *poll) {
    struct poll_fd *poll_fd;
    struct poll_fd *tmp;
    list_for_each_entry_safe(&poll->poll_fds, poll_fd, tmp, fds) {
        lock(&poll_fd->fd->poll_lock);
        list_remove(&poll_fd->polls);
        list_remove(&poll_fd->fds);
        unlock(&poll_fd->fd->poll_lock);
        free(poll_fd);
    }

    list_for_each_entry_safe(&poll->pollfd_freelist, poll_fd, tmp, fds) {
        list_remove(&poll_fd->fds);
        free(poll_fd);
    }

    real_poll_close(&poll->real);
    free(poll);
}

// Platform-specific real_poll implementations

#if HAVE_EPOLL

static int real_poll_init(struct real_poll *real) {
    real->fd = epoll_create1(0);
    if (real->fd < 0)
        return -1;
    return 0;
}

static int real_poll_wait(struct real_poll *real, struct real_poll_event *events, int max, struct timespec *timeout) {
    int timeout_millis = -1;
    if (timeout != NULL)
        timeout_millis = timeout->tv_sec * 1000 + timeout->tv_nsec / 1000000;
    return epoll_wait(real->fd, (struct epoll_event *) events, max, timeout_millis);
}

static int real_poll_update(struct real_poll *real, int fd, int types, void *data) {
    types &= ~EPOLLONESHOT;
    if (types == 0)
        return epoll_ctl(real->fd, EPOLL_CTL_DEL, fd, NULL);
    struct epoll_event epevent = {.events = types, .data.ptr = data};
    int err = epoll_ctl(real->fd, EPOLL_CTL_MOD, fd, &epevent);
    if (err < 0 && errno == ENOENT)
        err = epoll_ctl(real->fd, EPOLL_CTL_ADD, fd, &epevent);
    return err;
}

static void *rpe_data(struct real_poll_event *rpe) {
    return rpe->real.data.ptr;
}
static int rpe_events(struct real_poll_event *rpe) {
    return rpe->real.events;
}

#elif HAVE_KQUEUE

static int real_poll_init(struct real_poll *real) {
    real->fd = kqueue();
    if (real->fd < 0)
        return -1;
    return 0;
}

static int real_poll_update(struct real_poll *real, int fd, int types, void *data) {
    struct kevent e[3] = {
        {.filter = EVFILT_READ, .flags = types & (POLL_READ | POLL_HUP) ? EV_ADD : EV_DELETE},
        {.filter = EVFILT_WRITE, .flags = types & POLL_WRITE ? EV_ADD : EV_DELETE},
        {.filter = EVFILT_EXCEPT, .flags = types & POLL_ERR ? EV_ADD : EV_DELETE},
    };
    if (!(types & POLL_READ) && types & POLL_HUP) {
        // Set the low water mark really high so we'll only get woken up on a hangup
        e[0].fflags = NOTE_LOWAT;
        e[0].data = INT_MAX;
        // [T-ish-poll-spin-guard] xnu clamps the low water mark of a pipe to
        // its buffer size, so a FULL pipe fires EVFILT_READ anyway, level-
        // triggered, while the guest check (rightly) reports no hangup. A
        // reader that probes before reading - the Rust runtime polls fds 0-2
        // with events=0 before main() - then waits forever on a pipe its
        // upstream filled first, and the upstream stays blocked on that full
        // pipe: `head -n 7604 big.py | rg ...` hung at 100% CPU in the field.
        // Edge-triggered, the full buffer wakes the wait once; a real hangup
        // is still caught by the guest-side scan, which runs before every
        // wait, so nothing is missed.
        if (poll_guard_hupedge)
            e[0].flags |= EV_CLEAR;
    }
    for (int i = 0; i < 3; i++) {
        e[i].ident = fd;
        e[i].udata = data;
        e[i].flags |= EV_RECEIPT;
        if (types & POLL_EDGETRIGGERED)
            e[i].flags |= EV_CLEAR;
    }

    return kevent(real->fd, e, 3, e, 3, NULL);
}

static int real_poll_wait(struct real_poll *real, struct real_poll_event *events, int max, struct timespec *timeout) {
    return kevent(real->fd, NULL, 0, (struct kevent *) events, max, timeout);
}

static void *rpe_data(struct real_poll_event *rpe) {
    return rpe->real.udata;
}
static int rpe_events(struct real_poll_event *rpe) {
    if (rpe->real.filter == EVFILT_READ) {
        int events = 0;
        if (rpe->real.data > 0)
            events |= POLL_READ;
        if (rpe->real.flags & EV_EOF)
            events |= POLL_HUP;
        return events;
    }
    if (rpe->real.filter == EVFILT_WRITE) return POLL_WRITE;
    if (rpe->real.filter == EVFILT_EXCEPT) return POLL_ERR;
    return 0;
}

#endif

static void real_poll_close(struct real_poll *real) {
    safe_close(real->fd);
}

