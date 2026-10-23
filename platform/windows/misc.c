// The rest of POSIX the build needs: system information, time, randomness,
// backtraces, symbols, and child processes.
#include "px_impl.h"
#include <mmsystem.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <psapi.h>
#include <bcrypt.h>
#include <wchar.h>
#include <wctype.h>

NTSYSAPI LONG NTAPI NtSetTimerResolution(ULONG desired, BOOLEAN set, PULONG current);

void px_misc_init(void) {
    // 1ms scheduler ticks: the WSAPoll and condition variable timeouts the
    // emulator leans on round up to a tick, 15.6ms by default.
    ULONG current;
    NtSetTimerResolution(10000, TRUE, &current);
}

unsigned long GetCurrentThreadId_px(void) {
    return GetCurrentThreadId();
}

int px_uname(struct px_utsname *u) {
    memset(u, 0, sizeof(*u));
    strcpy(u->sysname, "Windows_NT");
    DWORD n = sizeof(u->nodename);
    if (!GetComputerNameA(u->nodename, &n))
        strcpy(u->nodename, "localhost");
    // RtlGetVersion tells the truth where GetVersionEx is shimmed
    typedef LONG (WINAPI *rtl_get_version_t)(OSVERSIONINFOW *);
    OSVERSIONINFOW vi = {.dwOSVersionInfoSize = sizeof(vi)};
    rtl_get_version_t rgv = (rtl_get_version_t) (void *) GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion");
    if (rgv)
        rgv(&vi);
    snprintf(u->release, sizeof(u->release), "%lu.%lu.%lu", vi.dwMajorVersion, vi.dwMinorVersion, vi.dwBuildNumber);
    strcpy(u->version, "Windows");
    SYSTEM_INFO si;
    GetNativeSystemInfo(&si);
    strcpy(u->machine, si.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_ARM64 ? "aarch64" : "x86_64");
    return 0;
}

long px_sysconf(int name) {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    switch (name) {
        case 30: /* _SC_PAGESIZE */ return 4096;
        case 83: /* _SC_NPROCESSORS_CONF */
        case 84: /* _SC_NPROCESSORS_ONLN */ return si.dwNumberOfProcessors;
        case 2: /* _SC_CLK_TCK */ return 100;
        case 4: /* _SC_OPEN_MAX */ return 65536;
        case 85: { /* _SC_PHYS_PAGES */
            MEMORYSTATUSEX ms = {.dwLength = sizeof(ms)};
            GlobalMemoryStatusEx(&ms);
            return (long) (ms.ullTotalPhys / 4096);
        }
    }
    errno = EINVAL;
    return -1;
}

static struct px_timeval tv_from_filetime(FILETIME ft) {
    uint64_t t = (((uint64_t) ft.dwHighDateTime << 32) | ft.dwLowDateTime) / 10;
    return (struct px_timeval) {.tv_sec = t / 1000000, .tv_usec = t % 1000000};
}

int px_getrusage(int who, struct px_rusage *ru) {
    memset(ru, 0, sizeof(*ru));
    FILETIME create, exit_, kernel, user;
    BOOL ok = who == 1 /* RUSAGE_THREAD */
        ? GetThreadTimes(GetCurrentThread(), &create, &exit_, &kernel, &user)
        : GetProcessTimes(GetCurrentProcess(), &create, &exit_, &kernel, &user);
    if (who == -1 /* RUSAGE_CHILDREN */)
        return 0;
    if (!ok)
        return px_fail_win();
    ru->ru_utime = tv_from_filetime(user);
    ru->ru_stime = tv_from_filetime(kernel);
    PROCESS_MEMORY_COUNTERS pmc;
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) {
        ru->ru_maxrss = pmc.PeakWorkingSetSize / 1024;
        ru->ru_majflt = pmc.PageFaultCount;
    }
    return 0;
}

int px_getrlimit(int res, struct px_rlimit *rl) {
    switch (res) {
        case 7: /* RLIMIT_NOFILE */
            rl->rlim_cur = rl->rlim_max = 65536;
            return 0;
        case 3: /* RLIMIT_STACK */
            rl->rlim_cur = 8 << 20;
            rl->rlim_max = ~0ull;
            return 0;
        default:
            rl->rlim_cur = rl->rlim_max = ~0ull;
            return 0;
    }
}

int px_setrlimit(int res, const struct px_rlimit *rl) {
    (void) res; (void) rl;
    return 0;
}

int px_getpagesize(void) {
    return 4096;
}

