#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <stdint.h>
#include <stdbool.h>
#include "kernel/calls.h"
#include "fs/path.h"

// === Path Normalize Cache ===
// Thread-local cache for frequently normalized paths
// Reduces redundant path normalization during Python import and file operations

#define PATH_CACHE_SIZE 64
#define PATH_CACHE_TTL_NS 100000000  // 100ms TTL

struct path_cache_entry {
    char input_path[MAX_PATH];     // Original path (with at_path prefix if any)
    char normalized[MAX_PATH];     // Normalized result
    uint64_t timestamp;            // nanosecond timestamp
    int flags;                     // N_SYMLINK_FOLLOW or N_SYMLINK_NOFOLLOW
    bool valid;
};

// Thread-local cache (one per thread for lock-free access)
static __thread struct path_cache_entry path_cache[PATH_CACHE_SIZE];
static __thread bool path_cache_initialized = false;

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

// Initialize thread-local cache
static void path_cache_init(void) {
    if (!path_cache_initialized) {
        memset(path_cache, 0, sizeof(path_cache));
        path_cache_initialized = true;
    }
}

// Try to get cached normalized path
// Returns 0 on cache hit, -1 on cache miss
static int path_cache_get(const char *full_path, int flags, char *out) {
    path_cache_init();

    uint32_t hash = path_hash(full_path);
    uint32_t index = hash % PATH_CACHE_SIZE;
    struct path_cache_entry *entry = &path_cache[index];

    // Check cache validity
    if (!entry->valid)
        return -1;

    // Check path and flags match
    if (strcmp(entry->input_path, full_path) != 0)
        return -1;

    if (entry->flags != flags)
        return -1;

    // Check TTL (time-to-live)
    uint64_t now = get_time_ns();
    if (now - entry->timestamp > PATH_CACHE_TTL_NS) {
        entry->valid = false;  // Expired
        return -1;
    }

    // Cache hit! Copy result
    strcpy(out, entry->normalized);
    return 0;
}

// Store normalized path in cache
static void path_cache_set(const char *full_path, int flags, const char *normalized) {
    path_cache_init();

    uint32_t hash = path_hash(full_path);
    uint32_t index = hash % PATH_CACHE_SIZE;
    struct path_cache_entry *entry = &path_cache[index];

    // Store in cache
    strncpy(entry->input_path, full_path, MAX_PATH - 1);
    entry->input_path[MAX_PATH - 1] = '\0';

    strncpy(entry->normalized, normalized, MAX_PATH - 1);
    entry->normalized[MAX_PATH - 1] = '\0';

    entry->flags = flags;
    entry->timestamp = get_time_ns();
    entry->valid = true;
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

    // Try cache lookup first
    if (path_cache_get(full_input, flags, out) == 0) {
        // Cache hit - fast return
        return 0;
    }

    // Cache miss - do full normalization
    int result = __path_normalize(at != NULL ? at_path : NULL, path, out, flags, 0);

    // Store result in cache (even on error, we cache the error)
    if (result == 0) {
        path_cache_set(full_input, flags, out);
    }

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
