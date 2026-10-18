// POSIX layer for the Windows build: the types and functions shared by the
// shim headers (platform/windows/include, seen by iSH's own sources through
// compat.h) and the implementation (platform/windows/*.c, built against the
// Windows headers instead). Everything here is px_-prefixed so the two sides
// never collide; compat.h maps the POSIX names onto these.
//
// Host fds are px fds: one table owns every file, directory, socket, pipe and
// console handle, so read/write/poll/dup work the same on all of them. Pipes
// and the console are bridged onto sockets so that a single WSAPoll can wait
// on any mix of fds, plus a per-thread wake socket that px_thread_interrupt()
// uses in place of pthread_kill(SIGUSR1).
//
// Paths are UTF-8 with '/' separators ("C:/x/y"); flags and constants use the
// Linux values from the shim headers.
#ifndef PLATFORM_WINDOWS_PX_H
#define PLATFORM_WINDOWS_PX_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <sys/types.h>
#include <time.h>
#include <pthread.h>

typedef unsigned px_uid_t;
typedef unsigned px_gid_t;
typedef unsigned px_mode_t;
typedef uint64_t px_ino_t;
typedef uint64_t px_dev_t;
typedef uint64_t px_nlink_t;
typedef int px_socklen_t;

struct px_stat {
    px_dev_t st_dev;
    px_ino_t st_ino;
    px_mode_t st_mode;
    px_nlink_t st_nlink;
    px_uid_t st_uid;
    px_gid_t st_gid;
    px_dev_t st_rdev;
    int64_t st_size;
    int64_t st_blksize;
    int64_t st_blocks;
    struct timespec st_atim;
    struct timespec st_mtim;
    struct timespec st_ctim;
};

struct px_iovec {
    void *iov_base;
    size_t iov_len;
};

struct px_msghdr {
    void *msg_name;
    px_socklen_t msg_namelen;
    struct px_iovec *msg_iov;
    size_t msg_iovlen;
    void *msg_control;
    size_t msg_controllen;
    int msg_flags;
};

struct px_pollfd {
    int fd;
    short events;
    short revents;
};

struct px_dirent {
    px_ino_t d_ino;
    int64_t d_off;
    unsigned short d_reclen;
    unsigned char d_type;
    char d_name[1024];
};
typedef struct px_dir px_DIR;

struct px_winsize {
    unsigned short ws_row, ws_col, ws_xpixel, ws_ypixel;
};

struct px_termios {
    unsigned c_iflag, c_oflag, c_cflag, c_lflag;
    unsigned char c_line;
    unsigned char c_cc[32];
    unsigned c_ispeed, c_ospeed;
};

struct px_utsname {
    char sysname[65], nodename[65], release[65], version[65], machine[65], domainname[65];
};

struct px_statvfs {
    unsigned long long f_bsize, f_frsize, f_blocks, f_bfree, f_bavail;
    unsigned long long f_files, f_ffree, f_favail, f_fsid, f_flag, f_namemax;
};

struct px_timeval {
    int64_t tv_sec;
    int64_t tv_usec;
};

struct px_rusage {
    struct px_timeval ru_utime, ru_stime;
    long long ru_maxrss, ru_ixrss, ru_idrss, ru_isrss, ru_minflt, ru_majflt, ru_nswap;
    long long ru_inblock, ru_oublock, ru_msgsnd, ru_msgrcv, ru_nsignals, ru_nvcsw, ru_nivcsw;
};

struct px_rlimit {
    uint64_t rlim_cur, rlim_max;
};

// fds
int px_open(const char *path, int flags, ...);
int px_openat(int dirfd, const char *path, int flags, ...);
int px_close(int fd);
ssize_t px_read(int fd, void *buf, size_t n);
ssize_t px_write(int fd, const void *buf, size_t n);
ssize_t px_pread(int fd, void *buf, size_t n, int64_t off);
ssize_t px_pwrite(int fd, const void *buf, size_t n, int64_t off);
ssize_t px_readv(int fd, const struct px_iovec *iov, int cnt);
ssize_t px_writev(int fd, const struct px_iovec *iov, int cnt);
ssize_t px_preadv(int fd, const struct px_iovec *iov, int cnt, int64_t off);
ssize_t px_pwritev(int fd, const struct px_iovec *iov, int cnt, int64_t off);
int64_t px_lseek(int fd, int64_t off, int whence);
int px_dup(int fd);
int px_dup2(int fd, int to);
int px_pipe(int fds[2]);
int px_fcntl(int fd, int cmd, ...);
int px_ioctl(int fd, unsigned long req, ...);
int px_isatty(int fd);
int px_fsync(int fd);
int px_ftruncate(int fd, int64_t len);
int px_truncate(const char *path, int64_t len);
int px_flock(int fd, int op);

