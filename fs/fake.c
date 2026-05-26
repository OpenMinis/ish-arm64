#include <stdarg.h>
#include <stdio.h>
#include <limits.h>
#include <string.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <sqlite3.h>
#include <unistd.h>
#include <dirent.h>
#include <errno.h>
#include <stdatomic.h>
#if defined(__APPLE__)
#  include <os/lock.h>
#  include <dispatch/dispatch.h>
#  include <mach/mach_time.h>
#  include <Block.h>
#endif
#include "util/signpost.h"

#include "debug.h"
#include "kernel/errno.h"
#include "kernel/task.h"
#include "fs/fd.h"
#include "fs/dev.h"
#include "fs/inode.h"
#include "fs/real.h"
#define ISH_INTERNAL
#include "fs/fake.h"

// TODO document database

/* ===== Bind Mount Support =====
 * Allows redirecting fakefs paths to external host directories via symlinks.
 * Files under bind-mounted paths get auto-created meta.db entries on access.
 * This avoids the need to copy files into the fakefs data/ directory. */

#define FAKEFS_MAX_BIND_MOUNTS 32

/* Compute a relative symlink target from link_dir to abs_target.
 * link_dir is the absolute path of the directory containing the symlink.
 * abs_target is the absolute path the symlink should point to.
 * Result is written to out (must be PATH_MAX). Returns 0 on success. */
static int compute_relative_path(const char *link_dir, const char *abs_target,
                                  char *out, size_t out_size) {
    /* Find common prefix (up to the last shared '/') */
    size_t common = 0;
    size_t last_slash = 0;
    for (size_t i = 0; link_dir[i] && abs_target[i]; i++) {
        if (link_dir[i] != abs_target[i])
            break;
        common = i + 1;
        if (link_dir[i] == '/')
            last_slash = common;
    }
    /* If both ended at common, check for trailing slash boundary */
    if (link_dir[common] == '\0' && abs_target[common] == '/') {
        last_slash = common;
    } else if (abs_target[common] == '\0' && link_dir[common] == '/') {
        last_slash = common;
    } else {
        common = last_slash;
    }

    /* Count remaining '/' in link_dir after common prefix => number of "../" */
    int ups = 0;
    for (const char *p = link_dir + common; *p; p++) {
        if (*p == '/')
            ups++;
    }
    /* If link_dir doesn't end with '/', the last component is the dir name */
    size_t ld_len = strlen(link_dir);
    if (ld_len > 0 && link_dir[ld_len - 1] != '/')
        ups++;

    out[0] = '\0';
    for (int i = 0; i < ups; i++) {
        if (i == 0)
            strlcpy(out, "..", out_size);
        else
            strlcat(out, "/..", out_size);
    }

    /* Append the remainder of abs_target after common prefix */
    const char *suffix = abs_target + common;
    if (*suffix == '/') suffix++; /* skip leading slash */
    if (*suffix) {
        if (ups > 0) strlcat(out, "/", out_size);
        strlcat(out, suffix, out_size);
    }

    if (out[0] == '\0') {
        strlcpy(out, ".", out_size);
    }

    return 0;
}

struct fakefs_bind_mount {
    char path[PATH_MAX];      /* Linux path prefix, e.g. "/var/minis/offloads" */
    char host_path[PATH_MAX]; /* Resolved host path the symlink points to */
    int  path_len;
    int  host_path_len;
    bool active;
    bool read_only;           /* Reject write syscalls with EROFS at fakefs layer. */
};

static struct fakefs_bind_mount g_bind_mounts[FAKEFS_MAX_BIND_MOUNTS];
struct mount *g_fakefs_mount = NULL;

/* Optional host-provided override hooks for guest↔host path translation.
 * NULL unless host registers via fakefs_set_path_*_hook(). Defined here
 * (before resolve_path() / bind_mount_translate_path() so both directions
 * can consult them). */
static _Atomic(fakefs_path_translate_hook_t) g_path_translate_hook = NULL;
static _Atomic(fakefs_path_reverse_hook_t) g_path_reverse_hook = NULL;

void fakefs_set_path_translate_hook(fakefs_path_translate_hook_t hook) {
    atomic_store_explicit(&g_path_translate_hook, hook, memory_order_release);
}

void fakefs_set_path_reverse_hook(fakefs_path_reverse_hook_t hook) {
    atomic_store_explicit(&g_path_reverse_hook, hook, memory_order_release);
}

/* Absolute host path of the fakefs `data/` directory, registered by the
 * host via fakefs_set_rootfs_data_path() at boot. Used to detect
 * "self-containing" bind mounts in fakefs_bind_mount_resolve_path() —
 * a mount whose host_path is an ancestor of g_rootfs_data_path would
 * cause every rootfs-internal fd to be reverse-mapped under that mount,
 * which is never what the caller wants. Empty string means "no rootfs
 * path registered" (suppression is disabled). */
static char g_rootfs_data_path[PATH_MAX] = {0};
static int g_rootfs_data_path_len = 0;

void fakefs_set_rootfs_data_path(const char *abs_host_path) {
    if (abs_host_path == NULL || abs_host_path[0] == '\0') {
        g_rootfs_data_path[0] = '\0';
        g_rootfs_data_path_len = 0;
        fprintf(stderr, "fakefs: rootfs_data_path cleared\n");
        return;
    }
    /* Strip any trailing slash so prefix arithmetic below works uniformly:
     * the ancestor check compares host_path[hlen] against '/' to enforce
     * a path-component boundary, which fails if g_rootfs_data_path itself
     * ends in '/' (it generally won't, but be defensive). */
    size_t n = strlen(abs_host_path);
    while (n > 1 && abs_host_path[n - 1] == '/') n--;
    if (n + 1 > sizeof(g_rootfs_data_path)) {
        fprintf(stderr,
                "fakefs: rootfs_data_path too long (%zu chars, max %zu) — ignored\n",
                n, sizeof(g_rootfs_data_path) - 1);
        return;
    }
    memcpy(g_rootfs_data_path, abs_host_path, n);
    g_rootfs_data_path[n] = '\0';
    g_rootfs_data_path_len = (int)n;
    fprintf(stderr, "fakefs: rootfs_data_path registered \"%s\" (len=%d)\n",
            g_rootfs_data_path, g_rootfs_data_path_len);
}

/* True if `host_path` is an ancestor of (or equal to) the registered
 * fakefs data directory. Returns false when no rootfs path is
 * registered, so the guard is a no-op until the host opts in. */
static bool fakefs_host_path_contains_rootfs(const char *host_path, int host_path_len) {
    if (g_rootfs_data_path_len == 0)
        return false;
    if (host_path_len > g_rootfs_data_path_len)
        return false;
    if (strncmp(host_path, g_rootfs_data_path, host_path_len) != 0)
        return false;
    /* Either exact match, or g_rootfs_data_path has a '/' immediately
     * after the host_path prefix (proper ancestor). Anything else is a
     * sibling-prefix false positive (e.g. /Users/x vs /Users/xyz). */
    char next = g_rootfs_data_path[host_path_len];
    return next == '/' || next == '\0';
}

/* Translate a resolved host path back to a Linux path for bind mounts.
 * e.g. "/Users/.../MinisChat/minis/<sid>/attachments/file.mp4"
 *   -> "/var/minis/attachments/file.mp4"
 * Returns true and writes to out_path if a match was found.
 *
 * Mounts whose host_path is an ancestor of the registered fakefs
 * `data/` directory are skipped — a "self-containing" mount would
 * otherwise reverse-map every rootfs-internal fd to a guest path under
 * itself, which corrupts execve / fakefs metadata lookups. The
 * suppression is no-op until fakefs_set_rootfs_data_path() is called by
 * the host. */