ssize_t px_getrandom(void *buf, size_t n, unsigned flags) {
    (void) flags;
    if (BCryptGenRandom(NULL, buf, (ULONG) n, BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0)
        return px_fail(EIO);
    return n;
}

int px_gettimeofday(struct px_timeval *tv, void *tz) {
    (void) tz;
    FILETIME ft;
    GetSystemTimePreciseAsFileTime(&ft);
    uint64_t t = (((uint64_t) ft.dwHighDateTime << 32) | ft.dwLowDateTime) - 116444736000000000ull;
    tv->tv_sec = t / 10000000;
    tv->tv_usec = (t % 10000000) / 10;
    return 0;
}

int px_backtrace(void **buf, int size) {
    return RtlCaptureStackBackTrace(1, size, buf, NULL);
}

void px_backtrace_symbols_fd(void *const *buf, int size, int fd) {
    HMODULE self = GetModuleHandleW(NULL);
    for (int i = 0; i < size; i++) {
        char line[96];
        int n = snprintf(line, sizeof(line), "#%-2d %p (ish+%#llx)\n", i, buf[i],
                (unsigned long long) ((char *) buf[i] - (char *) self));
        px_write(fd, line, n);
    }
}

void *px_dlsym(void *handle, const char *name) {
    if (handle != NULL)
        return (void *) GetProcAddress(handle, name);
    return (void *) GetProcAddress(GetModuleHandleW(NULL), name);
}

// ---- child processes (native offload)

struct child {
    int pid;
    HANDLE h;
    struct child *next;
};
static struct child *children;
static SRWLOCK children_lock = SRWLOCK_INIT;

static HANDLE child_find(int pid, bool take) {
    AcquireSRWLockExclusive(&children_lock);
    HANDLE h = NULL;
    for (struct child **c = &children; *c; c = &(*c)->next) {
        if ((*c)->pid == pid) {
            h = (*c)->h;
            if (take) {
                struct child *dead = *c;
                *c = dead->next;
                free(dead);
            }
            break;
        }
    }
    ReleaseSRWLockExclusive(&children_lock);
    return h;
}

// Appends arg to cmd quoted the way CommandLineToArgvW reads it back.
static void quote_arg(wchar_t **cmd, size_t *len, size_t *cap, const char *arg) {
    int n = MultiByteToWideChar(CP_UTF8, 0, arg, -1, NULL, 0);
    wchar_t *w = malloc(n * sizeof(wchar_t));
    MultiByteToWideChar(CP_UTF8, 0, arg, -1, w, n);
    size_t need = *len + 2 * n + 4;
    if (need > *cap) {
        *cap = need * 2;
        *cmd = realloc(*cmd, *cap * sizeof(wchar_t));
    }
    wchar_t *o = *cmd + *len;
    if (*len)
        *o++ = ' ';
    *o++ = '"';
    for (wchar_t *p = w; *p; p++) {
        size_t slashes = 0;
        while (p[0] == '\\') {
            slashes++;
            p++;
        }
        if (*p == '"' || *p == 0) {
            for (size_t i = 0; i < slashes * 2; i++)
                *o++ = '\\';
            if (*p == 0) {
                p--;
                break;
            }
            *o++ = '\\';
            *o++ = '"';
        } else {
            for (size_t i = 0; i < slashes; i++)
                *o++ = '\\';
            *o++ = *p;
        }
    }
    *o++ = '"';
    *o = 0;
    *len = o - *cmd;
    free(w);
}

// The child's end of one of its stdio fds: an inheritable handle. Sockets
// (our pipes) cannot be a child's stdio, so those get a pipe with a thread
// copying between it and the socket.
struct relay {
    HANDLE pipe;
    int fd;
    bool to_child;
};

static DWORD WINAPI relay_thread(void *param) {
    struct relay *r = param;
    char buf[8192];
    if (r->to_child) {
        for (;;) {
            ssize_t n = px_read(r->fd, buf, sizeof(buf));
            if (n <= 0)
                break;
            DWORD put;
            if (!WriteFile(r->pipe, buf, (DWORD) n, &put, NULL))
                break;
        }
    } else {
        for (;;) {
            DWORD got;
            if (!ReadFile(r->pipe, buf, sizeof(buf), &got, NULL) || got == 0)
                break;
            for (DWORD off = 0; off < got;) {
                ssize_t n = px_write(r->fd, buf + off, got - off);
                if (n <= 0)
                    goto done;
                off += n;
            }
        }
    }
done:
    CloseHandle(r->pipe);
    px_close(r->fd);
    free(r);
    return 0;
}

static HANDLE child_stdio(int fd, bool input, HANDLE *relay_out) {
    *relay_out = NULL;
    if (fd < 0) {
        SECURITY_ATTRIBUTES sa = {.nLength = sizeof(sa), .bInheritHandle = TRUE};
        return CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                OPEN_EXISTING, 0, NULL);
    }
    struct px_desc *d = px_get(fd);
    if (d == NULL)
        return NULL;
    HANDLE h = NULL;
    if (d->kind == PX_FILE || d->kind == PX_CONSOLE_OUT) {
        DuplicateHandle(GetCurrentProcess(), d->h, GetCurrentProcess(), &h, 0, TRUE, DUPLICATE_SAME_ACCESS);
    } else if (d->kind == PX_SOCK && d->is_stdin && px_stdin_handle() != INVALID_HANDLE_VALUE) {
        // the child inherits our stdin itself, as on POSIX, instead of a relay
        // that would read (and drain) it whether or not the child wants it
        DuplicateHandle(GetCurrentProcess(), px_stdin_handle(), GetCurrentProcess(), &h, 0, TRUE,
                DUPLICATE_SAME_ACCESS);
    } else if (d->kind == PX_SOCK) {
        SECURITY_ATTRIBUTES sa = {.nLength = sizeof(sa), .bInheritHandle = TRUE};
        HANDLE r, w;
        if (CreatePipe(&r, &w, &sa, 0)) {
            struct relay *rl = malloc(sizeof(*rl));
            rl->fd = px_dup(fd);
            rl->to_child = input;
            rl->pipe = input ? w : r;
            h = input ? r : w;
            SetHandleInformation(rl->pipe, HANDLE_FLAG_INHERIT, 0);
            *relay_out = CreateThread(NULL, 0, relay_thread, rl, CREATE_SUSPENDED, NULL);
        }
    }
    px_put(d);
    return h;
}

