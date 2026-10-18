// The guest snapshot (Windows): when Minis finds a shell command quiet and
// idle for two minutes (its ShellHang watch), it sets the named event
// Local\ish-state-<host pid> and this writes, into $ISH_STATE_DIR, what the
// guest is doing - every task with the syscall it is blocked in and for how
// long, its files (pipes with their buffered bytes and whether the writer
// has gone, sockets, files), and for every pipe which tasks hold each end.
// One look then says who waits on which fd and who is on the other side.
//
// Only when ish.exe is started with ISH_STATE_DIR (Minis does it): a thread
// blocked on the event, nothing else while ish runs normally. The snapshot
// only reads: guest state, and host fds through px_describe (getsockname,
// FIONREAD, a zero-timeout select). It takes pids_lock if it can within 2 s
// and reads without it otherwise (a stuck guest is not changing), saying so.
#ifdef _WIN32

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "kernel/calls.h"
#include "kernel/task.h"
#include "fs/fd.h"

int px_describe(int fd, char *buf, size_t size, int *local_port, int *peer_port);
bool px_state_watch(void (*fn)(void));
uint64_t px_tick_ms(void);
unsigned long px_host_pid(void);
int px_rename_replace(const char *from, const char *to);

extern const struct fd_ops realfs_fdops;
extern const struct fd_ops socket_fdops;

uint64_t state_dump_now_ms(void) {
    return px_tick_ms();
}

// x86_64 syscall names, for the ones a stuck command is usually in.
static const char *sc_name(unsigned nr) {
    switch (nr) {
        case 0: return "read";
        case 1: return "write";
        case 2: return "open";
        case 3: return "close";
        case 7: return "poll";
        case 17: return "pread64";
        case 18: return "pwrite64";
        case 19: return "readv";
        case 20: return "writev";
        case 23: return "select";
        case 32: return "dup";
        case 33: return "dup2";
        case 34: return "pause";
        case 35: return "nanosleep";
        case 40: return "sendfile";
        case 42: return "connect";
        case 43: return "accept";
        case 44: return "sendto";
        case 45: return "recvfrom";
        case 46: return "sendmsg";
        case 47: return "recvmsg";
        case 56: return "clone";
        case 57: return "fork";
        case 58: return "vfork";
        case 59: return "execve";
        case 61: return "wait4";
        case 62: return "kill";
        case 72: return "fcntl";
        case 73: return "flock";
        case 128: return "rt_sigtimedwait";
        case 130: return "rt_sigsuspend";
        case 202: return "futex";
        case 230: return "clock_nanosleep";
        case 232: return "epoll_wait";
        case 247: return "waitid";
        case 257: return "openat";
        case 270: return "pselect6";
        case 271: return "ppoll";
        case 281: return "epoll_pwait";
        case 275: return "splice";
        case 276: return "tee";
        case 288: return "accept4";
        case 326: return "copy_file_range";
        case 435: return "clone3";
        default: return NULL;
    }
}

// A growing text buffer (the summary goes first, so the body is kept).
struct out {
    char *buf;
    size_t len, cap;
};

static void outf(struct out *o, const char *fmt, ...) {
    va_list ap;
    for (;;) {
        va_start(ap, fmt);
        int n = vsnprintf(o->buf ? o->buf + o->len : NULL, o->buf ? o->cap - o->len : 0, fmt, ap);
        va_end(ap);
        if (n < 0)
            return;
        if (o->buf && o->len + (size_t) n < o->cap) {
            o->len += n;
            return;
        }
        size_t cap = (o->cap ? o->cap * 2 : 65536) + n;
        char *b = realloc(o->buf, cap);
        if (b == NULL)
            return;
        o->buf = b;
        o->cap = cap;
    }
}

// One end of a pipe, for the pipe table at the end.
struct pipe_end {
    int local, peer;
    pid_t_ pid;
    int fd;
    bool write;
    char comm[16];
};

#define MAX_ENDS 4096
static struct pipe_end ends[MAX_ENDS];
static int nends;

