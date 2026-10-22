#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>
#include "kernel/calls.h"
#include "util/sync.h"
#include "util/timer.h"
#include "fs/path.h"

// === Path Normalize Cache ===
//
// [T-ish-pathcache-global] One process-wide cache of path_normalize results.
//
// It used to be `__thread`: 64 entries x ~8 KB, memset on first use in every
// host thread. Every guest process is its own host thread (task_thread), so
// under fork+exec the cache was born empty for each process, served ~0 hits
// over a lifetime of a handful of opens, and cost a 525 KB memset per fork
// on top. A Time Profiler capture of a 40-way fork storm (2026-09-19) put
// 85% of 261% CPU under path_normalize: every component of every path did a
// fakefs readlink (SQLite) and, for directories, a stat (SQLite + host
// fstatat) — for the same dozen paths, millions of times.
//
// Now one table shared by all threads. Correctness keeps the old TTL bound
// and ADDS explicit invalidation: every namespace mutation that can change
// what a path resolves to (link/unlink/rename/symlink/mknod/mkdir/rmdir,
// mount/umount) bumps `path_cache_gen`, and an entry stored under an older
// generation is ignored. The generation is snapshotted BEFORE the
// normalization whose result is stored, so a mutation racing the walk can
// only make the entry stale-and-rejected, never stale-and-served. The TTL
// remains the bound for changes the kernel cannot observe (host-side edits
// under bind mounts).
//
// Only successful normalizations are cached — errors never are — so a
// cached entry can only ever be wrong about what a *symlink* resolves to.

#define PATH_CACHE_SIZE 512
#define PATH_CACHE_TTL_NS 100000000  // 100ms TTL

struct path_cache_entry {
    char input_path[MAX_PATH];     // Original path (with at_path prefix if any)
    char normalized[MAX_PATH];     // Normalized result
    uint64_t timestamp;            // nanosecond timestamp
    uint64_t gen;                  // path_cache_gen snapshot the result belongs to
    int flags;                     // N_SYMLINK_FOLLOW or N_SYMLINK_NOFOLLOW
    bool valid;
};

// The critical sections are a strcmp and a strcpy (~100 ns) against a miss
// path measured in tens of microseconds, so one mutex does not convoy even
// under a 40-thread storm. The table lives in BSS: nothing is memset per
// thread any more, and the 4 MB is shared instead of 525 KB per thread.
static struct path_cache_entry path_cache[PATH_CACHE_SIZE];
static lock_t path_cache_lock = LOCK_INITIALIZER;
static _Atomic uint64_t path_cache_gen = 1;

// [T-ish-cpu-top] Counters for the host's [CPUTop] line. Misses are split by
// reason so a log line can say WHY the cache is not helping: `slot` = the
// path was never cached or its slot holds another path (capacity/collision),
// `gen` = a namespace mutation invalidated it, `ttl` = older than 100 ms,
// `flags` = cached under the other follow/nofollow mode.
static _Atomic uint64_t pc_hits, pc_miss_slot, pc_miss_gen, pc_miss_ttl, pc_miss_flags, pc_invals;

void path_cache_stats(uint64_t out[6]) {
    out[0] = atomic_load(&pc_hits);      out[1] = atomic_load(&pc_miss_slot);
    out[2] = atomic_load(&pc_miss_gen);  out[3] = atomic_load(&pc_miss_ttl);
    out[4] = atomic_load(&pc_miss_flags); out[5] = atomic_load(&pc_invals);
}

void path_cache_invalidate(void) {
    atomic_fetch_add_explicit(&path_cache_gen, 1, memory_order_release);
    atomic_fetch_add(&pc_invals, 1);
}

static inline uint64_t path_cache_snapshot(void) {
    return atomic_load_explicit(&path_cache_gen, memory_order_acquire);
}

// Simple hash function for path strings
static inline uint32_t path_hash(const char *str) {
    uint32_t hash = 5381;
    int c;
    while ((c = *str++))
        hash = ((hash << 5) + hash) + c; // hash * 33 + c
    return hash;
}

// Get current time in nanoseconds (a cache age: the coarse clock will do)
static inline uint64_t get_time_ns(void) {
    return coarse_now_ns();
}