// Our environment with envp's entries laid over it, as a Win32 block.
static bool env_keep_host(const char *entry) {
    static const char *const keep[] = {"PATH=", "SYSTEMROOT=", "WINDIR=", "TEMP=", "TMP=", "COMSPEC=",
        "PATHEXT=", "SYSTEMDRIVE=", "USERPROFILE=", "APPDATA=", "LOCALAPPDATA=", "PROGRAMDATA=", "HOME=", NULL};
    for (const char *const *k = keep; *k; k++)
        if (_strnicmp(entry, *k, strlen(*k)) == 0)
            return true;
    return false;
}

static wchar_t *child_env(char *const envp[]) {
    size_t len = 0, cap = 4096;
    wchar_t *env = malloc(cap * sizeof(wchar_t));
    #define ENV_PUT(w, n) do { \
        if (len + (n) + 2 > cap) { cap = (len + (n) + 2) * 2; env = realloc(env, cap * sizeof(wchar_t)); } \
        memcpy(env + len, (w), (n) * sizeof(wchar_t)); len += (n); } while (0)
    // guest entries first (minus the ones Windows needs from us)...
    for (int i = 0; envp[i]; i++) {
        if (env_keep_host(envp[i]) || strchr(envp[i], '=') == NULL)
            continue;
        int n = MultiByteToWideChar(CP_UTF8, 0, envp[i], -1, NULL, 0);
        wchar_t *w = malloc(n * sizeof(wchar_t));
        MultiByteToWideChar(CP_UTF8, 0, envp[i], -1, w, n);
        ENV_PUT(w, (size_t) n);
        free(w);
    }
    // ...then ours, for every name the guest did not set
    wchar_t *host = GetEnvironmentStringsW();
    for (wchar_t *e = host; *e; e += wcslen(e) + 1) {
        wchar_t *eq = wcschr(e + 1, '=');
        if (eq == NULL)
            continue;
        size_t nlen = eq - e + 1;
        bool dup = false;
        for (size_t off = 0; off < len; off += wcslen(env + off) + 1)
            if (_wcsnicmp(env + off, e, nlen) == 0) { dup = true; break; }
        if (!dup)
            ENV_PUT(e, wcslen(e) + 1);
    }
    FreeEnvironmentStringsW(host);
    ENV_PUT(L"", 1);
    #undef ENV_PUT
    return env;
}

