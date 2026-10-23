// The host side of ish.exe --serve (kernel/serve.c): its control channel,
// which is ish.exe's own stdin and stdout, and the numbers it reports.
#include "px_impl.h"
#include <psapi.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static HANDLE ctl_in = INVALID_HANDLE_VALUE, ctl_out = INVALID_HANDLE_VALUE;
static SRWLOCK ctl_lock = SRWLOCK_INIT;

static HANDLE take(DWORD which) {
    HANDLE h = GetStdHandle(which), mine = INVALID_HANDLE_VALUE;
    if (h == NULL || h == INVALID_HANDLE_VALUE)
        return INVALID_HANDLE_VALUE;
    if (!DuplicateHandle(GetCurrentProcess(), h, GetCurrentProcess(), &mine, 0, FALSE, DUPLICATE_SAME_ACCESS))
        return INVALID_HANDLE_VALUE;
    return mine;
}

// Before anything else runs: the channel's handles become ours alone, and
// whatever would read stdin or print to stdout gets NUL.
bool px_serve_take_stdio(void) {
    ctl_in = take(STD_INPUT_HANDLE);
    ctl_out = take(STD_OUTPUT_HANDLE);
    if (ctl_in == INVALID_HANDLE_VALUE || ctl_out == INVALID_HANDLE_VALUE)
        return false;
    freopen("NUL", "rb", stdin);
    freopen("NUL", "wb", stdout);
    HANDLE nul_in = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    HANDLE nul_out = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    SetStdHandle(STD_INPUT_HANDLE, nul_in);
    SetStdHandle(STD_OUTPUT_HANDLE, nul_out);
    return true;
}

static char rbuf[65536];
static size_t rstart, rend;
static bool reof;

char *px_serve_read_line(void) {
    char *line = NULL;
    size_t len = 0;
    for (;;) {
        char *nl = rstart < rend ? memchr(rbuf + rstart, '\n', rend - rstart) : NULL;
        size_t take_n = nl != NULL ? (size_t) (nl - (rbuf + rstart)) : rend - rstart;
        if (take_n > 0 || nl != NULL) {
            char *l = realloc(line, len + take_n + 1);
            if (l == NULL) {
                free(line);
                return NULL;
            }
            line = l;
            memcpy(line + len, rbuf + rstart, take_n);
            len += take_n;
            line[len] = '\0';
        }
        rstart += take_n;
        if (nl != NULL) {
            rstart++;
            if (len > 0 && line[len - 1] == '\r')
                line[--len] = '\0';
            return line;
        }
        rstart = rend = 0;
        if (reof)
            break;
        DWORD got = 0;
        if (!ReadFile(ctl_in, rbuf, sizeof(rbuf), &got, NULL) || got == 0)
            reof = true;
        rend = got;
    }
    // EOF: a last line without its newline is still one
    return line;
}

void px_serve_write(const char *buf, size_t len) {
    AcquireSRWLockExclusive(&ctl_lock);
    while (len > 0) {
        DWORD put = 0;
        if (!WriteFile(ctl_out, buf, (DWORD) (len > (1u << 30) ? 1u << 30 : len), &put, NULL))
            break;
        buf += put;
        len -= put;
    }
    ReleaseSRWLockExclusive(&ctl_lock);
}

void px_serve_host_stats(uint64_t *private_kb, uint64_t *ws_kb, uint64_t *cpu_ms) {
    PROCESS_MEMORY_COUNTERS_EX pmc;
    *private_kb = *ws_kb = *cpu_ms = 0;
    if (GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS *) &pmc, sizeof(pmc))) {
        *private_kb = pmc.PrivateUsage / 1024;
        *ws_kb = pmc.WorkingSetSize / 1024;
    }
    FILETIME c, e, k, u;
    if (GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u)) {
        uint64_t t = (((uint64_t) k.dwHighDateTime << 32) | k.dwLowDateTime) +
                     (((uint64_t) u.dwHighDateTime << 32) | u.dwLowDateTime);
        *cpu_ms = t / 10000;
    }
}

uint64_t px_pthread_cpu_ms(pthread_t thread) {
    FILETIME c, e, k, u;
    HANDLE h = pthread_gethandle(thread);
    if (h == NULL || !GetThreadTimes(h, &c, &e, &k, &u))
        return 0;
    uint64_t t = (((uint64_t) k.dwHighDateTime << 32) | k.dwLowDateTime) +
                 (((uint64_t) u.dwHighDateTime << 32) | u.dwLowDateTime);
    return t / 10000;
}

void px_handle_close(void *handle) {
    if (handle != NULL && handle != INVALID_HANDLE_VALUE)
        CloseHandle(handle);
}
