// The px fd table, file I/O, pipes and the stdio/console bridge.
#include "px_impl.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#define PX_MAX_FDS 65536

struct px_slot {
    struct px_desc *d;
    bool cloexec;
};
static struct px_slot table[PX_MAX_FDS];
static SRWLOCK table_lock = SRWLOCK_INIT;

int px_errno_win(DWORD err) {
    switch (err) {
        case ERROR_FILE_NOT_FOUND:
        case ERROR_PATH_NOT_FOUND:
        case ERROR_INVALID_DRIVE:
        case ERROR_BAD_NETPATH:
        case ERROR_BAD_PATHNAME:
        case ERROR_INVALID_NAME:
        case ERROR_DELETE_PENDING:
        case ERROR_NOT_FOUND:
            return ENOENT;
        case ERROR_ACCESS_DENIED:
        case ERROR_SHARING_VIOLATION:
        case ERROR_WRITE_PROTECT:
            return EACCES;
        case ERROR_PRIVILEGE_NOT_HELD:
            return EPERM;
        case ERROR_FILE_EXISTS:
        case ERROR_ALREADY_EXISTS:
            return EEXIST;
        case ERROR_DIR_NOT_EMPTY:
            return ENOTEMPTY;
        case ERROR_DIRECTORY:
            return ENOTDIR;
        case ERROR_NOT_SAME_DEVICE:
            return EXDEV;
        case ERROR_DISK_FULL:
        case ERROR_HANDLE_DISK_FULL:
            return ENOSPC;
        case ERROR_BROKEN_PIPE:
        case ERROR_NO_DATA:
        case ERROR_PIPE_NOT_CONNECTED:
            return EPIPE;
        case ERROR_NOT_ENOUGH_MEMORY:
        case ERROR_OUTOFMEMORY:
        case ERROR_COMMITMENT_LIMIT:
            return ENOMEM;
        case ERROR_INVALID_HANDLE:
            return EBADF;
        case ERROR_FILENAME_EXCED_RANGE:
            return ENAMETOOLONG;
        case ERROR_CANT_RESOLVE_FILENAME:
            return ELOOP;
        case ERROR_NEGATIVE_SEEK:
        case ERROR_INVALID_PARAMETER:
            return EINVAL;
        case ERROR_NOT_SUPPORTED:
        case ERROR_INVALID_FUNCTION:
            return ENOTSUP;
        case ERROR_OPERATION_ABORTED:
            return EINTR;
        case ERROR_LOCK_VIOLATION:
        case ERROR_LOCK_FAILED:
            return EAGAIN;
        case ERROR_TOO_MANY_OPEN_FILES:
            return EMFILE;
        case ERROR_BUSY:
            return EBUSY;
        default:
            return EIO;
    }
}

static void init_once(void);
static INIT_ONCE init_guard = INIT_ONCE_STATIC_INIT;
static BOOL CALLBACK init_cb(PINIT_ONCE once, PVOID param, PVOID *ctx) {
    (void) once; (void) param; (void) ctx;
    init_once();
    return TRUE;
}
void px_init(void) {
    InitOnceExecuteOnce(&init_guard, init_cb, NULL, NULL);
}

static void pipe_end_close(struct px_desc *d);
static ssize_t pipe_read(struct px_desc *d, void *buf, size_t n);
static ssize_t pipe_write(struct px_desc *d, const void *buf, size_t n);

struct px_desc *px_desc_new(enum px_kind kind) {
    struct px_desc *d = calloc(1, sizeof(*d));
    if (d == NULL)
        return NULL;
    atomic_init(&d->refs, 1);
    d->kind = kind;
    d->h = INVALID_HANDLE_VALUE;
    d->s = INVALID_SOCKET;
    InitializeSRWLock(&d->lock);
    return d;
}

static void desc_free(struct px_desc *d) {
    switch (d->kind) {
        case PX_FILE:
            if (d->h != INVALID_HANDLE_VALUE)
                CloseHandle(d->h);
            px_fc_put(d->fc);
            break;
        case PX_SOCK:
            closesocket(d->s);
            if (d->is_icmp)
                closesocket(d->icmp_tx);
            break;
        case PX_CONSOLE_OUT:
            if (d->own_h)
                CloseHandle(d->h);
            break;
        case PX_NUL:
            break;
        case PX_PIPE:
            pipe_end_close(d);
            break;
    }
    free(d->path);
    free(d);
}

void px_put(struct px_desc *d) {
    if (d != NULL && atomic_fetch_sub(&d->refs, 1) == 1)
        desc_free(d);
}

struct px_desc *px_get(int fd) {
    px_init();
    if (fd < 0 || fd >= PX_MAX_FDS) {
        errno = EBADF;
        return NULL;
    }
    AcquireSRWLockShared(&table_lock);
    struct px_desc *d = table[fd].d;
    if (d != NULL)
        atomic_fetch_add(&d->refs, 1);
    ReleaseSRWLockShared(&table_lock);
    if (d == NULL)
        errno = EBADF;
    return d;
}

static int install_at(struct px_desc *d, int fd, bool cloexec) {
    // table_lock held exclusive
    struct px_desc *old = table[fd].d;
    table[fd].d = d;
    table[fd].cloexec = cloexec;
    if (old != NULL) {
        ReleaseSRWLockExclusive(&table_lock);
        px_put(old);
        AcquireSRWLockExclusive(&table_lock);
    }
    return fd;
}

int px_install(struct px_desc *d, int min, bool cloexec) {
    px_init();
    AcquireSRWLockExclusive(&table_lock);
    for (int fd = min < 0 ? 0 : min; fd < PX_MAX_FDS; fd++) {
        if (table[fd].d == NULL) {
            install_at(d, fd, cloexec);
            ReleaseSRWLockExclusive(&table_lock);
            return fd;
        }
    }
    ReleaseSRWLockExclusive(&table_lock);
    px_put(d);
    return px_fail(EMFILE);
}

