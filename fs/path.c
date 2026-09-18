#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>
#include "kernel/calls.h"
#include "util/sync.h"
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

void path_cache_invalidate(void) {
    atomic_fetch_add_explicit(&path_cache_gen, 1, memory_order_release);
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

// Get current time in nanoseconds
static inline uint64_t get_time_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

// Try to get cached normalized path
// Returns 0 on cache hit, -1 on cache miss
static int path_cache_get(const char *full_path, int flags, char *out, uint64_t gen) {
    uint32_t index = path_hash(full_path) % PATH_CACHE_SIZE;
    struct path_cache_entry *entry = &path_cache[index];
    uint64_t now = get_time_ns();

    lock(&path_cache_lock);
    bool hit = entry->valid
        && entry->gen == gen
        && entry->flags == flags
        && now - entry->timestamp <= PATH_CACHE_TTL_NS
        && strcmp(entry->input_path, full_path) == 0;
    if (hit)
        strcpy(out, entry->normalized);
    unlock(&path_cache_lock);
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
            struct mount *mount = find_mount_and_trim_path(possible_symlink);
            assert(path_is_normalized(possible_symlink));
            int res = _EINVAL;
            if (mount->fs->readlink)
                res = mount->fs->readlink(mount, possible_symlink, c, MAX_PATH - (c - out));
            if (res >= 0) {
                mount_release(mount);
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

            // if there's a slash after this component, ensure that if it
            // exists, it's a directory and that we have execute perms on it
            if (*(p - 1) == '/') {
                struct statbuf stat;
                int err = mount->fs->stat(mount, possible_symlink, &stat);
                mount_release(mount);
                if (err >= 0) {
                    if (!S_ISDIR(stat.mode))
                        return _ENOTDIR;
                    err = access_check(&stat, AC_X);
                    if (err < 0)
                        return err;
                }
            } else {
                mount_release(mount);
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
