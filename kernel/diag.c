// [FEAT-261027-15] See diag.h.
#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "kernel/diag.h"
#include "kernel/task.h"

_Atomic uint64_t ish_diag_counters[DIAG_COUNTER_COUNT];
_Atomic int64_t ish_diag_live_node, ish_diag_peak_procs, ish_diag_peak_node;
extern _Atomic int64_t ish_live_guest_procs;

// AOT image totals: jit.c registers its reader when the engine has one.
// out: images used, images rejected, AOT installs, moved, fixed, busy.
void (*ish_diag_jit_counts)(uint64_t out[6]);

static const char *const counter_names[DIAG_COUNTER_COUNT] = {
    [DIAG_UNMAP_CALLS] = "unmap_calls",
    [DIAG_UNMAP_PAGES] = "unmap_pages",
    [DIAG_UNMAP_HOST_FREES] = "unmap_host_frees",
    [DIAG_UNMAP_SHARED_MM] = "unmap_shared_mm",
    [DIAG_MEM_CHANGED] = "mem_changed",
    [DIAG_WX_CLEARS] = "wx_clears",
    [DIAG_JETSAM_ADDED] = "jetsam_added",
    [DIAG_JETSAM_FREED] = "jetsam_freed",
    [DIAG_VALVE_LEAKED] = "valve_leaked",
    [DIAG_DEFERRED_RELEASED] = "deferred_released",
    [DIAG_MM_DESTROYED] = "mm_destroyed",
    [DIAG_EXIT_GROUPS] = "exit_groups",
    [DIAG_EXECS] = "execs",
    [DIAG_EXEC_NODE_DEFAULT] = "node_default",
    [DIAG_EXEC_NODE_JITLESS] = "node_jitless",
    [DIAG_OWN_THREADS] = "own_threads",
    [DIAG_BORROWS] = "borrows",
    [DIAG_BORROW_RENAMES] = "borrow_renames",
    [DIAG_AOT_FIX_B] = "aot_fix_b",
    [DIAG_AOT_FIX_BCOND] = "aot_fix_bcond",
    [DIAG_AOT_FIX_CBZ] = "aot_fix_cbz",
    [DIAG_AOT_FIX_TBZ] = "aot_fix_tbz",
    [DIAG_AOT_FIX_ADR] = "aot_fix_adr",
    [DIAG_AOT_FIX_ADRP] = "aot_fix_adrp",
    [DIAG_AOT_FIX_OTHER] = "aot_fix_other",
    [DIAG_AOT_GIVEUP_DATA] = "aot_giveup_data",
    [DIAG_AOT_GIVEUP_LOOP] = "aot_giveup_loop",
    [DIAG_AOT_GIVEUP_INSN] = "aot_giveup_insn",
    [DIAG_AOT_GIVEUP_LDRLIT] = "aot_giveup_ldrlit",
    [DIAG_AOT_GIVEUP_ADRSHIFT] = "aot_giveup_adrshift",
    [DIAG_AOT_REJECTED_IMAGES] = "aot_rejected_images",
};

static const char *const event_names[] = {
    [DIAG_EV_EXEC] = "exec",
    [DIAG_EV_MM_DESTROY] = "mm_destroy",
    [DIAG_EV_EXIT_GROUP] = "exit_group",
    [DIAG_EV_BIG_MUNMAP] = "big_munmap",
    [DIAG_EV_VALVE_LEAK] = "valve_leak",
    [DIAG_EV_DEFERRED_RELEASE] = "deferred_release",
    [DIAG_EV_AOT_REJECT] = "aot_reject",
    [DIAG_EV_BORROW_RENAME] = "borrow_rename",
    [DIAG_EV_BORROW_BEGIN] = "borrow_begin",
};

// Milliseconds since the process started (monotonic; clock_gettime is
// async-signal-safe).
static uint64_t start_ns;
static uint64_t mono_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t) ts.tv_sec * 1000000000ull + (uint64_t) ts.tv_nsec;
}
__attribute__((constructor)) static void diag_start(void) {
    start_ns = mono_ns();
}
static uint64_t now_ms(void) {
    return (mono_ns() - start_ns) / 1000000ull;
}

