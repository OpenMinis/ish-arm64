// netfs — read-only filesystem served by host callbacks. See fs/netfs.h for
// the contract; the design is docs/internal/network-mount-design.md §3-A in
// the Minis app repository.
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "kernel/calls.h"
#include "kernel/errno.h"
#include "kernel/fs.h"
#include "kernel/task.h"
#include "kernel/signal.h"
#include "fs/fd.h"
#include "fs/inode.h"
#include "fs/netfs.h"
#include "fs/proc.h"
#include "fs/stat.h"
#include "util/list.h"
#include "util/sync.h"
#include "misc.h"

#define NETFS_MAGIC 0x6e657466 // "netf"

// MARK: - Host callbacks

static lock_t host_lock = LOCK_INITIALIZER;
static struct netfs_host_ops host;
static bool host_set;

int netfs_set_host_ops(const struct netfs_host_ops *ops) {
    if (ops != NULL && (ops->abi != NETFS_HOST_ABI || ops->size < sizeof(struct netfs_host_ops)))
        return _EINVAL;
    lock(&host_lock);
    if (ops == NULL) {
        host_set = false;
    } else {
        host = *ops;
        host_set = true;
    }
    unlock(&host_lock);
    return 0;
}

// A copy of the table, or false when none is registered.
static bool host_get(struct netfs_host_ops *out) {
    lock(&host_lock);
    bool ok = host_set;
    if (ok)
        *out = host;
    unlock(&host_lock);
    return ok;
}

// MARK: - Mount registry (for /proc/fs/netfs)

struct netfs_mount {
    uint64_t handle;
    uint64_t dev;
    char name[MAX_NAME];
    struct list link;
};

static lock_t registry_lock = LOCK_INITIALIZER;
static struct list registry = LIST_INITIALIZER(registry);
static uint64_t next_dev = 1;

static struct netfs_mount *mount_data(struct mount *mount) {
    return (struct netfs_mount *) mount->data;
}

// MARK: - Calls into the host

static uint64_t next_call_id = 1;

struct host_call {
    uint64_t id;
    sigset_t old_mask;
};

// Returns _EINTR (and sets nothing up) when the thread already has a fatal
// signal pending: one cancelled call must not be followed by a fresh,
// uncancelled one. path_normalize ignores errors of intermediate components,
// so `cat a/b/c` would otherwise go on to the next lookup after its first
// call was cancelled and block a full host deadline again.
static int call_begin(struct host_call *c) {
    c->id = __atomic_add_fetch(&next_call_id, 1, __ATOMIC_RELAXED);
    // SIGUSR1 is how iSH interrupts a thread's host syscall. A host call may
    // be running on a foreign stack (cgo), and iSH's handler has no
    // SA_ONSTACK, so keep it pending until the call is over. Cancellation
    // goes through host.cancel instead.
    sigset_t usr1;
    sigemptyset(&usr1);
    sigaddset(&usr1, SIGUSR1);
    pthread_sigmask(SIG_BLOCK, &usr1, &c->old_mask);
    if (current == NULL)
        return 0;
    // Publish first, then look: see task_fatal_signal_pending.
    atomic_store_explicit(&current->netfs_call, c->id, memory_order_seq_cst);
    if (task_fatal_signal_pending(current)) {
        atomic_store_explicit(&current->netfs_call, 0, memory_order_release);
        pthread_sigmask(SIG_SETMASK, &c->old_mask, NULL);
        return _EINTR;
    }
    return 0;
}

static void call_end(struct host_call *c) {
    if (current != NULL)
        atomic_store_explicit(&current->netfs_call, 0, memory_order_release);
    pthread_sigmask(SIG_SETMASK, &c->old_mask, NULL);
}

void netfs_cancel_task_call(struct task *task) {
    uint64_t id = atomic_load_explicit(&task->netfs_call, memory_order_acquire);
    if (id == 0)
        return;
    struct netfs_host_ops ops;
    if (host_get(&ops) && ops.cancel != NULL)
        ops.cancel(id);
}

static uint64_t budget_key(void) {
    if (current == NULL || current->group == NULL)
        return 0;
    return current->group->fs_context;
}

// Host errno (positive, Linux numbering) → iSH's negative form.
static int from_host(int err) {
    return err > 0 ? -err : err;
}

// MARK: - Attributes

