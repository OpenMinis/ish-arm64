#include "util/verbosetrace.h"

// Default OFF: a build that never calls ish_set_verbose_trace() stays quiet,
// which is the behaviour a shipped app wants.
bool ish_verbose_trace_enabled = false;

void ish_set_verbose_trace(bool enabled) {
    ish_verbose_trace_enabled = enabled;
}
