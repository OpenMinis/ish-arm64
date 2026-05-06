// N17: Apple os_signpost instrumentation, lightweight wrapper.
//
// We deliberately *do not* include <os/signpost.h> here because it
// pulls in <os/base.h> which uses clang-specific attributes that don't
// agree with the codebase's gnu11 + warning settings.  Instead we
// expose minimal void* / uint64_t typed wrappers; the implementation
// in signpost.c keeps the Apple headers contained.
//
// Macros expand to nothing when ISH_OS_SIGNPOST is undefined.
//
// To enable: meson setup -Dc_args="-DISH_OS_SIGNPOST=1 -fblocks"

#ifndef ISH_SIGNPOST_H
#define ISH_SIGNPOST_H

#include <stdint.h>

#ifdef ISH_OS_SIGNPOST

void ish_signpost_init(void);

// Categories. We expose them as small ints so headers don't need to
// pull in <os/log.h>.
enum ish_sp_cat {
    ISH_SP_CAT_EXEC = 0,
    ISH_SP_CAT_JIT,
    ISH_SP_CAT_FS,
    ISH_SP_CAT_SYSCALL,
    ISH_SP_CAT_TLB,
    ISH_SP_CAT_COUNT
};

uint64_t ish_signpost_begin(int cat, const char *name);
void ish_signpost_end(int cat, const char *name, uint64_t spid);
void ish_signpost_event(int cat, const char *name, const char *msg);

// Per-category macros — direct, no token-paste indirection.
#define ISH_SIGNPOST_SCOPE_BEGIN(category, name, id_var) \
    uint64_t id_var = ish_signpost_begin(_ISH_SP_##category, name)
#define ISH_SIGNPOST_SCOPE_END(category, name, id_var) \
    ish_signpost_end(_ISH_SP_##category, name, id_var)
#define ISH_SIGNPOST_EVENT(category, name, fmt, ...) \
    ish_signpost_event(_ISH_SP_##category, name, "")

#define _ISH_SP_exec    ISH_SP_CAT_EXEC
#define _ISH_SP_jit     ISH_SP_CAT_JIT
#define _ISH_SP_fs      ISH_SP_CAT_FS
#define _ISH_SP_syscall ISH_SP_CAT_SYSCALL
#define _ISH_SP_tlb     ISH_SP_CAT_TLB

#else

#define ISH_SIGNPOST_SCOPE_BEGIN(category, name, id_var) ((void)0)
#define ISH_SIGNPOST_SCOPE_END(category, name, id_var) ((void)0)
#define ISH_SIGNPOST_EVENT(category, name, fmt, ...) ((void)0)
static inline void ish_signpost_init(void) {}

#endif

#endif // ISH_SIGNPOST_H