int px_close(int fd) {
    if (fd < 0 || fd >= PX_MAX_FDS)
        return px_fail(EBADF);
    AcquireSRWLockExclusive(&table_lock);
    struct px_desc *d = table[fd].d;
    table[fd].d = NULL;
    ReleaseSRWLockExclusive(&table_lock);
    if (d == NULL)
        return px_fail(EBADF);
    px_put(d);
    return 0;
}

int px_dup(int fd) {
    struct px_desc *d = px_get(fd);
    if (d == NULL)
        return -1;
    return px_install(d, 0, false);
}

int px_dup2(int fd, int to) {
    if (to < 0 || to >= PX_MAX_FDS)
        return px_fail(EBADF);
    struct px_desc *d = px_get(fd);
    if (d == NULL)
        return -1;
    if (fd == to) {
        px_put(d);
        return to;
    }
    AcquireSRWLockExclusive(&table_lock);
    install_at(d, to, false);
    ReleaseSRWLockExclusive(&table_lock);
    return to;
}

int px_fd_path(int fd, char *buf, size_t size) {
    struct px_desc *d = px_get(fd);
    if (d == NULL)
        return -1;
    int err = 0;
    if (d->path == NULL)
        err = EBADF;
    else if (strlen(d->path) >= size)
        err = ENAMETOOLONG;
    else
        strcpy(buf, d->path);
    px_put(d);
    return err ? px_fail(err) : 0;
}

// ---- file I/O. The offset lives in the description and every transfer is
// positional, so pread/pwrite never disturb it.

static ssize_t file_pread(struct px_desc *d, void *buf, size_t n, int64_t off) {
    if (d->is_dir)
        return px_fail(EISDIR);
    if (d->fc != NULL)
        return px_fc_pread(d->fc, buf, n, off);
    if (n > (1u << 30))
        n = 1u << 30;
    OVERLAPPED ov = {.Offset = (DWORD) off, .OffsetHigh = (DWORD) (off >> 32)};
    DWORD got;
    if (!ReadFile(d->h, buf, (DWORD) n, &got, &ov)) {
        DWORD err = GetLastError();
        if (err == ERROR_HANDLE_EOF)
            return 0;
        return px_fail(px_errno_win(err));
    }
    return got;
}

static ssize_t file_pwrite(struct px_desc *d, const void *buf, size_t n, int64_t off, bool append) {
    if (d->is_dir)
        return px_fail(EISDIR);
    if (d->fc != NULL)
        return px_fail(EBADF);   // opened read-only
    if (n > (1u << 30))
        n = 1u << 30;
    OVERLAPPED ov = {.Offset = (DWORD) off, .OffsetHigh = (DWORD) (off >> 32)};
    if (append)
        ov.Offset = ov.OffsetHigh = 0xffffffff;
    DWORD put;
    if (!WriteFile(d->h, buf, (DWORD) n, &put, &ov))
        return px_fail_win();
    return put;
}

ssize_t px_read(int fd, void *buf, size_t n) {
    struct px_desc *d = px_get(fd);
    if (d == NULL)
        return -1;
    ssize_t res;
    switch (d->kind) {
        case PX_FILE:
            AcquireSRWLockExclusive(&d->lock);
            res = file_pread(d, buf, n, d->pos);
            // short from the start: that was all of it
            if (d->fc_fill && d->pos == 0 && res >= 0 && (size_t) res < n)
                px_fc_offer(d, buf, res);
            d->fc_fill = false;
            if (res > 0)
                d->pos += res;
            ReleaseSRWLockExclusive(&d->lock);
            break;
        case PX_SOCK:
            if (d->is_stdin)
                px_stdin_demand();
            res = px_sock_read(d, buf, n);
            break;
        case PX_PIPE:
            if (d->is_stdin)
                px_stdin_demand();
            res = pipe_read(d, buf, n);
            break;
        case PX_NUL:
            res = 0;
            break;
        default:
            res = px_fail(EBADF);
    }
    px_put(d);
    return res;
}

ssize_t px_write(int fd, const void *buf, size_t n) {
    struct px_desc *d = px_get(fd);
    if (d == NULL)
        return -1;
    ssize_t res;
    switch (d->kind) {
        case PX_FILE: {
            AcquireSRWLockExclusive(&d->lock);
            bool append = d->flags & _O_APPEND;
            res = file_pwrite(d, buf, n, d->pos, append);
            if (res > 0) {
                if (append) {
                    LARGE_INTEGER size;
                    if (GetFileSizeEx(d->h, &size))
                        d->pos = size.QuadPart;
                } else {
                    d->pos += res;
                }
            }
            ReleaseSRWLockExclusive(&d->lock);
            break;
        }
        case PX_SOCK:
            res = px_sock_write(d, buf, n);
            break;
        case PX_PIPE:
            res = pipe_write(d, buf, n);
            break;
        case PX_CONSOLE_OUT: {
            DWORD put;
            if (n > (1u << 30))
                n = 1u << 30;
            res = WriteFile(d->h, buf, (DWORD) n, &put, NULL) ? (ssize_t) put : px_fail_win();
            if (d->own_h)
                px_output_tick = GetTickCount64();
            break;
        }
        case PX_NUL:
            res = n;
            break;
        default:
            res = px_fail(EBADF);
    }
    px_put(d);
    return res;
}