static void describe_fd(struct out *o, struct task *t, int n, struct fd *fd) {
    char host[512] = "";
    int local = 0, peer = 0;
    const char *kind;
    if (fd->ops == &realfs_fdops || fd->ops == &socket_fdops) {
        kind = fd->ops == &socket_fdops ? "socket" : "real";
        if (fd->real_fd >= 0)
            px_describe(fd->real_fd, host, sizeof(host), &local, &peer);
    } else {
        // Linux S_IFMT values (the guest's).
        switch (fd->type & 0170000) {
            case 0020000: kind = "chardev"; break;
            case 0040000: kind = "dir"; break;
            case 0100000: kind = "file"; break;
            case 0010000: kind = "fifo"; break;
            case 0140000: kind = "socket"; break;
            default: kind = "other"; break;
        }
        snprintf(host, sizeof(host), "rdev=%#x", (unsigned) fd->stat.rdev);
    }
    if (fd->ops == &socket_fdops)
        outf(o, "    fd %d: socket domain=%d type=%d %s\n", n, fd->socket.domain, fd->socket.type, host);
    else
        outf(o, "    fd %d: %s %s\n", n, kind, host);
    bool is_pipe = strncmp(host, "pipe ", 5) == 0;
    if (is_pipe && nends < MAX_ENDS) {
        struct pipe_end *e = &ends[nends++];
        e->local = local;
        e->peer = peer;
        e->pid = t->pid;
        e->fd = n;
        e->write = strncmp(host, "pipe write", 10) == 0;
        memcpy(e->comm, t->comm, sizeof(e->comm));
        e->comm[sizeof(e->comm) - 1] = 0;
    }
}

static void describe_task(struct out *o, struct out *summary, struct task *t, uint64_t now) {
    pid_t_ ppid = t->parent ? t->parent->pid : 0;
    pid_t_ pgid = t->group ? t->group->pgid : 0;
    const char *state = t->zombie     ? "zombie"
                        : t->exiting  ? "exiting"
                        : t->blocking ? "blocked"
                        : t->sc_active ? "in syscall"
                                       : "running guest code";
    char comm[17];
    memcpy(comm, t->comm, 16);
    comm[16] = 0;
    outf(o, "task %d tgid=%d ppid=%d pgid=%d comm=%s state=%s", t->pid, t->tgid, ppid, pgid, comm, state);
    if (t->sc_active) {
        const char *name = sc_name(t->sc_nr);
        uint64_t secs = now >= t->sc_since_ms ? (now - t->sc_since_ms) / 1000 : 0;
        char call[160];
        if (name)
            snprintf(call, sizeof(call), "%s(%#llx, %#llx, %#llx)", name, (unsigned long long) t->sc_args[0],
                     (unsigned long long) t->sc_args[1], (unsigned long long) t->sc_args[2]);
        else
            snprintf(call, sizeof(call), "syscall %u(%#llx, %#llx, %#llx)", t->sc_nr,
                     (unsigned long long) t->sc_args[0], (unsigned long long) t->sc_args[1],
                     (unsigned long long) t->sc_args[2]);
        outf(o, " in %s for %llus", call, (unsigned long long) secs);
        if (secs >= 1)
            outf(summary, "%s pid %d (%s) %s %llus", summary->len ? ";" : "", t->pid, comm,
                 name ? name : "syscall", (unsigned long long) secs);
    }
    outf(o, "\n");
    // argv, for a process (threads share it).
    if (t->pid == t->tgid && t->mm && !t->zombie && t->mm->argv_end > t->mm->argv_start) {
        size_t size = t->mm->argv_end - t->mm->argv_start;
        if (size > 512)
            size = 512;
        char argv[513];
        if (user_read_task(t, t->mm->argv_start, argv, size) == 0) {
            for (size_t i = 0; i + 1 < size; i++)
                if (argv[i] == 0)
                    argv[i] = ' ';
            argv[size] = 0;
            outf(o, "  argv: %s\n", argv);
        }
    }
    if (t->pid == t->tgid && t->files) {
        struct fdtable *ft = t->files;
        for (unsigned n = 0; n < ft->size; n++)
            if (ft->files[n])
                describe_fd(o, t, n, ft->files[n]);
    }
}