bool fakefs_bind_mount_resolve_path(const char *resolved, char *out_path, size_t out_size) {
    for (int i = 0; i < FAKEFS_MAX_BIND_MOUNTS; i++) {
        if (!g_bind_mounts[i].active)
            continue;
        int hlen = g_bind_mounts[i].host_path_len;
        /* Skip mounts that contain the rootfs — reverse-mapping a
         * host path inside the fakefs `data/` directory through a
         * mount whose host_path encloses it yields a bogus guest
         * path that breaks execve / metadata lookups. */
        if (fakefs_host_path_contains_rootfs(g_bind_mounts[i].host_path, hlen)) {
            /* Only log once per process per mount slot — otherwise the hot
             * path floods the log on every execve. */
            static bool warned[FAKEFS_MAX_BIND_MOUNTS] = {false};
            if (!warned[i]) {
                warned[i] = true;
                fprintf(stderr,
                        "fakefs_bind_mount_resolve_path: skipping self-containing mount slot[%d] "
                        "host=\"%s\" (ancestor of rootfs data \"%s\") — "
                        "every rootfs fd would otherwise reverse-map into this mount\n",
                        i, g_bind_mounts[i].host_path, g_rootfs_data_path);
            }
            continue;
        }
        /* Try matching as-is first */
        if (strncmp(resolved, g_bind_mounts[i].host_path, hlen) == 0 &&
            (resolved[hlen] == '/' || resolved[hlen] == '\0')) {
            snprintf(out_path, out_size, "%s%s",
                     g_bind_mounts[i].path, resolved + hlen);
            return true;
        }
        /* F_GETPATH resolves /var -> /private/var on iOS.
         * If host_path starts with /var/ but resolved starts with /private/var/,
         * try matching with the /private prefix stripped from resolved. */
        if (strncmp(g_bind_mounts[i].host_path, "/var/", 5) == 0 &&
            strncmp(resolved, "/private/var/", 13) == 0) {
            const char *resolved_no_private = resolved + 8; /* skip "/private" */
            if (strncmp(resolved_no_private, g_bind_mounts[i].host_path, hlen) == 0 &&
                (resolved_no_private[hlen] == '/' || resolved_no_private[hlen] == '\0')) {
                snprintf(out_path, out_size, "%s%s",
                         g_bind_mounts[i].path, resolved_no_private + hlen);
                return true;
            }
        }
    }
    /* Static table miss — fall through to the host-provided reverse hook
     * if one is registered. The hook knows about per-context routes that
     * are not in g_bind_mounts[]. */
    fakefs_path_reverse_hook_t reverse =
        atomic_load_explicit(&g_path_reverse_hook, memory_order_acquire);
    if (reverse != NULL && reverse(resolved, out_path, out_size))
        return true;
    return false;
}

/* Translate a Linux path to a host absolute path if under a bind mount.
 * Handles both leading-slash and no-leading-slash forms.
 * Returns true and writes to out_path if the path is under a bind mount. */
/* Public wrapper for native offload handlers. See fake.h. */
bool fakefs_bind_mount_translate_path(const char *path, char *out_path, size_t out_size);

/* Internal variant exposes whether the match came from the per-context
 * translate hook vs. the static g_bind_mounts[] table. Callers that mutate
 * meta.db need to know — hook-routed paths must NOT be stored in meta.db,
 * since the `paths` table is keyed by guest path globally, while the hook
 * can map the same guest path to different host files per fs_context. */
static bool bind_mount_translate_path_ex(const char *path, char *out_path,
                                         size_t out_size, bool *out_via_hook) {
    if (out_via_hook != NULL) *out_via_hook = false;
    /* Normalize: if path lacks leading /, prepend it for comparison.
     * Bind mount table always stores paths with leading /. */
    char normalized[MAX_PATH];
    const char *cmp_path = path;
    if (path[0] != '/') {
        snprintf(normalized, sizeof(normalized), "/%s", path);
        cmp_path = normalized;
    }
    /* Host-provided hook wins over the static table. The hook sees the
     * normalized (leading-slash) path and the calling task's fs_context. */
    fakefs_path_translate_hook_t hook =
        atomic_load_explicit(&g_path_translate_hook, memory_order_acquire);
    if (hook != NULL) {
        uint64_t ctx = 0;
        if (current != NULL && current->group != NULL)
            ctx = current->group->fs_context;
        if (hook(cmp_path, ctx, out_path, out_size)) {
            if (out_via_hook != NULL) *out_via_hook = true;
            return true;
        }
    }
    for (int i = 0; i < FAKEFS_MAX_BIND_MOUNTS; i++) {
        if (!g_bind_mounts[i].active)
            continue;
        int len = g_bind_mounts[i].path_len;
        if (strncmp(g_bind_mounts[i].path, cmp_path, len) == 0 &&
            (cmp_path[len] == '/' || cmp_path[len] == '\0')) {
            snprintf(out_path, out_size, "%s%s",
                     g_bind_mounts[i].host_path, cmp_path + len);
            return true;
        }
    }
    return false;
}

static bool bind_mount_translate_path(const char *path, char *out_path, size_t out_size) {
    return bind_mount_translate_path_ex(path, out_path, out_size, NULL);
}

/* Check if a path is at or under a bind mount prefix.
 * Handles both "/var/minis/..." and "var/minis/..." (without leading /). */
static bool is_under_bind_mount(const char *path) {
    char normalized[MAX_PATH];
    const char *cmp_path = path;
    if (path[0] != '/') {
        snprintf(normalized, sizeof(normalized), "/%s", path);
        cmp_path = normalized;
    }
    for (int i = 0; i < FAKEFS_MAX_BIND_MOUNTS; i++) {
        if (!g_bind_mounts[i].active)
            continue;
        int len = g_bind_mounts[i].path_len;
        if (strncmp(g_bind_mounts[i].path, cmp_path, len) == 0 &&
            (cmp_path[len] == '/' || cmp_path[len] == '\0'))
            return true;
    }
    return false;
}

/* Return true if `path` is at or under a bind mount that was registered
 * with read_only=true. Used by the fakefs write entry points to reject
 * modifications with EROFS. Root bypasses the DAC access_check path, so
 * relying on the meta.db 0555 mode bits alone is insufficient — we enforce
 * it here instead. */
static bool is_under_readonly_bind_mount(const char *path) {
    char normalized[MAX_PATH];
    const char *cmp_path = path;
    if (path[0] != '/') {
        snprintf(normalized, sizeof(normalized), "/%s", path);
        cmp_path = normalized;
    }
    for (int i = 0; i < FAKEFS_MAX_BIND_MOUNTS; i++) {
        if (!g_bind_mounts[i].active || !g_bind_mounts[i].read_only)
            continue;
        int len = g_bind_mounts[i].path_len;
        if (strncmp(g_bind_mounts[i].path, cmp_path, len) == 0 &&
            (cmp_path[len] == '/' || cmp_path[len] == '\0'))
            return true;
    }
    return false;
}

/* Auto-create a meta.db entry for a path under a bind mount.
 * Probes the host filesystem to determine if it's a file or directory. */
static inode_t bind_mount_ensure_inode(struct fakefs_db *fs, struct mount *mount,
                                       const char *path) {
    db_begin_read(fs);
    inode_t ino = path_get_inode(fs, path);
    db_commit(fs);
    if (ino != 0)
        return ino;

    /* Check if it exists on host — use direct path for bind mounts */
    struct stat host_stat;
    char host_abs[PATH_MAX];
    if (bind_mount_translate_path(path, host_abs, sizeof(host_abs))) {
        if (stat(host_abs, &host_stat) < 0)
            return 0;
    } else if (fstatat(mount->root_fd, fix_path(path), &host_stat, AT_SYMLINK_NOFOLLOW) < 0) {
        if (fstatat(mount->root_fd, fix_path(path), &host_stat, 0) < 0)
            return 0;
    }

    uint32_t mode;
    if (S_ISDIR(host_stat.st_mode))
        mode = S_IFDIR | 0755;
    else if (S_ISLNK(host_stat.st_mode))
        mode = S_IFLNK | 0777;
    else
        mode = S_IFREG | 0644;

    struct ish_stat ishstat = {.mode = mode, .uid = 0, .gid = 0, .rdev = 0};
    db_begin_write(fs);
    ino = path_create(fs, path, &ishstat);
    db_commit(fs);
    return ino;
}