ssize_t px_pread(int fd, void *buf, size_t n, int64_t off) {
    struct px_desc *d = px_get(fd);
    if (d == NULL)
        return -1;
    ssize_t res = d->kind == PX_FILE ? file_pread(d, buf, n, off) : px_fail(ESPIPE);
    px_put(d);
    return res;
}

ssize_t px_pwrite(int fd, const void *buf, size_t n, int64_t off) {
    struct px_desc *d = px_get(fd);
    if (d == NULL)
        return -1;
    ssize_t res = d->kind == PX_FILE ? file_pwrite(d, buf, n, off, false) : px_fail(ESPIPE);
    px_put(d);
    return res;
}

ssize_t px_readv(int fd, const struct px_iovec *iov, int cnt) {
    ssize_t total = 0;
    for (int i = 0; i < cnt; i++) {
        if (iov[i].iov_len == 0)
            continue;
        ssize_t res = px_read(fd, iov[i].iov_base, iov[i].iov_len);
        if (res < 0)
            return total > 0 ? total : res;
        total += res;
        if ((size_t) res < iov[i].iov_len)
            break;
    }
    return total;
}

ssize_t px_writev(int fd, const struct px_iovec *iov, int cnt) {
    ssize_t total = 0;
    for (int i = 0; i < cnt; i++) {
        if (iov[i].iov_len == 0)
            continue;
        ssize_t res = px_write(fd, iov[i].iov_base, iov[i].iov_len);
        if (res < 0)
            return total > 0 ? total : res;
        total += res;
        if ((size_t) res < iov[i].iov_len)
            break;
    }
    return total;
}

ssize_t px_preadv(int fd, const struct px_iovec *iov, int cnt, int64_t off) {
    ssize_t total = 0;
    for (int i = 0; i < cnt; i++) {
        ssize_t res = px_pread(fd, iov[i].iov_base, iov[i].iov_len, off + total);
        if (res < 0)
            return total > 0 ? total : res;
        total += res;
        if ((size_t) res < iov[i].iov_len)
            break;
    }
    return total;
}

ssize_t px_pwritev(int fd, const struct px_iovec *iov, int cnt, int64_t off) {
    ssize_t total = 0;
    for (int i = 0; i < cnt; i++) {
        ssize_t res = px_pwrite(fd, iov[i].iov_base, iov[i].iov_len, off + total);
        if (res < 0)
            return total > 0 ? total : res;
        total += res;
        if ((size_t) res < iov[i].iov_len)
            break;
    }
    return total;
}

int64_t px_lseek(int fd, int64_t off, int whence) {
    struct px_desc *d = px_get(fd);
    if (d == NULL)
        return -1;
    int64_t res;
    if (d->kind != PX_FILE) {
        res = px_fail(ESPIPE);
    } else {
        AcquireSRWLockExclusive(&d->lock);
        int64_t base = 0;
        LARGE_INTEGER size;
        res = 0;
        switch (whence) {
            case SEEK_SET: base = 0; break;
            case SEEK_CUR: base = d->pos; break;
            case SEEK_END:
                if (d->fc != NULL)
                    size.QuadPart = px_fc_size(d->fc);
                else if (!GetFileSizeEx(d->h, &size))
                    res = px_fail_win();
                base = size.QuadPart;
                break;
            default: res = px_fail(EINVAL);
        }
        if (res == 0) {
            if (base + off < 0)
                res = px_fail(EINVAL);
            else
                res = d->pos = base + off;
        }
        ReleaseSRWLockExclusive(&d->lock);
    }
    px_put(d);
    return res;
}

int px_fsync(int fd) {
    struct px_desc *d = px_get(fd);
    if (d == NULL)
        return -1;
    int res = 0;
    if (d->kind == PX_FILE && !d->is_dir && d->fc == NULL && !FlushFileBuffers(d->h))
        res = px_fail_win();
    px_put(d);
    return res;
}

int px_ftruncate(int fd, int64_t len) {
    struct px_desc *d = px_get(fd);
    if (d == NULL)
        return -1;
    int res = 0;
    if (d->kind != PX_FILE || d->is_dir) {
        res = px_fail(EINVAL);
    } else {
        FILE_END_OF_FILE_INFO info = {.EndOfFile.QuadPart = len};
        if (d->fc != NULL)
            res = px_fail(EINVAL);   // opened read-only
        else if (!SetFileInformationByHandle(d->h, FileEndOfFileInfo, &info, sizeof(info)))
            res = px_fail_win();
    }
    px_put(d);
    return res;
}

int px_truncate(const char *path, int64_t len) {
    int fd = px_open(path, PX_O_WRONLY);
    if (fd < 0)
        return -1;
    int res = px_ftruncate(fd, len);
    int saved = errno;
    px_close(fd);
    errno = saved;
    return res;
}

int px_flock(int fd, int op) {
    struct px_desc *d = px_get(fd);
    if (d == NULL)
        return -1;
    int res = 0;
    if (d->kind != PX_FILE) {
        res = px_fail(EINVAL);
        goto out;
    }
    HANDLE h = px_desc_handle(d);
    if (h == INVALID_HANDLE_VALUE) {
        res = -1;
        goto out;
    }
    OVERLAPPED ov = {0};
    if (op & 8 /* LOCK_UN */) {
        if (!UnlockFileEx(h, 0, MAXDWORD, MAXDWORD, &ov) && GetLastError() != ERROR_NOT_LOCKED)
            res = px_fail_win();
        goto out;
    }
    // Windows locks do not convert in place: drop ours first, like flock.
    UnlockFileEx(h, 0, MAXDWORD, MAXDWORD, &ov);
    DWORD flags = LOCKFILE_FAIL_IMMEDIATELY;
    if (op & 2 /* LOCK_EX */)
        flags |= LOCKFILE_EXCLUSIVE_LOCK;
    // Poll for a blocking lock so a guest signal can still interrupt it.
    for (;;) {
        ov = (OVERLAPPED) {0};
        if (LockFileEx(h, flags, 0, MAXDWORD, MAXDWORD, &ov))
            break;
        DWORD err = GetLastError();
        if (err != ERROR_LOCK_VIOLATION && err != ERROR_IO_PENDING) {
            res = px_fail(px_errno_win(err));
            break;
        }
        if (op & 4 /* LOCK_NB */) {
            res = px_fail(EWOULDBLOCK);
            break;
        }
        struct timespec ts = {0, 10 * 1000000};
        if (px_nanosleep(&ts, NULL) < 0) {
            res = -1;
            break;
        }
    }
out:
    px_put(d);
    return res;
}

