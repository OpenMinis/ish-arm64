#include "util/macopt.h"
#if ISH_HVF   // util/macopt.h: nothing at all on iOS, nothing calls in
#include <Hypervisor/Hypervisor.h>
#include <dlfcn.h>
#include <libkern/OSCacheControl.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include "kernel/hvf.h"
#include "kernel/sba.h"
#include "kernel/task.h"
#include "kernel/memory.h"
#include "kernel/vdso.h"
#include "emu/interrupt.h"

// ---- Hypervisor.framework, looked up at run time: nothing to link
#define HV_FUNCS(X) X(hv_vm_create) X(hv_vm_map) X(hv_vm_unmap) X(hv_vcpu_create) X(hv_vcpu_destroy) \
    X(hv_vcpu_run) X(hv_vcpus_exit) X(hv_vcpu_get_reg) X(hv_vcpu_set_reg) X(hv_vcpu_get_sys_reg) \
    X(hv_vcpu_set_sys_reg) X(hv_vcpu_get_simd_fp_reg) X(hv_vcpu_set_simd_fp_reg) X(hv_vcpu_set_vtimer_mask) \
    X(hv_vm_config_create) X(hv_vm_config_get_max_ipa_size) X(hv_vm_config_set_ipa_size)
static struct {
#define X(f) __typeof__(f) *f;
    HV_FUNCS(X)
#undef X
} hv;
#define HV(f) hv.f

// ---- the VM's physical address space
#define K16 0x4000ull
#define BLK_BITS 21                        // handed out 2MB at a time
#define BLK (1ull << BLK_BITS)
#define IPA_STUB 0ull                      // the EL1 code below
#define IPA_PT BLK                         // stage-1 page tables
#define PT_SIZE (256ull << 20)
#define IPA_DYN (IPA_PT + PT_SIZE)         // windows and data objects
#define STUB_VA 0xffffffffffe00000ull      // where EL1 sees the stub (TTBR1)
#define STUB_FLUSH 0x800                   // tlbi aside1is (ASID in FAR_EL1), eret
#define STUB_SYNC_ASID 0x900               // tlbi aside1is (ASID in FAR_EL1), hvc #16
#define STUB_SYNC_ALL 0xa00                // tlbi vmalle1is, hvc #17
#define CPSR_EL1H 0x3c5                    // EL1 with SP_EL1, interrupts masked
#define EC_HVC_ 0x16

// stage-1 descriptors (4KB granule, 48-bit VA)
#define D_VALID 3ull                       // table, or page at level 3
#define D_AF (1ull << 10)
#define D_ISH (3ull << 8)
#define D_NG (1ull << 11)
#define D_EL0 (1ull << 6)
#define D_RO (1ull << 7)
#define D_PXN (1ull << 53)
#define D_UXN (1ull << 54)
#define D_ADDR 0x0000fffffffff000ull
#define USER_PAGE (D_VALID | D_AF | D_ISH | D_NG | D_EL0 | D_PXN)

static pthread_once_t once = PTHREAD_ONCE_INIT;
static int state = -1;
static unsigned ipa_bits;
static bool asid16;
static uint64_t tcr;

// g_lock: the page table arena, the 2MB blocks, ASIDs, window block tables,
// data objects' mappings. Taken after mem->lock and an mm's lock.
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static char *pt_host;
static uint64_t pt_next = IPA_PT, pt_free_list;
static uint64_t *blk_bits, nblk, blk_hint;
static uint16_t asid_next = 1, *asid_free;
static unsigned asid_nfree;
static uint64_t ttbr1, mm_ids;

static _Atomic uint64_t st_runs, st_sys, st_faults, st_fault_slow, st_s2, st_flush, st_kick, st_mrs, st_blocks, st_datamaps,
    st_copies, st_sync;

struct hvf_mm {
    uint64_t root;           // TTBR0 table
    uint16_t asid;
    uint64_t id;
    // bumped when a valid entry was cleared: each vCPU flushes this ASID
    // from the TLBs (all of them, inner shareable) before it runs it again
    _Atomic uint64_t gen;
    pthread_mutex_t lock;    // the page table, after mem->lock
};

struct hvf_thread {
    hv_vcpu_t vcpu;
    hv_vcpu_exit_t *exit;
    uint64_t mm_id, flushed;
    // what the vCPU holds, to only set what the kernel changed
    bool valid;
    uint64_t x[31], sp, tpidr;
    uint64_t fpcr, fpsr;
    union arm64_vreg q[32];
};
static __thread struct hvf_thread *self_t;
static __thread bool no_vcpu;

void hvf_stats(void) {
    if (state != 1)
        return;
    fprintf(stderr, "📊 HVF: runs %llu, syscalls %llu, faults %llu (to the kernel %llu), stage-2 %llu, "
            "flushes %llu (sync %llu), kicks %llu, mrs %llu | blocks %llu, data maps %llu (copies %llu)\n", st_runs,
            st_sys, st_faults, st_fault_slow, st_s2, st_flush, st_sync, st_kick, st_mrs, st_blocks, st_datamaps,
            st_copies);
}

static uint64_t *tbl(uint64_t ipa) {
    return (uint64_t *) (pt_host + (ipa - IPA_PT));
}

// g_lock held
static uint64_t pt_alloc(void) {
    uint64_t t = pt_free_list;
    if (t != 0) {
        pt_free_list = tbl(t)[0];
        tbl(t)[0] = 0;
        return t;
    }
    if (pt_next + 4096 > IPA_PT + PT_SIZE)
        return 0;
    t = pt_next;
    pt_next += 4096;
    return t;
}

static void pt_release(uint64_t t) {
    memset(tbl(t), 0, 4096);
    tbl(t)[0] = pt_free_list;
    pt_free_list = t;
}