// this exists only to override readdir to fix the returned inode numbers
static struct fd_ops fakefs_fdops;

static struct fd *fakefs_open(struct mount *mount, const char *path, int flags, int mode) {
    struct fakefs_db *fs = &mount->fakefs;

    /* Reject any write-intent open under a read-only bind mount.
     * iSH processes run as root and bypass the meta.db mode DAC check,
     * so the 0555 bits on the mount dir don't actually block anything. */
    if ((flags & (O_WRONLY_ | O_RDWR_ | O_CREAT_ | O_TRUNC_ | O_APPEND_)) &&
        is_under_readonly_bind_mount(path)) {
        return ERR_PTR(_EROFS);
    }

    /* For bind-mounted paths, open directly via host absolute path
     * instead of relying on symlink traversal (which iOS sandbox may block). */
    char host_abs[PATH_MAX];
    struct fd *fd;
    bool via_hook = false;
    bool via_bind = bind_mount_translate_path_ex(path, host_abs, sizeof(host_abs), &via_hook);
    if (via_bind) {
        int real_flags = 0;
        if (flags & O_RDONLY_) real_flags |= O_RDONLY;
        if (flags & O_WRONLY_) real_flags |= O_WRONLY;
        if (flags & O_RDWR_) real_flags |= O_RDWR;
        if (flags & O_CREAT_) real_flags |= O_CREAT;
        if (flags & O_EXCL_) real_flags |= O_EXCL;
        if (flags & O_TRUNC_) real_flags |= O_TRUNC;
        if (flags & O_APPEND_) real_flags |= O_APPEND;
        if (flags & O_NONBLOCK_) real_flags |= O_NONBLOCK;
        int fd_no = open(host_abs, real_flags, 0666);
        if (fd_no < 0) {
            return ERR_PTR(errno_map());
        }
        /* Bind-mount fast path opens the host file directly, bypassing
         * realfs_open. Emit the change event here so consumers (e.g. the
         * iCloud SessionFile sync tracker) see writes/creates/truncates
         * landing in bind-mounted dirs. We pass the *guest* path so the
         * Swift consumer can resolve the bind mount → session mapping
         * without needing to know about host APFS layout. */
        if (flags & (O_CREAT_ | O_TRUNC_ | O_WRONLY_ | O_RDWR_ | O_APPEND_)) {
            fakefs_record_change(path, FAKEFS_CHANGE_OP_WRITE);
        }
        fd = fd_create(&realfs_fdops);
        fd->real_fd = fd_no;
        fd->dir = NULL;
    } else {
        fd = realfs.open(mount, path, flags, 0666);
        if (IS_ERR(fd))
            return fd;
    }
    /* Hook-routed paths intentionally bypass meta.db: the `paths` table is
     * keyed globally by guest path, but the hook can map the same guest
     * path to different host files per fs_context. Storing them would
     * collide across contexts (one row, N host files). For these paths we
     * synthesize the fake inode from the host inode so operations within a
     * single fd still behave consistently. */
    if (via_hook) {
        struct stat host_stat;
        if (fstat(fd->real_fd, &host_stat) == 0)
            fd->fake_inode = (ino_t)host_stat.st_ino;
        else
            fd->fake_inode = 1;  /* non-zero so the ENOENT branch below is skipped */
        return fd;
    }
    db_begin_write(fs);
    fd->fake_inode = path_get_inode(fs, path);
    if (flags & O_CREAT_) {
        struct ish_stat ishstat;
        ishstat.mode = mode | S_IFREG;
        ishstat.uid = current->euid;
        ishstat.gid = current->egid;
        ishstat.rdev = 0;
        if (fd->fake_inode == 0) {
            path_create(fs, path, &ishstat);
            fd->fake_inode = path_get_inode(fs, path);
        }
    }
    db_commit(fs);
    if (fd->fake_inode == 0) {
        /* Auto-create for bind-mounted paths */
        if (is_under_bind_mount(path)) {
            fd->fake_inode = bind_mount_ensure_inode(fs, mount, path);
        }
        if (fd->fake_inode == 0) {
            fd_close(fd);
            return ERR_PTR(_ENOENT);
        }
    }
    fd->ops = &fakefs_fdops;
    return fd;
}

// WARNING: giant hack, just for file providerws
struct fd *fakefs_open_inode(struct mount *mount, ino_t inode) {
    struct fakefs_db *fs = &mount->fakefs;
    db_begin_read(fs);
    sqlite3_stmt *stmt = fs->stmt.path_from_inode;
    sqlite3_bind_int64(stmt, 1, inode);
step:
    if (!db_exec(fs, stmt)) {
        db_reset(fs, stmt);
        db_rollback(fs);
        return ERR_PTR(_ENOENT);
    }
    const char *path = (const char *) sqlite3_column_text(stmt, 0);
    struct fd *fd = realfs.open(mount, path, O_RDWR_, 0);
    if (PTR_ERR(fd) == _EISDIR)
        fd = realfs.open(mount, path, O_RDONLY_, 0);
    if (PTR_ERR(fd) == _ENOENT)
        goto step;
    db_reset(fs, stmt);
    db_commit(fs);
    fd->fake_inode = inode;
    fd->ops = &fakefs_fdops;
    return fd;
}

static int fakefs_link(struct mount *mount, const char *src, const char *dst) {
    struct fakefs_db *fs = &mount->fakefs;
    if (is_under_readonly_bind_mount(dst))
        return _EROFS;
    /* Hook-routed paths can't share meta.db rows, so hard-linking them is
     * not supported. Perform a host link() directly between translated host
     * paths when both endpoints are hook-routed. Mixing hook + non-hook
     * sides returns EXDEV, matching cross-fs link semantics. */
    char host_src[PATH_MAX], host_dst[PATH_MAX];
    bool src_hook = false, dst_hook = false;
    bool src_bind = bind_mount_translate_path_ex(src, host_src, sizeof(host_src), &src_hook);
    bool dst_bind = bind_mount_translate_path_ex(dst, host_dst, sizeof(host_dst), &dst_hook);
    if (src_hook != dst_hook)
        return _EXDEV;
    if (src_hook) {
        if (link(host_src, host_dst) < 0)
            return errno_map();
        fakefs_record_change(dst, FAKEFS_CHANGE_OP_WRITE);
        return 0;
    }
    (void)src_bind; (void)dst_bind;
    db_begin_write(fs);
    int err = realfs.link(mount, src, dst);
    if (err < 0) {
        db_rollback(fs);
        return err;
    }
    path_link(fs, src, dst);
    db_commit(fs);
    return 0;
}

static int fakefs_unlink(struct mount *mount, const char *path) {
    struct fakefs_db *fs = &mount->fakefs;
    if (is_under_readonly_bind_mount(path))
        return _EROFS;
    char host_abs[PATH_MAX];
    bool via_hook = false;
    bool via_bind = bind_mount_translate_path_ex(path, host_abs, sizeof(host_abs), &via_hook);
    /* Hook-routed paths bypass meta.db entirely (see fakefs_open). */
    if (via_hook) {
        if (unlink(host_abs) < 0)
            return errno_map();
        fakefs_record_change(path, FAKEFS_CHANGE_OP_UNLINK);
        return 0;
    }
    /* Auto-create entry if under bind mount so path_unlink won't die */
    if (is_under_bind_mount(path))
        bind_mount_ensure_inode(fs, mount, path);
    db_begin_write(fs);
    int err;
    if (via_bind) {
        if (unlink(host_abs) < 0) {
            err = errno_map();
            db_rollback(fs);
            return err;
        }
    } else {
        err = realfs.unlink(mount, path);
        if (err < 0) {
            db_rollback(fs);
            return err;
        }
    }
    ino_t ino = path_unlink(fs, path);
    db_commit(fs);
    if (via_bind)
        fakefs_record_change(path, FAKEFS_CHANGE_OP_UNLINK);
    inode_check_orphaned(mount, ino);
    return 0;
}

