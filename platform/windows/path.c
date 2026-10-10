// Paths: the *at family, stat, directories. Host paths are UTF-8 with '/'
// ("C:/x/y"); they become \\?\ paths for Win32, which also turns off the DOS
// device names (a file may be called "con") and the 260 character limit.
#include "px_impl.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <winioctl.h>

int px_trace_on = -1;

void px_trace(const char *op, const char *path, int err) {
    if (px_trace_on < 0)
        px_trace_on = getenv("PX_TRACE") != NULL;
    if (px_trace_on)
        fprintf(stderr, "[px] %s %s -> %s\n", op, path ? path : "(null)", strerror(err));
}

// ---- path strings

static SRWLOCK cwd_lock = SRWLOCK_INIT;
static char *cwd;

static void fix_slashes(char *p) {
    for (; *p; p++)
        if (*p == '\\')
            *p = '/';
}

static bool is_drive_abs(const char *p) {
    return ((p[0] >= 'A' && p[0] <= 'Z') || (p[0] >= 'a' && p[0] <= 'z')) && p[1] == ':' &&
        (p[2] == '/' || p[2] == '\\');
}

static char *cwd_get(void) {
    AcquireSRWLockExclusive(&cwd_lock);
    if (cwd == NULL) {
        wchar_t buf[32768];
        DWORD n = GetCurrentDirectoryW(32768, buf);
        int len = WideCharToMultiByte(CP_UTF8, 0, buf, (int) n, NULL, 0, NULL, NULL);
        cwd = malloc(len + 1);
        WideCharToMultiByte(CP_UTF8, 0, buf, (int) n, cwd, len, NULL, NULL);
        cwd[len] = '\0';
        fix_slashes(cwd);
    }
    char *copy = strdup(cwd);
    ReleaseSRWLockExclusive(&cwd_lock);
    return copy;
}

// Collapses "." and ".." and repeated slashes in place. The prefix (the "C:"
// or "//server/share" part) is never removed.
static void normalize(char *p) {
    size_t root;
    if (p[0] == '/' && p[1] == '/') {
        // //server/share
        char *s = strchr(p + 2, '/');
        s = s ? strchr(s + 1, '/') : NULL;
        root = s ? (size_t) (s - p) : strlen(p);
    } else {
        root = 2; // "C:"
    }
    char *out = p + root;
    const char *in = p + root;
    while (*in) {
        while (*in == '/')
            in++;
        if (!*in)
            break;
        const char *end = strchr(in, '/');
        size_t len = end ? (size_t) (end - in) : strlen(in);
        if (len == 1 && in[0] == '.') {
            // skip
        } else if (len == 2 && in[0] == '.' && in[1] == '.') {
            while (out > p + root && *--out != '/')
                ;
        } else {
            *out++ = '/';
            memmove(out, in, len);
            out += len;
        }
        in += len;
    }
    if (out == p + root)
        *out++ = '/';
    *out = '\0';
}

char *px_abspath(int dirfd, const char *path) {
    if (path == NULL) {
        errno = EFAULT;
        return NULL;
    }
    if (path[0] == '\0') {
        errno = ENOENT;
        return NULL;
    }
    char *base = NULL;
    if (is_drive_abs(path) || ((path[0] == '/' || path[0] == '\\') && (path[1] == '/' || path[1] == '\\'))) {
        base = NULL;
    } else if (path[0] == '/' || path[0] == '\\') {
        // rooted but no drive: the current drive
        char *c = cwd_get();
        c[2] = '\0';
        base = c;
    } else if (dirfd == PX_AT_FDCWD) {
        base = cwd_get();
    } else {
        struct px_desc *d = px_get(dirfd);
        if (d == NULL)
            return NULL;
        if (d->path == NULL || !d->is_dir) {
            px_put(d);
            errno = ENOTDIR;
            return NULL;
        }
        base = strdup(d->path);
        px_put(d);
    }
    size_t blen = base ? strlen(base) : 0;
    char *abs = malloc(blen + strlen(path) + 2);
    if (base) {
        strcpy(abs, base);
        if (path[0] != '/' && path[0] != '\\')
            strcat(abs, "/");
        strcat(abs, path);
        free(base);
    } else {
        strcpy(abs, path);
    }
    fix_slashes(abs);
    if (abs[0] >= 'a' && abs[0] <= 'z' && abs[1] == ':')
        abs[0] -= 'a' - 'A';
    normalize(abs);
    return abs;
}

// Characters NTFS refuses in names go to U+F000+c, like Cygwin, so a Linux
// name round-trips; so do a trailing dot or space.
static bool name_char_bad(wchar_t c) {
    return c < 0x20 || c == '"' || c == '*' || c == ':' || c == '<' || c == '>' || c == '?' || c == '|';
}

