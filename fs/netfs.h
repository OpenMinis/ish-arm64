#ifndef FS_NETFS_H
#define FS_NETFS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// ============================================================================
// netfs — a read-only filesystem served by host callbacks
// ============================================================================
//
// iSH knows nothing about networks, protocols or caches. The host (the app
// embedding iSH) registers a table of callbacks; netfs turns guest syscalls
// into those calls and back:
//   - paths: the mount point is stripped, the host gets "" for the mount root
//     and "/a/b" below it;
//   - inodes: st_ino is a 64-bit hash of that path, st_dev one value per mount;
//   - permissions are forced: directories 0555, files 0444 — never an x bit,
//     so nothing on a netfs mount can be executed (and mmap is not offered);
//   - every write-type operation returns EROFS;
//   - status: /proc/mounts lists netfs mounts, /proc/fs/netfs/<name> shows
//     the host's status text, netfs_log_transition() writes a dmesg line.
//
// It follows the shape of native offload's in-process handlers
// (kernel/native_offload.h): the callbacks run SYNCHRONOUSLY on the guest
// thread that made the syscall and may block, and a separate cancel callback
// is called from the signalling thread when that thread gets a fatal signal.
//
// Without registered callbacks netfs_mount() fails with ENODEV and nothing
// else changes; a user without network mounts pays nothing.
//
// --- Contract for the host ---
//   Threads:   stat/readdir/read run on guest threads, concurrently, for any
//              mount. They may block but MUST return by their own deadline;
//              iSH runs no timer of its own. No iSH lock is held during a call.
//   Cancel:    while a call runs, a fatal signal for that thread (SIGKILL, or
//              an unblocked signal whose action is the default "terminate")
//              makes iSH call cancel(call) on the SIGNALLING thread. The call
//              should then return EINTR soon. cancel must not block, must not
//              call back into iSH, and must ignore a call that already ended
//              or that it does not know. The signal is still delivered.
//   Signals:   SIGUSR1 (iSH's "interrupt a host syscall" signal) is blocked
//              on the calling thread for the duration of a call, so a host
//              running on a small or foreign stack (cgo) never takes it.
//   Memory:    path and buf belong to iSH and are valid only during the call;
//              after returning — also after a cancel — the host must not
//              touch buf. readdir's array is allocated by the host and handed
//              back through free_dirents. source/options are valid during
//              attach only.
//   Results:   0, or a POSITIVE LINUX errno (ENOENT, ETIMEDOUT, ...) — not the
//              host OS numbering.
//   status:    for /proc reads; must not block (serve a snapshot).
//
// ABI: abi must be NETFS_HOST_ABI and size sizeof(struct netfs_host_ops) or
// larger; fields are only ever appended.
// ============================================================================

#define NETFS_HOST_ABI 1

struct fs_ops;

struct netfs_host_attr {
    uint64_t size;
    int64_t mtime_sec;
    int32_t mtime_nsec;
    bool is_dir;
};

struct netfs_host_dirent {
    char name[256];
    struct netfs_host_attr attr;
};

struct netfs_host_ops {
    uint32_t abi;
    uint32_t size;
    // Prepare the remote named by source with options; MUST NOT touch the
    // network. *out_handle identifies it in every later call.
    int (*attach)(const char *source, const char *options, uint64_t *out_handle);
    void (*detach)(uint64_t handle);
    int (*stat)(uint64_t handle, const char *path, uint64_t call, uint64_t budget_key,
                struct netfs_host_attr *out);
    int (*readdir)(uint64_t handle, const char *path, uint64_t call, uint64_t budget_key,
                   struct netfs_host_dirent **out, size_t *out_count);
    void (*free_dirents)(struct netfs_host_dirent *entries);
    int (*read)(uint64_t handle, const char *path, uint64_t offset, void *buf, size_t len,
                uint64_t call, uint64_t budget_key, size_t *out_n);
    void (*cancel)(uint64_t call);
    int (*status)(uint64_t handle, char *buf, size_t size);
};

// Register the callbacks (copied). Returns 0, or _EINVAL on an ABI mismatch.
// Passing NULL unregisters; existing mounts then fail with ENOTCONN.
int netfs_set_host_ops(const struct netfs_host_ops *ops);

// Mount / unmount at runtime (takes mounts_lock). point is the guest path,
// e.g. "/var/minis/mounts/nas"; its directory must already exist in the
// filesystem above so it shows up in listings. source is shown in
// /proc/mounts (no credentials!), options in its option column.
int netfs_mount(const char *point, const char *source, const char *options);
int netfs_umount(const char *point);

// Write one line to the kernel log (dmesg), prefixed "netfs: ". The host
// calls this on state changes only. Safe from any thread.
void netfs_log_transition(const char *line);

struct task;
// For kernel/signal.c: called when `task` gets a fatal signal while it is in
// a netfs host call.
void netfs_cancel_task_call(struct task *task);

extern const struct fs_ops netfs;

#endif