// FNV-1a over the path inside the mount: unique and stable within a mount,
// which is all the inode table needs (fs/inode.c keys on mount + ino).
static ino_t path_ino(const char *path) {
    if (path[0] == '\0')
        return 1;
    uint64_t h = 0xcbf29ce484222325ULL;
    for (const unsigned char *p = (const unsigned char *) path; *p; p++) {
        h ^= *p;
        h *= 0x100000001b3ULL;
    }
    return h <= 1 ? h + 2 : h;
}

static void fill_stat(struct mount *mount, const char *path, const struct netfs_host_attr *attr,
                      struct statbuf *stat) {
    memset(stat, 0, sizeof(*stat));
    stat->dev = mount_data(mount)->dev;
    stat->inode = path_ino(path);
    // Forced, whatever the server says: nothing here is ever executable.
    stat->mode = attr->is_dir ? (S_IFDIR | 0555) : (S_IFREG | 0444);
    stat->nlink = attr->is_dir ? 2 : 1;
    stat->size = attr->is_dir ? 0 : attr->size;
    stat->blksize = 4096;
    stat->blocks = (stat->size + 511) / 512;
    stat->mtime = stat->ctime = stat->atime = (dword_t) attr->mtime_sec;
    stat->mtime_nsec = stat->ctime_nsec = stat->atime_nsec = (dword_t) attr->mtime_nsec;
}

static int host_stat(struct mount *mount, const char *path, struct netfs_host_attr *attr) {
    if (path[0] == '\0') {
        // The mount root is answered locally: `ls -l /var/minis/mounts`
        // must never wait on a server.
        memset(attr, 0, sizeof(*attr));
        attr->is_dir = true;
        return 0;
    }
    struct netfs_host_ops ops;
    if (!host_get(&ops))
        return _ENOTCONN;
    struct host_call c;
    int err = call_begin(&c);
    if (err < 0)
        return err;
    err = ops.stat(mount_data(mount)->handle, path, c.id, budget_key(), attr);
    call_end(&c);
    return from_host(err);
}

// MARK: - fs_ops

static int netfs_fs_mount(struct mount *mount) {
    struct netfs_host_ops ops;
    if (!host_get(&ops))
        return _ENODEV;
    struct netfs_mount *nm = calloc(1, sizeof(*nm));
    if (nm == NULL)
        return _ENOMEM;
    int err = from_host(ops.attach(mount->source, mount->info, &nm->handle));
    if (err < 0) {
        free(nm);
        return err;
    }
    const char *slash = strrchr(mount->point, '/');
    strncpy(nm->name, slash ? slash + 1 : mount->point, sizeof(nm->name) - 1);
    lock(&registry_lock);
    nm->dev = 0x6e657400 + next_dev++;
    list_add_tail(&registry, &nm->link);
    unlock(&registry_lock);
    mount->data = nm;
    return 0;
}

static int netfs_fs_umount(struct mount *mount) {
    struct netfs_mount *nm = mount_data(mount);
    if (nm == NULL)
        return 0;
    lock(&registry_lock);
    list_remove(&nm->link);
    unlock(&registry_lock);
    struct netfs_host_ops ops;
    if (host_get(&ops) && ops.detach != NULL)
        ops.detach(nm->handle);
    free(nm);
    mount->data = NULL;
    return 0;
}

static int netfs_fs_statfs(struct mount *UNUSED(mount), struct statfsbuf *stat) {
    memset(stat, 0, sizeof(*stat));
    stat->type = NETFS_MAGIC;
    stat->bsize = 4096;
    stat->namelen = 255;
    stat->frsize = 4096;
    return 0;
}

struct netfs_file {
    char path[MAX_PATH];
    struct netfs_host_attr attr;
    // directories: the listing, taken once at open
    struct netfs_host_dirent *entries;
    size_t count;
    size_t pos;
};

extern const struct fd_ops netfs_fdops;