wchar_t *px_wpath(const char *abs) {
    int n = MultiByteToWideChar(CP_UTF8, 0, abs, -1, NULL, 0);
    wchar_t *w = malloc((n + 8) * sizeof(wchar_t));
    size_t prefix;
    if (abs[0] == '/' && abs[1] == '/') {
        wcscpy(w, L"\\\\?\\UNC");
        MultiByteToWideChar(CP_UTF8, 0, abs + 1, -1, w + 7, n);
        prefix = 7;
    } else {
        wcscpy(w, L"\\\\?\\");
        MultiByteToWideChar(CP_UTF8, 0, abs, -1, w + 4, n);
        prefix = 4 + 2; // past "C:"
    }
    for (wchar_t *p = w + 4; *p; p++)
        if (*p == '/')
            *p = '\\';
    // map each name past the root
    wchar_t *p = w + prefix;
    while (*p) {
        if (*p == '\\') {
            p++;
            continue;
        }
        wchar_t *start = p;
        while (*p && *p != '\\') {
            if (name_char_bad(*p))
                *p = 0xf000 + *p;
            p++;
        }
        bool dots = (p - start == 1 && start[0] == '.') || (p - start == 2 && start[0] == '.' && start[1] == '.');
        if (!dots && (p[-1] == '.' || p[-1] == ' '))
            p[-1] = 0xf000 + p[-1];
    }
    // a trailing separator confuses some calls
    size_t len = wcslen(w);
    if (len > prefix + 1 && w[len - 1] == '\\')
        w[len - 1] = 0;
    return w;
}

char *px_name_from_w(const wchar_t *name, size_t len) {
    wchar_t *copy = malloc((len + 1) * sizeof(wchar_t));
    for (size_t i = 0; i < len; i++) {
        wchar_t c = name[i];
        copy[i] = (c > 0xf000 && c < 0xf080) ? c - 0xf000 : c;
    }
    copy[len] = 0;
    int n = WideCharToMultiByte(CP_UTF8, 0, copy, (int) len, NULL, 0, NULL, NULL);
    char *out = malloc(n + 1);
    WideCharToMultiByte(CP_UTF8, 0, copy, (int) len, out, n, NULL, NULL);
    out[n] = '\0';
    free(copy);
    return out;
}

HANDLE px_open_handle(const char *abs, DWORD access, DWORD disposition, DWORD flags) {
    wchar_t *w = px_wpath(abs);
    HANDLE h = CreateFileW(w, access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
            disposition, flags | FILE_FLAG_BACKUP_SEMANTICS, NULL);
    free(w);
    return h;
}

// ---- stat