static int fakefs_rmdir(struct mount *mount, const char *path) {
    struct fakefs_db *fs = &mount->fakefs;
    if (is_under_readonly_bind_mount(path))
        return _EROFS;
    char host_abs[PATH_MAX];
    bool via_hook = false;
    bool via_bind = bind_mount_translate_path_ex(path, host_abs, sizeof(host_abs), &via_hook);
    if (via_hook) {
        if (rmdir(host_abs) < 0)
            return errno_map();
        fakefs_record_change(path, FAKEFS_CHANGE_OP_UNLINK);
        return 0;
    }
    if (is_under_bind_mount(path))
        bind_mount_ensure_inode(fs, mount, path);
    db_begin_write(fs);
    int err;
    if (via_bind) {
        if (rmdir(host_abs) < 0) {
            err = errno_map();
            db_rollback(fs);
            return err;
        }
    } else {
        err = realfs.rmdir(mount, path);
        if (err < 0) {
            db_rollback(fs);
            return err;
        }
    }
    ino_t ino = path_unlink(fs, path);
    db_commit(fs);
    if (via_bind)
        fakefs_record_change(path, FAKEFS_CHANGE_OP_UNLINK);
    inode_check_orphaned(mount, ino);
    return 0;
}

static int fakefs_rename(struct mount *mount, const char *src, const char *dst) {
    struct fakefs_db *fs = &mount->fakefs;
    if (is_under_readonly_bind_mount(src) || is_under_readonly_bind_mount(dst))
        return _EROFS;
    char host_src[PATH_MAX], host_dst[PATH_MAX];
    bool src_hook = false, dst_hook = false;
    bool src_bind = bind_mount_translate_path_ex(src, host_src, sizeof(host_src), &src_hook);
    bool dst_bind = bind_mount_translate_path_ex(dst, host_dst, sizeof(host_dst), &dst_hook);
    if (src_bind != dst_bind || src_hook != dst_hook)
        return _EXDEV;
    if (src_hook) {
        if (rename(host_src, host_dst) < 0)
            return errno_map();
        fakefs_record_change(dst, FAKEFS_CHANGE_OP_RENAME);
        return 0;
    }
    db_begin_write(fs);
    path_rename(fs, src, dst);
    int err;
    if (src_bind) {
        if (rename(host_src, host_dst) < 0) {
            err = errno_map();
            db_rollback(fs);
            return err;
        }
    } else {
        err = realfs.rename(mount, src, dst);
        if (err < 0) {
            db_rollback(fs);
            return err;
        }
    }
    db_commit(fs);
    if (src_bind)
        fakefs_record_change(dst, FAKEFS_CHANGE_OP_RENAME);
    return 0;
}

static int fakefs_symlink(struct mount *mount, const char *target, const char *link) {
    struct fakefs_db *fs = &mount->fakefs;
    if (is_under_readonly_bind_mount(link))
        return _EROFS;
    /* fakefs's symlink model stores the target in a regular file plus an
     * S_IFLNK mode bit in meta.db. Hook-routed paths bypass meta.db, so
     * that representation can't round-trip. Reject the request rather
     * than creating a regular file that future stat/readlink can't
     * recognize as a symlink. */
    char host_chk[PATH_MAX];
    bool _vh = false;
    if (bind_mount_translate_path_ex(link, host_chk, sizeof(host_chk), &_vh) && _vh)
        return _EPERM;
    db_begin_write(fs);
    // create a file containing the target
    int fd = openat(mount->root_fd, fix_path(link), O_WRONLY | O_CREAT | O_EXCL, 0666);
    if (fd < 0) {
        db_rollback(fs);
        return errno_map();
    }
    ssize_t res = write(fd, target, strlen(target));
    close(fd);
    if (res < 0) {
        int saved_errno = errno;
        unlinkat(mount->root_fd, fix_path(link), 0);
        db_rollback(fs);
        errno = saved_errno;
        return errno_map();
    }

    // customize the stat info so it looks like a link
    struct ish_stat ishstat;
    ishstat.mode = S_IFLNK | 0777; // symlinks always have full permissions
    ishstat.uid = current->euid;
    ishstat.gid = current->egid;
    ishstat.rdev = 0;
    path_create(fs, link, &ishstat);
    db_commit(fs);
    return 0;
}

static int fakefs_mknod(struct mount *mount, const char *path, mode_t_ mode, dev_t_ dev) {
    struct fakefs_db *fs = &mount->fakefs;
    if (is_under_readonly_bind_mount(path))
        return _EROFS;
    mode_t_ real_mode = 0666;
    if (S_ISBLK(mode) || S_ISCHR(mode) || S_ISSOCK(mode))
        real_mode |= S_IFREG;
    else
        real_mode |= mode & S_IFMT;
    int err = realfs.mknod(mount, path, real_mode, 0);
    if (err < 0 && err != _EEXIST) {
        return err;
    }
    db_begin_write(fs);
    struct ish_stat stat;
    stat.mode = mode;
    stat.uid = current->euid;
    stat.gid = current->egid;
    stat.rdev = 0;
    if (S_ISBLK(mode) || S_ISCHR(mode))
        stat.rdev = dev;
    if (path_get_inode(fs, path) == 0)
        path_create(fs, path, &stat);
    db_commit(fs);
    return 0;
}

static int fakefs_stat(struct mount *mount, const char *path, struct statbuf *fake_stat) {
    ISH_SIGNPOST_SCOPE_BEGIN(fs, "fakefs_stat", _fs_spid);
    struct fakefs_db *fs = &mount->fakefs;
    /* Hook-routed paths bypass meta.db (see fakefs_open). Stat directly
     * against the host file; synthesize mode bits from host's. */
    char host_abs[PATH_MAX];
    bool via_hook = false;
    if (bind_mount_translate_path_ex(path, host_abs, sizeof(host_abs), &via_hook) && via_hook) {
        struct stat host_stat;
        if (stat(host_abs, &host_stat) < 0) {
            ISH_SIGNPOST_SCOPE_END(fs, "fakefs_stat", _fs_spid);
            return errno_map();
        }
        memset(fake_stat, 0, sizeof(*fake_stat));
        fake_stat->inode = (ino_t)host_stat.st_ino;
        /* Pick a sensible mode: keep host's S_IFMT, default to 0644/0755. */
        mode_t_ type = host_stat.st_mode & S_IFMT;
        fake_stat->mode = type | (S_ISDIR(host_stat.st_mode) ? 0755 : 0644);
        fake_stat->uid = 0;
        fake_stat->gid = 0;
        fake_stat->size = host_stat.st_size;
        fake_stat->blocks = host_stat.st_blocks;
        fake_stat->mtime = host_stat.st_mtimespec.tv_sec;
        fake_stat->mtime_nsec = host_stat.st_mtimespec.tv_nsec;
        fake_stat->atime = host_stat.st_atimespec.tv_sec;
        fake_stat->atime_nsec = host_stat.st_atimespec.tv_nsec;
        fake_stat->ctime = host_stat.st_ctimespec.tv_sec;
        fake_stat->ctime_nsec = host_stat.st_ctimespec.tv_nsec;
        ISH_SIGNPOST_SCOPE_END(fs, "fakefs_stat", _fs_spid);
        return 0;
    }
    db_begin_read(fs);
    struct ish_stat ishstat;
    ino_t inode;
    if (!path_read_stat(fs, path, &ishstat, &inode)) {
        db_rollback(fs);
        /* Auto-create for bind-mounted paths */
        if (is_under_bind_mount(path)) {
            inode = bind_mount_ensure_inode(fs, mount, path);
            if (inode != 0) {
                db_begin_read(fs);
                path_read_stat(fs, path, &ishstat, &inode);
                db_commit(fs);
                int err = realfs.stat(mount, path, fake_stat);
                if (err < 0)
                    return err;
                fake_stat->inode = inode;
                fake_stat->mode = ishstat.mode;
                fake_stat->uid = ishstat.uid;
                fake_stat->gid = ishstat.gid;
                fake_stat->rdev = ishstat.rdev;
                return 0;
            }
        }
        return _ENOENT;
    }
    int err;
    char host_stat_path[PATH_MAX];
    if (bind_mount_translate_path(path, host_stat_path, sizeof(host_stat_path))) {
        /* For bind-mounted paths, stat the host path directly to avoid
         * AT_SYMLINK_NOFOLLOW returning symlink stats instead of dir stats. */
        struct stat real_stat;
        if (stat(host_stat_path, &real_stat) < 0) {
            db_commit(fs);
            return errno_map();
        }
        /* Copy basic fields from real stat */
        fake_stat->size = real_stat.st_size;
        fake_stat->nlink = real_stat.st_nlink;
        fake_stat->atime = real_stat.st_atimespec.tv_sec;
        fake_stat->mtime = real_stat.st_mtimespec.tv_sec;
        fake_stat->ctime = real_stat.st_ctimespec.tv_sec;
        err = 0;
    } else {
        err = realfs.stat(mount, path, fake_stat);
    }
    db_commit(fs);
    if (err < 0)
        return err;
    fake_stat->inode = inode;
    fake_stat->mode = ishstat.mode;
    fake_stat->uid = ishstat.uid;
    fake_stat->gid = ishstat.gid;
    fake_stat->rdev = ishstat.rdev;
    ISH_SIGNPOST_SCOPE_END(fs, "fakefs_stat", _fs_spid);
    return 0;
}