static struct fd *netfs_fs_open(struct mount *mount, const char *path, int flags, int UNUSED(mode)) {
    if ((flags & O_ACCMODE_) != O_RDONLY_ || (flags & (O_CREAT_ | O_TRUNC_)))
        return ERR_PTR(_EROFS);
    struct netfs_file *file = calloc(1, sizeof(*file));
    if (file == NULL)
        return ERR_PTR(_ENOMEM);
    strncpy(file->path, path, sizeof(file->path) - 1);
    int err = host_stat(mount, path, &file->attr);
    if (err < 0)
        goto fail;
    if (file->attr.is_dir) {
        struct netfs_host_ops ops;
        if (!host_get(&ops)) {
            err = _ENOTCONN;
            goto fail;
        }
        struct host_call c;
        err = call_begin(&c);
        if (err < 0)
            goto fail;
        err = from_host(ops.readdir(mount_data(mount)->handle, path, c.id, budget_key(),
                                    &file->entries, &file->count));
        call_end(&c);
        if (err < 0)
            goto fail;
    }
    struct fd *fd = fd_create(&netfs_fdops);
    if (fd == NULL) {
        err = _ENOMEM;
        goto fail;
    }
    fd->fs_data = file;
    // Take the inode reference here so generic_openat does not hold the
    // global inodes_lock across our fstat (see fs/generic.c).
    fd->inode = inode_get(mount, path_ino(path));
    if (fd->inode == NULL) {
        fd_close(fd);
        return ERR_PTR(_ENOMEM);
    }
    return fd;
fail:
    if (file->entries != NULL) {
        struct netfs_host_ops ops;
        if (host_get(&ops))
            ops.free_dirents(file->entries);
    }
    free(file);
    return ERR_PTR(err);
}

static int netfs_fs_stat(struct mount *mount, const char *path, struct statbuf *stat) {
    struct netfs_host_attr attr;
    int err = host_stat(mount, path, &attr);
    if (err < 0)
        return err;
    fill_stat(mount, path, &attr, stat);
    return 0;
}

static int netfs_fs_fstat(struct fd *fd, struct statbuf *stat) {
    struct netfs_file *file = fd->fs_data;
    fill_stat(fd->mount, file->path, &file->attr, stat);
    return 0;
}

static int netfs_fs_getpath(struct fd *fd, char *buf) {
    struct netfs_file *file = fd->fs_data;
    strcpy(buf, file->path);
    return 0;
}

static int netfs_rofs_path(struct mount *UNUSED(m), const char *UNUSED(p)) { return _EROFS; }
static int netfs_rofs_2path(struct mount *UNUSED(m), const char *UNUSED(a), const char *UNUSED(b)) { return _EROFS; }
static int netfs_rofs_mkdir(struct mount *UNUSED(m), const char *UNUSED(p), mode_t_ UNUSED(mode)) { return _EROFS; }
static int netfs_rofs_mknod(struct mount *UNUSED(m), const char *UNUSED(p), mode_t_ UNUSED(mode), dev_t_ UNUSED(dev)) { return _EROFS; }
static int netfs_rofs_setattr(struct mount *UNUSED(m), const char *UNUSED(p), struct attr UNUSED(a)) { return _EROFS; }
static int netfs_rofs_fsetattr(struct fd *UNUSED(fd), struct attr UNUSED(a)) { return _EROFS; }
static int netfs_rofs_utime(struct mount *UNUSED(m), const char *UNUSED(p), struct timespec UNUSED(a), struct timespec UNUSED(b)) { return _EROFS; }

const struct fs_ops netfs = {
    .name = "netfs", .magic = NETFS_MAGIC,
    .mount = netfs_fs_mount,
    .umount = netfs_fs_umount,
    .statfs = netfs_fs_statfs,
    .open = netfs_fs_open,
    // No readlink: there are no symlinks here, and path_normalize skips the
    // call per component when it is NULL.
    .link = netfs_rofs_2path,
    .unlink = netfs_rofs_path,
    .rmdir = netfs_rofs_path,
    .rename = netfs_rofs_2path,
    .symlink = netfs_rofs_2path,
    .mknod = netfs_rofs_mknod,
    .mkdir = netfs_rofs_mkdir,
    .stat = netfs_fs_stat,
    .fstat = netfs_fs_fstat,
    .setattr = netfs_rofs_setattr,
    .fsetattr = netfs_rofs_fsetattr,
    .utime = netfs_rofs_utime,
    .getpath = netfs_fs_getpath,
};

// MARK: - fd_ops

static ssize_t netfs_pread(struct fd *fd, void *buf, size_t size, off_t off) {
    struct netfs_file *file = fd->fs_data;
    if (file->attr.is_dir)
        return _EISDIR;
    if (off < 0)
        return _EINVAL;
    if ((uint64_t) off >= file->attr.size || size == 0)
        return 0;
    struct netfs_host_ops ops;
    if (!host_get(&ops))
        return _ENOTCONN;
    size_t n = 0;
    struct host_call c;
    int err = call_begin(&c);
    if (err < 0)
        return err;
    err = from_host(ops.read(mount_data(fd->mount)->handle, file->path, (uint64_t) off,
                                 buf, size, c.id, budget_key(), &n));
    call_end(&c);
    if (err < 0)
        return err;
    return (ssize_t) (n > size ? size : n);
}