static void raise_peak(_Atomic int64_t *peak, int64_t value) {
    int64_t seen = atomic_load_explicit(peak, memory_order_relaxed);
    while (value > seen &&
           !atomic_compare_exchange_weak_explicit(peak, &seen, value, memory_order_relaxed, memory_order_relaxed)) {}
}

void ish_diag_note_procs(int64_t live) {
    raise_peak(&ish_diag_peak_procs, live);
}

// MARK: - Event ring

#define DIAG_RING 64
struct diag_ev {
    _Atomic uint64_t seq;   // index + 1 once written; 0 while being written
    uint64_t ms;
    int64_t a, b;
    int32_t pid;
    uint16_t type;
    char thread[16];
};
static struct diag_ev ring[DIAG_RING];
static _Atomic uint64_t ring_head;

void ish_diag_event(enum ish_diag_event type, int64_t a, int64_t b) {
    uint64_t idx = atomic_fetch_add_explicit(&ring_head, 1, memory_order_relaxed);
    struct diag_ev *e = &ring[idx % DIAG_RING];
    atomic_store_explicit(&e->seq, 0, memory_order_relaxed);
    e->ms = now_ms();
    e->a = a;
    e->b = b;
    e->pid = current ? (int32_t) current->pid : -1;
    e->type = (uint16_t) type;
    if (pthread_getname_np(pthread_self(), e->thread, sizeof(e->thread)) != 0)
        e->thread[0] = '\0';
    atomic_store_explicit(&e->seq, idx + 1, memory_order_release);
}

// MARK: - Thread ownership

enum { ROLE_APP = 0, ROLE_OWN = 1 };
struct diag_thread {
    uint8_t role;
    int32_t borrow_depth;   // > 0 while the app has lent this thread to the kernel
    int32_t last_pid;       // the last guest pid that ran here
    int32_t renamed_pid;    // the pid iSH renamed this thread for (0: never)
    uint32_t borrows;
    uint64_t last_ms;
};
// A pthread key, not __thread: Darwin allocates a thread's __thread block on
// first touch (malloc), which the crash handler must never do on a thread
// that has not been here; pthread_getspecific allocates nothing. The record
// stays with the thread (a GCD worker keeps its history) and is freed with it.
static pthread_key_t diag_key;
static pthread_once_t diag_key_once = PTHREAD_ONCE_INIT;
static _Atomic bool diag_key_ready;
static void diag_key_create(void) {
    if (pthread_key_create(&diag_key, free) == 0)
        atomic_store(&diag_key_ready, true);
}
static struct diag_thread *diag_self(void) {
    pthread_once(&diag_key_once, diag_key_create);
    if (!atomic_load(&diag_key_ready))
        return NULL;
    struct diag_thread *t = pthread_getspecific(diag_key);
    if (t == NULL && (t = calloc(1, sizeof(*t))) != NULL)
        pthread_setspecific(diag_key, t);
    return t;
}

void ish_diag_thread_own(int pid) {
    ish_diag_add(DIAG_OWN_THREADS, 1);
    struct diag_thread *t = diag_self();
    if (t == NULL)
        return;
    t->role = ROLE_OWN;
    t->last_pid = pid;
    t->last_ms = now_ms();
}

void ish_diag_thread_renamed(int pid) {
    struct diag_thread *t = diag_self();
    if (t == NULL)
        return;
    t->renamed_pid = pid;
    t->last_pid = pid;
    t->last_ms = now_ms();
    if (t->role != ROLE_OWN) {
        ish_diag_add(DIAG_BORROW_RENAMES, 1);
        ish_diag_event(DIAG_EV_BORROW_RENAME, pid, 0);
    }
}

void ish_diag_borrow_begin(void) {
    ish_diag_add(DIAG_BORROWS, 1);
    struct diag_thread *t = diag_self();
    if (t == NULL)
        return;
    t->borrow_depth++;
    t->borrows++;
    t->last_ms = now_ms();
}

