#ifndef ISH_INTERNAL
#error "for internal use only"
#endif

#ifndef FS_FAKE_H
#define FS_FAKE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "kernel/fs.h"
#include "fs/fake-db.h"
#include "misc.h"

struct fd *fakefs_open_inode(struct mount *mount, ino_t inode);

/* Bind mount API — redirect a fakefs path to an external host directory.
 * Creates a symlink in data/ and auto-creates meta.db entries on access.
 * Can be called from Swift/ObjC after the kernel has booted.
 *
 * `read_only`: when true, the top-level mount directory's meta.db entry is
 * written with mode 0555 (dr-xr-xr-x) instead of 0755, so iSH's generic
 * N_PARENT_DIR_WRITE check rejects any create/unlink/rename/rmdir with
 * EACCES at the normal access-check layer — no special fakefs plumbing
 * required. Existing writable mounts should pass `false`. */
int fakefs_bind_mount(const char *linux_path, const char *host_path, bool read_only);
int fakefs_bind_unmount(const char *linux_path);

/* Translate a resolved host path back to a Linux path via bind mount table.
 * Returns true and writes to out_path if a match was found. */
bool fakefs_bind_mount_resolve_path(const char *resolved, char *out_path, size_t out_size);

/* Translate a Linux (guest) path to its resolved host path via bind mount
 * table. Returns true and writes to out_path on a match. Used by native
 * offload handlers that execute on the host but receive guest paths. */
bool fakefs_bind_mount_translate_path(const char *path, char *out_path, size_t out_size);

/* ===== Bind-mount file change notifications =====
 *
 * realfs write/unlink/rename/truncate hooks call fakefs_record_change()
 * with the resolved Linux path (e.g. "/var/minis/workspace/foo.txt") when
 * the path resolves under an active bind mount. Events are appended to a
 * lock-protected ring buffer; a dispatch source is signaled. The hot path
 * is non-blocking and bounded (~200ns) — buffer overflow drops the oldest
 * event.
 *
 * Consumers register via fakefs_install_change_consumer(). The handler is
 * invoked on the consumer's serial dispatch queue with a batch of events;
 * dispatch_source coalesces multiple producer signals into one wakeup.
 */

/* Op codes — keep stable, ObjC bridge mirrors these. */
#define FAKEFS_CHANGE_OP_WRITE     0
#define FAKEFS_CHANGE_OP_UNLINK    1
#define FAKEFS_CHANGE_OP_RENAME    2  /* destination of rename */
#define FAKEFS_CHANGE_OP_TRUNCATE  3

struct fakefs_change_event {
    char     linux_path[1024];
    int      op;
    int64_t  timestamp_ns; /* mach_absolute_time() raw ticks */
    uint64_t fs_context;   /* current->group->fs_context at record time; 0 if none */
};

/* Producer — called from realfs write/unlink/rename/truncate paths.
 * Cheap: lock + memcpy + dispatch_source_merge_data. Safe to call from
 * any thread. No-op if no consumer is installed. */
void fakefs_record_change(const char *linux_path, int op);

/* Consumer registration — invoked once at iSH boot from ObjC on Apple
 * builds. `handler` runs on a dedicated serial queue. `batch` and
 * `count` are stack-owned for the duration of the call; copy if you
 * need to retain. Replacing an existing consumer is allowed but not
 * expected.
 *
 * Block syntax `^` is Clang-only. On non-Apple builds (Linux/GCC) we
 * fall back to a plain function-pointer typedef so the header still
 * compiles — the matching .c stub ignores its argument either way. */
#if defined(__APPLE__)
typedef void (^fakefs_change_handler_t)(const struct fakefs_change_event *batch, int count);
#else
typedef void (*fakefs_change_handler_t)(const struct fakefs_change_event *batch, int count);
#endif
void fakefs_install_change_consumer(fakefs_change_handler_t handler);

/* Diagnostic — number of events dropped due to ring overflow since boot. */
uint64_t fakefs_change_dropped_count(void);

/* ===== Path translate hook =====
 *
 * Optional host-provided override for guest→host path translation. When set,
 * fakefs consults the hook before the global g_bind_mounts[] table for every
 * path operation (open / stat / readdir / unlink / rename / rmdir / ...).
 * The hook receives the calling thread group's opaque fs_context value, so
 * the host can route the same guest path to different host directories per
 * context. iSH itself assigns no meaning to fs_context.
 *
 * Contract:
 *   - `guest_path` has leading slash, already normalized.
 *   - `fs_context` is current->group->fs_context, opaque to iSH.
 *   - Return true and write up to `out_size-1` bytes + NUL into `out_host_path`
 *     to override translation.
 *   - Return false to fall through to the global g_bind_mounts[] table.
 *   - Must be reentrant and non-blocking (called on hot fs path).
 */
typedef bool (*fakefs_path_translate_hook_t)(
    const char *guest_path,
    uint64_t fs_context,
    char *out_host_path,
    size_t out_size);

void fakefs_set_path_translate_hook(fakefs_path_translate_hook_t hook);