static struct timespec ts_from_filetime(FILETIME ft) {
    uint64_t t = ((uint64_t) ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    t -= 116444736000000000ull; // 1601 -> 1970, in 100ns
    return (struct timespec) {.tv_sec = t / 10000000, .tv_nsec = (t % 10000000) * 100};
}

static FILETIME filetime_from_ts(struct timespec ts) {
    uint64_t t = (uint64_t) ts.tv_sec * 10000000 + ts.tv_nsec / 100 + 116444736000000000ull;
    return (FILETIME) {.dwLowDateTime = (DWORD) t, .dwHighDateTime = (DWORD) (t >> 32)};
}

void px_stat_from_info(struct px_stat *st, const BY_HANDLE_FILE_INFORMATION *info, DWORD reparse_tag) {
    memset(st, 0, sizeof(*st));
    st->st_dev = info->dwVolumeSerialNumber;
    st->st_ino = ((uint64_t) info->nFileIndexHigh << 32) | info->nFileIndexLow;
    st->st_nlink = info->nNumberOfLinks;
    st->st_size = ((int64_t) info->nFileSizeHigh << 32) | info->nFileSizeLow;
    if (reparse_tag == IO_REPARSE_TAG_SYMLINK || reparse_tag == IO_REPARSE_TAG_MOUNT_POINT) {
        st->st_mode = PX_S_IFLNK | 0777;
    } else if (info->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
        st->st_mode = PX_S_IFDIR | 0755;
        st->st_size = 4096;
    } else {
        st->st_mode = PX_S_IFREG | 0644;
        if (info->dwFileAttributes & FILE_ATTRIBUTE_READONLY)
            st->st_mode &= ~0222;
    }
    st->st_blksize = 4096;
    st->st_blocks = (st->st_size + 511) / 512;
    st->st_atim = ts_from_filetime(info->ftLastAccessTime);
    st->st_mtim = ts_from_filetime(info->ftLastWriteTime);
    // Win32 has no inode change time; the last write is the closest
    st->st_ctim = st->st_mtim;
}

int px_fstat_handle(HANDLE h, struct px_stat *st) {
    BY_HANDLE_FILE_INFORMATION info;
    if (!GetFileInformationByHandle(h, &info))
        return px_fail_win();
    DWORD tag = 0;
    if (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
        FILE_ATTRIBUTE_TAG_INFO ti;
        if (GetFileInformationByHandleEx(h, FileAttributeTagInfo, &ti, sizeof(ti)))
            tag = ti.ReparseTag;
    }
    px_stat_from_info(st, &info, tag);
    return 0;
}

int px_fstat(int fd, struct px_stat *st) {
    struct px_desc *d = px_get(fd);
    if (d == NULL)
        return -1;
    int res = 0;
    memset(st, 0, sizeof(*st));
    switch (d->kind) {
        case PX_FILE:
            res = px_fstat_handle(d->h, st);
            break;
        case PX_SOCK:
            st->st_mode = d->is_tty ? PX_S_IFCHR | 0620 : d->is_pipe ? PX_S_IFIFO | 0600 : PX_S_IFSOCK | 0777;
            st->st_ino = (uint64_t) d->s;
            st->st_blksize = 4096;
            break;
        case PX_CONSOLE_OUT:
            switch (GetFileType(d->h)) {
                case FILE_TYPE_DISK:
                    res = px_fstat_handle(d->h, st);
                    break;
                case FILE_TYPE_PIPE:
                    st->st_mode = PX_S_IFIFO | 0600;
                    break;
                default:
                    st->st_mode = PX_S_IFCHR | 0620;
            }
            st->st_ino = (uint64_t) (uintptr_t) d->h;
            st->st_blksize = 4096;
            break;
        case PX_NUL:
            st->st_mode = PX_S_IFCHR | 0666;
            st->st_rdev = (1 << 8) | 3;
            break;
    }
    px_put(d);
    return res;
}

int px_fstatat(int dirfd, const char *path, struct px_stat *st, int flags) {
    if (path[0] == '\0' && (flags & 0x1000 /* AT_EMPTY_PATH */))
        return px_fstat(dirfd, st);
    char *abs = px_abspath(dirfd, path);
    if (abs == NULL)
        return -1;
    DWORD f = (flags & PX_AT_SYMLINK_NOFOLLOW) ? FILE_FLAG_OPEN_REPARSE_POINT : 0;
    HANDLE h = px_open_handle(abs, FILE_READ_ATTRIBUTES, OPEN_EXISTING, f);
    if (h == INVALID_HANDLE_VALUE) {
        int err = px_errno_win(GetLastError());
        px_trace("stat", abs, err);
        free(abs);
        return px_fail(err);
    }
    free(abs);
    int res = px_fstat_handle(h, st);
    CloseHandle(h);
    return res;
}

int px_stat(const char *path, struct px_stat *st) {
    return px_fstatat(PX_AT_FDCWD, path, st, 0);
}

int px_lstat(const char *path, struct px_stat *st) {
    return px_fstatat(PX_AT_FDCWD, path, st, PX_AT_SYMLINK_NOFOLLOW);
}

// ---- open

int px_openat(int dirfd, const char *path, int flags, ...) {
    char *abs = px_abspath(dirfd, path);
    if (abs == NULL)
        return -1;
    DWORD access;
    switch (flags & PX_O_ACCMODE) {
        case 0: access = GENERIC_READ; break;
        case PX_O_WRONLY: access = GENERIC_WRITE | FILE_READ_ATTRIBUTES; break;
        default: access = GENERIC_READ | GENERIC_WRITE; break;
    }
    if (flags & PX_O_PATH)
        access = FILE_READ_ATTRIBUTES;
    DWORD disposition;
    if ((flags & _O_CREAT) && (flags & _O_EXCL))
        disposition = CREATE_NEW;
    else if ((flags & _O_CREAT) && (flags & _O_TRUNC))
        disposition = CREATE_ALWAYS;
    else if (flags & _O_CREAT)
        disposition = OPEN_ALWAYS;
    else if ((flags & _O_TRUNC) && (flags & PX_O_ACCMODE))
        disposition = TRUNCATE_EXISTING;
    else
        disposition = OPEN_EXISTING;
    DWORD f = FILE_ATTRIBUTE_NORMAL;
    if (flags & PX_O_NOFOLLOW)
        f |= FILE_FLAG_OPEN_REPARSE_POINT;

    HANDLE h = px_open_handle(abs, access, disposition, f);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        if (err == ERROR_ACCESS_DENIED && (flags & PX_O_ACCMODE)) {
            wchar_t *w = px_wpath(abs);
            DWORD attr = GetFileAttributesW(w);
            free(w);
            if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY))
                err = ERROR_DIRECTORY; // EISDIR below
        }
        px_trace("open", abs, err == ERROR_DIRECTORY && (flags & PX_O_ACCMODE) ? EISDIR : px_errno_win(err));
        free(abs);
        if (err == ERROR_DIRECTORY && (flags & PX_O_ACCMODE))
            return px_fail(EISDIR);
        return px_fail(px_errno_win(err));
    }
    BY_HANDLE_FILE_INFORMATION info;
    GetFileInformationByHandle(h, &info);
    bool is_dir = info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY;
    int err = 0;
    if ((flags & PX_O_NOFOLLOW) && (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) && !(flags & PX_O_PATH))
        err = ELOOP;
    else if (is_dir && (flags & PX_O_ACCMODE))
        err = EISDIR;
    else if (!is_dir && (flags & PX_O_DIRECTORY))
        err = ENOTDIR;
    if (err) {
        CloseHandle(h);
        free(abs);
        return px_fail(err);
    }
    struct px_desc *d = px_desc_new(PX_FILE);
    d->h = h;
    d->is_dir = is_dir;
    d->flags = flags & (PX_O_ACCMODE | _O_APPEND | PX_O_NONBLOCK);
    d->path = abs;
    return px_install(d, 0, flags & PX_O_CLOEXEC);
}