int px_spawn(int *pid, const char *path, const int fds[3], char *const argv[], char *const envp[], const char *cwd) {
    px_init();
    wchar_t *cmd = NULL;
    size_t len = 0, cap = 0;
    for (int i = 0; argv[i] != NULL; i++)
        quote_arg(&cmd, &len, &cap, argv[i]);
    if (cmd == NULL) {
        cmd = calloc(1, sizeof(wchar_t));
    }
    wchar_t *env = envp != NULL ? child_env(envp) : NULL;
    char *abs = px_abspath(PX_AT_FDCWD, path);
    wchar_t *wpath = abs ? px_wpath(abs) : NULL;
    free(abs);
    wchar_t *wcwd = NULL;
    if (cwd != NULL) {
        char *acwd = px_abspath(PX_AT_FDCWD, cwd);
        if (acwd != NULL) {
            int n = MultiByteToWideChar(CP_UTF8, 0, acwd, -1, NULL, 0);
            wcwd = malloc(n * sizeof(wchar_t));
            MultiByteToWideChar(CP_UTF8, 0, acwd, -1, wcwd, n);
            for (wchar_t *p = wcwd; *p; p++)
                if (*p == '/')
                    *p = '\\';
            free(acwd);
        }
    }

    STARTUPINFOW si = {.cb = sizeof(si), .dwFlags = STARTF_USESTDHANDLES};
    HANDLE relays[3] = {0};
    HANDLE *std[3] = {&si.hStdInput, &si.hStdOutput, &si.hStdError};
    for (int i = 0; i < 3; i++)
        *std[i] = child_stdio(fds[i], i == 0, &relays[i]);
    PROCESS_INFORMATION pi;
    BOOL ok = wpath != NULL && CreateProcessW(wpath, cmd, NULL, NULL, TRUE,
            CREATE_UNICODE_ENVIRONMENT, env, wcwd, &si, &pi);
    DWORD err = GetLastError();
    for (int i = 0; i < 3; i++) {
        if (*std[i])
            CloseHandle(*std[i]);
        if (relays[i]) {
            ResumeThread(relays[i]);
            CloseHandle(relays[i]);
        }
    }
    free(cmd);
    free(env);
    free(wpath);
    free(wcwd);
    if (!ok)
        return px_errno_win(err);
    CloseHandle(pi.hThread);
    struct child *c = malloc(sizeof(*c));
    c->pid = (int) pi.dwProcessId;
    c->h = pi.hProcess;
    AcquireSRWLockExclusive(&children_lock);
    c->next = children;
    children = c;
    ReleaseSRWLockExclusive(&children_lock);
    *pid = c->pid;
    return 0;
}

int px_waitpid(int pid, int *status, int options) {
    HANDLE h = child_find(pid, false);
    if (h == NULL)
        return px_fail(ECHILD);
    DWORD w = WaitForSingleObject(h, (options & 1 /* WNOHANG */) ? 0 : INFINITE);
    if (w == WAIT_TIMEOUT)
        return 0;
    DWORD code = 0;
    GetExitCodeProcess(h, &code);
    child_find(pid, true);
    CloseHandle(h);
    if (status != NULL) {
        // TerminateProcess(128 + sig) from px_spawn_kill reads as a signal
        if (code > 128 && code < 128 + 32)
            *status = code - 128;
        else
            *status = (code & 0xff) << 8;
    }
    return pid;
}

int px_spawn_kill(int pid, int sig) {
    HANDLE h = child_find(pid, false);
    if (h == NULL)
        return px_fail(ESRCH);
    if (sig == 0)
        return 0;
    if (!TerminateProcess(h, 128 + sig))
        return px_fail_win();
    return 0;
}

static uint64_t u64_from_filetime(FILETIME ft) {
    return ((uint64_t) ft.dwHighDateTime << 32) | ft.dwLowDateTime;
}

void px_cpu_times(uint64_t *user, uint64_t *system, uint64_t *idle) {
    FILETIME i, k, u;
    *user = *system = *idle = 0;
    if (!GetSystemTimes(&i, &k, &u))
        return;
    *idle = u64_from_filetime(i);
    *user = u64_from_filetime(u);
    // kernel time includes idle time
    *system = u64_from_filetime(k) - *idle;
}

void px_mem_info(uint64_t *total, uint64_t *avail) {
    MEMORYSTATUSEX ms = {.dwLength = sizeof(ms)};
    GlobalMemoryStatusEx(&ms);
    *total = ms.ullTotalPhys;
    *avail = ms.ullAvailPhys;
}

void px_boot_mark(const char *what) {
    static int on = -1;
    if (on < 0)
        on = getenv("ISH_BOOT_TRACE") != NULL;
    if (!on)
        return;
    FILETIME made, e, k, u, now;
    GetProcessTimes(GetCurrentProcess(), &made, &e, &k, &u);
    GetSystemTimePreciseAsFileTime(&now);
    uint64_t a = ((uint64_t) made.dwHighDateTime << 32) | made.dwLowDateTime;
    uint64_t b = ((uint64_t) now.dwHighDateTime << 32) | now.dwLowDateTime;
    fprintf(stderr, "[boot] %-24s %7.2f ms\n", what, (b - a) / 1e4);
}

// The log file stays under LOG_MAX: past it the file becomes <name>.1 (the
// one before goes) and a new one starts. Every ish of a session appends to
// the same file, so the rotation happens under a mutex named for the path,
// and an ish still writing to what is now .1 notices it is too big and
// opens the new one. A process logs at most LOG_RATE lines a second to it.
#define LOG_MAX (64ull << 20)
#define LOG_RATE 500
static INIT_ONCE log_once = INIT_ONCE_STATIC_INIT;
static HANDLE log_file = INVALID_HANDLE_VALUE;
static HANDLE log_mutex;
static wchar_t *log_path, *log_path_old;
static bool log_stderr;
static int log_netdiag;