/* Optional reverse counterpart to the forward path-translate hook. Called
 * from realfs_getpath() to map a host APFS path (as returned by F_GETPATH
 * on a bind-mounted fd) back to its guest path. iSH consults this hook
 * after the static g_bind_mounts[] table; return true and write the guest
 * path into `out_guest_path` to override. The hook does not receive an
 * fs_context — F_GETPATH is per-fd, and the host path itself uniquely
 * identifies which bind-mount target was resolved.
 *
 * Same hot-path contract as the forward hook: must be reentrant and non-
 * blocking.
 */
typedef bool (*fakefs_path_reverse_hook_t)(
    const char *host_path,
    char *out_guest_path,
    size_t out_size);

void fakefs_set_path_reverse_hook(fakefs_path_reverse_hook_t hook);

/* ===== Delete guard =====
 *
 * [FEAT-261116-11] Optional host-provided check that runs before fakefs
 * removes something on a bind mount or a hook-routed path: unlink, rmdir, and
 * a rename whose destination exists (the destination is what is lost). The
 * hook may BLOCK - the host asks the user - so, unlike the translate hooks:
 *
 *   - it is called on the guest task's own thread, before any host change and
 *     before any meta.db change, with no fakefs lock and no pids_lock held;
 *     a refusal therefore leaves host and meta.db exactly as they were;
 *   - while it waits it must poll fakefs_guard_should_abort() (a few times a
 *     second) and return FAKEFS_GUARD_INTERRUPTED once that is true, or the
 *     task cannot be killed;
 *   - it must not change anything on the host itself.
 *
 * Paths of the plain rootfs (neither bind nor hook-routed) never reach it.
 * With no hook set, or with the scope bit of the path's kind cleared, the
 * delete path is unchanged: one atomic load and nothing else.
 *
 * Return value: FAKEFS_GUARD_PROCEED, FAKEFS_GUARD_PROCEED_WAITED (the hook
 * blocked first: fakefs translates the path again and fails the call with
 * EIO if it no longer leads to the same host path, instead of acting on the
 * new place), FAKEFS_GUARD_DENY (the call fails with EPERM),
 * FAKEFS_GUARD_INTERRUPTED (EINTR). Any other value is treated as DENY.
 */
#define FAKEFS_GUARD_OP_UNLINK  1
#define FAKEFS_GUARD_OP_RMDIR   2
#define FAKEFS_GUARD_OP_REPLACE 3   /* rename onto an existing destination */

#define FAKEFS_GUARD_PROCEED        0
#define FAKEFS_GUARD_PROCEED_WAITED 1
#define FAKEFS_GUARD_DENY          (-1)
#define FAKEFS_GUARD_INTERRUPTED   (-2)

/* Which kinds of path are sent to the hook. */
#define FAKEFS_GUARD_SCOPE_BIND   1u  /* static bind mounts (g_bind_mounts[]) */
#define FAKEFS_GUARD_SCOPE_ROUTED 2u  /* paths routed by the translate hook */

struct fakefs_guard_request {
    int op;                  /* FAKEFS_GUARD_OP_* */
    const char *guest_path;  /* what is removed, with a leading slash */
    const char *host_path;   /* its host path, exactly what fakefs will act on */
    const char *host_src;    /* REPLACE: host path of the rename's source, else NULL */
    bool is_dir;             /* REPLACE: the destination is a directory */
    bool routed;             /* translated by the path-translate hook */
    uint64_t fs_context;     /* current->group->fs_context */
    int pid;                 /* calling task */
    int tgid;                /* its process */
};

typedef int (*fakefs_guard_hook_t)(const struct fakefs_guard_request *request);

/* NULL removes the hook. */
void fakefs_set_guard_hook(fakefs_guard_hook_t hook);
/* Bit set of FAKEFS_GUARD_SCOPE_*; 0 (the default) sends nothing. */
void fakefs_set_guard_scope(unsigned scope);
unsigned fakefs_guard_scope(void);

/* For the hook, on the thread it was called on: true once the calling task
 * has a fatal signal pending or is being torn down - stop waiting. */
bool fakefs_guard_should_abort(void);

/* For the hook, on the thread it was called on: the process ids from the
 * caller's process up through its ancestors (caller first), at most `max`.
 * Returns how many were written. Takes pids_lock briefly; never blocks. */
int fakefs_guard_ancestry(int *tgids, int max);

/* How many times the hook has been called since boot (diagnostics, tests). */
uint64_t fakefs_guard_call_count(void);

/* Register the absolute host path of the fakefs `data/` directory (the
 * host filesystem location backing fakefs `root_fd`). Used by
 * fakefs_bind_mount_resolve_path() to suppress "self-containing" bind
 * mounts — e.g. on a desktop OS the user can register a bind mount
 * whose host path is an ancestor of the rootfs data directory itself
 * (mounting the user's home directory, when the rootfs lives somewhere
 * underneath it). Without this guard every rootfs-internal fd would be
 * reverse-mapped to a guest path under that mount, breaking execve
 * (EACCES) and fakefs metadata lookups.
 *
 * Pass an absolute canonical path (no symlinks, no trailing slash).
 * Pass NULL or "" to clear. Stored atomically — readers see a value
 * but not necessarily the very latest write.
 */
void fakefs_set_rootfs_data_path(const char *abs_host_path);

#endif