// ---- in-process pipes. pipe(), stdin and the inputs of ish.exe --serve's
// commands are a buffer here, both ends in this process, instead of a pair of
// 127.0.0.1 TCP sockets: those need Winsock and a loopback connection that
// security software, VPNs and broken LSPs can block, and when they could not
// be made stdin silently read as empty and pipe() failed. A thread that
// waits on a pipe (a blocking read or write, or a poll) hangs a px_waiter on
// it and is poked when the pipe changes.

#define PIPE_CAP 65536
#define PIPE_ATOMIC 4096 // PIPE_BUF: writes up to this never interleave

struct px_pipe {
    SRWLOCK lock;
    atomic_int refs; // the two ends
    int readers, writers;
    char *buf;       // PIPE_CAP bytes once something is written
    size_t head, len;
    struct px_waiter *waiters;
    int id;          // for px_describe: the ends report 2*id and 2*id+1
};

static atomic_int pipe_ids;

static struct px_pipe *pipe_new(void) {
    struct px_pipe *p = calloc(1, sizeof(*p));
    if (p == NULL)
        return NULL;
    InitializeSRWLock(&p->lock);
    atomic_init(&p->refs, 2);
    p->readers = p->writers = 1;
    p->id = atomic_fetch_add(&pipe_ids, 1) % 0x3fffffff + 1;
    return p;
}

static bool pipe_is_writer(struct px_desc *d) {
    return (d->flags & PX_O_ACCMODE) == PX_O_WRONLY;
}

// lock held
static void pipe_poke(struct px_pipe *p) {
    for (struct px_waiter *w = p->waiters; w != NULL; w = w->next)
        px_waiter_poke(w);
}

// lock held
static void pipe_hang(struct px_pipe *p, struct px_waiter *w) {
    w->prev = NULL;
    w->next = p->waiters;
    if (p->waiters != NULL)
        p->waiters->prev = w;
    p->waiters = w;
}

// lock held
static void pipe_unhang(struct px_pipe *p, struct px_waiter *w) {
    if (w->prev != NULL)
        w->prev->next = w->next;
    else if (p->waiters == w)
        p->waiters = w->next;
    if (w->next != NULL)
        w->next->prev = w->prev;
    w->prev = w->next = NULL;
}

static void pipe_end_close(struct px_desc *d) {
    struct px_pipe *p = d->pipe;
    if (p == NULL)
        return;
    AcquireSRWLockExclusive(&p->lock);
    if (pipe_is_writer(d))
        p->writers--;
    else
        p->readers--;
    pipe_poke(p);
    ReleaseSRWLockExclusive(&p->lock);
    if (atomic_fetch_sub(&p->refs, 1) == 1) {
        free(p->buf);
        free(p);
    }
}

// Waits for the pipe to change; lock held on entry and on return. 1 when it
// may have, -1 (errno EINTR) when interrupted.
static int pipe_wait(struct px_pipe *p) {
    struct px_waiter w;
    px_waiter_init(&w, false);
    pipe_hang(p, &w);
    ReleaseSRWLockExclusive(&p->lock);
    int res = px_poke_wait(-1);
    AcquireSRWLockExclusive(&p->lock);
    pipe_unhang(p, &w);
    return res < 0 ? -1 : 1;
}

static ssize_t pipe_read(struct px_desc *d, void *buf, size_t n) {
    struct px_pipe *p = d->pipe;
    if (pipe_is_writer(d))
        return px_fail(EBADF);
    if (n == 0)
        return 0;
    AcquireSRWLockExclusive(&p->lock);
    ssize_t res;
    for (;;) {
        if (p->len > 0) {
            size_t take = n < p->len ? n : p->len;
            size_t first = PIPE_CAP - p->head < take ? PIPE_CAP - p->head : take;
            memcpy(buf, p->buf + p->head, first);
            memcpy((char *) buf + first, p->buf, take - first);
            p->head = (p->head + take) % PIPE_CAP;
            p->len -= take;
            if (p->len == 0)
                p->head = 0;
            pipe_poke(p);
            res = (ssize_t) take;
            break;
        }
        if (p->writers == 0) {
            res = 0;
            break;
        }
        if (d->flags & PX_O_NONBLOCK) {
            res = px_fail(EAGAIN);
            break;
        }
        if (pipe_wait(p) < 0) {
            res = -1;
            break;
        }
    }
    ReleaseSRWLockExclusive(&p->lock);
    return res;
}