void ish_diag_borrow_end(void) {
    struct diag_thread *t = diag_self();
    if (t == NULL)
        return;
    if (current)
        t->last_pid = (int32_t) current->pid;
    if (t->borrow_depth > 0)
        t->borrow_depth--;
    t->last_ms = now_ms();
}

// MARK: - exec / node

void ish_diag_node_mode(int jitless) {
    ish_diag_add(jitless ? DIAG_EXEC_NODE_JITLESS : DIAG_EXEC_NODE_DEFAULT, 1);
    ish_diag_event(DIAG_EV_EXEC, jitless ? 2 : 1, 0);
}

static void group_node(struct tgroup *group, bool is_node) {
    if (group == NULL || is_node == group->diag_node)
        return;
    group->diag_node = is_node;
    int64_t step = is_node ? 1 : -1;
    int64_t live = atomic_fetch_add_explicit(&ish_diag_live_node, step, memory_order_relaxed) + step;
    if (is_node)
        raise_peak(&ish_diag_peak_node, live);
}

void ish_diag_exec(int is_node) {
    ish_diag_add(DIAG_EXECS, 1);
    if (current)
        group_node(current->group, is_node != 0);
}

void ish_diag_group_gone(struct tgroup *group) {
    group_node(group, false);
}

void ish_diag_group_forked(struct tgroup *group) {
    if (group)
        group->diag_node = false;
}

// MARK: - Text

size_t ish_diag_describe(char *buf, size_t size) {
    size_t n = 0;
#define PUT(...) do { if (n < size) { int w = snprintf(buf + n, size - n, __VA_ARGS__); if (w > 0) n += (size_t) w; } } while (0)
    PUT("procs=%lld peak_procs=%lld node=%lld peak_node=%lld",
        (long long) atomic_load(&ish_live_guest_procs), (long long) atomic_load(&ish_diag_peak_procs),
        (long long) atomic_load(&ish_diag_live_node), (long long) atomic_load(&ish_diag_peak_node));
    void (*jit_counts)(uint64_t[6]) = ish_diag_jit_counts;
    if (jit_counts) {
        uint64_t j[6];
        jit_counts(j);
        PUT(" aot_images=%llu aot_rejected=%llu aot_hits=%llu aot_moved=%llu aot_fixed=%llu aot_busy=%llu",
            (unsigned long long) j[0], (unsigned long long) j[1], (unsigned long long) j[2],
            (unsigned long long) j[3], (unsigned long long) j[4], (unsigned long long) j[5]);
    }
    for (int i = 0; i < DIAG_COUNTER_COUNT; i++)
        PUT(" %s=%llu", counter_names[i], (unsigned long long) atomic_load_explicit(&ish_diag_counters[i], memory_order_relaxed));
    PUT("\n");
#undef PUT
    if (n >= size && size > 0)
        n = size - 1;
    return n;
}

// MARK: - Crash time (async-signal-safe)

static char crash_buf[8192];
static size_t crash_len;

static void c_str(const char *s) {
    while (*s && crash_len < sizeof(crash_buf) - 1)
        crash_buf[crash_len++] = *s++;
}
static void c_strn(const char *s, size_t max) {
    for (size_t i = 0; i < max && s[i]; i++) {
        char c = s[i];
        if (crash_len < sizeof(crash_buf) - 1)
            crash_buf[crash_len++] = (c >= 32 && c < 127) ? c : '?';
    }
}
static void c_u64(uint64_t v) {
    char tmp[24];
    int i = 0;
    do { tmp[i++] = (char) ('0' + v % 10); v /= 10; } while (v && i < 24);
    while (i > 0 && crash_len < sizeof(crash_buf) - 1)
        crash_buf[crash_len++] = tmp[--i];
}
static void c_i64(int64_t v) {
    if (v < 0) { c_str("-"); c_u64((uint64_t) 0 - (uint64_t) v); } else c_u64((uint64_t) v);
}
static void c_flush(int fd) {
    size_t off = 0;
    while (off < crash_len) {
        ssize_t w = write(fd, crash_buf + off, crash_len - off);
        if (w <= 0)
            break;
        off += (size_t) w;
    }
    crash_len = 0;
}

