// Force-included (-include) into every iSH translation unit on Windows. Pulls
// in the mingw headers first, then supplies what POSIX has and they lack, and
// routes the fd, path, socket, memory and signal calls to the px layer
// (platform/windows/px.h). The empty headers in platform/windows/include only
// make the POSIX #includes resolve; everything lives here.
//
// The renames are object-like macros, so they also rename struct members of
// the same name (fd_ops.read, fd_ops.poll, ...). Every TU sees this file
// first, so they do so consistently.
#ifndef PLATFORM_WINDOWS_COMPAT_H
#define PLATFORM_WINDOWS_COMPAT_H
// Not for assembly, nor for the px implementation itself (built with
// -DPX_IMPL against the real Windows headers).
#if !defined(__ASSEMBLER__) && !defined(PX_IMPL)

// For sources whose own dependencies pull in windows.h (libarchive in
// fakefsify): declare Winsock's names before they are renamed below.
#ifdef PX_WITH_WINDOWS_H
#include <winsock2.h>
#include <windows.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <io.h>
#include <direct.h>
#include <process.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <dirent.h>
#include <signal.h>
#include <setjmp.h>
#include <time.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <limits.h>

#include "px.h"

// ---- types

typedef px_uid_t uid_t;
typedef px_gid_t gid_t;
typedef px_nlink_t nlink_t;
typedef int64_t blksize_t;
typedef int64_t blkcnt_t;
typedef px_socklen_t socklen_t;
typedef uint32_t in_addr_t;
typedef uint16_t in_port_t;
typedef unsigned short sa_family_t;
typedef unsigned long long rlim_t;
typedef unsigned tcflag_t;
typedef unsigned char cc_t;
typedef unsigned speed_t;

// ---- errno: the names mingw lacks, above its own range (1-42, 100-140)

#define ENOTBLK 200
#define EREMOTE 201
#define EMULTIHOP 202
#define EUSERS 203
#define ESOCKTNOSUPPORT 204
#define EPFNOSUPPORT 205
#define ESHUTDOWN 206
#define ETOOMANYREFS 207
#define EHOSTDOWN 208
#define ESTALE 209
#define EDQUOT 210

// ---- limits

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif
#undef MAX_PATH_POSIX
#define NAME_MAX 255
#define SSIZE_MAX INT64_MAX
#define IOV_MAX 1024
#define HOST_NAME_MAX 64

// ---- fcntl / open

#define O_NONBLOCK 0x100000
#define O_NDELAY O_NONBLOCK
#define O_NOFOLLOW 0x200000
#define O_CLOEXEC 0x400000
#define O_DIRECTORY 0x800000
#define O_SYNC 0x1000000
#define O_DSYNC O_SYNC
#define O_NOCTTY 0
#define O_ASYNC 0
#define O_PATH 0x2000000

#define F_DUPFD 0
#define F_GETFD 1
#define F_SETFD 2
#define F_GETFL 3
#define F_SETFL 4
#define F_GETLK 5
#define F_SETLK 6
#define F_SETLKW 7
#define F_GETPATH 50
#define F_DUPFD_CLOEXEC 1030
#define FD_CLOEXEC 1

#define AT_FDCWD -100
#define AT_SYMLINK_NOFOLLOW 0x100
#define AT_REMOVEDIR 0x200
#define AT_EACCESS 0x200
#define AT_SYMLINK_FOLLOW 0x400
#define AT_EMPTY_PATH 0x1000
#define UTIME_NOW ((1l << 30) - 1l)
#define UTIME_OMIT ((1l << 30) - 2l)

#define LOCK_SH 1
#define LOCK_EX 2
#define LOCK_NB 4
#define LOCK_UN 8

// ---- stat

#define S_IFLNK 0xa000
#define S_IFSOCK 0xc000
#define S_ISLNK(m) (((m) & S_IFMT) == S_IFLNK)
#define S_ISSOCK(m) (((m) & S_IFMT) == S_IFSOCK)
#define S_ISUID 04000
#define S_ISGID 02000
#define S_ISVTX 01000
#define st_atime st_atim.tv_sec
#define st_mtime st_mtim.tv_sec
#define st_ctime st_ctim.tv_sec
// the Darwin spellings, which some of iSH uses unconditionally
#define st_atimespec st_atim
#define st_mtimespec st_mtim
#define st_ctimespec st_ctim
// mingw's are 16 and 32 bits wide; Linux's are 64
#define ino_t px_ino_t
#define dev_t px_dev_t

#define major(d) ((unsigned) (((d) >> 8) & 0xfff))
#define minor(d) ((unsigned) (((d) & 0xff) | (((d) >> 12) & 0xfff00)))
#define makedev(ma, mi) ((((uint64_t) (ma) & 0xfff) << 8) | ((uint64_t) (mi) & 0xff) | (((uint64_t) (mi) & 0xfff00) << 12))

