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
            CloseHandle(d->h);
            break;
        case PX_SOCK:
            closesocket(d->s);
            break;
        case PX_CONSOLE_OUT:
        case PX_NUL:
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
            if (res > 0)
                d->pos += res;
            ReleaseSRWLockExclusive(&d->lock);
            break;
        case PX_SOCK:
            res = px_sock_read(d, buf, n);
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
        case PX_CONSOLE_OUT: {
            DWORD put;
            if (n > (1u << 30))
                n = 1u << 30;
            res = WriteFile(d->h, buf, (DWORD) n, &put, NULL) ? (ssize_t) put : px_fail_win();
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
                if (!GetFileSizeEx(d->h, &size))
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
    if (d->kind == PX_FILE && !d->is_dir && !FlushFileBuffers(d->h))
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
        if (!SetFileInformationByHandle(d->h, FileEndOfFileInfo, &info, sizeof(info)))
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
    OVERLAPPED ov = {0};
    if (op & 8 /* LOCK_UN */) {
        if (!UnlockFileEx(d->h, 0, MAXDWORD, MAXDWORD, &ov) && GetLastError() != ERROR_NOT_LOCKED)
            res = px_fail_win();
        goto out;
    }
    // Windows locks do not convert in place: drop ours first, like flock.
    UnlockFileEx(d->h, 0, MAXDWORD, MAXDWORD, &ov);
    DWORD flags = LOCKFILE_FAIL_IMMEDIATELY;
    if (op & 2 /* LOCK_EX */)
        flags |= LOCKFILE_EXCLUSIVE_LOCK;
    // Poll for a blocking lock so a guest signal can still interrupt it.
    for (;;) {
        ov = (OVERLAPPED) {0};
        if (LockFileEx(d->h, flags, 0, MAXDWORD, MAXDWORD, &ov))
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

int px_pipe(int fds[2]) {
    px_init();
    SOCKET s[2];
    if (px_sock_pair(1 /* SOCK_STREAM */, s) < 0)
        return -1;
    struct px_desc *r = px_desc_new(PX_SOCK), *w = px_desc_new(PX_SOCK);
    r->s = s[0];
    w->s = s[1];
    r->is_pipe = w->is_pipe = true;
    r->domain = w->domain = 1; // AF_UNIX
    r->type = w->type = 1;
    r->flags = 0;
    w->flags = PX_O_WRONLY;
    fds[0] = px_install(r, 0, false);
    fds[1] = px_install(w, 0, false);
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
                res = px_sock_ioctl_fionread(d, &avail);
            } else if (d->kind == PX_FILE && !d->is_dir) {
                LARGE_INTEGER size;
                if (GetFileSizeEx(d->h, &size) && size.QuadPart > d->pos)
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
// is pumped by a thread into a socket so it can be polled like anything else.

static HANDLE con_in = INVALID_HANDLE_VALUE;
static DWORD con_in_mode_saved, con_out_mode_saved;
static bool con_in_saved, con_out_saved;
static HANDLE con_out = INVALID_HANDLE_VALUE;

static bool send_all(SOCKET s, const char *buf, int n) {
    while (n > 0) {
        int put = send(s, buf, n, 0);
        if (put == SOCKET_ERROR) {
            if (WSAGetLastError() != WSAEWOULDBLOCK)
                return false;
            px_sock_wait(s, POLLWRNORM, 100);
            continue;
        }
        buf += put;
        n -= put;
    }
    return true;
}

static DWORD WINAPI stdin_pump(void *param) {
    SOCKET out = (SOCKET) param;
    HANDLE in = con_in;
    DWORD mode;
    bool console = GetConsoleMode(in, &mode);
    char buf[4096];
    wchar_t wbuf[1024];
    for (;;) {
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
        if (!send_all(out, buf, (int) got))
            break;
    }
    shutdown(out, SD_SEND);
    closesocket(out);
    return 0;
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
    SOCKET s[2];
    if (px_sock_pair(1, s) < 0)
        return px_desc_new(PX_NUL);
    struct px_desc *d = px_desc_new(PX_SOCK);
    d->s = s[0];
    d->is_pipe = true;
    d->is_tty = console;
    d->domain = d->type = 1;
    HANDLE thread = CreateThread(NULL, 0, stdin_pump, (void *) s[1], 0, NULL);
    if (thread != NULL)
        CloseHandle(thread);
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