static void pipes_table(struct out *o) {
    outf(o, "\npipes (each end: pid/comm/fd):\n");
    for (int i = 0; i < nends; i++) {
        struct pipe_end *a = &ends[i];
        if (a->local == 0)
            continue;
        int lo = a->local < a->peer ? a->local : a->peer, hi = a->local < a->peer ? a->peer : a->local;
        // Printed once per pipe: from its first end in the list.
        bool seen = false;
        for (int j = 0; j < i && !seen; j++) {
            int l2 = ends[j].local < ends[j].peer ? ends[j].local : ends[j].peer;
            int h2 = ends[j].local < ends[j].peer ? ends[j].peer : ends[j].local;
            seen = l2 == lo && h2 == hi;
        }
        if (seen)
            continue;
        outf(o, "  pipe %d-%d\n    read end:", lo, hi);
        int readers = 0, writers = 0;
        for (int j = 0; j < nends; j++) {
            int l2 = ends[j].local < ends[j].peer ? ends[j].local : ends[j].peer;
            int h2 = ends[j].local < ends[j].peer ? ends[j].peer : ends[j].local;
            if (l2 == lo && h2 == hi && !ends[j].write) {
                outf(o, " %d/%s/%d", ends[j].pid, ends[j].comm, ends[j].fd);
                readers++;
            }
        }
        outf(o, readers ? "\n    write end:" : " (no task holds it)\n    write end:");
        for (int j = 0; j < nends; j++) {
            int l2 = ends[j].local < ends[j].peer ? ends[j].local : ends[j].peer;
            int h2 = ends[j].local < ends[j].peer ? ends[j].peer : ends[j].local;
            if (l2 == lo && h2 == hi && ends[j].write) {
                outf(o, " %d/%s/%d", ends[j].pid, ends[j].comm, ends[j].fd);
                writers++;
            }
        }
        outf(o, writers ? "\n" : " (no task holds it: readers see EOF once it drains)\n");
    }
}

static void state_dump(void) {
    const char *dir = getenv("ISH_STATE_DIR");
    if (dir == NULL || *dir == 0)
        return;
    uint64_t now = px_tick_ms();
    struct out body = {0}, summary = {0};
    nends = 0;
    bool locked = false;
    for (int i = 0; i < 200 && !locked; i++) {
        if (trylock(&pids_lock) == 0)
            locked = true;
        else
            usleep(10000);
    }
    int tasks = 0;
    for (dword_t id = 1; id <= MAX_PID; id++) {
        struct pid *p = pid_get(id);
        if (p == NULL || p->task == NULL)
            continue;
        describe_task(&body, &summary, p->task, now);
        tasks++;
    }
    pipes_table(&body);
    if (locked)
        unlock(&pids_lock);

    char tmp[1024], path[1024];
    snprintf(path, sizeof(path), "%s\\ish-state-%lu.txt", dir, px_host_pid());
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (f != NULL) {
        fprintf(f, "summary: %d tasks%s%s%s\n", tasks, summary.len ? "; in a syscall 1s+:" : "",
                summary.buf ? summary.buf : "", locked ? "" : " (read without pids_lock: it was held)");
        fprintf(f, "ish guest snapshot, host pid %lu, uptime %llus\n\n", px_host_pid(),
                (unsigned long long) (now / 1000));
        if (body.buf)
            fwrite(body.buf, 1, body.len, f);
        fclose(f);
        px_rename_replace(tmp, path);
    }
    free(body.buf);
    free(summary.buf);
}

void state_dump_init(void) {
    const char *dir = getenv("ISH_STATE_DIR");
    if (dir != NULL && *dir != 0)
        px_state_watch(state_dump);
}

#endif
