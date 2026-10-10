// The posix_spawn subset kernel/native_offload.c uses, over px_spawn: stdio
// redirection (dup2 onto 0-2, /dev/null) and a working directory.
#ifndef PLATFORM_WINDOWS_SPAWN_H
#define PLATFORM_WINDOWS_SPAWN_H
#include <string.h>

typedef struct {
    int fds[3];         // -1: inherit NUL
    char *cwd;
} posix_spawn_file_actions_t;
typedef struct {
    int flags;
} posix_spawnattr_t;

static inline int posix_spawn_file_actions_init(posix_spawn_file_actions_t *a) {
    a->fds[0] = 0;
    a->fds[1] = 1;
    a->fds[2] = 2;
    a->cwd = NULL;
    return 0;
}
static inline int posix_spawn_file_actions_destroy(posix_spawn_file_actions_t *a) {
    free(a->cwd);
    return 0;
}
static inline int posix_spawn_file_actions_adddup2(posix_spawn_file_actions_t *a, int fd, int to) {
    if (to >= 0 && to < 3)
        a->fds[to] = fd;
    return 0;
}
static inline int posix_spawn_file_actions_addclose(posix_spawn_file_actions_t *a, int fd) {
    (void) a; (void) fd; // the child only ever gets its three stdio handles
    return 0;
}
static inline int posix_spawn_file_actions_addopen(posix_spawn_file_actions_t *a, int to,
        const char *path, int flags, int mode) {
    (void) path; (void) flags; (void) mode; // only ever /dev/null
    if (to >= 0 && to < 3)
        a->fds[to] = -1;
    return 0;
}
static inline int posix_spawn_file_actions_addchdir(posix_spawn_file_actions_t *a, const char *dir) {
    free(a->cwd);
    a->cwd = strdup(dir);
    return 0;
}
#define posix_spawn_file_actions_addchdir_np posix_spawn_file_actions_addchdir
static inline int posix_spawnattr_init(posix_spawnattr_t *a) { a->flags = 0; return 0; }
static inline int posix_spawnattr_destroy(posix_spawnattr_t *a) { (void) a; return 0; }
static inline int posix_spawnattr_setflags(posix_spawnattr_t *a, short flags) { a->flags = flags; return 0; }

static inline int posix_spawn(pid_t *pid, const char *path, const posix_spawn_file_actions_t *a,
        const posix_spawnattr_t *attr, char *const argv[], char *const envp[]) {
    (void) attr;
    int fds[3] = {0, 1, 2};
    if (a != NULL)
        memcpy(fds, a->fds, sizeof(fds));
    int p;
    int err = px_spawn(&p, path, fds, argv, envp, a ? a->cwd : NULL);
    if (err == 0)
        *pid = p;
    return err;
}
#endif