// As Linux: a blocking write puts all of buf (a write of up to PIPE_ATOMIC
// bytes in one piece) unless interrupted, then it says how much went; a
// non-blocking one puts what fits, all or nothing up to PIPE_ATOMIC. No
// reader: EPIPE.
static ssize_t pipe_write(struct px_desc *d, const void *buf, size_t n) {
    struct px_pipe *p = d->pipe;
    if (!pipe_is_writer(d))
        return px_fail(EBADF);
    if (n == 0)
        return 0;
    AcquireSRWLockExclusive(&p->lock);
    size_t put = 0;
    ssize_t res;
    for (;;) {
        if (p->readers == 0) {
            res = put > 0 ? (ssize_t) put : px_fail(EPIPE);
            break;
        }
        if (p->buf == NULL && (p->buf = malloc(PIPE_CAP)) == NULL) {
            res = put > 0 ? (ssize_t) put : px_fail(ENOMEM);
            break;
        }
        size_t room = PIPE_CAP - p->len, want = n - put;
        if (room > 0 && (want > PIPE_ATOMIC || room >= want)) {
            size_t give = want < room ? want : room;
            size_t tail = (p->head + p->len) % PIPE_CAP;
            size_t first = PIPE_CAP - tail < give ? PIPE_CAP - tail : give;
            memcpy(p->buf + tail, (const char *) buf + put, first);
            memcpy(p->buf, (const char *) buf + put + first, give - first);
            p->len += give;
            put += give;
            pipe_poke(p);
            if (put == n) {
                res = (ssize_t) put;
                break;
            }
            continue;
        }
        if (d->flags & PX_O_NONBLOCK) {
            res = put > 0 ? (ssize_t) put : px_fail(EAGAIN);
            break;
        }
        if (pipe_wait(p) < 0) {
            res = put > 0 ? (ssize_t) put : -1;
            break;
        }
    }
    ReleaseSRWLockExclusive(&p->lock);
    return res;
}

static size_t pipe_buffered(struct px_desc *d) {
    AcquireSRWLockShared(&d->pipe->lock);
    size_t len = d->pipe->len;
    ReleaseSRWLockShared(&d->pipe->lock);
    return len;
}

// lock held
static short pipe_revents_locked(struct px_desc *d, short events) {
    struct px_pipe *p = d->pipe;
    short re = 0;
    if (pipe_is_writer(d)) {
        if (p->readers == 0)
            re |= PX_POLLERR | (events & PX_POLLOUT);
        else if (p->len < PIPE_CAP)
            re |= events & PX_POLLOUT;
    } else {
        if (p->len > 0)
            re |= events & PX_POLLIN;
        // as the socket pairs did: a closed writer is POLLHUP, and readable
        if (p->writers == 0)
            re |= PX_POLLHUP | (events & PX_POLLIN);
    }
    return re;
}

short px_pipe_revents(struct px_desc *d, short events) {
    AcquireSRWLockShared(&d->pipe->lock);
    short re = pipe_revents_locked(d, events);
    ReleaseSRWLockShared(&d->pipe->lock);
    return re;
}

void px_pipe_watch(struct px_desc *d, struct px_waiter *w, bool on) {
    AcquireSRWLockExclusive(&d->pipe->lock);
    if (on)
        pipe_hang(d->pipe, w);
    else
        pipe_unhang(d->pipe, w);
    ReleaseSRWLockExclusive(&d->pipe->lock);
}

// Two descriptions on a new pipe: *r its read end, *w its write end.
static int pipe_pair(struct px_desc **r, struct px_desc **w) {
    struct px_pipe *p = pipe_new();
    *r = px_desc_new(PX_PIPE);
    *w = px_desc_new(PX_PIPE);
    if (p == NULL || *r == NULL || *w == NULL) {
        free(p);
        free(*r);
        free(*w);
        return px_fail(ENOMEM);
    }
    (*r)->pipe = (*w)->pipe = p;
    (*r)->is_pipe = (*w)->is_pipe = true;
    (*r)->flags = 0;
    (*w)->flags = PX_O_WRONLY;
    return 0;
}

// For the pumps (host threads, never interrupted): all of buf, or false
// once nobody reads the pipe.
static bool pipe_put_all(struct px_desc *w, const char *buf, size_t n) {
    while (n > 0) {
        ssize_t put = pipe_write(w, buf, n);
        if (put < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }
        buf += put;
        n -= (size_t) put;
    }
    return true;
}

static int px_port_of(const struct sockaddr_storage *a) {
    if (a->ss_family == AF_INET)
        return ntohs(((const struct sockaddr_in *) a)->sin_port);
    if (a->ss_family == AF_INET6)
        return ntohs(((const struct sockaddr_in6 *) a)->sin6_port);
    return 0;
}

