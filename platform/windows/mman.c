// mmap and friends on VirtualAlloc and file views.
//
// Anonymous memory is one VirtualAlloc per mmap (iSH only ever unmaps what it
// mapped, whole). A file maps as a view when the range lies inside the file;
// views start on 64K boundaries, so the caller gets a pointer into the view
// and munmap finds the view by that pointer. Past the end of the file there
// is nothing to map (a view would grow the file), so those mappings are
// copies -- written back on munmap/msync when shared and writable -- and so is
// a file mapped MAP_FIXED over memory already reserved.
#include "px_impl.h"
#include <winioctl.h>
#include <psapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PX_PROT_READ 1
#define PX_PROT_WRITE 2
#define PX_PROT_EXEC 4
#define PX_MAP_SHARED 1
#define PX_MAP_PRIVATE 2
#define PX_MAP_FIXED 0x10
#define PX_MAP_ANONYMOUS 0x20

struct mapping {
    char *user;         // what mmap returned
    char *base;         // what to release
    size_t len;
    bool view;
    HANDLE wb;          // shared copy to write back, or NULL
    int64_t wb_off;
    size_t wb_len;
    struct mapping *next;
};

#define BUCKETS 4096
static struct mapping *buckets[BUCKETS];
static SRWLOCK map_lock = SRWLOCK_INIT;

static size_t bucket_of(void *p) {
    return ((uintptr_t) p >> 12) % BUCKETS;
}

static void map_add(struct mapping *m) {
    AcquireSRWLockExclusive(&map_lock);
    size_t b = bucket_of(m->user);
    m->next = buckets[b];
    buckets[b] = m;
    ReleaseSRWLockExclusive(&map_lock);
}

static struct mapping *map_take(void *user) {
    AcquireSRWLockExclusive(&map_lock);
    struct mapping **p;
    for (p = &buckets[bucket_of(user)]; *p; p = &(*p)->next)
        if ((*p)->user == user)
            break;
    struct mapping *m = *p;
    if (m != NULL)
        *p = m->next;
    ReleaseSRWLockExclusive(&map_lock);
    return m;
}

static DWORD page_prot(int prot, bool copy_view) {
    bool w = prot & PX_PROT_WRITE, x = prot & PX_PROT_EXEC, r = prot & (PX_PROT_READ | PX_PROT_WRITE | PX_PROT_EXEC);
    if (!r)
        return PAGE_NOACCESS;
    if (copy_view)
        return w ? (x ? PAGE_EXECUTE_WRITECOPY : PAGE_WRITECOPY) : (x ? PAGE_EXECUTE_READ : PAGE_READONLY);
    if (x)
        return w ? PAGE_EXECUTE_READWRITE : PAGE_EXECUTE_READ;
    return w ? PAGE_READWRITE : PAGE_READONLY;
}

static void *alloc_anon(void *hint, size_t len, int prot) {
    DWORD type = prot ? MEM_RESERVE | MEM_COMMIT : MEM_RESERVE;
    void *p = NULL;
    if (hint != NULL)
        p = VirtualAlloc((void *) ((uintptr_t) hint & ~(uintptr_t) 0xffff), len, type, page_prot(prot, false));
    if (p == NULL)
        p = VirtualAlloc(NULL, len, type, page_prot(prot, false));
    return p;
}

static int64_t file_size(HANDLE h) {
    LARGE_INTEGER size;
    return GetFileSizeEx(h, &size) ? size.QuadPart : -1;
}

static void read_into(HANDLE h, char *dst, size_t len, int64_t off) {
    while (len > 0) {
        OVERLAPPED ov = {.Offset = (DWORD) off, .OffsetHigh = (DWORD) (off >> 32)};
        DWORD chunk = len > (1u << 30) ? (1u << 30) : (DWORD) len, got;
        if (!ReadFile(h, dst, chunk, &got, &ov) || got == 0)
            break;
        dst += got;
        len -= got;
        off += got;
    }
}