#define DT_UNKNOWN 0
#define DT_FIFO 1
#define DT_CHR 2
#define DT_DIR 4
#define DT_BLK 6
#define DT_REG 8
#define DT_LNK 10
#define DT_SOCK 12

// ---- unistd

#define F_OK 0
#define X_OK 1
#define W_OK 2
#define R_OK 4
#define _SC_PAGESIZE 30
#define _SC_PAGE_SIZE _SC_PAGESIZE
#define _SC_NPROCESSORS_ONLN 84
#define _SC_NPROCESSORS_CONF 83
#define _SC_CLK_TCK 2
#define _SC_PHYS_PAGES 85
#define _SC_OPEN_MAX 4
static inline uid_t getuid(void) { return 0; }
static inline uid_t geteuid(void) { return 0; }
static inline gid_t getgid(void) { return 0; }
static inline gid_t getegid(void) { return 0; }
static inline int getppid(void) { return 1; }

// ---- signals. Host signals mostly do not exist; these are tokens for the
// handful of host-side uses (SIGUSR1 interrupts, SIGINT from the console).

#define SIGHUP 1
#define SIGQUIT 3
#define SIGTRAP 5
#define SIGBUS 7
#define SIGKILL 9
#define SIGUSR1 10
#define SIGUSR2 12
#define SIGPIPE 13
#define SIGALRM 14
#define SIGCHLD 17
#define SIGCONT 18
#define SIGSTOP 19
#define SIGTSTP 20
#define SIGTTIN 23
#define SIGTTOU 24
#define SIGWINCH 28
#undef NSIG
#define NSIG 32

typedef unsigned long long sigset_t;
typedef struct {
    int si_signo, si_errno, si_code;
    void *si_addr;
} siginfo_t;
typedef struct {
    void *ss_sp;
    int ss_flags;
    size_t ss_size;
} stack_t;
struct sigaction {
    union {
        void (*sa_handler)(int);
        void (*sa_sigaction)(int, siginfo_t *, void *);
    };
    sigset_t sa_mask;
    int sa_flags;
};
#define SA_SIGINFO 4
#define SA_ONSTACK 0x08000000
#define SA_RESTART 0x10000000
#define SA_NODEFER 0x40000000
#define SA_RESETHAND 0x80000000
#define SIG_BLOCK 0
#define SIG_UNBLOCK 1
#define SIG_SETMASK 2
#define SIGSTKSZ 16384
#define MINSIGSTKSZ 4096

static inline int sigemptyset(sigset_t *s) { *s = 0; return 0; }
static inline int sigfillset(sigset_t *s) { *s = ~0ull; return 0; }
static inline int sigaddset(sigset_t *s, int n) { *s |= 1ull << n; return 0; }
static inline int sigdelset(sigset_t *s, int n) { *s &= ~(1ull << n); return 0; }
static inline int sigismember(const sigset_t *s, int n) { return !!(*s & (1ull << n)); }
static inline int sigprocmask(int how, const sigset_t *set, sigset_t *old) {
    (void) how; (void) set;
    if (old) *old = 0;
    return 0;
}
#define pthread_sigmask sigprocmask
static inline int sigaltstack(const stack_t *ss, stack_t *old) {
    (void) ss;
    if (old) *old = (stack_t) {0};
    return 0;
}
static inline int sigaction(int sig, const struct sigaction *act, struct sigaction *old) {
    if (old) *old = (struct sigaction) {0};
    if (act && sig == SIGINT && !(act->sa_flags & SA_SIGINFO))
        px_set_sigint_handler(act->sa_handler);
    return 0;
}
static inline void (*px_signal(int sig, void (*handler)(int)))(int) {
    if (sig == SIGINT && handler != SIG_IGN)
        px_set_sigint_handler(handler == SIG_DFL ? NULL : handler);
    return SIG_DFL;
}
#define signal px_signal
static inline int px_raise(int sig) { return px_kill(getpid(), sig); }
#define raise px_raise

#define sigjmp_buf jmp_buf
#define sigsetjmp(buf, savemask) setjmp(buf)
#define siglongjmp longjmp

// ---- the px renames

#undef open
#undef close
#undef read
#undef write
#undef lseek
#undef stat
#undef fstat
#undef lstat
#undef mkdir
#undef rmdir
#undef unlink
#undef access
#undef chdir
#undef getcwd
#undef chmod
#undef isatty
#undef dup
#undef dup2
#undef pipe
#undef ftruncate
#undef truncate
#undef fsync
#undef rename
#undef nanosleep
#undef sleep
#undef usleep
#undef gettimeofday

#define open px_open
#define openat px_openat
#define close px_close
#define read px_read
#define write px_write
#define pread px_pread
#define pwrite px_pwrite
#define readv px_readv
#define writev px_writev
#define preadv px_preadv
#define pwritev px_pwritev
#define lseek px_lseek
#define dup px_dup
#define dup2 px_dup2
#define pipe px_pipe
#define fcntl px_fcntl
#define ioctl px_ioctl
#define isatty px_isatty
#define fsync px_fsync
#define fdatasync px_fsync
#define ftruncate px_ftruncate
#define truncate px_truncate
#define flock px_flock