int px_describe(int fd, char *buf, size_t size, int *local_port, int *peer_port) {
    *local_port = *peer_port = 0;
    struct px_desc *d = px_get(fd);
    if (d == NULL)
        return snprintf(buf, size, "host fd %d not open", fd);
    int len = 0;
    switch (d->kind) {
        case PX_SOCK: {
            struct sockaddr_storage a;
            int al = sizeof(a);
            if (getsockname(d->s, (struct sockaddr *) &a, &al) == 0)
                *local_port = px_port_of(&a);
            al = sizeof(a);
            bool connected = getpeername(d->s, (struct sockaddr *) &a, &al) == 0;
            if (connected)
                *peer_port = px_port_of(&a);
            u_long avail = 0;
            ioctlsocket(d->s, FIONREAD, &avail);
            // Readable with nothing to read: the other end has closed (EOF).
            fd_set rs;
            FD_ZERO(&rs);
            FD_SET(d->s, &rs);
            struct timeval tv = {0, 0};
            bool readable = select(0, &rs, NULL, NULL, &tv) > 0;
            bool wr = (d->flags & PX_O_ACCMODE) == PX_O_WRONLY;
            if (d->is_stdin)
                len = snprintf(buf, size, "stdin bridge buffered=%lu%s", avail, readable && avail == 0 ? " eof" : "");
            else if (d->is_pipe)
                len = snprintf(buf, size, "pipe %s end, buffered=%lu%s%s", wr ? "write" : "read", avail,
                               !wr && readable && avail == 0 ? ", writer closed (EOF)" : "",
                               connected ? "" : ", peer gone");
            else
                len = snprintf(buf, size, "socket domain=%d type=%d local:%d peer:%d%s buffered=%lu%s",
                               d->domain, d->type, *local_port, *peer_port, connected ? "" : " (not connected)",
                               avail, readable && avail == 0 && connected ? " eof" : "");
            break;
        }
        case PX_FILE:
            len = snprintf(buf, size, "%s %s%s%s pos=%lld", d->is_dir ? "dir" : "file", d->path ? d->path : "?",
                           d->is_tty ? " (tty)" : "", d->fc != NULL ? " (cached)" : "", (long long) d->pos);
            break;
        case PX_CONSOLE_OUT: {
            DWORD type = GetFileType(d->h);
            len = snprintf(buf, size, "host %s", type == FILE_TYPE_PIPE ? "pipe (to Minis)" : type == FILE_TYPE_CHAR ? "console" : "file");
            break;
        }
        case PX_NUL:
            len = snprintf(buf, size, "NUL");
            break;
        case PX_PIPE: {
            AcquireSRWLockShared(&d->pipe->lock);
            size_t avail = d->pipe->len;
            bool wr = pipe_is_writer(d);
            bool eof = d->pipe->writers == 0, gone = wr ? d->pipe->readers == 0 : d->pipe->writers == 0;
            int id = d->pipe->id;
            ReleaseSRWLockShared(&d->pipe->lock);
            // kernel/state_dump.c pairs the two ends by these
            *local_port = wr ? 2 * id + 1 : 2 * id;
            *peer_port = wr ? 2 * id : 2 * id + 1;
            if (d->is_stdin)
                len = snprintf(buf, size, "stdin bridge buffered=%zu%s", avail, eof && avail == 0 ? " eof" : "");
            else
                len = snprintf(buf, size, "pipe %s end, buffered=%zu%s%s", wr ? "write" : "read", avail,
                               !wr && eof ? ", writer closed (EOF)" : "", gone ? ", peer gone" : "");
            break;
        }
    }
    if (len >= 0 && (size_t) len < size)
        len += snprintf(buf + len, size - len, " refs=%d", atomic_load(&d->refs));
    px_put(d);
    return len;
}

int px_pipe(int fds[2]) {
    px_init();
    struct px_desc *r, *w;
    if (pipe_pair(&r, &w) < 0)
        return -1;
    fds[0] = px_install(r, 0, false);
    if (fds[0] < 0) {
        px_put(w);
        return -1;
    }
    fds[1] = px_install(w, 0, false);
    if (fds[1] < 0) {
        px_close(fds[0]);
        return -1;
    }
    return 0;
}

int px_fcntl(int fd, int cmd, ...) {
    va_list ap;
    va_start(ap, cmd);
    intptr_t arg = va_arg(ap, intptr_t);
    va_end(ap);

    if (cmd == 0 /* F_DUPFD */ || cmd == 1030 /* F_DUPFD_CLOEXEC */) {
        struct px_desc *d = px_get(fd);
        if (d == NULL)
            return -1;
        return px_install(d, (int) arg, cmd == 1030);
    }
    if (cmd == 50 /* F_GETPATH */)
        return px_fd_path(fd, (char *) arg, PATH_MAX);

    struct px_desc *d = px_get(fd);
    if (d == NULL)
        return -1;
    int res = 0;
    switch (cmd) {
        case 1: /* F_GETFD */
            AcquireSRWLockShared(&table_lock);
            res = table[fd].cloexec ? 1 : 0;
            ReleaseSRWLockShared(&table_lock);
            break;
        case 2: /* F_SETFD */
            AcquireSRWLockExclusive(&table_lock);
            if (table[fd].d == d)
                table[fd].cloexec = arg & 1;
            ReleaseSRWLockExclusive(&table_lock);
            break;
        case 3: /* F_GETFL */
            res = d->flags;
            break;
        case 4: /* F_SETFL */
            d->flags = (d->flags & ~(PX_O_NONBLOCK | _O_APPEND)) | ((int) arg & (PX_O_NONBLOCK | _O_APPEND));
            break;
        case 5: case 6: case 7: /* F_GETLK, F_SETLK, F_SETLKW: emulated by the guest kernel */
            break;
        default:
            res = px_fail(EINVAL);
    }
    px_put(d);
    return res;
}

int px_ioctl(int fd, unsigned long req, ...) {
    va_list ap;
    va_start(ap, req);
    void *arg = va_arg(ap, void *);
    va_end(ap);
    struct px_desc *d = px_get(fd);
    if (d == NULL)
        return -1;
    int res = 0;
    switch (req) {
        case 0x541b: { /* FIONREAD */
            int avail = 0;
            if (d->kind == PX_SOCK) {
                if (d->is_stdin)
                    px_stdin_demand();
                res = px_sock_ioctl_fionread(d, &avail);
            } else if (d->kind == PX_PIPE) {
                if (d->is_stdin)
                    px_stdin_demand();
                avail = (int) pipe_buffered(d);
            } else if (d->kind == PX_FILE && !d->is_dir) {
                LARGE_INTEGER size;
                if (d->fc != NULL)
                    size.QuadPart = px_fc_size(d->fc);
                else if (!GetFileSizeEx(d->h, &size))
                    size.QuadPart = 0;
                if (size.QuadPart > d->pos)
                    avail = (int) (size.QuadPart - d->pos);
            }
            // callers pass int or size_t; both hold this little-endian
            *(int *) arg = avail;
            break;
        }
        case 0x5421: /* FIONBIO */
            if (*(int *) arg)
                d->flags |= PX_O_NONBLOCK;
            else
                d->flags &= ~PX_O_NONBLOCK;
            break;
        case 0x5413: /* TIOCGWINSZ */
            res = d->is_tty ? px_console_winsize(arg) : px_fail(ENOTTY);
            break;
        case 0x5414: /* TIOCSWINSZ */
            res = d->is_tty ? 0 : px_fail(ENOTTY);
            break;
        default:
            res = px_fail(ENOTTY);
    }
    px_put(d);
    return res;
}