static void write_back(struct mapping *m) {
    if (m->wb == NULL)
        return;
    const char *src = m->user;
    int64_t off = m->wb_off;
    size_t len = m->wb_len;
    while (len > 0) {
        OVERLAPPED ov = {.Offset = (DWORD) off, .OffsetHigh = (DWORD) (off >> 32)};
        DWORD chunk = len > (1u << 30) ? (1u << 30) : (DWORD) len, put;
        if (!WriteFile(m->wb, src, chunk, &put, &ov) || put == 0)
            break;
        src += put;
        len -= put;
        off += put;
    }
}

void *px_mmap(void *addr, size_t len, int prot, int flags, int fd, int64_t off) {
    if (len == 0) {
        errno = EINVAL;
        return (void *) -1;
    }
    if (flags & PX_MAP_ANONYMOUS) {
        if ((flags & PX_MAP_FIXED) && addr != NULL) {
            MEMORY_BASIC_INFORMATION mbi;
            if (VirtualQuery(addr, &mbi, sizeof(mbi)) && mbi.State != MEM_FREE) {
                // inside a reservation of ours: fresh zero pages
                VirtualFree(addr, len, MEM_DECOMMIT);
                if (prot && !VirtualAlloc(addr, len, MEM_COMMIT, page_prot(prot, false))) {
                    px_fail_win();
                    return (void *) -1;
                }
                return addr;
            }
            void *p = VirtualAlloc(addr, len, prot ? MEM_RESERVE | MEM_COMMIT : MEM_RESERVE, page_prot(prot, false));
            if (p != addr) {
                px_fail_win();
                return (void *) -1;
            }
            struct mapping *m = calloc(1, sizeof(*m));
            m->user = m->base = p;
            m->len = len;
            map_add(m);
            return p;
        }
        void *p = alloc_anon(addr, len, prot);
        if (p == NULL) {
            px_fail_win();
            return (void *) -1;
        }
        struct mapping *m = calloc(1, sizeof(*m));
        m->user = m->base = p;
        m->len = len;
        map_add(m);
        return p;
    }

    struct px_desc *d = px_get(fd);
    if (d == NULL)
        return (void *) -1;
    void *res = (void *) -1;
    if (d->kind != PX_FILE || d->is_dir) {
        errno = ENODEV;
        goto out;
    }
    int64_t size = file_size(d->h);
    bool shared_write = (flags & PX_MAP_SHARED) && (prot & PX_PROT_WRITE);
    if (shared_write && (d->flags & PX_O_ACCMODE) == 0) {
        errno = EACCES;
        goto out;
    }

    if ((flags & PX_MAP_FIXED) && addr != NULL) {
        // a copy into memory the caller already has
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery(addr, &mbi, sizeof(mbi)) || mbi.State == MEM_FREE) {
            if (!VirtualAlloc(addr, len, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE)) {
                px_fail_win();
                goto out;
            }
        } else {
            VirtualAlloc(addr, len, MEM_COMMIT, PAGE_READWRITE);
            DWORD old;
            VirtualProtect(addr, len, PAGE_READWRITE, &old);
        }
        size_t avail = off < size ? (size_t) (size - off) : 0;
        if (avail > len)
            avail = len;
        read_into(d->h, addr, avail, off);
        memset((char *) addr + avail, 0, len - avail);
        DWORD old;
        VirtualProtect(addr, len, page_prot(prot, false), &old);
        res = addr;
        goto out;
    }

    // A view also covers a mapping that ends in the file's last page: that
    // page maps whole, zeros past the end, as on Linux. Further out a view
    // would have to grow the file.
    int64_t size_pages = (size + 0xfff) & ~(int64_t) 0xfff;
    if (size >= 0 && off < size && off + (int64_t) len <= (shared_write ? size : size_pages)) {
        int64_t aligned = off & ~(int64_t) 0xffff;
        size_t delta = (size_t) (off - aligned);
        size_t view_len = len + delta;
        if ((int64_t) view_len > size - aligned)
            view_len = (size_t) (size - aligned);
        // A private view is charged commit for all of it up front (any page
        // could be written), where Linux charges for pages written: a JVM
        // pays 140MB for lib/modules and libjvm.so it only reads. Read-only
        // private views are plain read views; if the guest makes one writable
        // later, VirtualProtect refuses and the kernel copies each page on
        // its first write instead (pt_set_flags).
        bool copy = !(flags & PX_MAP_SHARED) && (prot & PX_PROT_WRITE);
        DWORD mprot = copy ? PAGE_WRITECOPY : shared_write ? PAGE_READWRITE : PAGE_READONLY;
        HANDLE fm = CreateFileMappingW(d->h, NULL, mprot, 0, 0, NULL);
        if (fm != NULL) {
            DWORD access = copy ? FILE_MAP_COPY : shared_write ? FILE_MAP_WRITE : FILE_MAP_READ;
            char *base = MapViewOfFile(fm, access, (DWORD) (aligned >> 32), (DWORD) aligned, view_len);
            CloseHandle(fm);
            if (base != NULL) {
                struct mapping *m = calloc(1, sizeof(*m));
                m->base = base;
                m->user = base + delta;
                m->len = len;
                m->view = true;
                map_add(m);
                res = m->user;
                goto out;
            }
        }
        // fall back to a copy
    }

    char *p = alloc_anon(NULL, len, PX_PROT_READ | PX_PROT_WRITE);
    if (p == NULL) {
        px_fail_win();
        goto out;
    }
    size_t avail = off < size ? (size_t) (size - off) : 0;
    if (avail > len)
        avail = len;
    read_into(d->h, p, avail, off);
    struct mapping *m = calloc(1, sizeof(*m));
    m->user = m->base = p;
    m->len = len;
    if (shared_write && avail > 0) {
        DuplicateHandle(GetCurrentProcess(), d->h, GetCurrentProcess(), &m->wb, 0, FALSE, DUPLICATE_SAME_ACCESS);
        m->wb_off = off;
        m->wb_len = avail;
    }
    map_add(m);
    if (!(prot & PX_PROT_WRITE)) {
        DWORD old;
        VirtualProtect(p, len, page_prot(prot, false), &old);
    }
    res = p;
