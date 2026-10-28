#ifndef KERNEL_SERVE_H
#define KERNEL_SERVE_H

#include <stdbool.h>
#include <stdnoreturn.h>

// ish.exe --serve: one kernel for many commands (kernel/serve.c)
extern bool ish_serve_mode;
// -f's argument, for ready
extern const char *ish_serve_rootfs;
// After boot, on the thread that is pid 1: serves until shutdown.
noreturn void serve_main(void);

// What this ish.exe runs, for ready, --arch and BUILD_INFO: the guest
// architecture (arm64, x86_64, x86), the host's (aarch64, x86_64) and how guest
// code runs: vm, jit (arm64 guest), translated (x86 guests) or gadgets.
static inline const char *ish_guest_arch(void) {
#if defined(GUEST_ARM64)
    return "arm64";
#elif defined(GUEST_X86_64)
    return "x86_64";
#else
    return "x86";
#endif
}
static inline const char *ish_host_arch(void) {
#if defined(__aarch64__)
    return "aarch64";
#elif defined(__x86_64__)
    return "x86_64";
#else
    return "unknown";
#endif
}
const char *ish_engine(void);

#endif
