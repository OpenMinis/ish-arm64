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
    char    linux_path[1024];
    int     op;
    int64_t timestamp_ns; /* mach_absolute_time() raw ticks */
};

/* Producer — called from realfs write/unlink/rename/truncate paths.
 * Cheap: lock + memcpy + dispatch_source_merge_data. Safe to call from
 * any thread. No-op if no consumer is installed. */
void fakefs_record_change(const char *linux_path, int op);

/* Consumer registration — invoked once at iSH boot from ObjC.
 * `handler` runs on a dedicated serial queue. `batch` and `count` are
 * stack-owned for the duration of the call; copy if you need to retain.
 * Replacing an existing consumer is allowed but not expected. */
typedef void (^fakefs_change_handler_t)(const struct fakefs_change_event *batch, int count);
void fakefs_install_change_consumer(fakefs_change_handler_t handler);

/* Diagnostic — number of events dropped due to ring overflow since boot. */
uint64_t fakefs_change_dropped_count(void);

#endif
