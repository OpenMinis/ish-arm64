// N17: os_signpost runtime support. Stub when ISH_OS_SIGNPOST is off.

#include "util/signpost.h"

#ifdef ISH_OS_SIGNPOST

#include <os/log.h>
#include <os/signpost.h>
#include <pthread.h>

static os_log_t logs[ISH_SP_CAT_COUNT];
static const char *cat_names[ISH_SP_CAT_COUNT] = {
    [ISH_SP_CAT_EXEC]    = "exec",
    [ISH_SP_CAT_JIT]     = "jit",
    [ISH_SP_CAT_FS]      = "fs",
    [ISH_SP_CAT_SYSCALL] = "syscall",
    [ISH_SP_CAT_TLB]     = "tlb",
};

static void do_init(void *unused) {
    (void)unused;
    for (int i = 0; i < ISH_SP_CAT_COUNT; i++)
        logs[i] = os_log_create("io.ish-app.ish", cat_names[i]);
}

void ish_signpost_init(void) {
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, (void(*)(void))do_init);
}

uint64_t ish_signpost_begin(int cat, const char *name) {
    if (cat < 0 || cat >= ISH_SP_CAT_COUNT) return 0;
    if (logs[cat] == NULL) ish_signpost_init();
    os_signpost_id_t id = os_signpost_id_generate(logs[cat]);
    os_signpost_interval_begin(logs[cat], id, "ish");
    return (uint64_t)id;
}

void ish_signpost_end(int cat, const char *name, uint64_t spid) {
    if (cat < 0 || cat >= ISH_SP_CAT_COUNT) return;
    if (logs[cat] == NULL) return;
    if (spid == 0) return;
    os_signpost_interval_end(logs[cat], (os_signpost_id_t)spid, "ish");
}

void ish_signpost_event(int cat, const char *name, const char *msg) {
    if (cat < 0 || cat >= ISH_SP_CAT_COUNT) return;
    if (logs[cat] == NULL) return;
    os_signpost_event_emit(logs[cat], OS_SIGNPOST_ID_EXCLUSIVE, "ish");
}

#endif