// paths
int px_fstat(int fd, struct px_stat *st);
int px_stat(const char *path, struct px_stat *st);
int px_lstat(const char *path, struct px_stat *st);
int px_fstatat(int dirfd, const char *path, struct px_stat *st, int flags);
int px_unlinkat(int dirfd, const char *path, int flags);
int px_unlink(const char *path);
int px_rmdir(const char *path);
int px_renameat(int olddirfd, const char *oldpath, int newdirfd, const char *newpath);
int px_rename(const char *oldpath, const char *newpath);
int px_mkdirat(int dirfd, const char *path, px_mode_t mode);
int px_mkdir(const char *path, px_mode_t mode);
int px_linkat(int olddirfd, const char *oldpath, int newdirfd, const char *newpath, int flags);
int px_link(const char *oldpath, const char *newpath);
int px_symlinkat(const char *target, int dirfd, const char *path);
int px_symlink(const char *target, const char *path);
ssize_t px_readlinkat(int dirfd, const char *path, char *buf, size_t size);
ssize_t px_readlink(const char *path, char *buf, size_t size);
int px_fchmodat(int dirfd, const char *path, px_mode_t mode, int flags);
int px_fchmod(int fd, px_mode_t mode);
int px_chmod(const char *path, px_mode_t mode);
int px_fchownat(int dirfd, const char *path, px_uid_t uid, px_gid_t gid, int flags);
int px_fchown(int fd, px_uid_t uid, px_gid_t gid);
int px_chown(const char *path, px_uid_t uid, px_gid_t gid);
int px_utimensat(int dirfd, const char *path, const struct timespec times[2], int flags);
int px_futimens(int fd, const struct timespec times[2]);
int px_faccessat(int dirfd, const char *path, int mode, int flags);
int px_access(const char *path, int mode);
int px_chdir(const char *path);
int px_fchdir(int fd);
char *px_getcwd(char *buf, size_t size);
char *px_realpath(const char *path, char *resolved);
int px_mkfifo(const char *path, px_mode_t mode);
int px_mkstemp(char *tmpl);
int px_fstatvfs(int fd, struct px_statvfs *st);
int px_statvfs(const char *path, struct px_statvfs *st);
// Makes a directory case sensitive, like a Linux file system; directories
// created under it later inherit that (NTFS, Windows 10 1803+).
int px_set_case_sensitive(const char *path);
// The host path an fd was opened with (F_GETPATH).
int px_fd_path(int fd, char *buf, size_t size);
// Diagnostics (kernel/state_dump.c, the guest snapshot): one line about
// host fd `fd`; for a socket also its local and peer ports (a pipe() end is
// one side of a loopback pair, so the two ends of a pipe have each other's
// ports). Only queries - nothing the guest could notice.
int px_describe(int fd, char *buf, size_t size, int *local_port, int *peer_port);
// The guest snapshot's trigger: the named event Local\ish-state-<host pid>;
// each time it is set, `fn` runs on a thread of its own (blocked in a wait
// the rest of the time: nothing while ish runs normally). False when it
// could not be made.
bool px_state_watch(void (*fn)(void));
uint64_t px_tick_ms(void);
unsigned long px_host_pid(void);
// rename(), replacing `to` if it exists.
int px_rename_replace(const char *from, const char *to);

px_DIR *px_opendir(const char *path);
px_DIR *px_fdopendir(int fd);
struct px_dirent *px_readdir(px_DIR *dir);
int px_closedir(px_DIR *dir);
void px_rewinddir(px_DIR *dir);
long px_telldir(px_DIR *dir);
void px_seekdir(px_DIR *dir, long pos);
int px_dirfd(px_DIR *dir);

// sockets
int px_socket(int domain, int type, int protocol);
int px_socketpair(int domain, int type, int protocol, int sv[2]);
int px_bind(int fd, const void *addr, px_socklen_t len);
int px_connect(int fd, const void *addr, px_socklen_t len);
int px_listen(int fd, int backlog);
int px_accept(int fd, void *addr, px_socklen_t *len);
int px_accept4(int fd, void *addr, px_socklen_t *len, int flags);
int px_getsockopt(int fd, int level, int name, void *val, px_socklen_t *len);
int px_setsockopt(int fd, int level, int name, const void *val, px_socklen_t len);
ssize_t px_send(int fd, const void *buf, size_t n, int flags);
ssize_t px_recv(int fd, void *buf, size_t n, int flags);
ssize_t px_sendto(int fd, const void *buf, size_t n, int flags, const void *addr, px_socklen_t len);
ssize_t px_recvfrom(int fd, void *buf, size_t n, int flags, void *addr, px_socklen_t *len);
ssize_t px_sendmsg(int fd, const struct px_msghdr *msg, int flags);
ssize_t px_recvmsg(int fd, struct px_msghdr *msg, int flags);
int px_shutdown(int fd, int how);
int px_getpeername(int fd, void *addr, px_socklen_t *len);
int px_getsockname(int fd, void *addr, px_socklen_t *len);
int px_poll(struct px_pollfd *fds, unsigned long nfds, int timeout);