out:
    px_put(d);
    return res;
}

int px_munmap(void *addr, size_t len) {
    struct mapping *m = map_take(addr);
    if (m == NULL) {
        // part of a mapping: give back the pages, keep the address range
        MEMORY_BASIC_INFORMATION mbi;
        if (VirtualQuery(addr, &mbi, sizeof(mbi)) && mbi.Type == MEM_PRIVATE && mbi.State == MEM_COMMIT)
            VirtualFree(addr, len, MEM_DECOMMIT);
        return 0;
    }
    if (m->wb != NULL) {
        write_back(m);
        CloseHandle(m->wb);
    }
    if (m->view)
        UnmapViewOfFile(m->base);
    else
        VirtualFree(m->base, 0, MEM_RELEASE);
    free(m);
    return 0;
}

int px_mprotect(void *addr, size_t len, int prot) {
    char *p = addr, *end = p + len;
    while (p < end) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery(p, &mbi, sizeof(mbi)))
            return px_fail_win();
        char *region_end = (char *) mbi.BaseAddress + mbi.RegionSize;
        size_t n = (region_end < end ? region_end : end) - p;
        if (mbi.State == MEM_FREE)
            return px_fail(ENOMEM);
        bool copy_view = mbi.Type == MEM_MAPPED &&
            (mbi.AllocationProtect == PAGE_WRITECOPY || mbi.AllocationProtect == PAGE_EXECUTE_WRITECOPY);
        if (mbi.State == MEM_RESERVE) {
            if (prot != 0 && !VirtualAlloc(p, n, MEM_COMMIT, page_prot(prot, false)))
                return px_fail_win();
        } else {
            DWORD old;
            DWORD want = page_prot(prot, copy_view);
            if (mbi.Type == MEM_MAPPED && !copy_view && want == PAGE_EXECUTE_READWRITE)
                want = PAGE_READWRITE;
            if (!VirtualProtect(p, n, want, &old))
                return px_fail_win();
        }
        p += n;
    }
    return 0;
}

