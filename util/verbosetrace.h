#ifndef UTIL_VERBOSETRACE_H
#define UTIL_VERBOSETRACE_H

#include <stdbool.h>
#include <stdio.h>

// [T-ios-log-verbose-tier] Runtime gate for the kernel's high-frequency
// filesystem traces.
//
// These traces (fakefs_readdir per directory entry, realfs_getpath per
// bind-mount resolve) write straight to stderr, which the app captures into
// its daily log file via dup2. They were unconditional: two measured days
// produced 183 MB / 1.68 M lines and 76 MB / 300 K lines respectively — the
// single largest contributor to a 775 MB log — with only 549 distinct paths
// behind the 1.68 M readdir lines.
//
// They are genuinely useful when chasing a bind-mount or path-translation
// bug on a real device, so deleting them outright would cost a diagnostic.
// Instead they are gated on the app's log level: OFF unless the user has
// switched logging to Verbose. The flag is plain `bool` written once from the
// app and read on a hot path; a torn read is impossible for a single byte and
// the worst case either way is one trace line's difference.
extern bool ish_verbose_trace_enabled;

void ish_set_verbose_trace(bool enabled);

// Emit only while verbose tracing is on. Argument evaluation is inside the
// branch, so a disabled trace costs one predictable-branch test and never
// formats its string.
#define VERBOSE_TRACE(...) do { \
    if (__builtin_expect(ish_verbose_trace_enabled, 0)) \
        fprintf(stderr, __VA_ARGS__); \
} while (0)

#endif