// memory
void *px_mmap(void *addr, size_t len, int prot, int flags, int fd, int64_t off);
int px_munmap(void *addr, size_t len);
int px_mprotect(void *addr, size_t len, int prot);
// The JIT's code and data regions, straight on VirtualAlloc: reserve address
// space (no commit charge) near hint, then commit pieces as they fill.
void *px_jit_reserve(void *hint, size_t len);
bool px_jit_commit(void *addr, size_t len, bool exec);
void px_jit_release(void *addr);
// A file mapped shared and read-write, size bytes (grown sparse if shorter),
// for the translation cache other ish processes map too. NULL on failure.
// path is UTF-8; directories are created.
void *px_shared_file_map(const char *path, size_t size);
// This executable's path (UTF-8, '/' separated), or NULL.
const char *px_self_exe(void);
// Brings [addr, addr + len) of a mapping in with one call (PrefetchVirtualMemory)
// instead of a fault per page.
void px_prefetch(void *addr, size_t len);
// %LOCALAPPDATA% (UTF-8), or NULL.
const char *px_local_appdata(void);
int px_madvise(void *addr, size_t len, int advice);
int px_msync(void *addr, size_t len, int flags);

// threads and signals
// Wakes the thread out of whatever px call it is blocked in (it returns
// EINTR), or out of the next one if it is not blocked yet.
void px_thread_interrupt(pthread_t thread);
int px_pthread_kill(pthread_t thread, int sig);
int px_kill(int pid, int sig);
int px_nanosleep(const struct timespec *req, struct timespec *rem);
// A wake-up flag one thread waits on and others set (an auto-reset event):
// what the futex code uses on Windows instead of a pipe, which is a pair of
// loopback sockets there. px_wake_wait returns 1 when set, 0 on timeout
// (timeout_ms < 0: none), -1/EINTR when the thread is interrupted
// (px_pthread_kill), like px_wsapoll.
// CPU time (user + kernel) of the calling thread
uint64_t px_thread_cpu_ns(void);
void *px_wake_new(void);
void px_wake_set(void *w);
void px_wake_clear(void *w);
int px_wake_wait(void *w, int timeout_ms);
void px_wake_free(void *w);
int px_usleep(unsigned usec);
unsigned px_sleep(unsigned sec);
// signal(SIGINT) / sigaction(SIGINT): the console's ^C handler.
void px_set_sigint_handler(void (*handler)(int));

// terminal
int px_tcgetattr(int fd, struct px_termios *t);
int px_tcsetattr(int fd, int act, const struct px_termios *t);
void px_cfmakeraw(struct px_termios *t);

// misc
int px_uname(struct px_utsname *u);
long px_sysconf(int name);
int px_getrusage(int who, struct px_rusage *ru);
int px_getrlimit(int res, struct px_rlimit *rl);
int px_setrlimit(int res, const struct px_rlimit *rl);
int px_getpagesize(void);
ssize_t px_getrandom(void *buf, size_t n, unsigned flags);
int px_gettimeofday(struct px_timeval *tv, void *tz);
int px_backtrace(void **buf, int size);
void px_backtrace_symbols_fd(void *const *buf, int size, int fd);
// Host-wide CPU times (100ns units: user excludes idle, system excludes
// idle), memory, and uptime, for /proc.
void px_cpu_times(uint64_t *user, uint64_t *system, uint64_t *idle);
void px_mem_info(uint64_t *total, uint64_t *avail);
uint64_t px_uptime_ms(void);
// The process's private committed memory (its footprint, for the memory
// governor), and a last-chance handler that reports host crashes.
uint64_t px_process_footprint(void);
void px_crash_init(void);
// PX_PROF=1: sample this thread (one that runs guest code)
void px_prof_register(void);
// _exit(): also puts the console back the way it was found.
_Noreturn void px_exit(int status);
// Starts a Windows program with fds[0..2] (px fds, -1 for NUL) as its stdio
// and cwd (NULL: ours). envp is laid over our own environment; the variables
// Windows itself depends on (PATH, SystemRoot, TEMP...) keep the host's
// values. Returns 0 or an errno value.
int px_spawn(int *pid, const char *path, const int fds[3], char *const argv[], char *const envp[], const char *cwd);
int px_waitpid(int pid, int *status, int options);

#endif