int px_madvise(void *addr, size_t len, int advice) {
    if (advice != 4 /* MADV_DONTNEED */)
        return 0;
    // Linux zero-fills private anonymous pages on next touch
    char *p = addr, *end = p + len;
    while (p < end) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery(p, &mbi, sizeof(mbi)))
            return px_fail_win();
        char *region_end = (char *) mbi.BaseAddress + mbi.RegionSize;
        size_t n = (region_end < end ? region_end : end) - p;
        if (mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE) {
            DWORD prot = mbi.Protect;
            VirtualFree(p, n, MEM_DECOMMIT);
            VirtualAlloc(p, n, MEM_COMMIT, prot);
        }
        p += n;
    }
    return 0;
}

int px_msync(void *addr, size_t len, int flags) {
    (void) flags;
    AcquireSRWLockShared(&map_lock);
    struct mapping *m;
    for (m = buckets[bucket_of(addr)]; m; m = m->next)
        if (m->user == addr)
            break;
    if (m != NULL)
        write_back(m);
    ReleaseSRWLockShared(&map_lock);
    FlushViewOfFile(addr, len);
    return 0;
}

// ---- the JIT's regions (asbestos/guest-x86/xjit.c): no mapping table, no
// emulation of POSIX protections, just the two VirtualAlloc steps.
void *px_jit_reserve(void *hint, size_t len) {
    void *p = VirtualAlloc((void *) ((uintptr_t) hint & ~(uintptr_t) 0xffff), len, MEM_RESERVE, PAGE_NOACCESS);
    return p;
}

bool px_jit_commit(void *addr, size_t len, bool exec) {
    return VirtualAlloc(addr, len, MEM_COMMIT, exec ? PAGE_EXECUTE_READWRITE : PAGE_READWRITE) != NULL;
}

void px_jit_release(void *addr) {
    VirtualFree(addr, 0, MEM_RELEASE);
}

// ---- the guest window (kernel/memory.c, FASTMEM): the low 4 GB of a guest
// address space at a fixed place in the host's, reserved with a guard on each
// side so that an access a little outside it faults too. Translated code
// reaches guest memory there with no check; what isn't committed, or is
// protected, faults, and the fault goes to that access's checked path.
// Write-watched: a fork copies only the pages written since they were
// committed or filled (px_fm_written).
// Windows given back, emptied, for the next ones: a hypervisor VM that had
// one mapped keeps it so (whp.c), which spares mapping it again.
#define FM_POOL 64
static struct { char *base; size_t len, guard; } fm_pool[FM_POOL];
static int fm_pooled;
static SRWLOCK fm_pool_lock = SRWLOCK_INIT;

void *px_fm_reserve(size_t len, size_t guard) {
    char *p = NULL;
    AcquireSRWLockExclusive(&fm_pool_lock);
    for (int i = fm_pooled - 1; i >= 0 && p == NULL; i--) {
        if (fm_pool[i].len == len && fm_pool[i].guard == guard) {
            p = fm_pool[i].base;
            fm_pool[i] = fm_pool[--fm_pooled];
        }
    }
    ReleaseSRWLockExclusive(&fm_pool_lock);
    if (p != NULL)
        return p;
    p = VirtualAlloc(NULL, len + 2 * guard, MEM_RESERVE | MEM_WRITE_WATCH, PAGE_NOACCESS);
    return p != NULL ? p + guard : NULL;
}

// Which of the n pages from addr were written since their last reset: a bit
// each in written (n bits). -1 if Windows can't say.
int px_fm_written(void *addr, size_t n, uint64_t *written) {
    memset(written, 0, (n + 63) / 64 * 8);
    PVOID *pages = malloc(n * sizeof(*pages));
    if (pages == NULL)
        return -1;
    ULONG_PTR count = n;
    DWORD gran;
    if (GetWriteWatch(0, addr, n * 4096, pages, &count, &gran) != 0) {
        free(pages);
        return -1;
    }
    for (ULONG_PTR i = 0; i < count; i++) {
        size_t k = (size_t) ((char *) pages[i] - (char *) addr) / 4096;
        if (k < n)
            written[k / 64] |= 1ull << (k % 64);
    }
    free(pages);
    return 0;
}