// Try to get cached normalized path
// Returns 0 on cache hit, -1 on cache miss
static int path_cache_get(const char *full_path, int flags, char *out, uint64_t gen) {
    uint32_t index = path_hash(full_path) % PATH_CACHE_SIZE;
    struct path_cache_entry *entry = &path_cache[index];
    uint64_t now = get_time_ns();

    lock(&path_cache_lock);
    _Atomic uint64_t *why = NULL;
    if (!entry->valid || strcmp(entry->input_path, full_path) != 0) why = &pc_miss_slot;
    else if (entry->gen != gen)                                    why = &pc_miss_gen;
    else if (now - entry->timestamp > PATH_CACHE_TTL_NS)          why = &pc_miss_ttl;
    else if (entry->flags != flags)                                why = &pc_miss_flags;
    bool hit = (why == NULL);
    if (hit)
        strcpy(out, entry->normalized);
    unlock(&path_cache_lock);
    atomic_fetch_add(hit ? &pc_hits : why, 1);
    return hit ? 0 : -1;
}

// Store normalized path in cache
static void path_cache_set(const char *full_path, int flags, const char *normalized, uint64_t gen) {
    uint32_t index = path_hash(full_path) % PATH_CACHE_SIZE;
    struct path_cache_entry *entry = &path_cache[index];
    uint64_t now = get_time_ns();

    lock(&path_cache_lock);
    strncpy(entry->input_path, full_path, MAX_PATH - 1);
    entry->input_path[MAX_PATH - 1] = '\0';
    strncpy(entry->normalized, normalized, MAX_PATH - 1);
    entry->normalized[MAX_PATH - 1] = '\0';
    entry->flags = flags;
    entry->timestamp = now;
    entry->gen = gen;   // the snapshot taken before the walk, not now
    entry->valid = true;
    unlock(&path_cache_lock);
}

// === Component cache ===
//
// The cache above is keyed by whole paths, so a walk over many different files
// (find, grep -r, an import searching sys.path) never hits it, and then every
// component of every path costs a fakefs readlink (a meta.db transaction) and
// every directory on the way a stat on top (another one, and a host stat):
// some 16 transactions for one lstat deep in /usr/lib. This caches what the
// walk learns about each normalized prefix on fakefs: a symlink and its
// target, a directory and the mode and owner its search check needs, or
// something that is neither. Same rules as above: the namespace generation
// (bumped by every mutation that can change these, and chmod/chown) and the
// TTL for what happens behind the kernel's back (bind mounts). Failures are
// never cached.
#define DC_SIZE 4096
#define DC_KEY 240
#define DC_TARGET 200
enum { DC_EMPTY, DC_SYMLINK, DC_DIR, DC_FILE, DC_NOTLINK };   // NOTLINK: exists, type not looked at
struct dentry_cache_entry {
    uint64_t gen, stamp;
    uint32_t hash;
    uint16_t kind, target_len;
    dword_t mode, uid, gid;
    char key[DC_KEY];
    char target[DC_TARGET];
};
static struct dentry_cache_entry dentry_cache[DC_SIZE];
static lock_t dentry_cache_lock = LOCK_INITIALIZER;
static _Atomic uint64_t dc_hits, dc_misses;

void dentry_cache_stats(uint64_t out[2]) {
    out[0] = atomic_load(&dc_hits);
    out[1] = atomic_load(&dc_misses);
}

static bool dentry_cache_get(const char *key, uint64_t gen, struct dentry_cache_entry *out) {
    size_t len = strlen(key);
    if (len >= DC_KEY)
        return false;
    uint32_t h = path_hash(key);
    struct dentry_cache_entry *e = &dentry_cache[h % DC_SIZE];
    uint64_t now = get_time_ns();
    lock(&dentry_cache_lock);
    bool hit = e->kind != DC_EMPTY && e->hash == h && e->gen == gen && now - e->stamp <= PATH_CACHE_TTL_NS &&
               strcmp(e->key, key) == 0;
    if (hit) {
        out->kind = e->kind;
        out->mode = e->mode;
        out->uid = e->uid;
        out->gid = e->gid;
        out->target_len = e->target_len;
        memcpy(out->target, e->target, e->target_len);
    }
    unlock(&dentry_cache_lock);
    atomic_fetch_add_explicit(hit ? &dc_hits : &dc_misses, 1, memory_order_relaxed);
    return hit;
}

static void dentry_cache_set(const char *key, uint64_t gen, int kind, const struct statbuf *stat,
                             const char *target, size_t target_len) {
    size_t len = strlen(key);
    if (len >= DC_KEY || target_len > DC_TARGET)
        return;
    uint32_t h = path_hash(key);
    struct dentry_cache_entry *e = &dentry_cache[h % DC_SIZE];
    uint64_t now = get_time_ns();
    lock(&dentry_cache_lock);
    e->kind = kind;
    e->hash = h;
    e->gen = gen;
    e->stamp = now;
    memcpy(e->key, key, len + 1);
    e->mode = stat ? stat->mode : 0;
    e->uid = stat ? stat->uid : 0;
    e->gid = stat ? stat->gid : 0;
    e->target_len = (uint16_t) target_len;
    if (target_len)
        memcpy(e->target, target, target_len);
    unlock(&dentry_cache_lock);
}

