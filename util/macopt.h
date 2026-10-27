#ifndef UTIL_MACOPT_H
#define UTIL_MACOPT_H

// ISH_MAC_OPT: the optimizations made for running ish on a Mac (see
// benchmark/macjit) -- the shadow window, precise TLB invalidation, the
// code region flush, the leaner dispatch, the path component cache and
// the rest. Everything they change is under #if ISH_MAC_OPT, so an iOS
// build compiles exactly what it did before them.
#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif
#if defined(__APPLE__) && TARGET_OS_OSX && defined(__aarch64__)
#define ISH_MAC_OPT 1
#else
#define ISH_MAC_OPT 0
#endif
// the shadow window (kernel/sba.h) is for the ARM64 guest
#if ISH_MAC_OPT && defined(GUEST_ARM64)
#define ISH_SBA 1
#else
#define ISH_SBA 0
#endif
// guest code run in a Hypervisor.framework VM (kernel/hvf.h), on the window
#define ISH_HVF ISH_SBA

#endif
