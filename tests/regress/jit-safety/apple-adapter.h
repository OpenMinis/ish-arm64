// Test boundary, not an Apple/iOS implementation. No executable mapping by JIT.
#define TARGET_OS_OSX 0
#define ISH_JIT_NO_EMIT 1
#include <stdint.h>
#include <time.h>
#include <sys/ucontext.h>
static inline void sys_icache_invalidate(void *p, unsigned long n) { __builtin___clear_cache(p, (char *)p+n); }
static inline uint64_t mach_absolute_time(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return t.tv_sec*1000000000ull+t.tv_nsec; }
typedef struct { unsigned numer, denom; } mach_timebase_info_data_t;
static inline void mach_timebase_info(mach_timebase_info_data_t *p) { p->numer=p->denom=1; }
// Deliberately synthetic Darwin register context: exercises sync logic only.
typedef struct { struct { uint64_t __x[29], __fp, __lr, __sp, __pc; } __ss;
    struct { uint64_t __esr; } __es;
} probe_mcontext;
typedef struct { probe_mcontext *uc_mcontext; } probe_ucontext;
#define ucontext_t probe_ucontext