#define stat px_stat
#define fstat px_fstat
#define lstat px_lstat
#define fstatat px_fstatat
#define unlinkat px_unlinkat
#define unlink px_unlink
#define rmdir px_rmdir
#define renameat px_renameat
#define rename px_rename
#define mkdirat px_mkdirat
#define mkdir px_mkdir
#define linkat px_linkat
#define link px_link
#define symlinkat px_symlinkat
#define symlink px_symlink
#define readlinkat px_readlinkat
#define readlink px_readlink
#define fchmodat px_fchmodat
#define fchmod px_fchmod
#define chmod px_chmod
#define fchownat px_fchownat
#define fchown px_fchown
#define chown px_chown
#define lchown px_chown
#define utimensat px_utimensat
#define futimens px_futimens
#define faccessat px_faccessat
#define access px_access
#define chdir px_chdir
#define fchdir px_fchdir
#define getcwd px_getcwd
#define realpath px_realpath
#define mkfifo px_mkfifo
#define mkstemp px_mkstemp
#define statvfs px_statvfs
#define fstatvfs px_fstatvfs

#define DIR px_DIR
#define dirent px_dirent
#define opendir px_opendir
#define fdopendir px_fdopendir
#define readdir px_readdir
#define closedir px_closedir
#define rewinddir px_rewinddir
#define telldir px_telldir
#define seekdir px_seekdir
#define dirfd px_dirfd

#define iovec px_iovec
#define msghdr px_msghdr
#define pollfd px_pollfd
#define socket px_socket
#define socketpair px_socketpair
#define bind px_bind
#define connect px_connect
#define listen px_listen
#define accept px_accept
#define accept4 px_accept4
#define getsockopt px_getsockopt
#define setsockopt px_setsockopt
#define send px_send
#define recv px_recv
#define sendto px_sendto
#define recvfrom px_recvfrom
#define sendmsg px_sendmsg
#define recvmsg px_recvmsg
#define shutdown px_shutdown
#define getpeername px_getpeername
#define getsockname px_getsockname
#define poll px_poll

#define mmap px_mmap
#define munmap px_munmap
#define mprotect px_mprotect
#define madvise px_madvise
#define msync px_msync

#define pthread_kill px_pthread_kill
#define kill px_kill
#define nanosleep px_nanosleep
#define usleep px_usleep
#define sleep px_sleep

#define winsize px_winsize
#define termios px_termios
#define tcgetattr px_tcgetattr
#define tcsetattr px_tcsetattr
#define cfmakeraw px_cfmakeraw
#define utsname px_utsname
#define uname px_uname
#define sysconf px_sysconf
#define rusage px_rusage
#define getrusage px_getrusage
#define rlimit px_rlimit
#define getrlimit px_getrlimit
#define setrlimit px_setrlimit
#define getpagesize px_getpagesize
#define getrandom px_getrandom
#define timeval px_timeval
#define gettimeofday px_gettimeofday
#define backtrace px_backtrace
#define backtrace_symbols_fd px_backtrace_symbols_fd
#define waitpid px_waitpid
#undef _exit
#define _exit px_exit

// ---- libc odds and ends

#include <stdarg.h>
static inline int dprintf(int fd, const char *fmt, ...) {
    char buf[4096];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > (int) sizeof(buf) - 1)
        n = sizeof(buf) - 1;
    return n < 0 ? n : (int) px_write(fd, buf, n);
}

static inline size_t strlcpy(char *dst, const char *src, size_t size) {
    size_t len = strlen(src);
    if (size != 0) {
        size_t n = len < size - 1 ? len : size - 1;
        memcpy(dst, src, n);
        dst[n] = '\0';
    }
    return len;
}
static inline size_t strlcat(char *dst, const char *src, size_t size) {
    size_t dlen = strnlen(dst, size);
    if (dlen == size)
        return size + strlen(src);
    return dlen + strlcpy(dst + dlen, src, size - dlen);
}
static inline void *memmem(const void *hay, size_t hlen, const void *needle, size_t nlen) {
    if (nlen == 0) return (void *) hay;
    for (const char *p = hay; hlen >= nlen; p++, hlen--)
        if (memcmp(p, needle, nlen) == 0) return (void *) p;
    return NULL;
}
static inline char *strsep(char **s, const char *delim) {
    char *tok = *s;
    if (tok == NULL) return NULL;
    char *end = tok + strcspn(tok, delim);
    if (*end) *end++ = '\0'; else end = NULL;
    *s = end;
    return tok;
}
#define gettid() ((int) GetCurrentThreadId_px())
unsigned long GetCurrentThreadId_px(void);
static inline int pthread_setname_np_px(pthread_t t, const char *name) { (void) t; (void) name; return 0; }
#undef pthread_setname_np
#define pthread_setname_np(...) pthread_setname_np_px(pthread_self(), "")

#endif
#endif
