// Agent-runtime harness: the ish CLI plus the host hooks the app installs
// (bind mounts, per-context path routing, fork guard, in-process offload
// handler, bind-mount change feed, timer tick hook), each switched on by an
// environment variable so tests/agent/run.sh can exercise them on any guest.
//
//   ISHT_BIND="/mnt/h=/host/dir[:ro],..."   fakefs_bind_mount
//   ISHT_ROUTE="/ctx=/host/dir"             path-translate hook, only for
//                                           groups whose fs_context is 1;
//                                           ISHT_CTX=1 sets it on init
//                                           (forks inherit it)
//   ISHT_FORK_LIMIT=N                       deny forks after N with EAGAIN
//   ISHT_HANDLER=1                          in-process handler "nativeecho"
//   ISHT_CHANGES=1                          print bind-mount change events
//   ISHT_TICKS=1                            count timer ticks, print at exit
// Bind mounts and fs_context need the root mounted and init to exist, which
// happens inside xX_main_Xx() just before it execs init. Hook that point.
// main.c's system headers must come before misc.h's macros.
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <execinfo.h>
#include <termios.h>
#include <unistd.h>
#include <pthread.h>
#ifdef __APPLE__
#include <mach/mach.h>
#include <sys/sysctl.h>
#endif
#include "kernel/init.h"
static void harness_after_root(void);
#define become_first_process() (become_first_process(), harness_after_root(), 0)
#define main ish_cli_main
#include "../../main.c"
#undef main
#undef become_first_process

#include <stdatomic.h>
#define ISH_INTERNAL
#include "fs/fake.h"

static char route_guest[256], route_host[1024];
static size_t route_guest_len;

static bool route_hook(const char *guest_path, uint64_t fs_context,
                       char *out, size_t out_size) {
    if (fs_context != 1 || route_guest_len == 0)
        return false;
    if (strncmp(guest_path, route_guest, route_guest_len) != 0)
        return false;
    const char *rest = guest_path + route_guest_len;
    if (*rest != '\0' && *rest != '/')
        return false;
    snprintf(out, out_size, "%s%s", route_host, rest);
    return true;
}

static bool route_reverse_hook(const char *host_path, char *out, size_t out_size) {
    size_t n = strlen(route_host);
    if (n == 0 || strncmp(host_path, route_host, n) != 0)
        return false;
    const char *rest = host_path + n;
    if (*rest != '\0' && *rest != '/')
        return false;
    snprintf(out, out_size, "%s%s", route_guest, rest);
    return true;
}

static atomic_int forks_left;
static int fork_guard(void) {
    return atomic_fetch_sub(&forks_left, 1) > 0 ? 0 : _EAGAIN;
}

static int nativeecho_main(int argc, char **argv, int UNUSED(in), int out, int UNUSED(err)) {
    dprintf(out, "nativeecho argc=%d", argc);
    for (int i = 1; i < argc; i++)
        dprintf(out, " [%s]", argv[i]);
    dprintf(out, "\n");
    return 7;
}

static atomic_ulong ticks;
// Init's exit ends in _exit(), so report from the hook instead of atexit.
static void print_changes(const struct fakefs_change_event *batch, int count) {
    for (int i = 0; i < count; i++)
        fprintf(stderr, "ISHT change op=%d ctx=%llu %s\n", batch[i].op,
                (unsigned long long) batch[i].fs_context, batch[i].linux_path);
}

static int tick_hook(void) {
    if (atomic_fetch_add(&ticks, 1) == 99)
        fprintf(stderr, "ISHT ticks=100\n");
    return 0;
}

static void setup_binds(char *spec) {
    for (char *item = strtok(spec, ","); item != NULL; item = strtok(NULL, ",")) {
        char *eq = strchr(item, '=');
        if (eq == NULL)
            continue;
        *eq = '\0';
        char *host = eq + 1;
        bool ro = false;
        size_t n = strlen(host);
        if (n > 3 && strcmp(host + n - 3, ":ro") == 0) {
            host[n - 3] = '\0';
            ro = true;
        }
        int err = fakefs_bind_mount(item, host, ro);
        fprintf(stderr, "ISHT bind %s -> %s%s = %d\n", item, host, ro ? " (ro)" : "", err);
    }
}

static void harness_after_root(void) {
    const char *e;
    if ((e = getenv("ISHT_BIND")) != NULL)
        setup_binds(strdup(e));
    if (getenv("ISHT_CTX"))
        current->group->fs_context = 1;
}

int main(int argc, char *const argv[]) {
    const char *e;
    if ((e = getenv("ISHT_ROUTE")) != NULL) {
        const char *eq = strchr(e, '=');
        if (eq != NULL) {
            snprintf(route_guest, sizeof(route_guest), "%.*s", (int) (eq - e), e);
            route_guest_len = strlen(route_guest);
            // The hook's output is used as-is, so hand it a canonical path
            // (bind mounts realpath() theirs for the same reason).
            if (realpath(eq + 1, route_host) == NULL)
                snprintf(route_host, sizeof(route_host), "%s", eq + 1);
            fakefs_set_path_translate_hook(route_hook);
            fakefs_set_path_reverse_hook(route_reverse_hook);
        }
    }
    if ((e = getenv("ISHT_FORK_LIMIT")) != NULL) {
        atomic_store(&forks_left, atoi(e));
        ish_set_fork_guard(fork_guard);
    }
    if (getenv("ISHT_HANDLER"))
        native_offload_add_handler("nativeecho", nativeecho_main);
    if (getenv("ISHT_CHANGES"))
#ifdef __APPLE__
        fakefs_install_change_consumer(^(const struct fakefs_change_event *batch, int count) {
            print_changes(batch, count);
        });
#else
        fakefs_install_change_consumer(print_changes);
#endif
    if (getenv("ISHT_TICKS"))
        ish_set_timer_tick_hook(tick_hook);
    return ish_cli_main(argc, argv);
}