void px_fm_unwritten(void *addr, size_t len) {
    ResetWriteWatch(addr, len);
}

void px_fm_release(void *base, size_t len, size_t guard) {
    char *start = (char *) base - guard;
    VirtualFree(start, len + 2 * guard, MEM_DECOMMIT);
    ResetWriteWatch(start, len + 2 * guard);
    AcquireSRWLockExclusive(&fm_pool_lock);
    bool kept = fm_pooled < FM_POOL;
    if (kept)
        fm_pool[fm_pooled++] = (typeof(fm_pool[0])) {base, len, guard};
    ReleaseSRWLockExclusive(&fm_pool_lock);
    if (!kept) {
        px_vm_forget(base);
        VirtualFree(start, 0, MEM_RELEASE);
    }
}

// Fresh zero pages (prot 0: none, they stay reserved).
int px_fm_commit_unfilled(void *addr, size_t len, int prot) {
    VirtualFree(addr, len, MEM_DECOMMIT);
    if (prot != 0 && !VirtualAlloc(addr, len, MEM_COMMIT, page_prot(prot, false)))
        return px_fail_win();
    return 0;
}
int px_fm_commit(void *addr, size_t len, int prot) {
    if (px_fm_commit_unfilled(addr, len, prot) < 0)
        return -1;
    if (prot != 0)
        px_vm_populate(addr, len, prot & PX_PROT_WRITE);
    return 0;
}

void px_fm_decommit(void *addr, size_t len) {
    VirtualFree(addr, len, MEM_DECOMMIT);
}

// Protection of committed pages; reserved ones are committed (zero) unless
// prot is 0.
int px_fm_protect(void *addr, size_t len, int prot) {
    char *p = addr, *end = p + len;
    while (p < end) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery(p, &mbi, sizeof(mbi)))
            return px_fail_win();
        char *region_end = (char *) mbi.BaseAddress + mbi.RegionSize;
        size_t n = (region_end < end ? region_end : end) - p;
        if (mbi.State == MEM_COMMIT) {
            DWORD old;
            if (!VirtualProtect(p, n, page_prot(prot, false), &old))
                return px_fail_win();
        } else if (prot != 0 && !VirtualAlloc(p, n, MEM_COMMIT, page_prot(prot, false))) {
            return px_fail_win();
        }
        p += n;
    }
    if (prot != 0)
        px_vm_populate(addr, len, prot & PX_PROT_WRITE);
    return 0;
}

// Which of the n pages from addr are in this process's working set (touched,
// and still there): a bit each in res. -1 if Windows can't say.
int px_fm_resident(void *addr, size_t n, uint64_t *res) {
    memset(res, 0, (n + 63) / 64 * 8);
    PSAPI_WORKING_SET_EX_INFORMATION *ws = malloc(n * sizeof(*ws));
    if (ws == NULL)
        return -1;
    for (size_t i = 0; i < n; i++)
        ws[i].VirtualAddress = (char *) addr + i * 4096;
    if (!QueryWorkingSetEx(GetCurrentProcess(), ws, (DWORD) (n * sizeof(*ws)))) {
        free(ws);
        return -1;
    }
    for (size_t i = 0; i < n; i++)
        if (ws[i].VirtualAttributes.Valid)
            res[i / 64] |= 1ull << (i % 64);
    free(ws);
    return 0;
}

// A view of a file that can't have been written: its pages are the file's.
bool px_fm_file_view(void *addr) {
    MEMORY_BASIC_INFORMATION mbi;
    return VirtualQuery(addr, &mbi, sizeof(mbi)) && mbi.Type == MEM_MAPPED &&
        (mbi.AllocationProtect == PAGE_READONLY || mbi.AllocationProtect == PAGE_EXECUTE_READ);
}

bool px_fm_committed(void *addr) {
    MEMORY_BASIC_INFORMATION mbi;
    return VirtualQuery(addr, &mbi, sizeof(mbi)) && mbi.State == MEM_COMMIT;
}