int px_isatty(int fd) {
    struct px_desc *d = px_get(fd);
    if (d == NULL)
        return 0;
    int res = d->is_tty;
    px_put(d);
    if (!res)
        errno = ENOTTY;
    return res;
}

// ---- stdio. stdout and stderr write straight to the inherited handles; stdin
// is pumped by a thread into an in-process pipe so it can be polled like
// anything else.

static HANDLE con_in = INVALID_HANDLE_VALUE;
static HANDLE stdin_want; // manual-reset: set while someone waits for stdin data

void px_stdin_demand(void) {
    if (stdin_want != NULL)
        SetEvent(stdin_want);
}

HANDLE px_stdin_handle(void) {
    return con_in;
}
static DWORD con_in_mode_saved, con_out_mode_saved;
static bool con_in_saved, con_out_saved;
static HANDLE con_out = INVALID_HANDLE_VALUE;

static DWORD WINAPI stdin_pump(void *param) {
    struct px_desc *out = param;
    HANDLE in = con_in;
    DWORD mode;
    bool console = GetConsoleMode(in, &mode);
    char buf[4096];
    wchar_t wbuf[1024];
    for (;;) {
        // wait until a reader or poller wants data
        WaitForSingleObject(stdin_want, INFINITE);
        ResetEvent(stdin_want);
        DWORD got = 0;
        if (console) {
            // ReadConsoleW, not ReadFile: the UTF-8 code page path still loses
            // non-ASCII input on some builds.
            DWORD wgot;
            if (!ReadConsoleW(in, wbuf, 1024 - 1, &wgot, NULL))
                break;
            if (wgot == 0)
                continue;
            int n = WideCharToMultiByte(CP_UTF8, 0, wbuf, (int) wgot, buf, sizeof(buf), NULL, NULL);
            if (n <= 0)
                continue;
            got = n;
        } else if (!ReadFile(in, buf, sizeof(buf), &got, NULL) || got == 0) {
            break;
        }
        if (!pipe_put_all(out, buf, got))
            break;
    }
    px_put(out); // the write end: the guest reads EOF
    return 0;
}

// A handle another process gave us (ish.exe --serve's commands get their
// stdio so): the reading end of an input is pumped into a pipe as stdin
// is, eagerly, until it ends or the guest closes its end.
struct handle_pump {
    HANDLE in;
    struct px_desc *out;
};

static DWORD WINAPI handle_pump(void *param) {
    struct handle_pump *p = param;
    char buf[16384];
    for (;;) {
        DWORD got = 0;
        if (!ReadFile(p->in, buf, sizeof(buf), &got, NULL) || got == 0)
            break;
        if (!pipe_put_all(p->out, buf, got))
            break;
    }
    px_put(p->out);
    CloseHandle(p->in);
    free(p);
    return 0;
}

_Atomic uint64_t px_output_tick;

int px_fd_from_handle(void *handle, bool input) {
    HANDLE h = handle;
    DWORD hflags;
    if (h == NULL || h == INVALID_HANDLE_VALUE || !GetHandleInformation(h, &hflags))
        return px_fail(EBADF);
    bool pipe = GetFileType(h) == FILE_TYPE_PIPE;
    if (!input) {
        struct px_desc *d = px_desc_new(PX_CONSOLE_OUT);
        if (d == NULL) {
            CloseHandle(h);
            return px_fail(ENOMEM);
        }
        d->h = h;
        d->own_h = true;
        d->is_pipe = pipe;
        d->flags = PX_O_WRONLY;
        return px_install(d, 0, true);
    }
    struct px_desc *r, *w;
    struct handle_pump *p = malloc(sizeof(*p));
    if (p == NULL || pipe_pair(&r, &w) < 0) {
        free(p);
        CloseHandle(h);
        return -1;
    }
    p->in = h;
    p->out = w;
    HANDLE thread = CreateThread(NULL, 64 << 10, handle_pump, p, 0, NULL);
    if (thread == NULL) {
        px_put(r);
        px_put(w);
        CloseHandle(h);
        free(p);
        return px_fail(EAGAIN);
    }
    CloseHandle(thread);
    return px_install(r, 0, true);
}

