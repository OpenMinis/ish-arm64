#include "platform/platform.h"

// Ticks are 10ms (see platform.h). The figures come from the px layer, which
// sees the Windows API (platform/windows/misc.c).
struct cpu_usage get_cpu_usage(void) {
    uint64_t user, system, idle;
    px_cpu_times(&user, &system, &idle);
    return (struct cpu_usage) {
        .user_ticks = user / 100000,
        .system_ticks = system / 100000,
        .idle_ticks = idle / 100000,
    };
}

struct mem_usage get_mem_usage(void) {
    uint64_t total, avail;
    px_mem_info(&total, &avail);
    // bytes, as on the other hosts (/proc/meminfo and sysinfo divide)
    return (struct mem_usage) {
        .total = total,
        .free = avail,
        .active = total - avail,
        .inactive = 0,
    };
}

struct uptime_info get_uptime(void) {
    // no load average on Windows
    return (struct uptime_info) {.uptime_ticks = px_uptime_ms() / 1000};
}