int px_open(const char *path, int flags, ...) {
    return px_openat(PX_AT_FDCWD, path, flags);
}

int px_mkstemp(char *tmpl) {
    size_t len = strlen(tmpl);
    if (len < 6 || strcmp(tmpl + len - 6, "XXXXXX") != 0)
        return px_fail(EINVAL);
    static const char chars[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    for (int tries = 0; tries < 100; tries++) {
        unsigned char rnd[6];
        px_getrandom(rnd, 6, 0);
        for (int i = 0; i < 6; i++)
            tmpl[len - 6 + i] = chars[rnd[i] % 62];
        int fd = px_open(tmpl, PX_O_RDWR | _O_CREAT | _O_EXCL);
        if (fd >= 0 || errno != EEXIST)
            return fd;
    }
    return px_fail(EEXIST);
}

// ---- namespace operations

#ifndef FILE_DISPOSITION_FLAG_DELETE
#define FILE_DISPOSITION_FLAG_DELETE 0x1
#define FILE_DISPOSITION_FLAG_POSIX_SEMANTICS 0x2
#define FILE_DISPOSITION_FLAG_IGNORE_READONLY_ATTRIBUTE 0x10
#endif
#define PX_FileDispositionInfoEx 21
#define PX_FileRenameInfoEx 22
#ifndef FILE_RENAME_FLAG_REPLACE_IF_EXISTS
#define FILE_RENAME_FLAG_REPLACE_IF_EXISTS 0x1
#define FILE_RENAME_FLAG_POSIX_SEMANTICS 0x2
#endif

static bool info_is_dir(const BY_HANDLE_FILE_INFORMATION *info) {
    // a symlink to a directory is still a link: unlink() removes it
    return (info->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
        !(info->dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT);
}

static int delete_handle(HANDLE h) {
    struct { DWORD Flags; } ex = {FILE_DISPOSITION_FLAG_DELETE | FILE_DISPOSITION_FLAG_POSIX_SEMANTICS |
        FILE_DISPOSITION_FLAG_IGNORE_READONLY_ATTRIBUTE};
    if (SetFileInformationByHandle(h, PX_FileDispositionInfoEx, &ex, sizeof(ex)))
        return 0;
    DWORD err = GetLastError();
    if (err == ERROR_DIR_NOT_EMPTY)
        return px_fail(ENOTEMPTY);
    // FAT and other file systems without POSIX deletes
    FILE_DISPOSITION_INFO di = {TRUE};
    if (SetFileInformationByHandle(h, FileDispositionInfo, &di, sizeof(di)))
        return 0;
    return px_fail_win();
}

int px_unlinkat(int dirfd, const char *path, int flags) {
    char *abs = px_abspath(dirfd, path);
    if (abs == NULL)
        return -1;
    HANDLE h = px_open_handle(abs, DELETE | FILE_READ_ATTRIBUTES, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT);
    free(abs);
    if (h == INVALID_HANDLE_VALUE)
        return px_fail_win();
    BY_HANDLE_FILE_INFORMATION info;
    int res;
    if (!GetFileInformationByHandle(h, &info))
        res = px_fail_win();
    else if ((flags & PX_AT_REMOVEDIR) && !info_is_dir(&info))
        res = px_fail(ENOTDIR);
    else if (!(flags & PX_AT_REMOVEDIR) && info_is_dir(&info))
        res = px_fail(EISDIR);
    else
        res = delete_handle(h);
    CloseHandle(h);
    return res;
}

int px_unlink(const char *path) {
    return px_unlinkat(PX_AT_FDCWD, path, 0);
}

int px_rmdir(const char *path) {
    return px_unlinkat(PX_AT_FDCWD, path, PX_AT_REMOVEDIR);
}

static int rename_handle(HANDLE h, const wchar_t *wdst) {
    size_t bytes = wcslen(wdst) * sizeof(wchar_t);
    size_t size = sizeof(FILE_RENAME_INFO) + bytes;
    FILE_RENAME_INFO *ri = calloc(1, size);
    ri->Flags = FILE_RENAME_FLAG_REPLACE_IF_EXISTS | FILE_RENAME_FLAG_POSIX_SEMANTICS;
    ri->RootDirectory = NULL;
    ri->FileNameLength = (DWORD) bytes;
    memcpy(ri->FileName, wdst, bytes);
    BOOL ok = SetFileInformationByHandle(h, PX_FileRenameInfoEx, ri, (DWORD) size);
    if (!ok && GetLastError() == ERROR_INVALID_PARAMETER) {
        ri->ReplaceIfExists = TRUE;
        ok = SetFileInformationByHandle(h, FileRenameInfo, ri, (DWORD) size);
    }
    DWORD err = GetLastError();
    free(ri);
    if (!ok) {
        SetLastError(err);
        return -1;
    }
    return 0;
}

int px_renameat(int olddirfd, const char *oldpath, int newdirfd, const char *newpath) {
    char *src = px_abspath(olddirfd, oldpath);
    if (src == NULL)
        return -1;
    char *dst = px_abspath(newdirfd, newpath);
    if (dst == NULL) {
        free(src);
        return -1;
    }
    int res = 0;
    if (strcmp(src, dst) == 0)
        goto out;
    HANDLE h = px_open_handle(src, DELETE | SYNCHRONIZE | FILE_READ_ATTRIBUTES, OPEN_EXISTING,
            FILE_FLAG_OPEN_REPARSE_POINT);
    if (h == INVALID_HANDLE_VALUE) {
        res = px_fail_win();
        goto out;
    }
    wchar_t *wdst = px_wpath(dst);
    if (rename_handle(h, wdst) < 0) {
        DWORD err = GetLastError();
        // POSIX lets a directory replace an empty one; Win32 does not
        BY_HANDLE_FILE_INFORMATION info;
        struct px_stat st;
        if (err == ERROR_ACCESS_DENIED && GetFileInformationByHandle(h, &info) && info_is_dir(&info) &&
                px_lstat(dst, &st) == 0 && (st.st_mode & PX_S_IFMT) == PX_S_IFDIR) {
            if (px_rmdir(dst) < 0)
                res = -1;
            else if (rename_handle(h, wdst) < 0)
                res = px_fail_win();
        } else {
            res = px_fail(px_errno_win(err));
        }
    }
    free(wdst);
    CloseHandle(h);
out:
    free(src);
    free(dst);
    return res;
}

int px_rename(const char *oldpath, const char *newpath) {
    return px_renameat(PX_AT_FDCWD, oldpath, PX_AT_FDCWD, newpath);
}

int px_mkdirat(int dirfd, const char *path, px_mode_t mode) {
    (void) mode;
    char *abs = px_abspath(dirfd, path);
    if (abs == NULL)
        return -1;
    wchar_t *w = px_wpath(abs);
    free(abs);
    BOOL ok = CreateDirectoryW(w, NULL);
    free(w);
    return ok ? 0 : px_fail_win();
}

int px_mkdir(const char *path, px_mode_t mode) {
    return px_mkdirat(PX_AT_FDCWD, path, mode);
}

int px_linkat(int olddirfd, const char *oldpath, int newdirfd, const char *newpath, int flags) {
    (void) flags;
    char *src = px_abspath(olddirfd, oldpath);
    if (src == NULL)
        return -1;
    char *dst = px_abspath(newdirfd, newpath);
    if (dst == NULL) {
        free(src);
        return -1;
    }
    wchar_t *wsrc = px_wpath(src), *wdst = px_wpath(dst);
    BOOL ok = CreateHardLinkW(wdst, wsrc, NULL);
    free(src); free(dst); free(wsrc); free(wdst);
    return ok ? 0 : px_fail_win();
}

int px_link(const char *oldpath, const char *newpath) {
    return px_linkat(PX_AT_FDCWD, oldpath, PX_AT_FDCWD, newpath, 0);
}

int px_symlinkat(const char *target, int dirfd, const char *path) {
    char *link = px_abspath(dirfd, path);
    if (link == NULL)
        return -1;
    // Is the target a directory? Resolve it from the link's parent.
    DWORD flags = SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE;
    char *parent = strdup(link);
    char *slash = strrchr(parent, '/');
    if (slash)
        *slash = '\0';
    char *resolved;
    if (is_drive_abs(target) || target[0] == '/') {
        resolved = px_abspath(PX_AT_FDCWD, target);
    } else {
        resolved = malloc(strlen(parent) + strlen(target) + 2);
        sprintf(resolved, "%s/%s", parent, target);
        normalize(resolved);
    }
    free(parent);
    if (resolved != NULL) {
        wchar_t *wr = px_wpath(resolved);
        DWORD attr = GetFileAttributesW(wr);
        if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY))
            flags |= SYMBOLIC_LINK_FLAG_DIRECTORY;
        free(wr);
        free(resolved);
    }
    int n = MultiByteToWideChar(CP_UTF8, 0, target, -1, NULL, 0);
    wchar_t *wt = malloc(n * sizeof(wchar_t));
    MultiByteToWideChar(CP_UTF8, 0, target, -1, wt, n);
    for (wchar_t *p = wt; *p; p++)
        if (*p == '/')
            *p = '\\';
    wchar_t *wl = px_wpath(link);
    free(link);
    BOOL ok = CreateSymbolicLinkW(wl, wt, flags);
    DWORD err = GetLastError();
    free(wl);
    free(wt);
    if (!ok)
        return px_fail(err == ERROR_PRIVILEGE_NOT_HELD ? EPERM : px_errno_win(err));
    return 0;
}