static int fakefs_fstat(struct fd *fd, struct statbuf *fake_stat) {
    struct fakefs_db *fs = &fd->mount->fakefs;
    int err = realfs.fstat(fd, fake_stat);
    if (err < 0)
        return err;
    db_begin_read(fs);
    struct ish_stat ishstat;
    if (!inode_read_stat_if_exist(fs, fd->fake_inode, &ishstat)) {
        db_rollback(fs);
        /* meta.db row missing. If the fd's path resolves through the
         * path-translate hook, the missing row is by design (hook-routed
         * paths bypass meta.db, see fakefs_open). Trust the realfs.fstat
         * result and synthesize sensible mode bits. Otherwise propagate the
         * ENOENT so unrelated bugs aren't masked. */
        char fd_path[MAX_PATH];
        if (realfs_getpath(fd, fd_path) == 0) {
            char host_chk[PATH_MAX];
            bool via_hook = false;
            if (bind_mount_translate_path_ex(fd_path, host_chk, sizeof(host_chk), &via_hook) && via_hook) {
                mode_t_ type = fake_stat->mode & S_IFMT;
                fake_stat->mode = type | (S_ISDIR(fake_stat->mode) ? 0755 : 0644);
                fake_stat->uid = 0;
                fake_stat->gid = 0;
                fake_stat->rdev = 0;
                return 0;
            }
        }
        return _ENOENT;
    }
    db_commit(fs);
    fake_stat->inode = fd->fake_inode;
    fake_stat->mode = ishstat.mode;
    fake_stat->uid = ishstat.uid;
    fake_stat->gid = ishstat.gid;
    fake_stat->rdev = ishstat.rdev;
    return 0;
}

static void fake_stat_setattr(struct ish_stat *ishstat, struct attr attr) {
    switch (attr.type) {
        case attr_uid:
            ishstat->uid = attr.uid;
            break;
        case attr_gid:
            ishstat->gid = attr.gid;
            break;
        case attr_mode:
            ishstat->mode = (ishstat->mode & S_IFMT) | (attr.mode & ~S_IFMT);
            break;
        case attr_size:
            die("attr_size should be handled by realfs");
    }
}

static int fakefs_setattr(struct mount *mount, const char *path, struct attr attr) {
    struct fakefs_db *fs = &mount->fakefs;
    if (is_under_readonly_bind_mount(path))
        return _EROFS;
    /* Hook-routed paths bypass meta.db (see fakefs_open). Apply size and
     * mode/owner changes directly on the host abs path; ignore the meta.db
     * side. For non-size attrs we use host syscalls so callers get a real
     * effect (or a real EPERM) rather than silent no-op or a meta.db die. */
    char host_abs_chk[PATH_MAX];
    bool _via_hook = false;
    if (bind_mount_translate_path_ex(path, host_abs_chk, sizeof(host_abs_chk), &_via_hook) && _via_hook) {
        int err = 0;
        switch (attr.type) {
            case attr_size:
                if (truncate(host_abs_chk, attr.size) < 0)
                    return errno_map();
                return 0;
            case attr_mode:
                if (chmod(host_abs_chk, attr.mode) < 0)
                    return errno_map();
                return 0;
            case attr_uid:
            case attr_gid:
                /* iSH runs as a non-root iOS app; host chown will EPERM.
                 * Match realfs_setattr's silent-on-EPERM behaviour. */
                return 0;
            default:
                return 0;
        }
        (void)err;
    }
    if (attr.type == attr_size)
        return realfs.setattr(mount, path, attr);
    db_begin_read(fs);
    struct ish_stat ishstat;
    ino_t inode;
    if (!path_read_stat(fs, path, &ishstat, &inode)) {
        db_rollback(fs);
        /* Auto-create for bind-mounted paths */
        if (is_under_bind_mount(path)) {
            inode = bind_mount_ensure_inode(fs, mount, path);
            if (inode != 0) {
                db_begin_read(fs);
                path_read_stat(fs, path, &ishstat, &inode);
                fake_stat_setattr(&ishstat, attr);
                inode_write_stat(fs, inode, &ishstat);
                db_commit(fs);
                return 0;
            }
        }
        return _ENOENT;
    }
    fake_stat_setattr(&ishstat, attr);
    inode_write_stat(fs, inode, &ishstat);
    db_commit(fs);
    return 0;
}

static int fakefs_fsetattr(struct fd *fd, struct attr attr) {
    struct fakefs_db *fs = &fd->mount->fakefs;
    if (attr.type == attr_size)
        return realfs.fsetattr(fd, attr);
    /* If this fd was opened on a hook-routed path, its fake_inode is the
     * host inode and won't be in meta.db. Detect that by probing meta.db;
     * if absent, apply attr directly to the host fd. */
    db_begin_read(fs);
    struct ish_stat ishstat;
    bool in_meta = inode_read_stat_if_exist(fs, fd->fake_inode, &ishstat);
    db_commit(fs);
    if (!in_meta) {
        switch (attr.type) {
            case attr_mode:
                if (fchmod(fd->real_fd, attr.mode) < 0)
                    return errno_map();
                return 0;
            case attr_uid:
            case attr_gid:
                return 0;  /* host EPERM expected; ignore (matches realfs) */
            default:
                return 0;
        }
    }
    db_begin_write(fs);
    fake_stat_setattr(&ishstat, attr);
    inode_write_stat(fs, fd->fake_inode, &ishstat);
    db_commit(fs);
    return 0;
}

static int fakefs_mkdir(struct mount *mount, const char *path, mode_t_ mode) {
    struct fakefs_db *fs = &mount->fakefs;
    if (is_under_readonly_bind_mount(path))
        return _EROFS;
    char host_abs[PATH_MAX];
    bool via_hook = false;
    bool via_bind = bind_mount_translate_path_ex(path, host_abs, sizeof(host_abs), &via_hook);
    if (via_hook) {
        if (mkdir(host_abs, 0777) < 0)
            return errno_map();
        fakefs_record_change(path, FAKEFS_CHANGE_OP_WRITE);
        return 0;
    }
    db_begin_write(fs);
    int err;
    if (via_bind) {
        if (mkdir(host_abs, 0777) < 0) {
            err = errno_map();
            db_rollback(fs);
            return err;
        }
    } else {
        err = realfs.mkdir(mount, path, 0777);
        if (err < 0) {
            db_rollback(fs);
            return err;
        }
    }
    struct ish_stat ishstat;
    ishstat.mode = mode | S_IFDIR;
    ishstat.uid = current->euid;
    ishstat.gid = current->egid;
    ishstat.rdev = 0;
    path_create(fs, path, &ishstat);
    db_commit(fs);
    if (via_bind)
        fakefs_record_change(path, FAKEFS_CHANGE_OP_WRITE);
    return 0;
}