static bool env_on(const char *name) {
    const char *v = getenv(name);
    return v != NULL && strcmp(v, "1") == 0;
}

static HANDLE log_open(void) {
    // FILE_APPEND_DATA alone: each write lands whole at the end, with every
    // ish of a session writing to the file
    return CreateFileW(log_path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
}

static BOOL CALLBACK log_init(PINIT_ONCE once, void *param, void **ctx) {
    (void) once; (void) param; (void) ctx;
    log_stderr = env_on("ISH_LOG_STDERR");
    // a path in any language: the variable in UTF-16, as it was set
    const wchar_t *path = _wgetenv(L"ISH_LOG_FILE");
    if (path != NULL && path[0] != L'\0') {
        size_t len = wcslen(path);
        log_path = _wcsdup(path);
        log_path_old = malloc((len + 3) * sizeof(wchar_t));
        if (log_path != NULL && log_path_old != NULL) {
            swprintf(log_path_old, len + 3, L"%ls.1", path);
            uint32_t hash = 2166136261u;
            for (size_t i = 0; i < len; i++)
                hash = (hash ^ towlower(path[i])) * 16777619u;
            wchar_t name[64];
            swprintf(name, 64, L"Local\\ish-log-%08x", hash);
            log_mutex = CreateMutexW(NULL, FALSE, name);
            log_file = log_open();
        }
    }
    log_netdiag = env_on("ISH_NETDIAG") ? 2 : log_file != INVALID_HANDLE_VALUE ? 1 : 0;
    return TRUE;
}

static void log_setup(void) {
    InitOnceExecuteOnce(&log_once, log_init, NULL, NULL);
}

bool px_log_stderr(void) {
    log_setup();
    return log_stderr;
}

int px_log_netdiag(void) {
    log_setup();
    return log_netdiag;
}

static bool log_too_big(HANDLE h) {
    LARGE_INTEGER size;
    return GetFileSizeEx(h, &size) && (uint64_t) size.QuadPart >= LOG_MAX;
}

static void log_rotate(void) {
    if (log_mutex != NULL)
        WaitForSingleObject(log_mutex, 5000);
    HANDLE h = log_open();
    if (h != INVALID_HANDLE_VALUE && log_too_big(h)) {
        CloseHandle(h);
        if (!MoveFileExW(log_path, log_path_old, MOVEFILE_REPLACE_EXISTING)) {
            // .1 is open where it can't be replaced: start the file over
            h = CreateFileW(log_path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    NULL, TRUNCATE_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
            if (h != INVALID_HANDLE_VALUE)
                CloseHandle(h);
        }
        h = log_open();
    }
    if (h != INVALID_HANDLE_VALUE) {
        CloseHandle(log_file);
        log_file = h;
    }
    if (log_mutex != NULL)
        ReleaseMutex(log_mutex);
}

static void log_write(const char *line) {
    if (log_too_big(log_file))
        log_rotate();
    SYSTEMTIME t;
    GetLocalTime(&t);
    size_t len = strlen(line);
    char *buf = malloc(len + 64);
    if (buf == NULL)
        return;
    int n = sprintf(buf, "%04u-%02u-%02u %02u:%02u:%02u.%03u [%lu] ", t.wYear, t.wMonth, t.wDay,
            t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, GetCurrentProcessId());
    memcpy(buf + n, line, len);
    buf[n + len] = '\n';
    DWORD put;
    WriteFile(log_file, buf, (DWORD) (n + len + 1), &put, NULL);
    free(buf);
}

// the file, against px_log_set_file() changing it
static SRWLOCK log_switch = SRWLOCK_INIT;

static void log_line(const char *line);

// called with printk's lock held
void px_log_line(const char *line) {
    log_setup();
    if (log_stderr)
        fprintf(stderr, "%s\n", line);
    AcquireSRWLockExclusive(&log_switch);
    log_line(line);
    ReleaseSRWLockExclusive(&log_switch);
}

void px_log_set_file(const char *path) {
    log_setup();
    AcquireSRWLockExclusive(&log_switch);
    if (log_file != INVALID_HANDLE_VALUE)
        CloseHandle(log_file);
    log_file = INVALID_HANDLE_VALUE;
    if (log_mutex != NULL)
        CloseHandle(log_mutex);
    log_mutex = NULL;
    free(log_path);
    free(log_path_old);
    log_path = log_path_old = NULL;
    int n = path != NULL && path[0] != '\0' ? MultiByteToWideChar(CP_UTF8, 0, path, -1, NULL, 0) : 0;
    if (n > 0 && (log_path = malloc(n * sizeof(wchar_t))) != NULL &&
            (log_path_old = malloc((n + 2) * sizeof(wchar_t))) != NULL) {
        MultiByteToWideChar(CP_UTF8, 0, path, -1, log_path, n);
        swprintf(log_path_old, n + 2, L"%ls.1", log_path);
        uint32_t hash = 2166136261u;
        for (int i = 0; log_path[i] != L'\0'; i++)
            hash = (hash ^ towlower(log_path[i])) * 16777619u;
        wchar_t name[64];
        swprintf(name, 64, L"Local\\ish-log-%08x", hash);
        log_mutex = CreateMutexW(NULL, FALSE, name);
        log_file = log_open();
    }
    ReleaseSRWLockExclusive(&log_switch);
}

static void log_line(const char *line) {
    if (log_file == INVALID_HANDLE_VALUE)
        return;
    static uint64_t second;
    static unsigned lines;
    static unsigned long dropped;
    uint64_t now = GetTickCount64() / 1000;
    if (now != second) {
        if (dropped > 0) {
            char note[96];
            sprintf(note, "printk: %lu lines left out (more than %d a second)", dropped, LOG_RATE);
            log_write(note);
        }
        second = now;
        lines = 0;
        dropped = 0;
    }
    if (++lines > LOG_RATE) {
        if (dropped++ == 0) {
            char note[96];
            sprintf(note, "printk: more than %d lines a second, leaving out the rest of this one", LOG_RATE);
            log_write(note);
        }
        return;
    }
    log_write(line);
}

void px_log_notice(const char *line) {
    if (px_log_stderr())
        return; // px_log_line wrote it
    static _Atomic uint64_t last;
    uint64_t now = GetTickCount64();
    uint64_t prev = atomic_load(&last);
    if (prev != 0 && now - prev < 10000)
        return;
    if (!atomic_compare_exchange_strong(&last, &prev, now))
        return;
    fprintf(stderr, "ish: %s\n", line);
}

uint64_t px_uptime_ms(void) {
    return GetTickCount64();
}

uint64_t px_process_footprint(void) {
    PROCESS_MEMORY_COUNTERS_EX pmc;
    if (!GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS *) &pmc, sizeof(pmc)))
        return 0;
    return pmc.PrivateUsage;
}