static struct px_desc *stdio_desc(DWORD which) {
    HANDLE h = GetStdHandle(which);
    if (h == NULL || h == INVALID_HANDLE_VALUE)
        return px_desc_new(PX_NUL);
    DWORD mode;
    bool console = GetConsoleMode(h, &mode);
    if (which != STD_INPUT_HANDLE) {
        struct px_desc *d = px_desc_new(PX_CONSOLE_OUT);
        d->h = h;
        d->is_tty = console;
        d->flags = PX_O_WRONLY;
        if (console) {
            if (con_out == INVALID_HANDLE_VALUE) {
                con_out = h;
                con_out_mode_saved = mode;
                con_out_saved = true;
            }
            SetConsoleMode(h, mode | ENABLE_PROCESSED_OUTPUT | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
        }
        return d;
    }
    con_in = h;
    if (console) {
        con_in_mode_saved = mode;
        con_in_saved = true;
    }
    struct px_desc *d, *w;
    if (pipe_pair(&d, &w) < 0) {
        fprintf(stderr, "ish: stdin: no memory for its pipe; stdin is empty\n");
        return px_desc_new(PX_NUL);
    }
    d->is_stdin = true;
    d->is_tty = console;
    stdin_want = CreateEventW(NULL, TRUE, FALSE, NULL);
    HANDLE thread = CreateThread(NULL, 0, stdin_pump, w, 0, NULL);
    if (thread != NULL) {
        CloseHandle(thread);
    } else {
        fprintf(stderr, "ish: stdin: cannot start its pump (error %lu); stdin is empty\n", GetLastError());
        px_put(w);
    }
    return d;
}

void px_console_restore(void) {
    if (con_in_saved)
        SetConsoleMode(con_in, con_in_mode_saved);
    if (con_out_saved)
        SetConsoleMode(con_out, con_out_mode_saved);
}

int px_console_tcgetattr(struct px_termios *t) {
    memset(t, 0, sizeof(*t));
    DWORD mode = ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT | ENABLE_PROCESSED_INPUT;
    if (con_in != INVALID_HANDLE_VALUE)
        GetConsoleMode(con_in, &mode);
    t->c_iflag = 0000400 /* ICRNL */ | 0040000 /* IUTF8 */;
    t->c_oflag = 0000001 /* OPOST */ | 0000004 /* ONLCR */;
    t->c_cflag = 0000060 /* CS8 */ | 0000200 /* CREAD */ | 0000017 /* B38400 */;
    if (mode & ENABLE_LINE_INPUT)
        t->c_lflag |= 0000002 /* ICANON */ | 0100000 /* IEXTEN */;
    if (mode & ENABLE_ECHO_INPUT)
        t->c_lflag |= 0000010 /* ECHO */ | 0000020 /* ECHOE */ | 0000040 /* ECHOK */ | 0001000 /* ECHOCTL */;
    if (mode & ENABLE_PROCESSED_INPUT)
        t->c_lflag |= 0000001 /* ISIG */;
    static const unsigned char cc[] = {3, 28, 127, 21, 4, 0, 1, 0, 17, 19, 26, 0, 18, 15, 23, 22, 0};
    memcpy(t->c_cc, cc, sizeof(cc));
    t->c_ispeed = t->c_ospeed = 0000017;
    return 0;
}

int px_console_tcsetattr(const struct px_termios *t) {
    if (con_in == INVALID_HANDLE_VALUE)
        return px_fail(ENOTTY);
    DWORD mode = ENABLE_EXTENDED_FLAGS | ENABLE_INSERT_MODE | ENABLE_QUICK_EDIT_MODE;
    if (t->c_lflag & 0000002 /* ICANON */) {
        mode |= ENABLE_LINE_INPUT;
        if (t->c_lflag & 0000010 /* ECHO */)
            mode |= ENABLE_ECHO_INPUT;
    } else {
        mode |= ENABLE_VIRTUAL_TERMINAL_INPUT;
    }
    if (t->c_lflag & 0000001 /* ISIG */)
        mode |= ENABLE_PROCESSED_INPUT;
    if (!SetConsoleMode(con_in, mode))
        return px_fail_win();
    return 0;
}

int px_console_winsize(struct px_winsize *ws) {
    CONSOLE_SCREEN_BUFFER_INFO info;
    HANDLE h = con_out;
    if (h == INVALID_HANDLE_VALUE)
        h = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!GetConsoleScreenBufferInfo(h, &info)) {
        HANDLE c = CreateFileW(L"CONOUT$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                NULL, OPEN_EXISTING, 0, NULL);
        bool ok = c != INVALID_HANDLE_VALUE && GetConsoleScreenBufferInfo(c, &info);
        if (c != INVALID_HANDLE_VALUE)
            CloseHandle(c);
        if (!ok)
            return px_fail(ENOTTY);
    }
    ws->ws_col = info.srWindow.Right - info.srWindow.Left + 1;
    ws->ws_row = info.srWindow.Bottom - info.srWindow.Top + 1;
    ws->ws_xpixel = ws->ws_ypixel = 0;
    return 0;
}

int px_tcgetattr(int fd, struct px_termios *t) {
    if (!px_isatty(fd))
        return px_fail(ENOTTY);
    return px_console_tcgetattr(t);
}

int px_tcsetattr(int fd, int act, const struct px_termios *t) {
    (void) act;
    if (!px_isatty(fd))
        return px_fail(ENOTTY);
    // stdout's flags are fixed: the console always translates \n.
    return px_console_tcsetattr(t);
}

void px_cfmakeraw(struct px_termios *t) {
    t->c_iflag &= ~(0000001 | 0000002 | 0000010 | 0000040 | 0000100 | 0000200 | 0000400 | 0002000);
    t->c_oflag &= ~0000001;
    t->c_lflag &= ~(0000010 | 0000100 | 0000002 | 0000001 | 0100000);
    t->c_cflag &= ~(0000060 | 0000400);
    t->c_cflag |= 0000060;
    t->c_cc[6 /* VMIN */] = 1;
    t->c_cc[5 /* VTIME */] = 0;
}

_Noreturn void px_exit(int status) {
    extern void px_prof_dump(void);
    px_prof_dump();
    px_console_restore();
    fflush(NULL);
    ExitProcess(status);
}

extern void px_misc_init(void);

static void init_once(void) {
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
    px_misc_init();
    extern void px_prof_init(void);
    px_prof_init();
    table[1].d = stdio_desc(STD_OUTPUT_HANDLE);
    table[2].d = stdio_desc(STD_ERROR_HANDLE);
    table[0].d = stdio_desc(STD_INPUT_HANDLE);
}