int px_symlink(const char *target, const char *path) {
    return px_symlinkat(target, PX_AT_FDCWD, path);
}

typedef struct {
    ULONG ReparseTag;
    USHORT ReparseDataLength;
    USHORT Reserved;
    union {
        struct {
            USHORT SubstituteNameOffset, SubstituteNameLength, PrintNameOffset, PrintNameLength;
            ULONG Flags;
            WCHAR PathBuffer[1];
        } SymbolicLinkReparseBuffer;
        struct {
            USHORT SubstituteNameOffset, SubstituteNameLength, PrintNameOffset, PrintNameLength;
            WCHAR PathBuffer[1];
        } MountPointReparseBuffer;
    };
} px_reparse_buffer;

ssize_t px_readlinkat(int dirfd, const char *path, char *buf, size_t size) {
    char *abs = px_abspath(dirfd, path);
    if (abs == NULL)
        return -1;
    HANDLE h = px_open_handle(abs, FILE_READ_ATTRIBUTES, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT);
    free(abs);
    if (h == INVALID_HANDLE_VALUE)
        return px_fail_win();
    union {
        px_reparse_buffer rb;
        char bytes[MAXIMUM_REPARSE_DATA_BUFFER_SIZE];
    } u;
    DWORD got;
    BOOL ok = DeviceIoControl(h, FSCTL_GET_REPARSE_POINT, NULL, 0, &u, sizeof(u), &got, NULL);
    DWORD err = GetLastError();
    CloseHandle(h);
    if (!ok)
        return px_fail(err == ERROR_NOT_A_REPARSE_POINT ? EINVAL : px_errno_win(err));
    const WCHAR *name;
    USHORT len;
    if (u.rb.ReparseTag == IO_REPARSE_TAG_SYMLINK) {
        typeof(u.rb.SymbolicLinkReparseBuffer) *s = &u.rb.SymbolicLinkReparseBuffer;
        if (s->PrintNameLength) {
            name = s->PathBuffer + s->PrintNameOffset / 2;
            len = s->PrintNameLength / 2;
        } else {
            name = s->PathBuffer + s->SubstituteNameOffset / 2;
            len = s->SubstituteNameLength / 2;
        }
    } else if (u.rb.ReparseTag == IO_REPARSE_TAG_MOUNT_POINT) {
        typeof(u.rb.MountPointReparseBuffer) *m = &u.rb.MountPointReparseBuffer;
        name = m->PathBuffer + m->PrintNameOffset / 2;
        len = m->PrintNameLength / 2;
    } else {
        return px_fail(EINVAL);
    }
    if (len >= 4 && wcsncmp(name, L"\\??\\", 4) == 0) {
        name += 4;
        len -= 4;
    }
    char *target = px_name_from_w(name, len);
    fix_slashes(target);
    size_t tlen = strlen(target);
    if (tlen > size)
        tlen = size;
    memcpy(buf, target, tlen);
    free(target);
    return tlen;
}