static LONG WINAPI crash_filter(EXCEPTION_POINTERS *ep) {
    EXCEPTION_RECORD *er = ep->ExceptionRecord;
    CONTEXT *c = ep->ContextRecord;
    char buf[512];
    HMODULE self = GetModuleHandleW(NULL);
    int n = snprintf(buf, sizeof(buf), "\n=== HOST CRASH: exception %#lx at %p (ish+%#llx) ===\n",
            er->ExceptionCode, er->ExceptionAddress,
            (unsigned long long) ((char *) er->ExceptionAddress - (char *) self));
    px_write(2, buf, n);
    if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2) {
        n = snprintf(buf, sizeof(buf), "%s %#llx\n", er->ExceptionInformation[0] ? "write" : "read",
                (unsigned long long) er->ExceptionInformation[1]);
        px_write(2, buf, n);
    }
#ifdef __x86_64__
    n = snprintf(buf, sizeof(buf),
            "rax %016llx rbx %016llx rcx %016llx rdx %016llx\n"
            "rsi %016llx rdi %016llx rbp %016llx rsp %016llx\n"
            "r8  %016llx r9  %016llx r10 %016llx r11 %016llx\n"
            "r12 %016llx r13 %016llx r14 %016llx r15 %016llx\n",
            c->Rax, c->Rbx, c->Rcx, c->Rdx, c->Rsi, c->Rdi, c->Rbp, c->Rsp,
            c->R8, c->R9, c->R10, c->R11, c->R12, c->R13, c->R14, c->R15);
    px_write(2, buf, n);
#else
    (void) c;
#endif
    void *bt[32];
    int frames = RtlCaptureStackBackTrace(0, 32, bt, NULL);
    px_backtrace_symbols_fd(bt, frames, 2);
    px_exit(139);
}

void px_crash_init(void) {
    SetUnhandledExceptionFilter(crash_filter);
}