static off_t_ netfs_lseek(struct fd *fd, off_t_ off, int whence) {
    struct netfs_file *file = fd->fs_data;
    if (file->attr.is_dir) {
        // getdents seeks back by one entry index (fs/dir.c)
        if (whence != LSEEK_SET || off < 0)
            return _EINVAL;
        file->pos = (size_t) off;
        return off;
    }
    int err = generic_seek(fd, off, whence, file->attr.size);
    if (err < 0)
        return err;
    return fd->offset;
}

static int netfs_readdir(struct fd *fd, struct dir_entry *entry) {
    struct netfs_file *file = fd->fs_data;
    if (!file->attr.is_dir)
        return _ENOTDIR;
    if (file->pos >= file->count)
        return 0;
    struct netfs_host_dirent *d = &file->entries[file->pos++];
    char child[MAX_PATH];
    snprintf(child, sizeof(child), "%s/%s", file->path, d->name);
    entry->inode = path_ino(child);
    strncpy(entry->name, d->name, sizeof(entry->name) - 1);
    entry->name[sizeof(entry->name) - 1] = '\0';
    return 1;
}

static unsigned long netfs_telldir(struct fd *fd) {
    struct netfs_file *file = fd->fs_data;
    return file->pos;
}

static void netfs_seekdir(struct fd *fd, unsigned long pos) {
    struct netfs_file *file = fd->fs_data;
    file->pos = pos;
}

static int netfs_close(struct fd *fd) {
    struct netfs_file *file = fd->fs_data;
    if (file == NULL)
        return 0;
    if (file->entries != NULL) {
        struct netfs_host_ops ops;
        if (host_get(&ops))
            ops.free_dirents(file->entries);
        else
            free(file->entries); // host gone: best effort
    }
    free(file);
    fd->fs_data = NULL;
    return 0;
}

const struct fd_ops netfs_fdops = {
    .pread = netfs_pread,
    .lseek = netfs_lseek,
    .readdir = netfs_readdir,
    .telldir = netfs_telldir,
    .seekdir = netfs_seekdir,
    .close = netfs_close,
    // No mmap: the kernel answers ENODEV, and exec refuses (no x bit).
};

// MARK: - Mounting from the host

int netfs_mount(const char *point, const char *source, const char *options) {
    lock(&mounts_lock);
    int err = do_mount(&netfs, source, point, options ? options : "", MS_READONLY_);
    unlock(&mounts_lock);
    return err;
}

int netfs_umount(const char *point) {
    lock(&mounts_lock);
    int err = do_umount(point);
    unlock(&mounts_lock);
    return err;
}

void netfs_log_transition(const char *line) {
    printk("netfs: %s\n", line);
}

// MARK: - /proc/fs/netfs/<name>

static void proc_netfs_getname(struct proc_entry *entry, char *buf) {
    strcpy(buf, entry->name);
}

static int proc_netfs_show(struct proc_entry *entry, struct proc_data *data) {
    struct netfs_host_ops ops;
    bool have = host_get(&ops);
    char text[2048];
    int err = _ENOENT;
    lock(&registry_lock);
    struct netfs_mount *nm;
    list_for_each_entry(&registry, nm, link) {
        if (strcmp(nm->name, entry->name) != 0)
            continue;
        if (!have || ops.status == NULL) {
            err = _ENOTCONN;
            break;
        }
        text[0] = '\0';
        err = from_host(ops.status(nm->handle, text, sizeof(text)));
        break;
    }
    unlock(&registry_lock);
    if (err < 0)
        return err;
    proc_buf_append(data, text, strnlen(text, sizeof(text)));
    return 0;
}

static struct proc_dir_entry proc_netfs_mount = {
    .getname = proc_netfs_getname,
    .show = proc_netfs_show,
};

static bool proc_netfs_readdir(struct proc_entry *UNUSED(entry), unsigned long *index,
                               struct proc_entry *next_entry) {
    char *name = NULL;
    unsigned long i = 0;
    lock(&registry_lock);
    struct netfs_mount *nm;
    list_for_each_entry(&registry, nm, link) {
        if (i++ == *index) {
            name = strdup(nm->name);
            break;
        }
    }
    unlock(&registry_lock);
    if (name == NULL)
        return false;
    next_entry->meta = &proc_netfs_mount;
    next_entry->name = name;
    (*index)++;
    return true;
}

struct proc_children proc_fs_children = PROC_CHILDREN({
    {"netfs", S_IFDIR, .readdir = proc_netfs_readdir},
});