static ssize_t file_readlink(struct mount *mount, const char *path, char *buf, size_t bufsize) {
    // broken symlinks can't be included in an iOS app or else Xcode craps out
    int fd = openat(mount->root_fd, fix_path(path), O_RDONLY);
    if (fd < 0)
        return errno_map();
    int err = read(fd, buf, bufsize);
    close(fd);
    if (err < 0)
        return errno_map();
    return err;
}

static ssize_t fakefs_readlink(struct mount *mount, const char *path, char *buf, size_t bufsize) {
    struct fakefs_db *fs = &mount->fakefs;
    db_begin_read(fs);
    struct ish_stat ishstat;
    if (!path_read_stat(fs, path, &ishstat, NULL)) {
        db_rollback(fs);
        return _ENOENT;
    }
    if (!S_ISLNK(ishstat.mode)) {
        db_rollback(fs);
        return _EINVAL;
    }
    ssize_t err = realfs.readlink(mount, path, buf, bufsize);
    if (err == _EINVAL)
        err = file_readlink(mount, path, buf, bufsize);
    db_commit(fs);
    return err;
}

static int fakefs_readdir(struct fd *fd, struct dir_entry *entry) {
    assert(fd->ops == &fakefs_fdops);
    int res;
retry:
    res = realfs_fdops.readdir(fd, entry);
    if (res <= 0)
        return res;

    // this is annoying
    char entry_path[MAX_PATH + 1];
    realfs_getpath(fd, entry_path);

    /* Debug: log readdir for bind-mounted paths */
    if (strstr(entry_path, "minis") != NULL || strstr(entry_path, "Library/MinisChat") != NULL)
        fprintf(stderr, "fakefs_readdir: getpath=\"%s\" entry=\"%s\"\n", entry_path, entry->name);

    if (strcmp(entry->name, "..") == 0) {
        if (strcmp(entry_path, "") != 0) {
            *strrchr(entry_path, '/') = '\0';
        }
    } else if (strcmp(entry->name, ".") != 0) {
        // god I don't know what to do if this would overflow
        strcat(entry_path, "/");
        strcat(entry_path, entry->name);
    }

    /* Hook-routed entries are not in meta.db by design (see fakefs_open).
     * Keep the inode that realfs_readdir gave us (it came straight from the
     * host directory, so it's stable for the duration of the open fd). */
    char host_check[PATH_MAX];
    bool entry_via_hook = false;
    bind_mount_translate_path_ex(entry_path, host_check, sizeof(host_check), &entry_via_hook);
    if (entry_via_hook) {
        return res;
    }

    struct fakefs_db *fs = &fd->mount->fakefs;
    db_begin_read(fs);
    entry->inode = path_get_inode(fs, entry_path);
    db_commit(fs);

    if (entry->inode == 0) {
        /* Auto-create for bind-mounted paths */
        if (is_under_bind_mount(entry_path)) {
            entry->inode = bind_mount_ensure_inode(fs, fd->mount, entry_path);
        }
        /* Still no inode? Skip this entry to avoid crashes */
        if (entry->inode == 0)
            goto retry;
    }
    return res;
}

static struct fd_ops fakefs_fdops;
static void __attribute__((constructor)) init_fake_fdops() {
    fakefs_fdops = realfs_fdops;
    fakefs_fdops.readdir = fakefs_readdir;
}

/* ===== Public Bind Mount API ===== */

/* Create a relative symlink at root_fd/host_link -> host_path.
 * Computes a relative target so openat() can follow the symlink
 * without escaping the sandbox (absolute symlinks fail on iOS). */
static int create_relative_symlink(int root_fd, const char *host_link,
                                    const char *host_path) {
    /* Get the absolute path of root_fd (F_GETPATH resolves symlinks,
     * e.g. /var -> /private/var on iOS) */
    char root_abs[PATH_MAX];
    if (fcntl(root_fd, F_GETPATH, root_abs) != 0) {
        fprintf(stderr, "create_relative_symlink: F_GETPATH failed\n");
        /* Fall back to absolute symlink */
        return symlinkat(host_path, root_fd, host_link);
    }

    /* Resolve host_path too, so both paths use the same prefix
     * (e.g. both /private/var/... instead of mixing /var/... and /private/var/...) */
    char host_real[PATH_MAX];
    if (realpath(host_path, host_real) == NULL) {
        fprintf(stderr, "create_relative_symlink: realpath(%s) failed, falling back\n", host_path);
        return symlinkat(host_path, root_fd, host_link);
    }

    /* Build the absolute path of the symlink's parent directory */
    char link_abs[PATH_MAX];
    snprintf(link_abs, sizeof(link_abs), "%s/%s", root_abs, host_link);
    /* Strip the last component to get the parent dir */
    char *last_slash = strrchr(link_abs, '/');
    if (last_slash) *last_slash = '\0';

    /* Compute relative path from link parent dir to host_path */
    char rel_target[PATH_MAX];
    compute_relative_path(link_abs, host_real, rel_target, sizeof(rel_target));

    fprintf(stderr, "create_relative_symlink: \"%s\" -> \"%s\" (relative: \"%s\")\n",
            host_link, host_path, rel_target);

    return symlinkat(rel_target, root_fd, host_link);
}

/* Recursively remove a directory tree relative to dir_fd.
 * Handles non-empty directories that unlinkat(AT_REMOVEDIR) can't delete. */
static void remove_tree_at(int dir_fd, const char *path) {
    int fd = openat(dir_fd, path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    if (fd < 0) {
        /* Not a directory or doesn't exist — try plain unlink */
        unlinkat(dir_fd, path, 0);
        return;
    }
    DIR *dir = fdopendir(fd);
    if (!dir) {
        close(fd);
        unlinkat(dir_fd, path, 0);
        return;
    }
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
            continue;
        if (ent->d_type == DT_DIR) {
            remove_tree_at(dirfd(dir), ent->d_name);
        } else {
            unlinkat(dirfd(dir), ent->d_name, 0);
        }
    }
    closedir(dir); /* also closes fd */
    unlinkat(dir_fd, path, AT_REMOVEDIR);
}