// ---- PX_PROF=1 (or =<file>): a sampling profiler for hosts without perf. A
// thread samples the instruction pointer of every thread that ran the guest
// (the thread that called px_prof_register) once a millisecond, and px_exit
// prints the hottest pcs: as offsets into ish.exe for llvm-symbolizer, or as
// whatever the JIT's describe hook makes of its own code.
#define PROF_THREADS 64
#define PROF_SLOTS 65536
static HANDLE prof_threads[PROF_THREADS];
static ULONG64 prof_cycles[PROF_THREADS];  // a thread that hardly ran since the last tick is waiting: not sampled
static uint64_t prof_idle;
static atomic_int prof_nthreads;
static struct { uint64_t pc, count; } prof_hist[PROF_SLOTS];
static uint64_t prof_total, prof_dropped;
// samples in system DLLs, by the first ish.exe frame up the stack
static struct { uint64_t pc, count; } prof_via[PROF_SLOTS];
static uint64_t prof_via_total, prof_via_lost;

static void prof_add(void *table, uint64_t pc) {
    struct { uint64_t pc, count; } *h = table;
    size_t j = (pc * 0x9e3779b97f4a7c15ull) >> 48;
    for (size_t probe = 0; probe < PROF_SLOTS; probe++, j = (j + 1) % PROF_SLOTS) {
        if (h[j].pc == pc || h[j].count == 0) {
            h[j].pc = pc;
            h[j].count++;
            return;
        }
    }
    prof_dropped++;
}

// Walks up from a pc in some DLL to the first frame in ish.exe (x64 unwind
// data; JIT code has none, which ends the walk).
static uint64_t prof_unwind_to_image(CONTEXT *c, uintptr_t base) {
#ifdef __x86_64__
    for (int k = 0; k < 64; k++) {
        uint64_t pc = c->Rip;
        if (pc >= base && pc - base < (64u << 20))
            return pc;
        DWORD64 image;
        PRUNTIME_FUNCTION fe = RtlLookupFunctionEntry(pc, &image, NULL);
        if (fe == NULL)
            return 0;
        void *handler_data;
        DWORD64 frame;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, image, pc, fe, c, &handler_data, &frame, NULL);
        if (c->Rip == 0)
            return 0;
    }
#endif
    (void) c; (void) base;
    return 0;
}
static bool prof_on;
static void (*prof_describe)(FILE *f, uintptr_t pc, unsigned count, uint64_t total);
static void (*prof_finish)(void);

static DWORD WINAPI prof_thread(void *param) {
    (void) param;
    uintptr_t base = (uintptr_t) GetModuleHandleW(NULL);
    timeBeginPeriod(1);
    for (;;) {
        Sleep(1);
        int n = atomic_load(&prof_nthreads);
        for (int i = 0; i < n && i < PROF_THREADS; i++) {
            HANDLE t = prof_threads[i];
            if (t == NULL)
                continue;
            ULONG64 cyc = 0;
            // a waiting thread that wakes up now and then still uses a few
            // cycles: count it only if it ran for a good part of the tick
            if (QueryThreadCycleTime(t, &cyc) && cyc - prof_cycles[i] < 1000000) {
                prof_cycles[i] = cyc;
                prof_idle++;
                continue;
            }
            prof_cycles[i] = cyc;
            if (SuspendThread(t) == (DWORD) -1)
                continue;
            CONTEXT c = {.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER};
            BOOL ok = GetThreadContext(t, &c);
            if (!ok) {
                ResumeThread(t);
                continue;
            }
#ifdef __x86_64__
            uint64_t pc = c.Rip;
#else
            uint64_t pc = c.Pc;
#endif
            // in a DLL (not ish.exe, not JIT code: those have no module)?
            HMODULE mod = NULL;
            if ((pc < base || pc - base >= (64u << 20)) &&
                    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR) pc, &mod)) {
                CONTEXT u = c;
                uint64_t via = prof_unwind_to_image(&u, base);
                prof_via_total++;
                if (via != 0)
                    prof_add(prof_via, via);
                else
                    prof_via_lost++;
            }
            ResumeThread(t);
            prof_total++;
            size_t j = (pc * 0x9e3779b97f4a7c15ull) >> 48;
            for (size_t probe = 0;; j = (j + 1) % PROF_SLOTS) {
                if (prof_hist[j].pc == pc || prof_hist[j].count == 0) {
                    prof_hist[j].pc = pc;
                    prof_hist[j].count++;
                    break;
                }
                if (++probe == PROF_SLOTS) {
                    prof_dropped++;
                    break;
                }
            }
        }
    }
    return 0;
}

void px_prof_register(void) {
    if (!prof_on)
        return;
    int i = atomic_fetch_add(&prof_nthreads, 1);
    if (i >= PROF_THREADS)
        return;
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &prof_threads[i],
            THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_LIMITED_INFORMATION, FALSE, 0);
}

void px_prof_hooks(void (*describe)(FILE *f, uintptr_t pc, unsigned count, uint64_t total),
                   void (*finish)(void)) {
    prof_describe = describe;
    prof_finish = finish;
}