ssize_t px_readlink(const char *path, char *buf, size_t size) {
    return px_readlinkat(PX_AT_FDCWD, path, buf, size);
}

int px_fchmodat(int dirfd, const char *path, px_mode_t mode, int flags) {
    (void) flags;
    char *abs = px_abspath(dirfd, path);
    if (abs == NULL)
        return -1;
    wchar_t *w = px_wpath(abs);
    free(abs);
    DWORD attr = GetFileAttributesW(w);
    int res = 0;
    if (attr == INVALID_FILE_ATTRIBUTES) {
        res = px_fail_win();
    } else if (!(attr & FILE_ATTRIBUTE_DIRECTORY)) {
        DWORD want = (mode & 0222) ? attr & ~FILE_ATTRIBUTE_READONLY : attr | FILE_ATTRIBUTE_READONLY;
        if (want != attr && !SetFileAttributesW(w, want))
            res = px_fail_win();
    }
    free(w);
    return res;
}

int px_chmod(const char *path, px_mode_t mode) {
    return px_fchmodat(PX_AT_FDCWD, path, mode, 0);
}

int px_fchmod(int fd, px_mode_t mode) {
    char path[PATH_MAX];
    if (px_fd_path(fd, path, sizeof(path)) < 0)
        return -1;
    return px_fchmodat(PX_AT_FDCWD, path, mode, 0);
}

int px_fchownat(int dirfd, const char *path, px_uid_t uid, px_gid_t gid, int flags) {
    (void) dirfd; (void) path; (void) uid; (void) gid; (void) flags;
    return 0;
}
int px_fchown(int fd, px_uid_t uid, px_gid_t gid) {
    (void) fd; (void) uid; (void) gid;
    return 0;
}
int px_chown(const char *path, px_uid_t uid, px_gid_t gid) {
    (void) path; (void) uid; (void) gid;
    return 0;
}

#define PX_UTIME_NOW ((1l << 30) - 1l)
#define PX_UTIME_OMIT ((1l << 30) - 2l)

