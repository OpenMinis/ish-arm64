// Sample a command's user PC every N occurrences of a PMU event (macOS, root).
//
//   pmisample <event> <period> <command> [args...]
//
// The command starts suspended; the configurable counter counting <event>
// (a kperfdata name such as INST_ALL) fires a PMI every <period> events,
// whose kperf action takes the user call stack of the running thread when it
// belongs to the command. The samples go to kdebug (class 37): run this
// under `ktrace` set up to record them (benchmark/macjit/pmi/sample.sh).
// With INST_ALL each sample stands for the same number of retired
// instructions, so where they land is where the instructions went.

#include <dlfcn.h>
#include <errno.h>
#include <signal.h>
#include <spawn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>

extern char **environ;

#define KPC_MAX 32
#define KPERF_SAMPLER_USTACK (1U << 3)

typedef struct kpep_db kpep_db;
typedef struct kpep_config kpep_config;
typedef struct kpep_event kpep_event;

static int (*kpc_force_all_ctrs_set)(int);
static int (*kpc_set_config)(uint32_t, uint64_t *);
static int (*kpc_set_counting)(uint32_t);
static int (*kpc_set_thread_counting)(uint32_t);
static int (*kpc_set_period)(uint32_t, uint64_t *);
static int (*kpc_set_actionid)(uint32_t, uint32_t *);
static int (*kperf_action_count_set)(uint32_t);
static int (*kperf_action_count_get)(uint32_t *);
static int (*kperf_action_samplers_set)(uint32_t, uint32_t);
static int (*kperf_action_filter_set_by_pid)(uint32_t, int32_t);
static int (*kperf_sample_set)(uint32_t);
static int (*kperf_reset)(void);
static int (*kpep_db_create)(const char *, kpep_db **);
static int (*kpep_config_create)(kpep_db *, kpep_config **);
static int (*kpep_config_force_counters)(kpep_config *);
static int (*kpep_db_event)(kpep_db *, const char *, kpep_event **);
static int (*kpep_config_add_event)(kpep_config *, kpep_event **, uint32_t, uint32_t *);
static int (*kpep_config_kpc_classes)(kpep_config *, uint32_t *);
static int (*kpep_config_kpc_count)(kpep_config *, size_t *);
static int (*kpep_config_kpc_map)(kpep_config *, size_t *, size_t);
static int (*kpep_config_kpc)(kpep_config *, uint64_t *, size_t);

#define LOAD(h, sym) do { *(void **) &sym = dlsym(h, #sym); if (!sym) { fprintf(stderr, "missing %s\n", #sym); return 1; } } while (0)
#define CHECK(x) do { int r_ = (x); if (r_) { fprintf(stderr, "%s failed: %d errno %d\n", #x, r_, errno); return 1; } } while (0)

int main(int argc, char **argv) {
    if (argc < 4) {
        fprintf(stderr, "usage: %s <event> <period> <command> [args...]\n", argv[0]);
        return 2;
    }
    const char *event = argv[1];
    uint64_t period = strtoull(argv[2], NULL, 0);
    void *kperf = dlopen("/System/Library/PrivateFrameworks/kperf.framework/kperf", RTLD_NOW);
    void *kpep = dlopen("/System/Library/PrivateFrameworks/kperfdata.framework/kperfdata", RTLD_NOW);
    if (!kperf || !kpep)
        return fprintf(stderr, "cannot load kperf\n"), 1;
    LOAD(kperf, kpc_force_all_ctrs_set); LOAD(kperf, kpc_set_config); LOAD(kperf, kpc_set_counting);
    LOAD(kperf, kpc_set_thread_counting); LOAD(kperf, kpc_set_period); LOAD(kperf, kpc_set_actionid);
    LOAD(kperf, kperf_action_count_set); LOAD(kperf, kperf_action_count_get); LOAD(kperf, kperf_action_samplers_set);
    LOAD(kperf, kperf_action_filter_set_by_pid); LOAD(kperf, kperf_sample_set); LOAD(kperf, kperf_reset);
    LOAD(kpep, kpep_db_create); LOAD(kpep, kpep_config_create); LOAD(kpep, kpep_config_force_counters);
    LOAD(kpep, kpep_db_event); LOAD(kpep, kpep_config_add_event); LOAD(kpep, kpep_config_kpc_classes);
    LOAD(kpep, kpep_config_kpc_count); LOAD(kpep, kpep_config_kpc_map); LOAD(kpep, kpep_config_kpc);

    CHECK(kpc_force_all_ctrs_set(1));
    kpep_db *db;
    kpep_config *cfg;
    kpep_event *ev;
    CHECK(kpep_db_create(NULL, &db));
    CHECK(kpep_config_create(db, &cfg));
    CHECK(kpep_config_force_counters(cfg));
    CHECK(kpep_db_event(db, event, &ev));
    uint32_t err = 0;
    CHECK(kpep_config_add_event(cfg, &ev, 0, &err));
    uint32_t classes = 0;
    size_t count = 0, map[KPC_MAX] = {0};
    uint64_t regs[KPC_MAX] = {0};
    CHECK(kpep_config_kpc_classes(cfg, &classes));
    CHECK(kpep_config_kpc_count(cfg, &count));
    CHECK(kpep_config_kpc_map(cfg, map, sizeof(map)));
    CHECK(kpep_config_kpc(cfg, regs, sizeof(regs)));

    // The command, suspended until sampling is set up for its pid.
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    // _POSIX_SPAWN_DISABLE_ASLR (0x100): the command's own addresses stay
    // at their link-time place, so samples in it can be symbolized afterwards
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_START_SUSPENDED | 0x100);
    pid_t pid;
    if (posix_spawnp(&pid, argv[3], NULL, &attr, argv + 3, environ) != 0)
        return perror("posix_spawnp"), 1;

    // map[0]: index of our event's counter among all counters (fixed ones
    // first); periods and action ids are given for the classes' counters.
    uint64_t periods[KPC_MAX] = {0};
    uint32_t actions[KPC_MAX] = {0};
    periods[map[0]] = period;
    actions[map[0]] = 1;
    CHECK(kperf_reset());
    uint32_t nactions = 0;
    CHECK(kperf_action_count_get(&nactions));   // it only grows
    if (nactions < 1)
        CHECK(kperf_action_count_set(1));
    CHECK(kperf_action_samplers_set(1, KPERF_SAMPLER_USTACK));
    if (!getenv("PMISAMPLE_ALL"))
        CHECK(kperf_action_filter_set_by_pid(1, pid));
    else
        CHECK(kperf_action_filter_set_by_pid(1, -1));
    CHECK(kpc_set_config(classes, regs));
    CHECK(kpc_set_period(classes, periods));
    CHECK(kpc_set_actionid(classes, actions));
    CHECK(kpc_set_counting(classes));
    CHECK(kpc_set_thread_counting(classes));
    CHECK(kperf_sample_set(1));
    fprintf(stderr, "pmisample: %s every %llu, counter %zu, pid %d\n", event, (unsigned long long) period, map[0], pid);

    kill(pid, SIGCONT);
    int status;
    waitpid(pid, &status, 0);
    kperf_sample_set(0);
    kperf_reset();
    kpc_set_counting(0);
    kpc_set_thread_counting(0);
    kpc_force_all_ctrs_set(0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}
