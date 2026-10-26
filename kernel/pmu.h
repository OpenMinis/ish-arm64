#ifndef KERNEL_PMU_H
#define KERNEL_PMU_H

// Hardware performance counters (macOS only, see kernel/pmu.c): ISH_PMU=EV,...
#include "util/macopt.h"
#if ISH_MAC_OPT
void pmu_start(void);        // main, before guest code runs
void pmu_thread_init(void);  // every guest task's host thread
void pmu_report(void);       // guest pid 1 exits: print its thread's counts
#endif

#endif