static int set_times(HANDLE h, const struct timespec times[2]) {
    FILETIME now;
    GetSystemTimePreciseAsFileTime(&now);
    FILETIME ft[2];
    FILETIME *p[2];
    for (int i = 0; i < 2; i++) {
        if (times == NULL || times[i].tv_nsec == PX_UTIME_NOW) {
            ft[i] = now;
            p[i] = &ft[i];
        } else if (times[i].tv_nsec == PX_UTIME_OMIT) {
            p[i] = NULL;
        } else {
            ft[i] = filetime_from_ts(times[i]);
            p[i] = &ft[i];
        }
    }
    return SetFileTime(h, NULL, p[0], p[1]) ? 0 : px_fail_win();
}

int px_utimensat(int dirfd, const char *path, const struct timespec times[2], int flags) {
    char *abs = px_abspath(dirfd, path);
    if (abs == NULL)
        return -1;
    HANDLE h = px_open_handle(abs, FILE_WRITE_ATTRIBUTES, OPEN_EXISTING,
            (flags & PX_AT_SYMLINK_NOFOLLOW) ? FILE_FLAG_OPEN_REPARSE_POINT : 0);
    free(abs);
    if (h == INVALID_HANDLE_VALUE)
        return px_fail_win();
    int res = set_times(h, times);
    CloseHandle(h);
    return res;
}

int px_futimens(int fd, const struct timespec times[2]) {
    char path[PATH_MAX];
    if (px_fd_path(fd, path, sizeof(path)) < 0)
        return -1;
    return px_utimensat(PX_AT_FDCWD, path, times, 0);
}

int px_faccessat(int dirfd, const char *path, int mode, int flags) {
    (void) flags;
    char *abs = px_abspath(dirfd, path);
    if (abs == NULL)
        return -1;
    wchar_t *w = px_wpath(abs);
    free(abs);
    DWORD attr = GetFileAttributesW(w);
    free(w);
    if (attr == INVALID_FILE_ATTRIBUTES)
        return px_fail_win();
    if ((mode & 2 /* W_OK */) && (attr & FILE_ATTRIBUTE_READONLY) && !(attr & FILE_ATTRIBUTE_DIRECTORY))
        return px_fail(EACCES);
    return 0;
}

int px_access(const char *path, int mode) {
    return px_faccessat(PX_AT_FDCWD, path, mode, 0);
}

int px_chdir(const char *path) {
    char *abs = px_abspath(PX_AT_FDCWD, path);
    if (abs == NULL)
        return -1;
    wchar_t *w = px_wpath(abs);
    DWORD attr = GetFileAttributesW(w);
    if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
        int err = attr == INVALID_FILE_ATTRIBUTES ? px_errno_win(GetLastError()) : ENOTDIR;
        free(w);
        free(abs);
        return px_fail(err);
    }
    SetCurrentDirectoryW(w);
    free(w);
    AcquireSRWLockExclusive(&cwd_lock);
    free(cwd);
    cwd = abs;
    ReleaseSRWLockExclusive(&cwd_lock);
    return 0;
}

int px_fchdir(int fd) {
    char path[PATH_MAX];
    if (px_fd_path(fd, path, sizeof(path)) < 0)
        return -1;
    return px_chdir(path);
}

char *px_getcwd(char *buf, size_t size) {
    char *c = cwd_get();
    if (buf == NULL) {
        if (size != 0 && strlen(c) >= size) {
            free(c);
            errno = ERANGE;
            return NULL;
        }
        return c;
    }
    if (strlen(c) >= size) {
        free(c);
        errno = ERANGE;
        return NULL;
    }
    strcpy(buf, c);
    free(c);
    return buf;
}

char *px_realpath(const char *path, char *resolved) {
    char *abs = px_abspath(PX_AT_FDCWD, path);
    if (abs == NULL)
        return NULL;
    HANDLE h = px_open_handle(abs, FILE_READ_ATTRIBUTES, OPEN_EXISTING, 0);
    free(abs);
    if (h == INVALID_HANDLE_VALUE) {
        px_fail_win();
        return NULL;
    }
    wchar_t buf[32768];
    DWORD n = GetFinalPathNameByHandleW(h, buf, 32768, VOLUME_NAME_DOS);
    CloseHandle(h);
    if (n == 0 || n >= 32768) {
        px_fail_win();
        return NULL;
    }
    const wchar_t *p = buf;
    bool unc = false;
    if (wcsncmp(p, L"\\\\?\\UNC\\", 8) == 0) {
        p += 7;
        unc = true;
    } else if (wcsncmp(p, L"\\\\?\\", 4) == 0) {
        p += 4;
    }
    char *out = px_name_from_w(p, wcslen(p));
    fix_slashes(out);
    if (unc) {
        char *u = malloc(strlen(out) + 2);
        sprintf(u, "/%s", out);
        free(out);
        out = u;
    }
    if (resolved == NULL)
        return out;
    if (strlen(out) >= PATH_MAX) {
        free(out);
        errno = ENAMETOOLONG;
        return NULL;
    }
    strcpy(resolved, out);
    free(out);
    return resolved;
}

