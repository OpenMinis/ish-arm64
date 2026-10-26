// Hardware performance counters for the emulator thread (macOS, needs root).
//
// ISH_PMU=EV1,EV2,...  configures the listed Apple PMU events (names from the
// kperfdata database, e.g. FIXED_CYCLES, BRANCH_INDIR_MISPRED_NONSPEC) through
// the private kperf/kperfdata frameworks, with per-thread counting enabled.
// pmu_report() prints the calling thread's counts; kernel/exit.c calls it
// when guest pid 1 exits, which covers the thread that ran it.

#include "kernel/pmu.h"
#if ISH_MAC_OPT   // the private kperf frameworks: macOS only, nothing at all elsewhere

#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define KPC_MAX_COUNTERS 32
#define PMU_MAX_EVENTS 16

typedef struct kpep_db kpep_db;
typedef struct kpep_config kpep_config;
typedef struct kpep_event kpep_event;

static int (*kpc_force_all_ctrs_set)(int);
static int (*kpc_set_config)(uint32_t, uint64_t *);
static int (*kpc_set_counting)(uint32_t);
static int (*kpc_set_thread_counting)(uint32_t);
static int (*kpc_get_thread_counters)(uint32_t, uint32_t, uint64_t *);
static int (*kpep_db_create)(const char *, kpep_db **);
static int (*kpep_config_create)(kpep_db *, kpep_config **);
static int (*kpep_config_force_counters)(kpep_config *);
static int (*kpep_db_event)(kpep_db *, const char *, kpep_event **);
static int (*kpep_config_add_event)(kpep_config *, kpep_event **, uint32_t, uint32_t *);
static int (*kpep_config_kpc_classes)(kpep_config *, uint32_t *);
static int (*kpep_config_kpc_count)(kpep_config *, size_t *);
static int (*kpep_config_kpc_map)(kpep_config *, size_t *, size_t);
static int (*kpep_config_kpc)(kpep_config *, uint64_t *, size_t);

static int pmu_active;
static int n_events;
static char *event_names[PMU_MAX_EVENTS];
static size_t counter_map[KPC_MAX_COUNTERS];

void pmu_thread_init(void);

#define LOAD(h, sym) do { *(void **) &sym = dlsym(h, #sym); if (!sym) { fprintf(stderr, "PMU: missing %s\n", #sym); return; } } while (0)

void pmu_start(void) {
    const char *spec = getenv("ISH_PMU");
    if (spec == NULL || spec[0] == '\0')
        return;
    void *kperf = dlopen("/System/Library/PrivateFrameworks/kperf.framework/kperf", RTLD_NOW);
    void *kpep = dlopen("/System/Library/PrivateFrameworks/kperfdata.framework/kperfdata", RTLD_NOW);
    if (!kperf || !kpep) {
        fprintf(stderr, "PMU: cannot load kperf frameworks\n");
        return;
    }
    LOAD(kperf, kpc_force_all_ctrs_set);
    LOAD(kperf, kpc_set_config);
    LOAD(kperf, kpc_set_counting);
    LOAD(kperf, kpc_set_thread_counting);
    LOAD(kperf, kpc_get_thread_counters);
    LOAD(kpep, kpep_db_create);
    LOAD(kpep, kpep_config_create);
    LOAD(kpep, kpep_config_force_counters);
    LOAD(kpep, kpep_db_event);
    LOAD(kpep, kpep_config_add_event);
    LOAD(kpep, kpep_config_kpc_classes);
    LOAD(kpep, kpep_config_kpc_count);
    LOAD(kpep, kpep_config_kpc_map);
    LOAD(kpep, kpep_config_kpc);

    if (kpc_force_all_ctrs_set(1) != 0) {
        fprintf(stderr, "PMU: kpc_force_all_ctrs_set failed (run as root)\n");
        return;
    }
    kpep_db *db;
    kpep_config *cfg;
    if (kpep_db_create(NULL, &db) || kpep_config_create(db, &cfg) || kpep_config_force_counters(cfg)) {
        fprintf(stderr, "PMU: kpep setup failed\n");
        return;
    }
    char *list = strdup(spec);
    for (char *tok = strtok(list, ","); tok && n_events < PMU_MAX_EVENTS; tok = strtok(NULL, ",")) {
        kpep_event *ev;
        if (kpep_db_event(db, tok, &ev)) {
            fprintf(stderr, "PMU: unknown event %s\n", tok);
            continue;
        }
        uint32_t err = 0;
        if (kpep_config_add_event(cfg, &ev, 0, &err)) {
            fprintf(stderr, "PMU: cannot add event %s (err %u; out of counters?)\n", tok, err);
            continue;
        }
        event_names[n_events++] = tok;
    }
    uint32_t classes = 0;
    size_t reg_count = 0;
    uint64_t regs[KPC_MAX_COUNTERS] = {0};
    if (kpep_config_kpc_classes(cfg, &classes) || kpep_config_kpc_count(cfg, &reg_count) ||
            kpep_config_kpc_map(cfg, counter_map, sizeof(counter_map)) ||
            kpep_config_kpc(cfg, regs, sizeof(regs))) {
        fprintf(stderr, "PMU: config export failed\n");
        return;
    }
    if (kpc_set_config(classes, regs) || kpc_set_counting(classes) || kpc_set_thread_counting(classes)) {
        fprintf(stderr, "PMU: kpc enable failed\n");
        return;
    }
    pmu_active = 1;
    pmu_thread_init();
    fprintf(stderr, "📟 PMU: counting %d events\n", n_events);
}

// Keep counted threads on the performance cores: the event encodings differ
// on the efficiency cores, and a thread that migrates there reads back garbage
// (wrapped negative deltas) for several events.
#include <pthread.h>
#include <sys/qos.h>
void pmu_thread_init(void) {
    if (pmu_active)
        pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
}

void pmu_report(void) {
    if (!pmu_active)
        return;
    uint64_t buf[KPC_MAX_COUNTERS] = {0};
    if (kpc_get_thread_counters(0, KPC_MAX_COUNTERS, buf)) {
        fprintf(stderr, "PMU: read failed\n");
        return;
    }
    fprintf(stderr, "📟 PMU (guest pid 1 thread):\n");
    for (int i = 0; i < n_events; i++)
        fprintf(stderr, "   %-34s %16llu\n", event_names[i], (unsigned long long) buf[counter_map[i]]);
}

#endif