int fakefs_bind_mount(const char *linux_path, const char *host_path, bool read_only) {
    fprintf(stderr, "fakefs_bind_mount: ENTER linux_path=\"%s\" (len=%zu) host_path=\"%s\" (len=%zu) read_only=%d\n",
            linux_path, strlen(linux_path), host_path, strlen(host_path), read_only ? 1 : 0);

    if (g_fakefs_mount == NULL) {
        fprintf(stderr, "fakefs_bind_mount: FAIL g_fakefs_mount is NULL\n");
        return _ENODEV;
    }

    const char *fixed = fix_path(linux_path);
    fprintf(stderr, "fakefs_bind_mount: fix_path(\"%s\") => \"%s\" (len=%zu)\n",
            linux_path, fixed, strlen(fixed));

    /* Log root_fd info for context */
    {
        char fd_path[PATH_MAX];
        if (fcntl(g_fakefs_mount->root_fd, F_GETPATH, fd_path) == 0) {
            fprintf(stderr, "fakefs_bind_mount: root_fd=%d path=\"%s\"\n",
                    g_fakefs_mount->root_fd, fd_path);
        } else {
            fprintf(stderr, "fakefs_bind_mount: root_fd=%d (F_GETPATH failed)\n",
                    g_fakefs_mount->root_fd);
        }
    }

    /* Verify host path exists (directory or regular file) */
    struct stat st;
    if (stat(host_path, &st) < 0) {
        fprintf(stderr, "fakefs_bind_mount: FAIL host_path does not exist\n");
        return _ENOENT;
    }
    bool is_file_mount = S_ISREG(st.st_mode);
    if (!S_ISDIR(st.st_mode) && !is_file_mount) {
        fprintf(stderr, "fakefs_bind_mount: FAIL host_path is neither a dir nor a regular file\n");
        return _ENOENT;
    }

    /* Resolve symlinks in host_path so it matches what F_GETPATH returns.
     * e.g. ~/Library/GroupContainersAlias/... -> ~/Library/Group Containers/... */
    char resolved_host[PATH_MAX];
    if (realpath(host_path, resolved_host) != NULL)
        host_path = resolved_host;

    /* Compute the meta.db mode for the top-level mount point based on whether
     * the mount is read-only and whether it's a file or directory. */
    uint32_t dir_mode = read_only ? 0555 : 0755;
    uint32_t file_mode = read_only ? 0444 : 0644;
    uint32_t top_mode = is_file_mount ? (S_IFREG | file_mode) : (S_IFDIR | dir_mode);

    /* Check if already mounted at this path — update if host_path or mode changed */
    for (int i = 0; i < FAKEFS_MAX_BIND_MOUNTS; i++) {
        if (g_bind_mounts[i].active &&
            strcmp(g_bind_mounts[i].path, linux_path) == 0) {
            bool host_changed = strcmp(g_bind_mounts[i].host_path, host_path) != 0;
            /* Always refresh the read-only flag — the user may have flipped
             * writability in Settings without changing anything else. */
            g_bind_mounts[i].read_only = read_only;

            /* Always refresh the meta.db mode — the read-only flag may have
             * flipped even if host_path is unchanged (e.g. user toggled
             * writability in Settings). */
            {
                struct fakefs_db *fs = &g_fakefs_mount->fakefs;
                db_begin_write(fs);
                inode_t existing = path_get_inode(fs, linux_path);
                if (existing != 0) {
                    struct ish_stat prev;
                    if (inode_read_stat_if_exist(fs, existing, &prev)) {
                        prev.mode = top_mode;
                        inode_write_stat(fs, existing, &prev);
                        fprintf(stderr, "fakefs_bind_mount: slot[%d] updated meta.db mode to 0%o\n",
                                i, top_mode & 07777);
                    }
                } else {
                    struct ish_stat ishstat = {
                        .mode = top_mode, .uid = 0, .gid = 0, .rdev = 0
                    };
                    path_create(fs, linux_path, &ishstat);
                    fprintf(stderr, "fakefs_bind_mount: slot[%d] created meta.db entry with mode 0%o\n",
                            i, top_mode & 07777);
                }
                db_commit(fs);
            }

            if (!host_changed) {
                fprintf(stderr, "fakefs_bind_mount: slot[%d] already mounted with same host_path, mode refreshed\n", i);
                return 0;
            }
            /* host_path changed (e.g. session switch) — update slot and symlink */
            fprintf(stderr, "fakefs_bind_mount: slot[%d] host_path changed from \"%s\" to \"%s\"\n",
                    i, g_bind_mounts[i].host_path, host_path);
            strlcpy(g_bind_mounts[i].host_path, host_path,
                    sizeof(g_bind_mounts[i].host_path));
            g_bind_mounts[i].host_path_len = strlen(host_path);

            char host_link[PATH_MAX];
            snprintf(host_link, sizeof(host_link), "%s", fix_path(linux_path));
            fprintf(stderr, "fakefs_bind_mount: slot[%d] UPDATE symlink: host_link=\"%s\" -> host_path=\"%s\" (root_fd=%d)\n",
                    i, host_link, host_path, g_fakefs_mount->root_fd);
            /* Remove whatever is at the path (file, symlink, or directory tree) */
            remove_tree_at(g_fakefs_mount->root_fd, host_link);
            int err = create_relative_symlink(g_fakefs_mount->root_fd, host_link, host_path);
            fprintf(stderr, "fakefs_bind_mount: slot[%d] symlinkat returned %d (errno=%d)\n", i, err, err < 0 ? errno : 0);
            if (err < 0) {
                g_bind_mounts[i].active = false;
                return _EINVAL;
            }
            return 0;
        }
    }

    /* Find a free slot */
    for (int i = 0; i < FAKEFS_MAX_BIND_MOUNTS; i++) {
        if (!g_bind_mounts[i].active) {
            fprintf(stderr, "fakefs_bind_mount: using free slot[%d]\n", i);
            strlcpy(g_bind_mounts[i].path, linux_path,
                    sizeof(g_bind_mounts[i].path));
            g_bind_mounts[i].path_len = strlen(linux_path);
            strlcpy(g_bind_mounts[i].host_path, host_path,
                    sizeof(g_bind_mounts[i].host_path));
            g_bind_mounts[i].host_path_len = strlen(host_path);
            g_bind_mounts[i].active = true;
            g_bind_mounts[i].read_only = read_only;

            /* Create symlink on host: data/<linux_path> -> <host_path>
             * First remove any existing file/dir at the path (may be non-empty) */
            char host_link[PATH_MAX];
            snprintf(host_link, sizeof(host_link), "%s", fix_path(linux_path));

            fprintf(stderr, "fakefs_bind_mount: slot[%d] NEW symlink: host_link=\"%s\" -> host_path=\"%s\" (root_fd=%d)\n",
                    i, host_link, host_path, g_fakefs_mount->root_fd);

            /* Remove whatever is at the path (file, symlink, or directory tree) */
            remove_tree_at(g_fakefs_mount->root_fd, host_link);

            /* Create the symlink (relative path to avoid iOS sandbox issues) */
            int err = create_relative_symlink(g_fakefs_mount->root_fd, host_link, host_path);
            fprintf(stderr, "fakefs_bind_mount: slot[%d] symlinkat returned %d (errno=%d)\n", i, err, err < 0 ? errno : 0);
            if (err < 0) {
                g_bind_mounts[i].active = false;
                return _EINVAL;
            }

            /* Verify the symlink was created correctly */
            char readbuf[PATH_MAX];
            ssize_t rlen = readlinkat(g_fakefs_mount->root_fd, host_link, readbuf, sizeof(readbuf) - 1);
            if (rlen > 0) {
                readbuf[rlen] = '\0';
                fprintf(stderr, "fakefs_bind_mount: slot[%d] VERIFY readlinkat(\"%s\") => \"%s\"\n", i, host_link, readbuf);
            } else {
                fprintf(stderr, "fakefs_bind_mount: slot[%d] VERIFY readlinkat(\"%s\") FAILED (errno=%d)\n", i, host_link, errno);
            }

            /* Ensure the mount point exists in meta.db (dir or file), with the
             * correct mode based on the read_only flag. If an entry already
             * exists from a previous run we rewrite the mode so the read-only
             * state always matches what Settings says. */
            struct fakefs_db *fs = &g_fakefs_mount->fakefs;
            db_begin_write(fs);
            inode_t ino = path_get_inode(fs, linux_path);
            if (ino == 0) {
                struct ish_stat ishstat = {
                    .mode = top_mode, .uid = 0, .gid = 0, .rdev = 0
                };
                path_create(fs, linux_path, &ishstat);
                fprintf(stderr, "fakefs_bind_mount: slot[%d] created meta.db entry for \"%s\" mode=0%o\n",
                        i, linux_path, top_mode & 07777);
            } else {
                struct ish_stat prev;
                if (inode_read_stat_if_exist(fs, ino, &prev)) {
                    prev.mode = top_mode;
                    inode_write_stat(fs, ino, &prev);
                }
                fprintf(stderr, "fakefs_bind_mount: slot[%d] meta.db entry refreshed for \"%s\" (inode=%lld) mode=0%o\n",
                        i, linux_path, (long long)ino, top_mode & 07777);
            }
            db_commit(fs);

            return 0;
        }
    }

    fprintf(stderr, "fakefs_bind_mount: FAIL no free slots (max=%d)\n", FAKEFS_MAX_BIND_MOUNTS);
    return _ENOMEM; /* no free slots */
}

int fakefs_bind_unmount(const char *linux_path) {
    if (g_fakefs_mount == NULL)
        return _ENODEV;

    for (int i = 0; i < FAKEFS_MAX_BIND_MOUNTS; i++) {
        if (g_bind_mounts[i].active &&
            strcmp(g_bind_mounts[i].path, linux_path) == 0) {
            g_bind_mounts[i].active = false;

            /* Remove the symlink */
            char host_link[PATH_MAX];
            snprintf(host_link, sizeof(host_link), "%s", fix_path(linux_path));
            unlinkat(g_fakefs_mount->root_fd, host_link, 0);

            return 0;
        }
    }

    return _ENOENT;
}