int px_mkfifo(const char *path, px_mode_t mode) {
    (void) path; (void) mode;
    return px_fail(EPERM);
}

static int statvfs_path(const char *abs, struct px_statvfs *st) {
    wchar_t *w = px_wpath(abs);
    ULARGE_INTEGER avail, total, free_;
    BOOL ok = GetDiskFreeSpaceExW(w, &avail, &total, &free_);
    free(w);
    if (!ok)
        return px_fail_win();
    memset(st, 0, sizeof(*st));
    st->f_bsize = st->f_frsize = 4096;
    st->f_blocks = total.QuadPart / 4096;
    st->f_bfree = free_.QuadPart / 4096;
    st->f_bavail = avail.QuadPart / 4096;
    st->f_files = 1 << 24;
    st->f_ffree = st->f_favail = 1 << 23;
    st->f_namemax = 255;
    return 0;
}

int px_statvfs(const char *path, struct px_statvfs *st) {
    char *abs = px_abspath(PX_AT_FDCWD, path);
    if (abs == NULL)
        return -1;
    int res = statvfs_path(abs, st);
    free(abs);
    return res;
}

int px_fstatvfs(int fd, struct px_statvfs *st) {
    char path[PATH_MAX];
    if (px_fd_path(fd, path, sizeof(path)) < 0)
        return -1;
    return statvfs_path(path, st);
}

// ---- directories

struct px_dir {
    int fd;
    HANDLE h;
    bool restart;
    bool eof;
    long index;         // entries returned since the start
    size_t off;         // next record in buf, or SIZE_MAX when buf is spent
    struct px_dirent ent;
    _Alignas(8) char buf[65536];
};

px_DIR *px_fdopendir(int fd) {
    struct px_desc *d = px_get(fd);
    if (d == NULL)
        return NULL;
    if (d->kind != PX_FILE || !d->is_dir) {
        px_put(d);
        errno = ENOTDIR;
        return NULL;
    }
    px_DIR *dir = malloc(sizeof(*dir));
    dir->fd = fd;
    dir->h = d->h;
    dir->restart = true;
    dir->eof = false;
    dir->index = 0;
    dir->off = SIZE_MAX;
    px_put(d);
    return dir;
}

px_DIR *px_opendir(const char *path) {
    int fd = px_open(path, PX_O_DIRECTORY);
    if (fd < 0)
        return NULL;
    px_DIR *dir = px_fdopendir(fd);
    if (dir == NULL)
        px_close(fd);
    return dir;
}

struct px_dirent *px_readdir(px_DIR *dir) {
    for (;;) {
        if (dir->off == SIZE_MAX) {
            if (dir->eof)
                return NULL;
            FILE_INFO_BY_HANDLE_CLASS cls = dir->restart ? FileIdBothDirectoryRestartInfo : FileIdBothDirectoryInfo;
            dir->restart = false;
            if (!GetFileInformationByHandleEx(dir->h, cls, dir->buf, sizeof(dir->buf))) {
                DWORD err = GetLastError();
                dir->eof = true;
                if (err != ERROR_NO_MORE_FILES)
                    errno = px_errno_win(err);
                return NULL;
            }
            dir->off = 0;
        }
        FILE_ID_BOTH_DIR_INFO *info = (FILE_ID_BOTH_DIR_INFO *) (dir->buf + dir->off);
        dir->off = info->NextEntryOffset ? dir->off + info->NextEntryOffset : SIZE_MAX;
        char *name = px_name_from_w(info->FileName, info->FileNameLength / sizeof(WCHAR));
        size_t len = strlen(name);
        if (len >= sizeof(dir->ent.d_name)) {
            free(name);
            continue;
        }
        memcpy(dir->ent.d_name, name, len + 1);
        free(name);
        dir->ent.d_ino = (uint64_t) info->FileId.QuadPart;
        if (info->FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
            dir->ent.d_type = 10; // DT_LNK
        else if (info->FileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            dir->ent.d_type = 4; // DT_DIR
        else
            dir->ent.d_type = 8; // DT_REG
        dir->ent.d_reclen = sizeof(dir->ent);
        dir->ent.d_off = ++dir->index;
        return &dir->ent;
    }
}

void px_rewinddir(px_DIR *dir) {
    dir->restart = true;
    dir->eof = false;
    dir->index = 0;
    dir->off = SIZE_MAX;
}

long px_telldir(px_DIR *dir) {
    return dir->index;
}

void px_seekdir(px_DIR *dir, long pos) {
    px_rewinddir(dir);
    while (dir->index < pos && px_readdir(dir) != NULL)
        ;
}

int px_dirfd(px_DIR *dir) {
    return dir->fd;
}

int px_closedir(px_DIR *dir) {
    int res = px_close(dir->fd);
    free(dir);
    return res;
}
