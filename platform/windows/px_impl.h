// Internals of the Windows POSIX layer. Built with -DPX_IMPL against the
// Windows headers; constants arriving from callers carry Linux values (see
// compat.h and include/px_net.h), which is why they are spelled as numbers
// with the Linux name in a comment.
#ifndef PLATFORM_WINDOWS_PX_IMPL_H
#define PLATFORM_WINDOWS_PX_IMPL_H

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdatomic.h>
#include <errno.h>
#include <fcntl.h>
#include "px.h"

// errno values compat.h adds beyond mingw's own
#define PX_ENOTBLK 200
#define PX_ESHUTDOWN 206
#define PX_EHOSTDOWN 208

// open flags (compat.h)
#define PX_O_ACCMODE 3
#define PX_O_WRONLY 1
#define PX_O_RDWR 2
#define PX_O_NONBLOCK 0x100000
#define PX_O_NOFOLLOW 0x200000
#define PX_O_CLOEXEC 0x400000
#define PX_O_DIRECTORY 0x800000
#define PX_O_PATH 0x2000000
#define PX_AT_FDCWD -100
#define PX_AT_SYMLINK_NOFOLLOW 0x100
#define PX_AT_REMOVEDIR 0x200

// stat modes (same as Linux)
#define PX_S_IFMT 0xf000
#define PX_S_IFSOCK 0xc000
#define PX_S_IFLNK 0xa000
#define PX_S_IFREG 0x8000
#define PX_S_IFDIR 0x4000
#define PX_S_IFCHR 0x2000
#define PX_S_IFIFO 0x1000

// poll events (Linux)
#define PX_POLLIN 0x001
#define PX_POLLPRI 0x002
#define PX_POLLOUT 0x004
#define PX_POLLERR 0x008
#define PX_POLLHUP 0x010
#define PX_POLLNVAL 0x020
#define PX_POLLRDHUP 0x2000

enum px_kind {
    PX_FILE = 1,    // a file or directory handle
    PX_SOCK,        // a socket: real ones, and the pipes and stdin bridged onto them
    PX_CONSOLE_OUT, // an inherited stdout/stderr handle (console, pipe or file)
    PX_NUL,         // NUL: reads EOF, writes vanish
};

// An open file description, shared by dup()s.
struct px_desc {
    atomic_int refs;
    enum px_kind kind;
    HANDLE h;
    SOCKET s;
    int flags;          // PX_O_ACCMODE bits, O_APPEND (_O_APPEND), PX_O_NONBLOCK
    bool is_dir;
    bool is_tty;        // isatty(): the console, or stdin bridged from it
    bool is_pipe;       // a pipe() end: EPIPE rather than ECONNRESET
    bool is_stdin;      // the stdin bridge: reading or polling it asks the pump for more
    int domain, type, protocol; // sockets, Linux values
    // An emulated ICMP datagram ("ping") socket: s reads the replies that
    // the IcmpSendEcho2 workers write into icmp_tx (see net.c).
    bool is_icmp;
    bool icmp_striphdr;           // IP_STRIPHDR: no IP header (else raw-like)
    SOCKET icmp_tx;
    struct sockaddr_in icmp_peer; // connect()ed destination, if any
    int64_t rcvtimeo_ms, sndtimeo_ms; // SO_RCVTIMEO/SO_SNDTIMEO, as stored
    SRWLOCK lock;       // pos
    int64_t pos;        // files: the offset, kept here (all I/O is positional)
    char *path;         // files: the absolute host path it was opened by, '/'
};

// Takes a reference; NULL (errno EBADF) for a bad fd.
struct px_desc *px_get(int fd);
void px_put(struct px_desc *d);
struct px_desc *px_desc_new(enum px_kind kind);
// Installs d at the lowest free fd >= min; consumes the reference.
int px_install(struct px_desc *d, int min, bool cloexec);
void px_init(void);

// errno from GetLastError() / WSAGetLastError()
int px_errno_win(DWORD err);
int px_errno_wsa(int err);
static inline int px_fail_win(void) { errno = px_errno_win(GetLastError()); return -1; }
static inline int px_fail_wsa(void) { errno = px_errno_wsa(WSAGetLastError()); return -1; }
static inline int px_fail(int err) { errno = err; return -1; }

// PX_TRACE=1: log path operations that fail, with the errno
extern int px_trace_on;
void px_trace(const char *op, const char *path, int err);

// paths (path.c)
// The absolute '/' form of path relative to dirfd; returns malloc'd or NULL.
char *px_abspath(int dirfd, const char *path);
// The Win32 \\?\ form of an absolute '/' path, with names Windows refuses
// mapped into the private use area (as Cygwin does). Returns malloc'd.
wchar_t *px_wpath(const char *abs);
// The reverse of the name mapping, for one name from a directory listing.
char *px_name_from_w(const wchar_t *name, size_t len);
HANDLE px_open_handle(const char *abs, DWORD access, DWORD disposition, DWORD flags);
void px_stat_from_info(struct px_stat *st, const BY_HANDLE_FILE_INFORMATION *info, DWORD reparse_tag);
int px_fstat_handle(HANDLE h, struct px_stat *st);

// sockets (net.c)
// A connected pair over loopback TCP (stream) or UDP (dgram), non-blocking.
int px_sock_pair(int type, SOCKET out[2]);
int px_sock_wait(SOCKET s, short events, int timeout_ms);
ssize_t px_sock_read(struct px_desc *d, void *buf, size_t n);
ssize_t px_sock_write(struct px_desc *d, const void *buf, size_t n);
int px_sock_ioctl_fionread(struct px_desc *d, int *out);

// waiting (wait.c)
// Blocks in WSAPoll on fds plus this thread's wake socket. Returns the
// WSAPoll result, or -1 with errno EINTR when interrupted.
int px_wsapoll(WSAPOLLFD *fds, ULONG n, int timeout_ms);
// The thread was interrupted (consumes it).
bool px_interrupted(void);

// console (fd.c)
// The stdin bridge only reads the real stdin when someone wants its data
// (a read, a poll for input, FIONREAD), so the host's stdin is not drained
// on behalf of a guest that never reads it.
void px_stdin_demand(void);
// The real stdin handle behind the bridge (for a child to inherit).
HANDLE px_stdin_handle(void);
int px_console_tcgetattr(struct px_termios *t);
int px_console_tcsetattr(const struct px_termios *t);
int px_console_winsize(struct px_winsize *ws);

#endif