// n contiguous 2MB blocks of IPA, 0 if none. g_lock held.
static uint64_t blk_alloc(uint64_t n) {
    for (int pass = 0; pass < 2; pass++) {
        uint64_t run = 0;
        for (uint64_t i = pass ? 0 : blk_hint; i < nblk; i++) {
            if (blk_bits[i / 64] & (1ull << (i % 64))) {
                run = 0;
                continue;
            }
            if (++run < n)
                continue;
            uint64_t first = i + 1 - n;
            for (uint64_t j = first; j <= i; j++)
                blk_bits[j / 64] |= 1ull << (j % 64);
            blk_hint = i + 1;
            return IPA_DYN + (first << BLK_BITS);
        }
    }
    return 0;
}

static void blk_release(uint64_t ipa, uint64_t n) {
    uint64_t first = (ipa - IPA_DYN) >> BLK_BITS;
    for (uint64_t j = first; j < first + n; j++)
        blk_bits[j / 64] &= ~(1ull << (j % 64));
    if (first < blk_hint)
        blk_hint = first;
}

// Entries cleared without dropping them from the TLBs right away (a thread
// with no vCPU cleared them): until a flush of everything, a vCPU may still
// reach the memory they pointed at, so it keeps its guest physical
// addresses. deferred counts such clears, covered how many of them a full
// flush took care of; IPA held back is tagged with deferred.
static _Atomic uint64_t deferred;
static uint64_t covered;
static struct held { uint64_t ipa, n, tag; } *held;
static unsigned n_held, cap_held;
static _Atomic bool any_held;

// g_lock held
static void ipa_release(uint64_t ipa, uint64_t n) {
    uint64_t tag = atomic_load(&deferred);
    if (tag == covered)
        return blk_release(ipa, n);
    if (n_held == cap_held) {
        unsigned cap = cap_held ? cap_held * 2 : 64;
        struct held *h = realloc(held, cap * sizeof(*h));
        if (h == NULL)
            return;   // leaked rather than reused too early
        held = h;
        cap_held = cap;
    }
    held[n_held++] = (struct held) {ipa, n, tag};
    any_held = true;
}

// The entry for va at level 3, creating tables on the way if create (NULL
// when there is none, or no memory for tables). g_lock held if create.
static uint64_t *pte_at(uint64_t root, uint64_t va, bool create) {
    uint64_t t = root;
    for (int lvl = 0; lvl < 3; lvl++) {
        uint64_t *e = &tbl(t)[(va >> (39 - 9 * lvl)) & 511];
        uint64_t d = __atomic_load_n(e, __ATOMIC_ACQUIRE);
        if (!(d & 1)) {
            if (!create)
                return NULL;
            uint64_t n = pt_alloc();
            if (n == 0)
                return NULL;
            d = n | D_VALID;
            __atomic_store_n(e, d, __ATOMIC_RELEASE);
        }
        t = d & D_ADDR;
    }
    return &tbl(t)[(va >> 12) & 511];
}