void px_prof_init(void) {
    if (getenv("PX_PROF") == NULL)
        return;
    prof_on = true;
    CreateThread(NULL, 0, prof_thread, NULL, 0, NULL);
}

void px_prof_dump(void) {
    if (!prof_on || prof_total == 0)
        return;
    const char *path = getenv("PX_PROF");
    FILE *f = strcmp(path, "1") != 0 ? fopen(path, "w") : NULL;
    if (f == NULL)
        f = stderr;
    uintptr_t base = (uintptr_t) GetModuleHandleW(NULL);
    fprintf(f, "[prof] %llu samples (%llu dropped, %llu idle skipped), ish.exe at %p\n",
            (unsigned long long) prof_total, (unsigned long long) prof_dropped,
            (unsigned long long) prof_idle, (void *) base);
    for (int rank = 0; rank < 3000; rank++) {
        size_t best = 0;
        for (size_t j = 0; j < PROF_SLOTS; j++)
            if (prof_hist[j].count > prof_hist[best].count)
                best = j;
        if (prof_hist[best].count == 0)
            break;
        uint64_t pc = prof_hist[best].pc;
        unsigned count = (unsigned) prof_hist[best].count;
        if (pc >= base && pc - base < (64u << 20))
            fprintf(f, "%6u %5.2f%% ish.exe+%#llx\n", count, 100.0 * count / prof_total,
                    (unsigned long long) (pc - base));
        else {
            HMODULE m = NULL;
            bool in_dll = GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                    GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR) (uintptr_t) pc, &m);
            if (!in_dll && prof_describe != NULL) {
                prof_describe(f, pc, count, prof_total);
                prof_hist[best].count = 0;
                continue;
            }
            wchar_t name[MAX_PATH] = L"?";
            if (in_dll)
                GetModuleFileNameW(m, name, MAX_PATH);
            const wchar_t *bn = wcsrchr(name, L'\\');
            fprintf(f, "%6u %5.2f%% DLL %ls+%#llx\n", count, 100.0 * count / prof_total,
                    bn ? bn + 1 : name, (unsigned long long) (pc - (uintptr_t) m));
        }
        prof_hist[best].count = 0;
    }
    fprintf(f, "VIA %llu samples in DLLs (%llu not traced back to ish.exe), by caller:\n",
            (unsigned long long) prof_via_total, (unsigned long long) prof_via_lost);
    for (int rank = 0; rank < 200; rank++) {
        size_t best = 0;
        for (size_t j = 0; j < PROF_SLOTS; j++)
            if (prof_via[j].count > prof_via[best].count)
                best = j;
        if (prof_via[best].count == 0)
            break;
        fprintf(f, "%6u %5.2f%% via ish.exe+%#llx\n", (unsigned) prof_via[best].count,
                100.0 * prof_via[best].count / prof_total, (unsigned long long) (prof_via[best].pc - base));
        prof_via[best].count = 0;
    }
    if (f != stderr)
        fclose(f);
    if (prof_finish != NULL)
        prof_finish();
}

// --- The guest snapshot's trigger (kernel/state_dump.c) ---------------------

static void (*state_fn)(void);
static HANDLE state_event;

static DWORD WINAPI px_state_thread(LPVOID arg) {
    (void) arg;
    for (;;) {
        if (WaitForSingleObject(state_event, INFINITE) != WAIT_OBJECT_0)
            return 0;
        state_fn();
    }
}

bool px_state_watch(void (*fn)(void)) {
    wchar_t name[64];
    swprintf(name, 64, L"Local\\ish-state-%lu", GetCurrentProcessId());
    state_event = CreateEventW(NULL, FALSE, FALSE, name);
    if (state_event == NULL)
        return false;
    state_fn = fn;
    HANDLE t = CreateThread(NULL, 0, px_state_thread, NULL, 0, NULL);
    if (t == NULL)
        return false;
    CloseHandle(t);
    return true;
}

uint64_t px_tick_ms(void) {
    return GetTickCount64();
}

bool px_emulated(void) {
#if defined(__x86_64__)
    static volatile LONG emulated = -1;
    if (emulated < 0) {
        typedef BOOL (WINAPI *wow64_t)(HANDLE, USHORT *, USHORT *);
        wow64_t fn = (wow64_t) (void *) GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "IsWow64Process2");
        USHORT process = 0, native = 0;
        emulated = fn != NULL && fn(GetCurrentProcess(), &process, &native) && native == IMAGE_FILE_MACHINE_ARM64;
    }
    return emulated;
#else
    return false;
#endif
}

unsigned long px_host_pid(void) {
    return GetCurrentProcessId();
}

int px_rename_replace(const char *from, const char *to) {
    return MoveFileExA(from, to, MOVEFILE_REPLACE_EXISTING) ? 0 : -1;
}