bool px_fm_committed_until(void *addr, char **end) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(addr, &mbi, sizeof(mbi))) {
        *end = (char *) addr + 4096;
        return false;
    }
    *end = (char *) mbi.BaseAddress + mbi.RegionSize;
    return mbi.State == MEM_COMMIT;
}

static uintptr_t (*fault_redirect)(uintptr_t pc, uintptr_t r11);
static LONG CALLBACK fault_redirect_veh(PEXCEPTION_POINTERS ep) {
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || fault_redirect == NULL)
        return EXCEPTION_CONTINUE_SEARCH;
    uintptr_t to = fault_redirect((uintptr_t) ep->ContextRecord->Rip, (uintptr_t) ep->ContextRecord->R11);
    if (to == 0)
        return EXCEPTION_CONTINUE_SEARCH;
    ep->ContextRecord->Rip = to;
    return EXCEPTION_CONTINUE_EXECUTION;
}

void px_fault_redirect(uintptr_t (*fn)(uintptr_t pc, uintptr_t r11)) {
    if (fault_redirect == NULL)
        AddVectoredExceptionHandler(1, fault_redirect_veh);
    fault_redirect = fn;
}

// ---- the translation cache's file (asbestos/guest-x86_64/xlat.c)
static wchar_t *px_utf8_to_w(const char *s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    wchar_t *w = n > 0 ? malloc(n * sizeof(wchar_t)) : NULL;
    if (w != NULL)
        MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
    return w;
}

void *px_shared_file_map(const char *path, size_t size) {
    wchar_t *w = px_utf8_to_w(path);
    if (w == NULL)
        return NULL;
    // the directories on the way
    for (wchar_t *p = w + 3; *p; p++) {
        if (*p == L'/' || *p == L'\\') {
            wchar_t c = *p;
            *p = 0;
            CreateDirectoryW(w, NULL);
            *p = c;
        }
    }
    HANDLE f = CreateFileW(w, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    free(w);
    if (f == INVALID_HANDLE_VALUE)
        return NULL;
    // sparse: only what is written takes disk space (ISH_XCACHE_SPARSE=0: not)
    const char *sp = getenv("ISH_XCACHE_SPARSE");
    if (!(sp && sp[0] == '0')) {
        DWORD ret;
        DeviceIoControl(f, FSCTL_SET_SPARSE, NULL, 0, NULL, 0, &ret, NULL);
    }
    HANDLE m = CreateFileMappingW(f, NULL, PAGE_READWRITE, (DWORD) ((uint64_t) size >> 32), (DWORD) size, NULL);
    CloseHandle(f);
    if (m == NULL)
        return NULL;
    void *p = MapViewOfFile(m, FILE_MAP_ALL_ACCESS, 0, 0, size);
    CloseHandle(m);
    return p;
}

static char *px_w_to_utf8(const wchar_t *w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    char *s = n > 0 ? malloc(n) : NULL;
    if (s != NULL) {
        WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, NULL, NULL);
        for (char *p = s; *p; p++)
            if (*p == '\\')
                *p = '/';
    }
    return s;
}

void px_prefetch(void *addr, size_t len) {
    WIN32_MEMORY_RANGE_ENTRY range = {addr, len};
    PrefetchVirtualMemory(GetCurrentProcess(), 1, &range, 0);
}

const char *px_self_exe(void) {
    static char *path;
    if (path == NULL) {
        wchar_t w[MAX_PATH * 2];
        DWORD n = GetModuleFileNameW(NULL, w, MAX_PATH * 2);
        if (n == 0 || n >= MAX_PATH * 2)
            return NULL;
        path = px_w_to_utf8(w);
    }
    return path;
}

const char *px_local_appdata(void) {
    static char *path;
    if (path == NULL) {
        wchar_t w[MAX_PATH * 2];
        DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", w, MAX_PATH * 2);
        if (n == 0 || n >= MAX_PATH * 2)
            return NULL;
        path = px_w_to_utf8(w);
    }
    return path;
}
