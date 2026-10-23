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

#endif