void ish_diag_write_crash(int fd) {
    if (fd < 0)
        return;
    crash_len = 0;
    char name[16] = {0};
    pthread_getname_np(pthread_self(), name, sizeof(name));
    uint64_t tid = 0;
#if __APPLE__
    pthread_threadid_np(NULL, &tid);
#endif
    // Who owned the crashing thread. Its record, if it ever ran kernel code;
    // `current` (a __thread) is read only then, when it surely exists.
    struct diag_thread *t = atomic_load(&diag_key_ready) ? pthread_getspecific(diag_key) : NULL;
    c_str("[ish-diag] thread=\""); c_strn(name, sizeof(name));
    c_str("\" tid="); c_u64(tid);
    if (t == NULL) {
        c_str(" role=app in_kernel=no ish_history=none");
    } else {
        c_str(" role="); c_str(t->role == ROLE_OWN ? "ish" : "app");
        c_str(" in_kernel="); c_str(t->role == ROLE_OWN || t->borrow_depth > 0 ? "yes" : "no");
        c_str(" borrow_depth="); c_i64(t->borrow_depth);
        c_str(" borrows="); c_u64(t->borrows);
        c_str(" last_pid="); c_i64(t->last_pid);
        c_str(" renamed_pid="); c_i64(t->renamed_pid);
        c_str(" last_ms="); c_u64(t->last_ms);
        c_str(" current="); c_i64(current ? (int64_t) current->pid : -1);
    }
    c_str(" now_ms="); c_u64(now_ms());
    c_str("\n");
    // Counters (atomic loads only).
    c_str("[ish-diag] procs="); c_i64(atomic_load(&ish_live_guest_procs));
    c_str(" peak_procs="); c_i64(atomic_load(&ish_diag_peak_procs));
    c_str(" node="); c_i64(atomic_load(&ish_diag_live_node));
    c_str(" peak_node="); c_i64(atomic_load(&ish_diag_peak_node));
    void (*jit_counts)(uint64_t[6]) = ish_diag_jit_counts;
    if (jit_counts) {
        uint64_t j[6];
        jit_counts(j);
        static const char *const jn[6] = {" aot_images=", " aot_rejected=", " aot_hits=", " aot_moved=", " aot_fixed=", " aot_busy="};
        for (int i = 0; i < 6; i++) { c_str(jn[i]); c_u64(j[i]); }
    }
    for (int i = 0; i < DIAG_COUNTER_COUNT; i++) {
        uint64_t v = atomic_load_explicit(&ish_diag_counters[i], memory_order_relaxed);
        if (v == 0)
            continue;
        c_str(" "); c_str(counter_names[i]); c_str("="); c_u64(v);
    }
    c_str("\n");
    c_flush(fd);
    // The event ring, oldest first.
    uint64_t head = atomic_load_explicit(&ring_head, memory_order_acquire);
    uint64_t first = head > DIAG_RING ? head - DIAG_RING : 0;
    for (uint64_t idx = first; idx < head; idx++) {
        struct diag_ev *e = &ring[idx % DIAG_RING];
        if (atomic_load_explicit(&e->seq, memory_order_acquire) != idx + 1)
            continue;
        c_str("[ish-diag] ev "); c_u64(e->ms); c_str("ms ");
        c_str(e->type < sizeof(event_names) / sizeof(event_names[0]) && event_names[e->type] ? event_names[e->type] : "?");
        c_str(" pid="); c_i64(e->pid);
        c_str(" thread=\""); c_strn(e->thread, sizeof(e->thread)); c_str("\"");
        c_str(" a="); c_i64(e->a);
        c_str(" b="); c_i64(e->b);
        c_str("\n");
        if (crash_len > sizeof(crash_buf) - 256)
            c_flush(fd);
    }
    c_flush(fd);
}