static int __path_normalize(const char *at_path, const char *path, char *out, int flags, int levels) {
    // you must choose one
    if (flags & N_SYMLINK_FOLLOW)
        assert(!(flags & N_SYMLINK_NOFOLLOW));
    else
        assert(flags & N_SYMLINK_NOFOLLOW);

    const char *p = path;
    char *o = out;
    *o = '\0';
    int n = MAX_PATH - 1;

    if (strcmp(path, "") == 0)
        return _ENOENT;

    if (at_path != NULL && strcmp(at_path, "/") != 0) {
        // [T-ish-pathnorm-overflow] The base path must leave room for at least
        // "/x" plus the terminator, or the component loop below writes past
        // `out`. `n` is signed and every later check is `n == 0`, so a base
        // that overruns the buffer used to drive `n` NEGATIVE, skipping the
        // ENAMETOOLONG exit entirely: each iteration then wrote a bare '/'
        // (line "output a slash" is unconditional) while the name copy was
        // blocked by `--n > 0`, producing "//" runs that fail
        // path_is_normalized() — the assert seen in the field — after already
        // having scribbled past the end of the caller's MAX_PATH buffer.
        size_t at_len = strlen(at_path);
        if (at_len + 2 > (size_t) n)
            return _ENAMETOOLONG;
        strcpy(o, at_path);
        n -= at_len;
        o += at_len;
    }

    while (*p == '/')
        p++;

    while (*p != '\0') {
        if (p[0] == '.') {
            if (p[1] == '\0' || p[1] == '/') {
                // single dot path component, ignore
                p++;
                while (*p == '/')
                    p++;
                continue;
            } else if (p[1] == '.' && (p[2] == '\0' || p[2] == '/')) {
                // double dot path component, delete the last component
                if (o != out) {
                    do {
                        o--;
                        n++;
                    } while (*o != '/');
                }
                p += 2;
                while (*p == '/')
                    p++;
                continue;
            }
        }

        // [T-ish-pathnorm-overflow] Bail BEFORE writing the separator when
        // there is no room for it plus at least one name byte and the NUL.
        // Previously the slash was written unconditionally and only an exact
        // `n == 0` was caught afterwards, so an already-exhausted buffer both
        // overflowed and emitted "//".
        if (n < 2)
            return _ENAMETOOLONG;
        // output a slash
        *o++ = '/'; n--;
        char *c = o;
        // copy up to a slash or null
        while (*p != '/' && *p != '\0' && --n > 0)
            *o++ = *p++;
        // eat any slashes
        while (*p == '/')
            p++;

        // `<= 0` rather than `== 0`: the loop above can leave n at 0 having
        // consumed its last byte, and a defensive lower bound keeps a future
        // accounting slip from sailing past this exit again.
        if (n <= 0)
            return _ENAMETOOLONG;

        if ((flags & N_SYMLINK_FOLLOW) || *p != '\0') {
            // this buffer is used to store the path that we're readlinking, then
            // if it turns out to point to a symlink it's reused as the buffer
            // passed to the next path_normalize call
            char possible_symlink[MAX_PATH];
            *o = '\0';
            strcpy(possible_symlink, out);
            // a slash after this component: it must be a directory we may search
            bool need_dir = *(p - 1) == '/';
            uint64_t gen = path_cache_snapshot();
            struct dentry_cache_entry known;
            bool cached = dentry_cache_get(out, gen, &known) && (known.kind != DC_NOTLINK || !need_dir);
            // the key, before readlink writes the target over the component
            char key[DC_KEY];
            if ((size_t) (o - out) < sizeof(key))
                memcpy(key, out, o - out + 1);
            else
                gen = 0;
            struct mount *mount = NULL;
            int res = _EINVAL;
            if (cached) {
                if (known.kind == DC_SYMLINK) {
                    if ((size_t) known.target_len > (size_t) (MAX_PATH - (c - out)))
                        return _ENAMETOOLONG;
                    memcpy(c, known.target, known.target_len);
                    res = known.target_len;
                }
            } else {
                mount = find_mount_and_trim_path(possible_symlink);
                assert(path_is_normalized(possible_symlink));
                if (mount->fs->readlink)
                    res = mount->fs->readlink(mount, possible_symlink, c, MAX_PATH - (c - out));
                if (mount->fs != &fakefs)
                    gen = 0;   // procfs (/proc/self) and the like: not for keeping
            }
            if (res >= 0) {
                if (mount != NULL) {
                    mount_release(mount);
                    if (gen != 0)
                        dentry_cache_set(key, gen, DC_SYMLINK, NULL, c, res);
                }
                if (levels >= 5)
                    return _ELOOP;
                // readlink does not null terminate
                c[res] = '\0';
                // if we should restart from the root, copy down
                if (*c == '/')
                    memmove(out, c, strlen(c) + 1);
                char *expanded_path = possible_symlink;
                strcpy(expanded_path, out);
                if (strcmp(p, "") != 0) {
                    strcat(expanded_path, "/");
                    strcat(expanded_path, p);
                }
                return __path_normalize(NULL, expanded_path, out, flags, levels + 1);
            }

            if (cached) {
                if (need_dir) {
                    if (known.kind != DC_DIR)
                        return _ENOTDIR;
                    struct statbuf stat = {.mode = known.mode, .uid = known.uid, .gid = known.gid};
                    int err = access_check(&stat, AC_X);
                    if (err < 0)
                        return err;
                }
            } else if (need_dir) {
                // if there's a slash after this component, ensure that if it
                // exists, it's a directory and that we have execute perms on it
                struct statbuf stat;
                int err = mount->fs->stat(mount, possible_symlink, &stat);
                mount_release(mount);
                if (err >= 0) {
                    if (res == _EINVAL && gen != 0)
                        dentry_cache_set(key, gen, S_ISDIR(stat.mode) ? DC_DIR : DC_FILE, &stat, NULL, 0);
                    if (!S_ISDIR(stat.mode))
                        return _ENOTDIR;
                    err = access_check(&stat, AC_X);
                    if (err < 0)
                        return err;
                }
            } else {
                mount_release(mount);
                if (res == _EINVAL && gen != 0)
                    dentry_cache_set(key, gen, DC_NOTLINK, NULL, NULL, 0);
            }
        }
    }

    *o = '\0';
    assert(path_is_normalized(out));

    return 0;
}