static int fakefs_mount(struct mount *mount) {
    char db_path[PATH_MAX];
    strcpy(db_path, mount->source);
    char *basename = strrchr(db_path, '/') + 1;
    assert(strcmp(basename, "data") == 0);
    strcpy(basename, "meta.db");

    // do this now so rebuilding can use root_fd
    int err = realfs.mount(mount);
    if (err < 0)
        return err;

    err = fake_db_init(&mount->fakefs, db_path, mount->root_fd);
    if (err < 0)
        return err;

    /* Store global reference for bind mount API */
    g_fakefs_mount = mount;
    memset(g_bind_mounts, 0, sizeof(g_bind_mounts));

    return 0;
}

static int fakefs_umount(struct mount *mount) {
    int err = fake_db_deinit(&mount->fakefs);
    if (err != SQLITE_OK) {
        printk("sqlite failed to close: %d\n", err);
    }
    /* return realfs.umount(mount); */
    return 0;
}

static void fakefs_inode_orphaned(struct mount *mount, ino_t inode) {
    struct fakefs_db *fs = &mount->fakefs;
    db_begin_write(fs);
    sqlite3_bind_int64(fs->stmt.try_cleanup_inode, 1, inode);
    db_exec_reset(fs, fs->stmt.try_cleanup_inode);
    db_commit(fs);
}

const struct fs_ops fakefs = {
    .name = "fake", .magic = 0x66616b65,
    .mount = fakefs_mount,
    .umount = fakefs_umount,
    .statfs = realfs_statfs,
    .open = fakefs_open,
    .readlink = fakefs_readlink,
    .link = fakefs_link,
    .unlink = fakefs_unlink,
    .rename = fakefs_rename,
    .symlink = fakefs_symlink,
    .mknod = fakefs_mknod,

    .close = realfs_close,
    .stat = fakefs_stat,
    .fstat = fakefs_fstat,
    .flock = realfs_flock,
    .setattr = fakefs_setattr,
    .fsetattr = fakefs_fsetattr,
    .getpath = realfs_getpath,
    .utime = realfs_utime,

    .mkdir = fakefs_mkdir,
    .rmdir = fakefs_rmdir,

    .inode_orphaned = fakefs_inode_orphaned,
};

bool fakefs_bind_mount_translate_path(const char *path, char *out_path, size_t out_size) {
    return bind_mount_translate_path(path, out_path, out_size);
}

/* ===== Bind-mount file change notifications =====
 *
 * Lock-protected ring buffer collects (linux_path, op, timestamp) events
 * from realfs hot paths. A dispatch_source coalesces wakeups for a serial
 * consumer queue; the registered handler drains the ring in batches.
 *
 * Ring full → drop oldest event and bump g_dropped_count (observable via
 * fakefs_change_dropped_count()). The producer side is bounded:
 *   lock + memcpy(linux_path, ~64B avg) + atomic_store + merge_data
 * which on Apple Silicon measures around 150–250ns.
 *
 * Implementation is Apple-only: it relies on GCD (dispatch sources),
 * Clang blocks (Block_copy / Block_release), os_unfair_lock and mach
 * time. On non-Apple builds (Linux / x86 iSH), the public API still
 * resolves at link time but every entry point is a no-op so the host
 * never observes events. This matches the upstream iSH stance of
 * keeping fs/fake.c portable while letting Apple-side ports layer
 * platform features on top.
 */

#if defined(__APPLE__)

#define FAKEFS_CHANGE_RING_SIZE 1024  /* must be power of two */

static struct fakefs_change_event g_change_ring[FAKEFS_CHANGE_RING_SIZE];
static _Atomic uint64_t g_change_head = 0;  /* next write slot */
static _Atomic uint64_t g_change_tail = 0;  /* next read slot */
static os_unfair_lock g_change_ring_lock = OS_UNFAIR_LOCK_INIT;
static _Atomic uint64_t g_change_dropped = 0;

static dispatch_queue_t g_change_consumer_queue = NULL;
static dispatch_source_t g_change_consumer_source = NULL;
static fakefs_change_handler_t g_change_handler = NULL;

/* Drain up to `max` events from the ring into `out`. Returns count drained.
 * Called only on the consumer queue. */
static int fakefs_change_drain(struct fakefs_change_event *out, int max) {
    int n = 0;
    os_unfair_lock_lock(&g_change_ring_lock);
    uint64_t head = atomic_load_explicit(&g_change_head, memory_order_acquire);
    uint64_t tail = atomic_load_explicit(&g_change_tail, memory_order_relaxed);
    while (tail < head && n < max) {
        out[n++] = g_change_ring[tail & (FAKEFS_CHANGE_RING_SIZE - 1)];
        tail++;
    }
    atomic_store_explicit(&g_change_tail, tail, memory_order_release);
    os_unfair_lock_unlock(&g_change_ring_lock);
    return n;
}

void fakefs_record_change(const char *linux_path, int op) {
    if (g_change_consumer_source == NULL || linux_path == NULL)
        return;

    os_unfair_lock_lock(&g_change_ring_lock);
    uint64_t head = atomic_load_explicit(&g_change_head, memory_order_relaxed);
    uint64_t tail = atomic_load_explicit(&g_change_tail, memory_order_acquire);
    if (head - tail >= FAKEFS_CHANGE_RING_SIZE) {
        /* Full — drop oldest. */
        atomic_store_explicit(&g_change_tail, tail + 1, memory_order_release);
        atomic_fetch_add_explicit(&g_change_dropped, 1, memory_order_relaxed);
    }
    struct fakefs_change_event *slot = &g_change_ring[head & (FAKEFS_CHANGE_RING_SIZE - 1)];
    strlcpy(slot->linux_path, linux_path, sizeof(slot->linux_path));
    slot->op = op;
    slot->timestamp_ns = (int64_t)mach_absolute_time();
    slot->fs_context = (current != NULL && current->group != NULL)
        ? current->group->fs_context : 0;
    atomic_store_explicit(&g_change_head, head + 1, memory_order_release);
    os_unfair_lock_unlock(&g_change_ring_lock);

    dispatch_source_merge_data(g_change_consumer_source, 1);
}

void fakefs_install_change_consumer(fakefs_change_handler_t handler) {
    if (handler == NULL)
        return;
    if (g_change_consumer_source != NULL) {
        /* Replace handler — supported but unusual. */
        if (g_change_handler) Block_release(g_change_handler);
        g_change_handler = Block_copy(handler);
        return;
    }

    g_change_handler = Block_copy(handler);
    g_change_consumer_queue = dispatch_queue_create(
        "com.openminis.ish.fakechange", DISPATCH_QUEUE_SERIAL);
    g_change_consumer_source = dispatch_source_create(
        DISPATCH_SOURCE_TYPE_DATA_ADD, 0, 0, g_change_consumer_queue);

    dispatch_source_set_event_handler(g_change_consumer_source, ^{
        struct fakefs_change_event batch[64];
        int n;
        while ((n = fakefs_change_drain(batch, 64)) > 0) {
            if (g_change_handler) g_change_handler(batch, n);
            if (n < 64) break;  /* drained everything */
        }
    });
    dispatch_resume(g_change_consumer_source);
}

uint64_t fakefs_change_dropped_count(void) {
    return atomic_load_explicit(&g_change_dropped, memory_order_relaxed);
}

#else /* !__APPLE__ — non-Apple stubs.
       *
       * The host (Swift / ObjC) bridge that consumes these events only
       * exists on Apple. On Linux / x86 iSH builds we still need the
       * three symbols to resolve at link time (callers in fs/fake.c and
       * fs/real.c invoke them unconditionally), but they must do
       * nothing — no ring buffer, no consumer queue, no host wakeups. */

void fakefs_record_change(const char *linux_path, int op) {
    (void)linux_path; (void)op;
}

void fakefs_install_change_consumer(fakefs_change_handler_t handler) {
    (void)handler;
}

uint64_t fakefs_change_dropped_count(void) {
    return 0;
}

#endif /* __APPLE__ */