static bool init_vm(void) {
    void *fw = dlopen("/System/Library/Frameworks/Hypervisor.framework/Hypervisor", RTLD_NOW);
    if (fw == NULL)
        return false;
#define X(f) if ((hv.f = dlsym(fw, #f)) == NULL) return false;
    HV_FUNCS(X)
#undef X
    uint32_t max = 0;
    HV(hv_vm_config_get_max_ipa_size)(&max);
    ipa_bits = max < 40 ? max : 40;
    if (ipa_bits < 36)
        return false;
    hv_vm_config_t cfg = HV(hv_vm_config_create)();
    HV(hv_vm_config_set_ipa_size)(cfg, ipa_bits);
    hv_return_t r = HV(hv_vm_create)(cfg);
    if (r != HV_SUCCESS) {
        if (getenv("ISH_HVF") != NULL)   // asked for it
            fprintf(stderr, "iSH: no Hypervisor.framework VM (%#x)%s; running guest code the other way\n", r,
                    r == HV_DENIED ? ", the com.apple.security.hypervisor entitlement is missing (tools/hvf-sign.sh)" : "");
        return false;
    }
    // EL1: every vector entry leaves to us with its index as the hvc
    // immediate; the flush stub drops an ASID from every TLB and returns.
    uint32_t *stub = mmap(NULL, K16, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    pt_host = mmap(NULL, PT_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
    if (stub == MAP_FAILED || pt_host == MAP_FAILED)
        return false;
    for (unsigned i = 0; i < 16; i++)
        stub[i * 32] = 0xd4000002 | (i << 5);   // hvc #i
    static const uint32_t flush[] = {
        0xd518d080,   // msr tpidr_el1, x0
        0xd5386000,   // mrs x0, far_el1
        0xd5088340,   // tlbi aside1is, x0
        0xd538d080,   // mrs x0, tpidr_el1
        0xd5033b9f,   // dsb ish
        0xd5033fdf,   // isb
        0xd69f03e0,   // eret
    };
    memcpy((char *) stub + STUB_FLUSH, flush, sizeof(flush));
    memcpy((char *) stub + STUB_SYNC_ASID, flush, sizeof(flush));
    stub[(STUB_SYNC_ASID + sizeof(flush)) / 4 - 1] = 0xd4000202;   // hvc #16
    static const uint32_t flush_all[] = {
        0xd508831f,   // tlbi vmalle1is
        0xd5033b9f,   // dsb ish
        0xd5033fdf,   // isb
        0xd4000222,   // hvc #17
    };
    memcpy((char *) stub + STUB_SYNC_ALL, flush_all, sizeof(flush_all));
    if (HV(hv_vm_map)(stub, IPA_STUB, K16, HV_MEMORY_READ | HV_MEMORY_EXEC) != HV_SUCCESS ||
            HV(hv_vm_map)(pt_host, IPA_PT, PT_SIZE, HV_MEMORY_READ | HV_MEMORY_WRITE) != HV_SUCCESS)
        return false;
    ttbr1 = pt_alloc();
    uint64_t *e = pte_at(ttbr1, STUB_VA, true);
    if (e == NULL)
        return false;
    *e = IPA_STUB | D_VALID | D_AF | D_ISH | D_UXN;   // EL1 only, executable there
    nblk = ((1ull << ipa_bits) - IPA_DYN) >> BLK_BITS;
    blk_bits = calloc((nblk + 63) / 64, sizeof(uint64_t));
    asid_free = calloc(65536, sizeof(uint16_t));
    if (blk_bits == NULL || asid_free == NULL)
        return false;
    return true;
}
static void init(void) {
    const char *v = getenv("ISH_HVF");
    state = !(v && v[0] == '0') && init_vm();
}

// On unless ISH_HVF=0, where there is a VM to be had.
bool hvf_on(void) {
    if (state < 0)
        pthread_once(&once, init);
    return state == 1;
}

static struct hvf_thread *thread_get(void) {
    if (self_t != NULL)
        return self_t;
    if (no_vcpu)
        return NULL;
    struct hvf_thread *t = calloc(1, sizeof(*t));
    if (t == NULL || HV(hv_vcpu_create)(&t->vcpu, &t->exit, NULL) != HV_SUCCESS) {
        // at most so many per VM: this thread runs guest code the other way
        free(t);
        no_vcpu = true;
        return NULL;
    }
    hv_vcpu_t v = t->vcpu;
    pthread_mutex_lock(&g_lock);
    if (tcr == 0) {
        uint64_t mmfr0 = 0;
        HV(hv_vcpu_get_sys_reg)(v, HV_SYS_REG_ID_AA64MMFR0_EL1, &mmfr0);
        asid16 = ((mmfr0 >> 4) & 0xf) == 2;
        uint64_t ips = ipa_bits >= 44 ? 4 : ipa_bits >= 42 ? 3 : ipa_bits >= 40 ? 2 : 1;
        // 48-bit VA on both halves, 4KB granule, write-back inner shareable walks
        tcr = 16 | (1ull << 8) | (1ull << 10) | (3ull << 12) | (16ull << 16) | (1ull << 24) | (1ull << 26) |
              (3ull << 28) | (2ull << 30) | (ips << 32) | ((uint64_t) asid16 << 36);
    }
    pthread_mutex_unlock(&g_lock);
    HV(hv_vcpu_set_sys_reg)(v, HV_SYS_REG_MAIR_EL1, 0xff);   // attribute 0: normal, write-back
    HV(hv_vcpu_set_sys_reg)(v, HV_SYS_REG_TCR_EL1, tcr);
    HV(hv_vcpu_set_sys_reg)(v, HV_SYS_REG_TTBR1_EL1, ttbr1);
    HV(hv_vcpu_set_sys_reg)(v, HV_SYS_REG_VBAR_EL1, STUB_VA);
    HV(hv_vcpu_set_sys_reg)(v, HV_SYS_REG_CPACR_EL1, 3ull << 20);   // FP and SIMD at EL0; SVE, SME trap
    // EL0 reads both counters; an event stream wakes WFE
    HV(hv_vcpu_set_sys_reg)(v, HV_SYS_REG_CNTKCTL_EL1, 3 | (1 << 2) | (9 << 4));
    // MMU and caches on; EL0 may use DC ZVA, CTR_EL0, WFI/WFE and cache
    // maintenance (UCI) as on Linux; SP alignment checked at EL0
    uint64_t sctlr = 1 | (1 << 2) | (1 << 4) | (1 << 11) | (1 << 12) | (1 << 14) | (1 << 15) | (1 << 16) |
                     (1 << 18) | (1 << 20) | (1 << 22) | (1 << 23) | (1 << 26) | (1 << 28) | (1 << 29);
    HV(hv_vcpu_set_sys_reg)(v, HV_SYS_REG_SCTLR_EL1, sctlr);
    HV(hv_vcpu_set_vtimer_mask)(v, true);
    self_t = t;
    return t;
}

void hvf_thread_exit(void) {
    struct hvf_thread *t = self_t;
    if (t == NULL)
        return;
    if (current != NULL)
        current->hvf_vcpu = 0;
    self_t = NULL;
    HV(hv_vcpu_destroy)(t->vcpu);
    free(t);
}

void hvf_kick(struct task *task) {
    uint64_t v = atomic_load(&task->hvf_vcpu);
    if (v == 0 || state != 1)
        return;
    hv_vcpu_t c = v - 1;
    st_kick++;
    HV(hv_vcpus_exit)(&c, 1);
}

static struct hvf_mm *mm_get(struct mem *mem) {
    struct hvf_mm *m = __atomic_load_n(&mem->hvf, __ATOMIC_ACQUIRE);
    if (m != NULL)
        return m;
    pthread_mutex_lock(&g_lock);
    m = mem->hvf;
    if (m == NULL) {
        uint16_t max = asid16 ? 65535 : 255;
        uint16_t asid = asid_next <= max ? asid_next++ : asid_nfree ? asid_free[--asid_nfree] : 0;
        uint64_t root = asid ? pt_alloc() : 0;
        m = root ? calloc(1, sizeof(*m)) : NULL;
        if (m != NULL) {
            m->root = root;
            m->asid = asid;
            m->id = ++mm_ids;
            pthread_mutex_init(&m->lock, NULL);
            __atomic_store_n(&mem->hvf, m, __ATOMIC_RELEASE);
        } else {
            if (root)
                pt_release(root);
            if (asid)
                asid_free[asid_nfree++] = asid;
        }
    }
    pthread_mutex_unlock(&g_lock);
    return m;
}

static void tree_free(uint64_t t, int lvl) {
    if (lvl < 3) {
        for (unsigned i = 0; i < 512; i++) {
            uint64_t d = tbl(t)[i];
            if (d & 1)
                tree_free(d & D_ADDR, lvl + 1);
        }
    }
    pt_release(t);
}

void hvf_mm_destroy(struct mem *mem) {
    struct hvf_mm *m = mem->hvf;
    if (m == NULL)
        return;
    // ISH_HVF_STATS=1: the counts so far, as each address space goes
    static int stats = -1;
    if (stats < 0) {
        const char *v = getenv("ISH_HVF_STATS");
        stats = v && v[0] == '1';
    }
    if (stats)
        hvf_stats();
    mem->hvf = NULL;
    pthread_mutex_lock(&g_lock);
    tree_free(m->root, 0);
    // a vCPU flushes an ASID as it first runs an address space (thread_enter())
    asid_free[asid_nfree++] = m->asid;
    pthread_mutex_unlock(&g_lock);
    pthread_mutex_destroy(&m->lock);
    free(m);
}

// Drop asid's entries (all, if asid < 0) from the TLBs of every core before
// going on, as Linux does before munmap() or mprotect() return: the calling
// thread's vCPU runs a stub that does it. false: this thread has no vCPU.
static bool sync_flush(int asid) {
    struct hvf_thread *t = self_t;
    if (t == NULL)
        return false;
    hv_vcpu_t v = t->vcpu;
    uint64_t at = STUB_VA + (asid >= 0 ? STUB_SYNC_ASID : STUB_SYNC_ALL);
    for (;;) {
        if (asid >= 0)
            HV(hv_vcpu_set_sys_reg)(v, HV_SYS_REG_FAR_EL1, (uint64_t) asid << 48);
        HV(hv_vcpu_set_reg)(v, HV_REG_PC, at);
        HV(hv_vcpu_set_reg)(v, HV_REG_CPSR, CPSR_EL1H);
        if (HV(hv_vcpu_run)(v) != HV_SUCCESS)
            return false;
        hv_vcpu_exit_t *x = t->exit;
        if (x->reason == HV_EXIT_REASON_EXCEPTION && (x->exception.syndrome >> 26) == EC_HVC_)
            break;
        // a kick came first (it stays poked): x0 may be parked in TPIDR_EL1
        uint64_t pc;
        HV(hv_vcpu_get_reg)(v, HV_REG_PC, &pc);
        if (asid >= 0 && pc - at >= 4 && pc - at <= 12) {
            uint64_t x0;
            HV(hv_vcpu_get_sys_reg)(v, HV_SYS_REG_TPIDR_EL1, &x0);
            HV(hv_vcpu_set_reg)(v, HV_REG_X0, x0);
        }
    }
    st_sync++;
    return true;
}

// Before a vCPU runs: if IPA is held back, flush everything and let it go.
static void release_held(void) {
    if (!atomic_load(&any_held))
        return;
    uint64_t tag = atomic_load(&deferred);
    if (!sync_flush(-1))
        return;
    pthread_mutex_lock(&g_lock);
    if (tag > covered)
        covered = tag;
    unsigned keep = 0;
    for (unsigned i = 0; i < n_held; i++) {
        if (held[i].tag <= covered)
            blk_release(held[i].ipa, held[i].n);
        else
            held[keep++] = held[i];
    }
    n_held = keep;
    any_held = keep != 0;
    pthread_mutex_unlock(&g_lock);
}

void hvf_invalidate(struct mem *mem, page_t start, pages_t pages) {
    struct hvf_mm *m = mem->hvf;
    if (m == NULL || pages == 0)
        return;
    bool cleared = false;
    pthread_mutex_lock(&m->lock);
    for (uint64_t p = start, end = start + pages; p < end && p < (1ull << 36);) {
        uint64_t t = m->root;
        int lvl;
        for (lvl = 0; lvl < 3; lvl++) {
            uint64_t d = tbl(t)[(p >> (27 - 9 * lvl)) & 511];
            if (!(d & 1))
                break;
            t = d & D_ADDR;
        }
        if (lvl < 3) {   // nothing below this entry
            uint64_t span = 1ull << (27 - 9 * lvl);
            p = (p & ~(span - 1)) + span;
            continue;
        }
        for (uint64_t *e = tbl(t); p < end; p++) {
            if (e[p & 511]) {
                __atomic_store_n(&e[p & 511], 0, __ATOMIC_RELEASE);
                cleared = true;
            }
            if ((p & 511) == 511) {
                p++;
                break;
            }
        }
    }
    if (cleared) {
        __asm__ volatile("dsb ish" ::: "memory");
        if (!sync_flush(m->asid)) {
            // each vCPU flushes the ASID as it next runs it, the IPA
            // pages pointed at waits for a flush of everything
            atomic_fetch_add(&m->gen, 1);
            atomic_fetch_add(&deferred, 1);
        }
    }
    pthread_mutex_unlock(&m->lock);
}

void hvf_region_remap(struct sba_region *r, uint64_t a, uint64_t b) {
    if (r->hvf_blk == NULL || b <= a)
        return;
    a &= ~(K16 - 1);
    b = (b + K16 - 1) & ~(K16 - 1);
    pthread_mutex_lock(&g_lock);
    for (uint64_t x = a; x < b;) {
        uint64_t blk = x >> BLK_BITS, next = (blk + 1) << BLK_BITS, hi = next < b ? next : b;
        uint32_t ipa_blk = r->hvf_blk[blk];
        if (ipa_blk != 0) {
            uint64_t ipa = ((uint64_t) ipa_blk << BLK_BITS) + (x & (BLK - 1));
            HV(hv_vm_unmap)(ipa, hi - x);
            HV(hv_vm_map)(r->base + x, ipa, hi - x, HV_MEMORY_READ | HV_MEMORY_WRITE | HV_MEMORY_EXEC);
        }
        x = hi;
    }
    pthread_mutex_unlock(&g_lock);
}

void hvf_region_free(struct sba_region *r) {
    if (r->hvf_blk == NULL)
        return;
    pthread_mutex_lock(&g_lock);
    for (uint64_t blk = 0; blk < (SBA_SPAN >> BLK_BITS); blk++) {
        if (r->hvf_blk[blk] == 0)
            continue;
        uint64_t ipa = (uint64_t) r->hvf_blk[blk] << BLK_BITS;
        HV(hv_vm_unmap)(ipa, BLK);
        blk_release(ipa, 1);
    }
    pthread_mutex_unlock(&g_lock);
    free(r->hvf_blk);
    r->hvf_blk = NULL;
}

// The vdso is in our data segment, not on a page boundary: the VM gets a
// copy that is (it never changes).
static char *vdso_copy;

// The host memory the VM is given for d.
static char *data_host(struct data *d) {
    if (d->hvf_copy != NULL)
        return d->hvf_copy;
    if (d->data != vdso_data)
        return d->data;
    if (vdso_copy == NULL) {
        char *c = mmap(NULL, sizeof(vdso_data), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        if (c == MAP_FAILED)
            return NULL;
        memcpy(c, vdso_data, sizeof(vdso_data));
        vdso_copy = c;
    }
    return vdso_copy;
}

static void data_range(struct data *d, uintptr_t *lo, uintptr_t *hi) {
    uintptr_t h = (uintptr_t) data_host(d);
    *lo = h & ~(uintptr_t) (K16 - 1);
    *hi = (h + d->size + K16 - 1) & ~(uintptr_t) (K16 - 1);
}

void hvf_data_free(struct data *d) {
    if (d->hvf_ipa == 0)
        return;
    uintptr_t lo, hi;
    pthread_mutex_lock(&g_lock);
    data_range(d, &lo, &hi);
    uint64_t ipa = d->hvf_ipa - ((uintptr_t) data_host(d) - lo);
    HV(hv_vm_unmap)(ipa, hi - lo);
    ipa_release(ipa, (hi - lo + BLK - 1) >> BLK_BITS);
    pthread_mutex_unlock(&g_lock);
    if (d->hvf_copy != NULL)
        munmap((void *) lo, hi - lo);
    d->hvf_ipa = 0;
    d->hvf_copy = NULL;
}

// Where the VM sees the guest page e maps, 0 if nowhere. g_lock held.
static uint64_t page_ipa(struct mem *mem, struct pt_entry *e) {
    struct sba_region *r = mem->sba;
    char *h = (char *) e->data->data + e->offset;
    if (r != NULL && e->data->sba == r && h >= r->base && h < r->base + SBA_SPAN) {
        uint64_t off = (uint64_t) (h - r->base), blk = off >> BLK_BITS;
        if (r->hvf_blk == NULL && (r->hvf_blk = calloc(SBA_SPAN >> BLK_BITS, sizeof(uint32_t))) == NULL)
            return 0;
        if (r->hvf_blk[blk] == 0) {
            uint64_t ipa = blk_alloc(1);
            if (ipa == 0)
                return 0;
            if (HV(hv_vm_map)(r->base + (blk << BLK_BITS), ipa, BLK,
                              HV_MEMORY_READ | HV_MEMORY_WRITE | HV_MEMORY_EXEC) != HV_SUCCESS) {
                blk_release(ipa, 1);
                return 0;
            }
            r->hvf_blk[blk] = (uint32_t) (ipa >> BLK_BITS);
            st_blocks++;
        }
        return ((uint64_t) r->hvf_blk[blk] << BLK_BITS) + (off & (BLK - 1));
    }
    // Anything else: the data object's host memory, as a whole.
    struct data *d = e->data;
    if (d->hvf_ipa == 0) {
        if (data_host(d) == NULL)
            return 0;
        uintptr_t lo, hi;
        data_range(d, &lo, &hi);
        uint64_t n = (hi - lo + BLK - 1) >> BLK_BITS;
        uint64_t ipa = blk_alloc(n);
        if (ipa == 0)
            return 0;
        // as the host has it: a read-only file mapping stays read-only
        if (HV(hv_vm_map)((void *) lo, ipa, hi - lo, HV_MEMORY_READ | HV_MEMORY_WRITE | HV_MEMORY_EXEC) != HV_SUCCESS &&
                HV(hv_vm_map)((void *) lo, ipa, hi - lo, HV_MEMORY_READ | HV_MEMORY_EXEC) != HV_SUCCESS) {
            // A shared mapping of a file opened read-only: the VM refuses
            // memory it could never be given write access to. The guest
            // cannot write it either: show the VM a copy. (It then misses
            // what others write to the file later.)
            char *c = mmap(NULL, hi - lo, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
            if (c == MAP_FAILED) {
                blk_release(ipa, n);
                return 0;
            }
            memcpy(c + ((uintptr_t) d->data - lo), d->data, d->size);
            if (HV(hv_vm_map)(c, ipa, hi - lo, HV_MEMORY_READ | HV_MEMORY_EXEC) != HV_SUCCESS) {
                munmap(c, hi - lo);
                blk_release(ipa, n);
                return 0;
            }
            d->hvf_copy = c + ((uintptr_t) d->data - lo);
            lo = (uintptr_t) c;
            st_copies++;
        }
        d->hvf_ipa = ipa + ((uintptr_t) data_host(d) - lo);
        st_datamaps++;
    }
    return d->hvf_ipa + e->offset;
}

// Point page's entry where e says, with what its flags allow. Any mapped
// page can be read (as mem_ptr() has it: PROT_NONE pages, and the vdso,
// mapped with no flags at all) and run: ish maps ELF segments without
// P_EXEC, and the JIT never checked. A page is executable once something
// ran there (exec), or when mapped so; only then do the instruction caches
// have to forget what the page held before, written by the host (read in,
// copied) or not. m->lock and g_lock held.
static bool put_page(struct hvf_mm *m, struct mem *mem, page_t page, struct pt_entry *e, bool exec) {
    uint64_t ipa = page_ipa(mem, e);
    if (ipa == 0)
        return false;
    uint64_t *pte = pte_at(m->root, (uint64_t) page << PAGE_BITS, true);
    if (pte == NULL)
        return false;
    exec = exec || (e->flags & P_EXEC) || (*pte != 0 && !(*pte & D_UXN));
    uint64_t d = (ipa & D_ADDR) | USER_PAGE | (P_WRITABLE(e->flags) ? 0 : D_RO) | (exec ? 0 : D_UXN);
    if (*pte == d)
        return true;
    if (exec && (*pte == 0 || (*pte & D_UXN)) && e->data->data != vdso_data)
        sys_icache_invalidate((char *) e->data->data + e->offset, PAGE_SIZE);
    __atomic_store_n(pte, d, __ATOMIC_RELEASE);
    return true;
}

#define AROUND 16   // pages around a fault mapped with it

// A fault at va (a store, or an instruction fetch, if so): 1 if its page can
// be mapped now (it then is), 0 for the kernel to decide.
static int fault_in(struct hvf_mm *m, struct mem *mem, uint64_t va, bool write, bool exec) {
    if (va >> 48)
        return 0;
    page_t page = va >> PAGE_BITS;
    int ok = 0;
    read_wrlock(&mem->lock);
    struct pt_entry *e = mem_pt(mem, page);
    if (e != NULL && (!write || P_WRITABLE(e->flags))) {
        pthread_mutex_lock(&m->lock);
        pthread_mutex_lock(&g_lock);
        ok = put_page(m, mem, page, e, exec);
        // and the pages around it in the same table, as they are now (code
        // around code)
        for (int dir = -1; ok && dir <= 1; dir += 2) {
            for (page_t q = page + dir, n = 0; n < AROUND && (q >> 9) == (page >> 9); q += dir, n++) {
                struct pt_entry *f = mem_pt(mem, q);
                if (f == NULL)
                    break;
                uint64_t *pte = pte_at(m->root, (uint64_t) q << PAGE_BITS, false);
                if (pte != NULL && *pte != 0)
                    continue;
                if (!put_page(m, mem, q, f, exec))
                    break;
            }
        }
        pthread_mutex_unlock(&g_lock);
        pthread_mutex_unlock(&m->lock);
    }
    read_wrunlock(&mem->lock);
    if (ok)
        __asm__ volatile("dsb ish" ::: "memory");
    return ok;
}

// The guest's registers into the vCPU: only what changed since it had them.
static void regs_in(struct hvf_thread *t, struct cpu_state *cpu) {
    hv_vcpu_t v = t->vcpu;
    bool all = !t->valid;
    for (int i = 0; i < 31; i++) {
        if (all || cpu->regs[i] != t->x[i]) {
            HV(hv_vcpu_set_reg)(v, HV_REG_X0 + i, cpu->regs[i]);
            t->x[i] = cpu->regs[i];
        }
    }
    if (all || cpu->sp != t->sp)
        HV(hv_vcpu_set_sys_reg)(v, HV_SYS_REG_SP_EL0, t->sp = cpu->sp);
    if (all || cpu->tls_ptr != t->tpidr)
        HV(hv_vcpu_set_sys_reg)(v, HV_SYS_REG_TPIDR_EL0, t->tpidr = cpu->tls_ptr);
    if (all || cpu->fpcr != t->fpcr)
        HV(hv_vcpu_set_reg)(v, HV_REG_FPCR, t->fpcr = cpu->fpcr);
    if (all || cpu->fpsr != t->fpsr)
        HV(hv_vcpu_set_reg)(v, HV_REG_FPSR, t->fpsr = cpu->fpsr);
    for (int i = 0; i < 32; i++) {
        if (all || cpu->fp[i].q != t->q[i].q) {
            hv_simd_fp_uchar16_t q;
            memcpy(&q, &cpu->fp[i], 16);
            HV(hv_vcpu_set_simd_fp_reg)(v, HV_SIMD_FP_REG_Q0 + i, q);
            t->q[i] = cpu->fp[i];
        }
    }
    t->valid = true;
}

// And back, for the kernel.
static void regs_out(struct hvf_thread *t, struct cpu_state *cpu, uint64_t pc, uint64_t pstate) {
    hv_vcpu_t v = t->vcpu;
    for (int i = 0; i < 31; i++)
        HV(hv_vcpu_get_reg)(v, HV_REG_X0 + i, &t->x[i]);
    memcpy(cpu->regs, t->x, sizeof(t->x));
    HV(hv_vcpu_get_sys_reg)(v, HV_SYS_REG_SP_EL0, &t->sp);
    HV(hv_vcpu_get_sys_reg)(v, HV_SYS_REG_TPIDR_EL0, &t->tpidr);
    HV(hv_vcpu_get_reg)(v, HV_REG_FPCR, &t->fpcr);
    HV(hv_vcpu_get_reg)(v, HV_REG_FPSR, &t->fpsr);
    cpu->sp = t->sp;
    cpu->tls_ptr = t->tpidr;
    cpu->fpcr = (uint32_t) t->fpcr;
    cpu->fpsr = (uint32_t) t->fpsr;
    for (int i = 0; i < 32; i++) {
        hv_simd_fp_uchar16_t q;
        HV(hv_vcpu_get_simd_fp_reg)(v, HV_SIMD_FP_REG_Q0 + i, &q);
        memcpy(&t->q[i], &q, 16);
    }
    memcpy(cpu->fp, t->q, sizeof(t->q));
    cpu->pc = pc;
    arm64_set_nzcv(cpu, (uint32_t) pstate & 0xf0000000);
}

static void set_reg(struct hvf_thread *t, unsigned rt, uint64_t val) {
    if (rt >= 31)
        return;
    HV(hv_vcpu_set_reg)(t->vcpu, HV_REG_X0 + rt, val);
    t->x[rt] = val;
}

// Run the guest at pc: through the flush stub when this ASID may still be
// in the TLBs with entries since cleared.
static void enter(struct hvf_thread *t, struct hvf_mm *m, uint64_t pc, uint64_t pstate) {
    hv_vcpu_t v = t->vcpu;
    if (t->mm_id != m->id) {
        HV(hv_vcpu_set_sys_reg)(v, HV_SYS_REG_TTBR0_EL1, m->root | ((uint64_t) m->asid << 48));
        t->mm_id = m->id;
        t->flushed = UINT64_MAX;   // the ASID may have been someone else's
    }
    uint64_t gen = atomic_load(&m->gen);
    if (t->flushed != gen) {
        HV(hv_vcpu_set_sys_reg)(v, HV_SYS_REG_FAR_EL1, (uint64_t) m->asid << 48);
        HV(hv_vcpu_set_sys_reg)(v, HV_SYS_REG_ELR_EL1, pc);
        HV(hv_vcpu_set_sys_reg)(v, HV_SYS_REG_SPSR_EL1, pstate);
        HV(hv_vcpu_set_reg)(v, HV_REG_PC, STUB_VA + STUB_FLUSH);
        HV(hv_vcpu_set_reg)(v, HV_REG_CPSR, CPSR_EL1H);
        t->flushed = gen;
        st_flush++;
    } else {
        HV(hv_vcpu_set_reg)(v, HV_REG_PC, pc);
        HV(hv_vcpu_set_reg)(v, HV_REG_CPSR, pstate);
    }
}

// Where EL0 is after an exit that did not come through the vectors.
// Where EL0 is after an exit that did not come through the vectors: -1. Or
// the index of the vector EL0 just took an exception to, if the exit (a
// kick) came before its hvc: ELR_EL1 and the rest then say what it was --
// the svc of a system call, which must not be lost.
static int where(struct hvf_thread *t, uint64_t *pc, uint64_t *pstate) {
    hv_vcpu_t v = t->vcpu;
    uint64_t cpsr, at;
    HV(hv_vcpu_get_reg)(v, HV_REG_CPSR, &cpsr);
    if ((cpsr & 0xf) == 0) {
        HV(hv_vcpu_get_reg)(v, HV_REG_PC, pc);
        *pstate = cpsr;
        return -1;
    }
    HV(hv_vcpu_get_reg)(v, HV_REG_PC, &at);
    if (at >= STUB_VA && at < STUB_VA + 16 * 0x80)
        return (int) ((at - STUB_VA) >> 7);
    // In the flush stub: EL0 has not run, the flush may not have. It keeps
    // x0 in TPIDR_EL1 from its first instruction to its fourth.
    uint64_t off = at - (STUB_VA + STUB_FLUSH);
    if (off >= 4 && off <= 12) {
        uint64_t x0;
        HV(hv_vcpu_get_sys_reg)(v, HV_SYS_REG_TPIDR_EL1, &x0);
        set_reg(t, 0, x0);
    }
    HV(hv_vcpu_get_sys_reg)(v, HV_SYS_REG_ELR_EL1, pc);
    HV(hv_vcpu_get_sys_reg)(v, HV_SYS_REG_SPSR_EL1, pstate);
    t->flushed = UINT64_MAX;
    return -1;
}

static uint32_t insn_at(struct mem *mem, uint64_t pc) {
    uint32_t insn = 0;
    read_wrlock(&mem->lock);
    struct pt_entry *e = mem_pt(mem, PAGE(pc));
    if (e != NULL)
        memcpy(&insn, (char *) e->data->data + e->offset + PGOFFSET(pc), 4);
    read_wrunlock(&mem->lock);
    return insn;
}

// The system registers EL0 cannot read here, with the values the gadgets
// give (asbestos/guest-arm64/gadgets-aarch64/math.S, mrs_sysreg). false: not
// one of them.
static bool emulate_mrs(struct hvf_thread *t, uint32_t insn, uint64_t *pstate) {
    if ((insn & 0xfff00000) != 0xd5300000)
        return false;
    unsigned key = (insn >> 5) & 0x7fff;   // op0:op1:CRn:CRm:op2
    unsigned rt = insn & 0x1f;
#define SYSREG(op0, op1, crn, crm, op2) (((op0 & 1) << 14) | (op1 << 11) | (crn << 7) | (crm << 3) | op2)
    switch (key) {
        case SYSREG(3, 0, 0, 4, 0):   // ID_AA64PFR0_EL1: EL0 and EL1 AArch64
            set_reg(t, rt, 0x11);
            return true;
        case SYSREG(3, 0, 0, 4, 1):   // ID_AA64PFR1_EL1
        case SYSREG(3, 0, 0, 4, 4):   // ID_AA64ZFR0_EL1
        case SYSREG(3, 0, 0, 6, 0):   // ID_AA64ISAR0_EL1
        case SYSREG(3, 0, 0, 6, 1):   // ID_AA64ISAR1_EL1
            set_reg(t, rt, 0);
            return true;
        case SYSREG(3, 3, 2, 4, 0):   // RNDR, RNDRRS: none, Z set
        case SYSREG(3, 3, 2, 4, 1):
            set_reg(t, rt, 0);
            *pstate = (*pstate & ~0xf0000000ull) | 0x40000000;
            return true;
    }
#undef SYSREG
    return false;
}

// Fault on the stage-2 side: a data object or block whose host pages the VM
// lost (or never had writable). 1: run it again.
static int s2_fault(uint64_t ipa) {
    (void) ipa;
    st_s2++;
    return 0;
}

#define EC_UNKNOWN 0x00
#define EC_WFX 0x01
#define EC_SVC 0x15
#define EC_HVC 0x16
#define EC_SYSREG 0x18
#define EC_IABT 0x20
#define EC_PC_ALIGN 0x22
#define EC_DABT 0x24
#define EC_SP_ALIGN 0x26
#define EC_BRK 0x3c

int hvf_run(struct cpu_state *cpu) {
    struct hvf_thread *t = thread_get();
    if (t == NULL)
        return HVF_NO;
    struct mem *mem = current->mem;
    struct hvf_mm *m = mm_get(mem);
    if (m == NULL)
        return HVF_NO;
    current->hvf_vcpu = t->vcpu + 1;
    st_runs++;
    regs_in(t, cpu);
    uint64_t pc = cpu->pc, pstate = cpu->nzcv & 0xf0000000;
    uint64_t last_pc = 1, last_far = 1;
    unsigned same = 0;
    for (;;) {
        // seq_cst both ways with cpu_poke() and hvf_kick(): either we see
        // the poke here, or they see the vCPU and kick it
        if (__atomic_exchange_n(cpu->poked_ptr, false, __ATOMIC_SEQ_CST)) {
            regs_out(t, cpu, pc, pstate);
            return INT_TIMER;
        }
        release_held();
        enter(t, m, pc, pstate);
        if (HV(hv_vcpu_run)(t->vcpu) != HV_SUCCESS) {
            fprintf(stderr, "iSH: hv_vcpu_run failed at %#llx\n", (unsigned long long) pc);
            regs_out(t, cpu, pc, pstate);
            return INT_GPF;
        }
        hv_vcpu_exit_t *x = t->exit;
        uint64_t syn = x->exception.syndrome;
        unsigned ec2 = syn >> 26;
        int vec;
        if (x->reason != HV_EXIT_REASON_EXCEPTION) {
            // a kick (cpu_poke()), or the timer we never asked for
            vec = where(t, &pc, &pstate);
            if (vec < 0) {
                if (x->reason == HV_EXIT_REASON_CANCELED) {
                    regs_out(t, cpu, pc, pstate);
                    return INT_TIMER;
                }
                continue;
            }
            // on its way through a vector: that first (the poke stays)
        } else if (ec2 == EC_HVC) {
            vec = syn & 0xffff;
        } else {
            // trapped to the hypervisor itself
            where(t, &pc, &pstate);
            if (ec2 == EC_WFX) {
                pc += 4;
                sched_yield();
                continue;
            }
            if (ec2 == EC_SYSREG) {   // a register the hypervisor keeps: reads as zero
                if (syn & 1)
                    set_reg(t, (syn >> 5) & 0x1f, 0);
                pc += 4;
                continue;
            }
            if ((ec2 == EC_DABT || ec2 == EC_IABT) && s2_fault(x->exception.physical_address))
                continue;
            regs_out(t, cpu, pc, pstate);
            cpu->segfault_addr = x->exception.virtual_address;
            cpu->segfault_was_write = ec2 == EC_DABT && (syn & (1 << 6));
            cpu->segfault_block_pc = cpu->segfault_precise_pc = pc;
            return INT_GPF;
        }
        // through the vectors: what EL0 took to EL1
        hv_vcpu_t v = t->vcpu;
        HV(hv_vcpu_get_sys_reg)(v, HV_SYS_REG_ELR_EL1, &pc);
        HV(hv_vcpu_get_sys_reg)(v, HV_SYS_REG_SPSR_EL1, &pstate);
        if (vec != 8) {
            if (vec < 8) {   // in the stub itself: start over
                fprintf(stderr, "iSH: hvf: exception %u at EL1\n", vec);
                t->flushed = UINT64_MAX;
            }
            continue;        // an interrupt: none are routed here
        }
        uint64_t esr, far;
        HV(hv_vcpu_get_sys_reg)(v, HV_SYS_REG_ESR_EL1, &esr);
        unsigned ec = (esr >> 26) & 0x3f;
        if (ec == EC_SVC) {
            st_sys++;
            regs_out(t, cpu, pc, pstate);
            return INT_SYSCALL;
        }
        if (ec == EC_DABT || ec == EC_IABT) {
            HV(hv_vcpu_get_sys_reg)(v, HV_SYS_REG_FAR_EL1, &far);
            unsigned fsc = esr & 0x3f, kind = fsc >> 2;
            bool write = ec == EC_DABT && (esr & (1 << 6)) && !(esr & (1 << 8));
            st_faults++;
            // translation, access flag or permission: map it if allowed,
            // unless that already did not help here
            if (pc == last_pc && far == last_far) {
                same++;
                t->flushed = UINT64_MAX;   // mapped and still faulting: a stale TLB entry?
            } else {
                same = 0;
            }
            last_pc = pc;
            last_far = far;
            if (kind >= 1 && kind <= 3 && same < 4 && fault_in(m, mem, far, write, ec == EC_IABT))
                continue;
            st_fault_slow++;
            regs_out(t, cpu, pc, pstate);
            cpu->segfault_addr = far;
            cpu->segfault_was_write = write;
            cpu->segfault_block_pc = cpu->segfault_precise_pc = pc;
            return INT_GPF;
        }
        if (ec == EC_UNKNOWN || ec == EC_SYSREG) {
            if (emulate_mrs(t, insn_at(mem, pc), &pstate)) {
                st_mrs++;
                pc += 4;
                continue;
            }
            regs_out(t, cpu, pc, pstate);
            return INT_UNDEFINED;
        }
        if (ec == EC_BRK) {
            regs_out(t, cpu, pc, pstate);
            return INT_BREAKPOINT;
        }
        if (ec == EC_PC_ALIGN || ec == EC_SP_ALIGN) {
            HV(hv_vcpu_get_sys_reg)(v, HV_SYS_REG_FAR_EL1, &far);
            regs_out(t, cpu, pc, pstate);
            cpu->segfault_addr = ec == EC_PC_ALIGN ? pc : far;
            cpu->segfault_was_write = false;
            cpu->segfault_block_pc = cpu->segfault_precise_pc = pc;
            return INT_GPF;
        }
        // SVE, SME, FP exception traps, illegal state, ...
        regs_out(t, cpu, pc, pstate);
        return INT_UNDEFINED;
    }
}

#endif