int path_normalize(struct fd *at, const char *path, char *out, int flags) {
    assert(at != NULL);
    if (strcmp(path, "") == 0)
        return _ENOENT;

    // start with root or cwd, depending on whether it starts with a slash
    lock(&current->fs->lock);
    if (path[0] == '/')
        at = current->fs->root;
    else if (at == AT_PWD)
        at = current->fs->pwd;
    unlock(&current->fs->lock);

    char at_path[MAX_PATH];
    if (at != NULL) {
        int err = generic_getpath(at, at_path);
        if (err < 0)
            return err;
        assert(path_is_normalized(at_path));
    }

    // Build full input path for cache lookup
    char full_input[MAX_PATH];
    if (at != NULL && strcmp(at_path, "/") != 0) {
        snprintf(full_input, MAX_PATH, "%s/%s", at_path, path);
    } else {
        strncpy(full_input, path, MAX_PATH - 1);
        full_input[MAX_PATH - 1] = '\0';
    }

    // Snapshot the generation BEFORE looking up or walking: a mutation that
    // lands during the walk bumps past it, so the result we then store is
    // already stale-tagged and the next lookup rejects it.
    uint64_t gen = path_cache_snapshot();
    if (path_cache_get(full_input, flags, out, gen) == 0)
        return 0;

    int result = __path_normalize(at != NULL ? at_path : NULL, path, out, flags, 0);

    // Only successful results are cached; an error must be re-derived.
    if (result == 0)
        path_cache_set(full_input, flags, out, gen);

    return result;
}


bool path_is_normalized(const char *path) {
    while (*path != '\0') {
        if (*path != '/')
            return false;
        path++;
        if (*path == '/')
            return false;
        while (*path != '/' && *path != '\0')
            path++;
    }
    return true;
}

bool path_next_component(const char **path, char *component, int *err) {
    const char *p = *path;
    if (*p == '\0')
        return false;

    assert(*p == '/');
    p++;
    char *c = component;
    while (*p != '/' && *p != '\0') {
        *c++ = *p++;
        if (c - component >= MAX_NAME) {
            *err = _ENAMETOOLONG;
            return false;
        }
    }
    *c = '\0';
    *path = p;
    return true;
}
