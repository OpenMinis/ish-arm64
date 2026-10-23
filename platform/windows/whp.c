// A Windows Hypervisor Platform VM that runs x86_64 guest code as is
// (asbestos/guest-x86_64/vm.c), all of ish's address spaces in it as an
// operating system has its processes: a Windows process can map memory into
// one partition only. Each address space (struct px_vm) has a slot of guest
// physical memory, 4GB at SLOT_GPA, that is its window, and page tables of
// its own that map the low 4GB of addresses there, user and writable; the
// host's protection of the window decides what faults. A processor runs one
// address space at a time, with its page tables in cr3.
//
// Every slot's page tables also map 4GB up to our own pages, supervisor only,
// at guest physical 0: the page tables, and for each processor a syscall stub
// and its mailbox (px.h). A syscall goes to the stub, which hands it to ish
// through the mailbox without leaving the VM; exceptions and accesses the
// host's protection refuses do leave it.
//
// Each processor is run by a thread of its own, so that the thread whose task
// it runs is free to wait on the mailbox while the guest runs.
//
// WinHvPlatform.dll is loaded at run time: it is only there with Hyper-V.
#include "px_impl.h"
#include <winhvplatform.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#define GB (1ull << 30)
#define VM_VPS 64
#define SLOTS 64
#define SLOT_GPA(s) ((uint64_t) ((s) + 1) * 4 * GB)
#define HV_VA (4 * GB)                  // where our pages are, at guest physical 0
// our pages: each slot's page tables, then a stub page and a mailbox page for
// each processor
#define HV_PT(s) ((size_t) (s) * 0x2000)
#define HV_VP(vp) (HV_PT(SLOTS) + (size_t) (vp) * 0x2000)
#define HV_SIZE HV_VP(VM_VPS)
// A window goes into the VM a chunk at a time, as the guest first touches it:
// mapping all 4GB costs some 60ms, and as much again to unmap.
#ifndef CHUNK
#define CHUNK (8ull << 20)
#endif
#define CHUNKS (4 * GB / CHUNK)
// The pauses the host waits on a mailbox before sleeping. (Adapting it to how
// close together a thread's syscalls come woke the guest's stub more often,
// and gained nothing.)
#define HOST_SPIN 4000
#ifndef GUEST_SPIN
#define GUEST_SPIN "2000000"   // the stub's loops before it stops waiting
#endif
#define PORT_WAIT 0xe9
#define PORT_FULL 0xea

#define FN(name) static __typeof__(name) *p_##name
FN(WHvGetCapability);
FN(WHvCreatePartition);
FN(WHvSetPartitionProperty);
FN(WHvSetupPartition);
FN(WHvMapGpaRange);
FN(WHvUnmapGpaRange);
FN(WHvCreateVirtualProcessor);
FN(WHvSetVirtualProcessorRegisters);
FN(WHvGetVirtualProcessorRegisters);
FN(WHvRunVirtualProcessor);
FN(WHvCancelRunVirtualProcessor);
FN(WHvAdviseGpaRange);
FN(WHvGetVirtualProcessorCounters);
#undef FN

// Pages above the window are mapped one at a time, as the guest faults on
// them: page tables from a pool of the slot's, and the host memory behind
// them put in guest physical memory (HIGH_GPA up) EXTENT at a time.
#define PTPOOL_PAGES 2048
#define PTPOOL_GPA(s) ((1ull << 30) + (uint64_t) (s) * PTPOOL_PAGES * 0x1000)
#define HIGH_GPA(s) ((16ull << 40) + (uint64_t) (s) * (256ull << 30))
#define EXTENT (2ull << 20)
#define EXTENTS 4096
struct extent {
    uintptr_t host;                // 0: none
    size_t len;
    uint64_t gpa;
};

struct px_vm {
    int slot;
    uint8_t *window;               // mapped (as far as chunk_mapped says), NULL: none
    uint8_t chunk_mapped[CHUNKS];
    uint8_t *ptpool;               // page tables for above the window
    unsigned ptused;
    uint64_t gpa_next;
    struct extent extents[EXTENTS];
    unsigned nextents;
    volatile LONG pt_gen;          // bumped when a page table entry goes: TLBs to flush
    volatile bool hold;            // its processors stay stopped (px_vm_hold)
    SRWLOCK hold_lock;             // hold, and px_vm_wait running one again
};

struct vp {
    int index;
    volatile bool made, making, in_use;
    struct px_vm *vm;              // the address space it runs
    HANDLE go, stopped;            // auto-reset events
    volatile LONG running;
    WHV_RUN_VP_EXIT_CONTEXT exit;
    bool step_out;                 // stopped at PORT_WAIT: step over it
    HRESULT hr;
    // Setting a segment register makes the next run slower (20us here,
    // nested): fs and gs only when they change, cs and ss only after the stub
    uint64_t fs, gs;
    bool seg_set, kernel_segs;
    LONG loaded_gen;               // vm->pt_gen when cr3 was last loaded
};

static WHV_PARTITION_HANDLE part;
static uint8_t *hv;
static SRWLOCK lock = SRWLOCK_INIT;   // the slots, their chunks, the processors
static struct px_vm slots[SLOTS];
static bool slot_used[SLOTS];
static struct vp vps[VM_VPS];
static volatile LONG64 stub_stops;
// the CPUID the guest sees (px_vm_cpuid)
#define CPUID_MAX 32
static WHV_X64_CPUID_RESULT cpuid[CPUID_MAX];
static unsigned ncpuid;

// What cpuid returns for leaf, instead of the processor's: the features the
// guest sees in the VM have to be the ones it would see translated, as an
// address space can go on translated. Before the first px_vm_new.
void px_vm_cpuid(uint32_t leaf, uint32_t eax, uint32_t ebx, uint32_t ecx, uint32_t edx) {
    if (ncpuid < CPUID_MAX)
        cpuid[ncpuid++] = (WHV_X64_CPUID_RESULT) {.Function = leaf, .Eax = eax, .Ebx = ebx, .Ecx = ecx, .Edx = edx};
}

// The stub, copied to each processor's stub page; the mailbox is the page
// after it (struct px_vm_mailbox). The guest's syscall comes here at CPL0 with
// its rip in rcx and rflags in r11, and its registers otherwise untouched.
// They go to the mailbox, and the stub waits for an answer: rax and sysret, or
// to be stopped after loading the mailbox's fxsave for ish to set every
// register. Waiting too long it stops too, until ish runs it again, and so it
// does right away if ish sleeps instead of watching the mailbox, or if the
// syscall is going to block (park). No pause
// in the loop: pause-loop exiting would leave the VM. It stops with an out to
// a port of ours (hlt would leave the processor halted until an interrupt):
// PORT_WAIT is stepped over when it runs again, PORT_FULL is where ish sets
// the registers.
extern const uint8_t px_vm_stub[], px_vm_stub_end[];
#define MB(off) "px_vm_stub+0x1000+" #off "(%rip)"
__asm__(
    ".section .rdata,\"dr\"\n"
    ".balign 16\n"
    ".globl px_vm_stub\n"
    "px_vm_stub:\n"
    "movq %rax, " MB(0x08) "\n"
    "movq %rcx, " MB(0x10) "\n"
    "movq %rdx, " MB(0x18) "\n"
    "movq %rbx, " MB(0x20) "\n"
    "movq %rsp, " MB(0x28) "\n"
    "movq %rbp, " MB(0x30) "\n"
    "movq %rsi, " MB(0x38) "\n"
    "movq %rdi, " MB(0x40) "\n"
    "movq %r8, " MB(0x48) "\n"
    "movq %r9, " MB(0x50) "\n"
    "movq %r10, " MB(0x58) "\n"
    "movq %r11, " MB(0x60) "\n"
    "movq %r12, " MB(0x68) "\n"
    "movq %r13, " MB(0x70) "\n"
    "movq %r14, " MB(0x78) "\n"
    "movq %r15, " MB(0x80) "\n"
    "fxsave64 " MB(0x100) "\n"
    "movl $1, " MB(0) "\n"           // PX_MB_CALL
    "mfence\n"
    "cmpl $0, " MB(4) "\n"           // asleep: wake it
    "jne 5f\n"
    "1: xorl %eax, %eax\n"
    "2: cmpl $1, " MB(0) "\n"
    "jne 3f\n"
    "cmpl $0, " MB(0x8c) "\n"        // park: the syscall blocks
    "jne 5f\n"
    "incl %eax\n"
    "cmpl $" GUEST_SPIN ", %eax\n"
    "jb 2b\n"
    "outb %al, $0xe9\n"             // PORT_WAIT
    "jmp 1b\n"
    "3: cmpl $2, " MB(0) "\n"        // PX_MB_ANSWER
    "jne 4f\n"
    "cmpl $0, " MB(0x88) "\n"        // flush: the page tables changed
    "je 6f\n"
    "movl $0, " MB(0x88) "\n"
    "movq %cr3, %rax\n"
    "movq %rax, %cr3\n"
    "6: movq " MB(0x08) ", %rax\n"
    "movl $0, " MB(0) "\n"
    "sysretq\n"
    "4: fxrstor64 " MB(0x100) "\n"   // PX_MB_FULL
    "outb %al, $0xea\n"             // PORT_FULL
    "ud2\n"
    "5: outb %al, $0xe9\n"          // PORT_WAIT
    "jmp 1b\n"
    ".globl px_vm_stub_end\n"
    "px_vm_stub_end:\n"
    ".text\n");
_Static_assert(offsetof(struct px_vm_mailbox, gpr) == 0x08, "mailbox layout");
_Static_assert(offsetof(struct px_vm_mailbox, fx) == 0x100, "mailbox layout");
_Static_assert(offsetof(struct px_vm_mailbox, flush) == 0x88, "mailbox layout");
_Static_assert(offsetof(struct px_vm_mailbox, park) == 0x8c, "mailbox layout");

// Failures are said when VMs were asked for (ISH_WHP=1), not when ish only
// found it could have them and falls back to translating quietly
bool px_vm_verbose = true;
#define CK(x) do { HRESULT hr_ = (x); if (FAILED(hr_)) { if (px_vm_verbose) fprintf(stderr, "[vm] %s: 0x%08lx\n", #x, (unsigned long) hr_); goto fail; } } while (0)

static bool load_(void) {
    // System32 only: not one planted next to ish.exe or in the current
    // directory. Missing (the Windows Hypervisor Platform feature off): NULL,
    // and no dialog, as for any LoadLibrary.
    HMODULE m = LoadLibraryExW(L"WinHvPlatform.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (m == NULL)
        return false;
#define GET(name) if ((p_##name = (void *) GetProcAddress(m, #name)) == NULL) return false
    GET(WHvGetCapability);
    GET(WHvCreatePartition);
    GET(WHvSetPartitionProperty);
    GET(WHvSetupPartition);
    GET(WHvMapGpaRange);
    GET(WHvUnmapGpaRange);
    GET(WHvCreateVirtualProcessor);
    GET(WHvSetVirtualProcessorRegisters);
    GET(WHvGetVirtualProcessorRegisters);
    GET(WHvRunVirtualProcessor);
    GET(WHvCancelRunVirtualProcessor);
#undef GET
    p_WHvAdviseGpaRange = (void *) GetProcAddress(m, "WHvAdviseGpaRange");   // Windows 10 2004 on
    p_WHvGetVirtualProcessorCounters = (void *) GetProcAddress(m, "WHvGetVirtualProcessorCounters");
    WHV_CAPABILITY cap;
    UINT32 w;
    return SUCCEEDED(p_WHvGetCapability(WHvCapabilityCodeHypervisorPresent, &cap, sizeof(cap), &w)) &&
           cap.HypervisorPresent;
}
static bool loaded;
static BOOL CALLBACK load_once(INIT_ONCE *once, void *param, void **ctx) {
    (void) once; (void) param; (void) ctx;
    loaded = load_();
    return TRUE;
}
// (main asks whether there is a hypervisor while the maker thread makes the
// partition: once)
static bool load(void) {
    static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
    InitOnceExecuteOnce(&once, load_once, NULL, NULL);
    return loaded;
}

// The partition and our pages, once (px_vm_prepare may be at it already)
static bool partition_made;
static BOOL CALLBACK partition_once(INIT_ONCE *once, void *param, void **ctx);
static bool partition(void) {
    static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
    InitOnceExecuteOnce(&once, partition_once, NULL, NULL);
    return partition_made;
}

// Making a processor takes milliseconds (2-3 here, nested): a thread of its
// own makes them ahead, and starts with the partition, which ish's start can
// hide. One for the first thread and one more, ready for a second; once a
// second wants one (a program starting its threads), VP_SPARE not in use.
// (Making them all ahead cost a program with one thread 10ms of CPU.)
#define VP_SPARE 4
static HANDLE maker_wake;
static volatile LONG vp_claims;   // processors given to threads so far

static unsigned vp_want(void) {
    return vp_claims <= 1 ? 1 : VP_SPARE;
}

static bool vp_make(int vp);
// with the lock: one to make (it is the caller's to make, without the lock),
// or -1
static int vp_to_make(void) {
    for (int i = 0; i < VM_VPS; i++) {
        if (!vps[i].made && !vps[i].making) {
            vps[i].making = true;
            return i;
        }
    }
    return -1;
}
static void vp_made(int vp, bool ok) {
    AcquireSRWLockExclusive(&lock);
    vps[vp].making = false;
    vps[vp].made = ok;
    ReleaseSRWLockExclusive(&lock);
}
// with the lock: how many are made, or being made, and not in use
static unsigned vp_spare(void) {
    unsigned n = 0;
    for (int i = 0; i < VM_VPS; i++)
        n += vps[i].making || (vps[i].made && !vps[i].in_use && !vps[i].running);
    return n;
}

// arg: whether to find out first if VMs are worth having here
static DWORD WINAPI maker_thread(void *arg) {
    if ((arg != NULL && !px_vm_supported()) || !partition())
        return 0;
    for (;;) {
        AcquireSRWLockExclusive(&lock);
        int vp = vp_spare() < vp_want() ? vp_to_make() : -1;
        ReleaseSRWLockExclusive(&lock);
        if (vp >= 0) {
            vp_made(vp, vp_make(vp));
            continue;
        }
        WaitForSingleObject(maker_wake, INFINITE);
    }
    return 0;
}

void px_vm_prepare(bool check) {
    maker_wake = CreateEventW(NULL, FALSE, FALSE, NULL);
    HANDLE t = maker_wake != NULL ? CreateThread(NULL, 64 << 10, maker_thread, check ? (void *) 1 : NULL, 0, NULL)
                                  : NULL;
    if (t != NULL)
        CloseHandle(t);
}

static BOOL CALLBACK partition_once(INIT_ONCE *once, void *param, void **ctx) {
    (void) once; (void) param; (void) ctx;
    if (!load())
        return TRUE;
    CK(p_WHvCreatePartition(&part));
    WHV_PARTITION_PROPERTY p;
    memset(&p, 0, sizeof(p));
    p.ProcessorCount = VM_VPS;
    CK(p_WHvSetPartitionProperty(part, WHvPartitionPropertyCodeProcessorCount, &p, sizeof(p)));
    memset(&p, 0, sizeof(p));
    p.ExtendedVmExits.ExceptionExit = 1;
    CK(p_WHvSetPartitionProperty(part, WHvPartitionPropertyCodeExtendedVmExits, &p, sizeof(p)));
    memset(&p, 0, sizeof(p));
    p.ExceptionExitBitmap = (1 << 0) | (1 << 1) | (1 << 3) | (1 << 4) | (1 << 5) | (1 << 6) | (1 << 7) |
                            (1 << 8) | (1 << 10) | (1 << 11) | (1 << 12) | (1 << 13) | (1 << 14) |
                            (1 << 16) | (1 << 17) | (1 << 19);
    CK(p_WHvSetPartitionProperty(part, WHvPartitionPropertyCodeExceptionExitBitmap, &p, sizeof(p)));
    if (ncpuid > 0)
        CK(p_WHvSetPartitionProperty(part, WHvPartitionPropertyCodeCpuidResultList, cpuid, ncpuid * sizeof(cpuid[0])));
    CK(p_WHvSetupPartition(part));
    hv = VirtualAlloc(NULL, HV_SIZE, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (hv == NULL)
        goto fail;
    for (int s = 0; s < SLOTS; s++) {
        uint64_t *pml4 = (uint64_t *) (hv + HV_PT(s)), *pdpt = (uint64_t *) (hv + HV_PT(s) + 0x1000);
        pml4[0] = (HV_PT(s) + 0x1000) | 7;
        for (int i = 0; i < 4; i++)
            pdpt[i] = (SLOT_GPA(s) + (uint64_t) i * GB) | 0x87;   // 1GB pages, present, writable, user
        pdpt[4] = 0 | 0x83;                                      // ours: not user
        slots[s].slot = s;
    }
    size_t stub_len = px_vm_stub_end - px_vm_stub;
    for (int i = 0; i < VM_VPS; i++) {
        memcpy(hv + HV_VP(i), px_vm_stub, stub_len);
        vps[i].index = i;
    }
    CK(p_WHvMapGpaRange(part, hv, 0, HV_SIZE,
                        WHvMapGpaRangeFlagRead | WHvMapGpaRangeFlagWrite | WHvMapGpaRangeFlagExecute));
    partition_made = true;
    px_boot_mark("vm: partition made");
    return TRUE;
fail:
    return TRUE;
}

static bool map_chunk(struct px_vm *vm, uint64_t c);

// An address space's slot, or NULL if there is no hypervisor (px_vm_present
// says) or no slot left
struct px_vm *px_vm_new(void) {
    if (!partition())
        return NULL;
    AcquireSRWLockExclusive(&lock);
    struct px_vm *vm = NULL;
    for (int s = 0; s < SLOTS && vm == NULL; s++) {
        if (!slot_used[s]) {
            slot_used[s] = true;
            vm = &slots[s];
        }
    }
    // its pool of page tables for above the window
    if (vm != NULL && vm->ptpool == NULL) {
        vm->ptpool = VirtualAlloc(NULL, PTPOOL_PAGES * 0x1000, MEM_RESERVE, PAGE_READWRITE);
        if (vm->ptpool == NULL || FAILED(p_WHvMapGpaRange(part, vm->ptpool, PTPOOL_GPA(vm->slot), PTPOOL_PAGES * 0x1000,
                                                          WHvMapGpaRangeFlagRead | WHvMapGpaRangeFlagWrite))) {
            if (vm->ptpool != NULL)
                VirtualFree(vm->ptpool, 0, MEM_RELEASE);
            vm->ptpool = NULL;
            slot_used[vm->slot] = false;
            vm = NULL;
        }
    }
    ReleaseSRWLockExclusive(&lock);
    return vm;
}

bool px_vm_present(void) {
    return partition();
}

// Whether VMs are worth having here, without making one: the hypervisor
// platform is on (Windows' "Windows Hypervisor Platform" feature, and a
// processor with virtualization, nested if Windows is a VM itself), Windows
// fills pages ahead (WHvAdviseGpaRange, 10 2004 on: without it each page
// first touched costs as much as an exit) and there is more than one
// processor (a syscall is a processor spinning while a thread of ish works).
static bool supported;
static BOOL CALLBACK supported_once(INIT_ONCE *once, void *param, void **ctx) {
    (void) once; (void) param; (void) ctx;
    supported = load() && p_WHvAdviseGpaRange != NULL && GetActiveProcessorCount(ALL_PROCESSOR_GROUPS) > 1;
    return TRUE;
}
bool px_vm_supported(void) {
    static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
    InitOnceExecuteOnce(&once, supported_once, NULL, NULL);
    return supported;
}

static void unmap_all(struct px_vm *vm) {
    for (uint64_t c = 0; c < CHUNKS; c++) {
        if (vm->chunk_mapped[c]) {
            p_WHvUnmapGpaRange(part, SLOT_GPA(vm->slot) + c * CHUNK, CHUNK);
            vm->chunk_mapped[c] = 0;
        }
    }
}

// Its guest physical memory is window from now on: mapped already if it was
// the slot's last one too
void px_vm_attach(struct px_vm *vm, void *window) {
    if (vm->window == window)
        return;
    AcquireSRWLockExclusive(&lock);
    unmap_all(vm);
    vm->window = window;
    ReleaseSRWLockExclusive(&lock);
    // what is there already
    for (uint8_t *p = window; p < (uint8_t *) window + 4 * GB;) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery(p, &mbi, sizeof(mbi)))
            break;
        uint8_t *end = (uint8_t *) mbi.BaseAddress + mbi.RegionSize;
        if (mbi.State == MEM_COMMIT && (mbi.Protect & (PAGE_READWRITE | PAGE_READONLY))) {
            for (uint64_t c = (p - (uint8_t *) window) / CHUNK; c * CHUNK < (uint64_t) (end - (uint8_t *) window); c++)
                map_chunk(vm, c);   // which fills them
        }
        p = end;
    }
}

// === Above the window ===

#define PTE_P 1ull
#define PTE_RW 2ull
#define PTE_U 4ull
#define PTE_NX (1ull << 63)
#define PTE_ADDR 0x000ffffffffff000ull

// A table's entries from its guest physical address: the slot's own top two
// (in our pages) or one of its pool
static uint64_t *table_at(struct px_vm *vm, uint64_t gpa) {
    if (gpa < HV_SIZE)
        return (uint64_t *) (hv + gpa);
    return (uint64_t *) (vm->ptpool + (gpa - PTPOOL_GPA(vm->slot)));
}

#define PTE_PS 0x80ull

// the next level's table under entry *e (at level shift), made if need be; a
// large page there (the window's) split into the pages it maps. NULL: none
// (or out of tables, when make).
static uint64_t *table_under(struct px_vm *vm, uint64_t *e, int shift, bool make) {
    if ((*e & PTE_P) && !(*e & PTE_PS))
        return table_at(vm, *e & PTE_ADDR);
    if (!make || vm->ptused == PTPOOL_PAGES)
        return NULL;
    uint8_t *page = vm->ptpool + (size_t) vm->ptused * 0x1000;
    if (!VirtualAlloc(page, 0x1000, MEM_COMMIT, PAGE_READWRITE))
        return NULL;
    uint64_t *t = (uint64_t *) page;
    if (*e & PTE_PS) {
        // a 1GB page into 2MB ones, a 2MB page into 4KB ones
        uint64_t base = *e & PTE_ADDR & ~((1ull << shift) - 1), flags = *e & (PTE_P | PTE_RW | PTE_U | PTE_NX);
        for (int i = 0; i < 512; i++)
            t[i] = (base + ((uint64_t) i << (shift - 9))) | flags | (shift > 21 ? PTE_PS : 0);
    } else {
        memset(page, 0, 0x1000);
    }
    *e = (PTPOOL_GPA(vm->slot) + (uint64_t) vm->ptused * 0x1000) | PTE_P | PTE_RW | PTE_U;
    vm->ptused++;
    return t;
}

static uint64_t *pte_of(struct px_vm *vm, uint64_t va, bool make) {
    uint64_t *t = (uint64_t *) (hv + HV_PT(vm->slot));
    for (int shift = 39; shift > 12; shift -= 9) {
        t = table_under(vm, &t[(va >> shift) & 511], shift, make);
        if (t == NULL)
            return NULL;
    }
    return &t[(va >> 12) & 511];
}

// where the host page is in guest physical memory, its extent mapped if need
// be: the part of the allocation [base, base + size) in host's EXTENT
static bool gpa_of(struct px_vm *vm, uintptr_t host, uintptr_t base, size_t size, uint64_t *gpa) {
    uintptr_t start = host & ~(EXTENT - 1), end = start + EXTENT;
    if (start < base)
        start = base;
    if (end > base + size)
        end = base + size;
    unsigned h = (unsigned) ((start >> 12) * 0x9e3779b1u) % EXTENTS;
    for (unsigned probe = 0; probe < EXTENTS; probe++, h = (h + 1) % EXTENTS) {
        struct extent *x = &vm->extents[h];
        if (x->host == start && x->len == end - start) {
            *gpa = x->gpa + (host - start);
            return true;
        }
        if (x->host != 0)
            continue;
        if (vm->nextents >= EXTENTS * 3 / 4 || vm->gpa_next + EXTENT > (256ull << 30))
            return false;
        uint64_t at = HIGH_GPA(vm->slot) + vm->gpa_next;
        if (FAILED(p_WHvMapGpaRange(part, (void *) start, at, end - start,
                                    WHvMapGpaRangeFlagRead | WHvMapGpaRangeFlagWrite | WHvMapGpaRangeFlagExecute)))
            return false;
        vm->gpa_next += EXTENT;
        *x = (struct extent) {start, end - start, at};
        vm->nextents++;
        *gpa = at + (host - start);
        // filled ahead, as the window's pages are (px_vm_populate)
        for (uintptr_t p = start; p < end && p_WHvAdviseGpaRange != NULL;) {
            MEMORY_BASIC_INFORMATION mbi;
            if (!VirtualQuery((void *) p, &mbi, sizeof(mbi)))
                break;
            uintptr_t next = (uintptr_t) mbi.BaseAddress + mbi.RegionSize;
            if (next > end)
                next = end;
            if (mbi.State == MEM_COMMIT && (mbi.Protect & (PAGE_READWRITE | PAGE_READONLY | PAGE_EXECUTE_READ |
                                                           PAGE_EXECUTE_READWRITE | PAGE_WRITECOPY))) {
                WHV_MEMORY_RANGE_ENTRY r = {at + (p - start), next - p};
                WHV_ADVISE_GPA_RANGE adv;
                memset(&adv, 0, sizeof(adv));
                adv.Populate.AccessType = (mbi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE))
                                          ? WHvMemoryAccessWrite : WHvMemoryAccessRead;
                p_WHvAdviseGpaRange(part, &r, 1, WHvAdviseGpaRangeCodePopulate, &adv, sizeof(adv));
            }
            p = next;
        }
        return true;
    }
    return false;
}

// (4GB to 5GB is our pages', mapped by a 1GB entry: not for the guest. Below
// it is the window, mapped by 1GB entries, which pages kept elsewhere split.)
#define HIGH_VA (HV_VA + GB)

bool px_vm_map_page(struct px_vm *vm, uint64_t va, void *host, void *base, size_t size, bool write, bool exec) {
    if ((va >= HV_VA && va < HIGH_VA) || va >= (1ull << 47))
        return false;
    AcquireSRWLockExclusive(&lock);
    uint64_t gpa;
    uint64_t *pte = NULL;
    bool ok = gpa_of(vm, (uintptr_t) host, (uintptr_t) base, size, &gpa) && (pte = pte_of(vm, va, true)) != NULL;
    if (ok) {
        uint64_t e = gpa | PTE_P | PTE_U | (write ? PTE_RW : 0) | (exec ? 0 : PTE_NX);
        if ((*pte & PTE_P) && *pte != e)
            InterlockedIncrement(&vm->pt_gen);   // a TLB may have the old one
        *pte = e;
    }
    ReleaseSRWLockExclusive(&lock);
    return ok;
}

// Above the window the entries go; in it, they map the window again. True
// when an entry changed (a TLB may have it: px_vm_shootdown).
bool px_vm_unmap_pages(struct px_vm *vm, uint64_t va, uint64_t len) {
    if (vm->ptused == 0)
        return false;
    if (len > (1ull << 47) - va)
        len = (1ull << 47) - va;
    AcquireSRWLockExclusive(&lock);
    bool cleared = false;
    for (uint64_t end = va + len; va < end;) {
        if (va >= HV_VA && va < HIGH_VA) {
            va = HIGH_VA;
            continue;
        }
        // a level missing (or a large page) skips all it would have mapped
        uint64_t *t = (uint64_t *) (hv + HV_PT(vm->slot));
        int shift = 39;
        for (; shift > 12; shift -= 9) {
            uint64_t *next = table_under(vm, &t[(va >> shift) & 511], shift, false);
            if (next == NULL)
                break;
            t = next;
        }
        if (shift > 12) {
            va = (va | ((1ull << shift) - 1)) + 1;
            continue;
        }
        uint64_t *pte = &t[(va >> 12) & 511];
        uint64_t window = va < HV_VA ? (SLOT_GPA(vm->slot) + va) | PTE_P | PTE_RW | PTE_U : 0;
        if (*pte != window) {
            *pte = window;
            cleared = true;
        }
        va += 0x1000;
    }
    if (cleared)
        InterlockedIncrement(&vm->pt_gen);
    ReleaseSRWLockExclusive(&lock);
    return cleared;
}

// While held, the processors of vm don't run its code: those running it stop
// (and the ones in the stub with a syscall stay there), and px_vm_wait
// doesn't run them again by itself; ish does, once it has mem->lock again,
// which the caller has to write. For what can't change all at once in the
// window: a page committed, then read into (fm_fill_run).
void px_vm_hold(struct px_vm *vm, bool hold) {
    AcquireSRWLockExclusive(&vm->hold_lock);
    vm->hold = hold;
    ReleaseSRWLockExclusive(&vm->hold_lock);
    if (!hold)
        return;
    bool wait[VM_VPS];
    unsigned n = 0;
    for (int i = 0; i < VM_VPS; i++) {
        struct vp *v = &vps[i];
        wait[i] = v->vm == vm && v->running && px_vm_mailbox(vm, i)->state != PX_MB_CALL;
        if (wait[i]) {
            p_WHvCancelRunVirtualProcessor(part, i, 0);
            n++;
        }
    }
    for (unsigned spin = 1; n > 0; spin++) {
        n = 0;
        for (int i = 0; i < VM_VPS; i++) {
            struct vp *v = &vps[i];
            if (wait[i])
                wait[i] = v->vm == vm && v->running && px_vm_mailbox(vm, i)->state != PX_MB_CALL;
            if (!wait[i])
                continue;
            n++;
            if (spin % 4096 == 0)
                p_WHvCancelRunVirtualProcessor(part, i, 0);
        }
        if (n > 0)
            YieldProcessor();
    }
}

// What a TLB shootdown is for: the processors running vm's code with page
// tables older than now (vm->pt_gen) stop, and load them again as they go on
// (px_vm_go). One in the stub with a syscall loads them before going back
// (px_vm_answer). Returns when none of them can use an entry that went.
void px_vm_shootdown(struct px_vm *vm) {
    LONG gen = vm->pt_gen;
    bool wait[VM_VPS];
    unsigned n = 0;
    for (int i = 0; i < VM_VPS; i++) {
        struct vp *v = &vps[i];
        wait[i] = v->vm == vm && v->running && v->loaded_gen != gen;
        if (wait[i]) {
            p_WHvCancelRunVirtualProcessor(part, i, 0);
            n++;
        }
    }
    for (unsigned spin = 1; n > 0; spin++) {
        n = 0;
        for (int i = 0; i < VM_VPS; i++) {
            struct vp *v = &vps[i];
            if (!wait[i])
                continue;
            wait[i] = v->vm == vm && v->running && v->loaded_gen != gen &&
                      px_vm_mailbox(vm, i)->state != PX_MB_CALL;
            if (!wait[i])
                continue;
            n++;
            // a cancel can miss one about to run
            if (spin % 4096 == 0)
                p_WHvCancelRunVirtualProcessor(part, i, 0);
        }
        if (n > 0)
            YieldProcessor();
    }
}

void px_vm_forget_host(void *host, size_t len) {
    if (hv == NULL)
        return;
    uintptr_t from = (uintptr_t) host, to = from + len;
    AcquireSRWLockExclusive(&lock);
    for (int s = 0; s < SLOTS; s++) {
        struct px_vm *vm = &slots[s];
        if (vm->nextents == 0)
            continue;
        bool removed = false;
        for (unsigned i = 0; i < EXTENTS; i++) {
            struct extent *x = &vm->extents[i];
            if (x->host == 0 || x->host >= to || x->host + x->len <= from)
                continue;
            p_WHvUnmapGpaRange(part, x->gpa, x->len);
            x->host = 0;
            vm->nextents--;
            removed = true;
        }
        if (removed) {
            // rehash what is left, as the probe chains may have holes now
            struct extent *old = malloc(sizeof(vm->extents));
            if (old != NULL) {
                memcpy(old, vm->extents, sizeof(vm->extents));
                memset(vm->extents, 0, sizeof(vm->extents));
                for (unsigned i = 0; i < EXTENTS; i++) {
                    if (old[i].host == 0)
                        continue;
                    unsigned h = (unsigned) ((old[i].host >> 12) * 0x9e3779b1u) % EXTENTS;
                    while (vm->extents[h].host != 0)
                        h = (h + 1) % EXTENTS;
                    vm->extents[h] = old[i];
                }
                free(old);
            }
        }
    }
    ReleaseSRWLockExclusive(&lock);
}

// everything above the window goes (its address space does)
static void high_reset(struct px_vm *vm) {
    for (unsigned i = 0; i < EXTENTS; i++) {
        if (vm->extents[i].host != 0)
            p_WHvUnmapGpaRange(part, vm->extents[i].gpa, vm->extents[i].len);
    }
    memset(vm->extents, 0, sizeof(vm->extents));
    vm->nextents = 0;
    vm->gpa_next = 0;
    if (vm->ptused > 0) {
        uint64_t *pml4 = (uint64_t *) (hv + HV_PT(vm->slot)), *pdpt = (uint64_t *) (hv + HV_PT(vm->slot) + 0x1000);
        for (int i = 1; i < 512; i++)
            pml4[i] = 0;
        for (int i = 0; i < 4; i++)
            pdpt[i] = (SLOT_GPA(vm->slot) + (uint64_t) i * GB) | 0x87;   // the window, whole again
        for (int i = 5; i < 512; i++)
            pdpt[i] = 0;
        VirtualFree(vm->ptpool, (size_t) vm->ptused * 0x1000, MEM_DECOMMIT);
        vm->ptused = 0;
        InterlockedIncrement(&vm->pt_gen);
    }
}

static void vp_stop(struct vp *v) {
    while (v->running) {
        p_WHvCancelRunVirtualProcessor(part, v->index, 0);
        WaitForSingleObject(v->stopped, 1);
    }
}

// (Its mailbox stays as it is: a syscall posted there is still the thread's
// to make, px_vm_wait tells it; the next owner gets it emptied.)
static void vp_free(struct vp *v) {
    v->in_use = false;
    v->vm = NULL;
    v->step_out = false;
    v->kernel_segs = true;   // it may have stopped in the stub
}

// The address space is going away: the processors running it stopped and
// free, what was above the window unmapped. Its window stays mapped, emptied
// (px_fm_release): the next address space that gets it (px_vm_window) needs no
// mapping.
void px_vm_detach(struct px_vm *vm) {
    AcquireSRWLockExclusive(&lock);
    for (int i = 0; i < VM_VPS; i++) {
        struct vp *v = &vps[i];
        if (v->in_use && v->vm == vm) {
            vp_stop(v);
            vp_free(v);
        }
    }
    high_reset(vm);
    ReleaseSRWLockExclusive(&lock);
}

void *px_vm_window(struct px_vm *vm) {
    return vm->window;
}

// the window itself is going away
void px_vm_forget(void *window) {
    if (hv == NULL)
        return;
    AcquireSRWLockExclusive(&lock);
    for (int s = 0; s < SLOTS; s++) {
        if (slots[s].window == window) {
            unmap_all(&slots[s]);
            slots[s].window = NULL;
        }
    }
    ReleaseSRWLockExclusive(&lock);
}

static bool map_chunk(struct px_vm *vm, uint64_t c) {
    if (vm->chunk_mapped[c])
        return true;
    AcquireSRWLockExclusive(&lock);
    HRESULT hr = S_OK;
    if (!vm->chunk_mapped[c] && vm->window != NULL) {
        hr = p_WHvMapGpaRange(part, vm->window + c * CHUNK, SLOT_GPA(vm->slot) + c * CHUNK, CHUNK,
                              WHvMapGpaRangeFlagRead | WHvMapGpaRangeFlagWrite | WHvMapGpaRangeFlagExecute);
        if (SUCCEEDED(hr))
            vm->chunk_mapped[c] = 1;
        static int trace = -1;
        if (trace < 0)
            trace = getenv("ISH_VM_TRACE") != NULL;
        if (trace)
            fprintf(stderr, "[vm] slot %d maps chunk %llu (%llx)\n", vm->slot, (unsigned long long) c,
                    (unsigned long long) (c * CHUNK));
    }
    ReleaseSRWLockExclusive(&lock);
    if (FAILED(hr)) {
        fprintf(stderr, "[vm] map %llx: 0x%08lx\n", (unsigned long long) (c * CHUNK), (unsigned long) hr);
        return false;
    }
    // what px_vm_populate skipped while it wasn't mapped
    for (uint8_t *p = vm->window + c * CHUNK, *end = p + CHUNK; p < end;) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery(p, &mbi, sizeof(mbi)))
            break;
        uint8_t *next = (uint8_t *) mbi.BaseAddress + mbi.RegionSize;
        if (next > end)
            next = end;
        if (mbi.State == MEM_COMMIT && (mbi.Protect & (PAGE_READWRITE | PAGE_READONLY)))
            px_vm_populate(p, next - p, (mbi.Protect & PAGE_READWRITE) != 0);
        p = next;
    }
    return true;
}

// The hypervisor fills its page tables for a window one page at a time as
// the guest first touches each (not an exit we see, but as slow as one):
// fresh pages are filled ahead instead, the first POPULATE_MAX of a range.
// A page the guest touches first costs ~37 us on the test host, one filled
// ahead ~0.6 us, so this is generous: 2MB made a 100MB JSON's json.loads 2.5x
// and node's readFileSync 6x slower, and 64MB costs nothing measurable on the
// daily suite. (Gathering them to fill together when the guest runs next came
// out slower; filling less (64KB, 256KB) much slower.)
#define POPULATE_MAX (64u << 20)
static volatile LONG64 pop_calls, pop_bytes, pop_ticks;
void px_vm_populate_stats(uint64_t *calls, uint64_t *bytes, uint64_t *us) {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    *calls = pop_calls;
    *bytes = pop_bytes;
    *us = (uint64_t) pop_ticks * 1000000 / f.QuadPart;
}
void px_vm_populate(void *addr, size_t len, bool write) {
    uint8_t *a = addr;
    if (hv == NULL || p_WHvAdviseGpaRange == NULL || len == 0)
        return;
    if (len > POPULATE_MAX)
        len = POPULATE_MAX;
    for (int s = 0; s < SLOTS; s++) {
        struct px_vm *vm = &slots[s];
        uint8_t *w = vm->window;
        if (w == NULL || a < w || a >= w + 4 * GB)
            continue;
        uint64_t off = a - w;
        if (!vm->chunk_mapped[off / CHUNK] || !vm->chunk_mapped[(off + len - 1) / CHUNK])
            return;   // filled when the guest gets there
        WHV_MEMORY_RANGE_ENTRY r = {SLOT_GPA(s) + off, len};
        WHV_ADVISE_GPA_RANGE adv;
        memset(&adv, 0, sizeof(adv));
        adv.Populate.AccessType = write ? WHvMemoryAccessWrite : WHvMemoryAccessRead;
        // timed only for the stats: QueryPerformanceCounter costs ~4us in a
        // nested VM
        static int timed = -1;
        if (timed < 0)
            timed = getenv("ISH_XJIT_STATS") != NULL;
        LARGE_INTEGER t0 = {0}, t1 = {0};
        if (timed)
            QueryPerformanceCounter(&t0);
        p_WHvAdviseGpaRange(part, &r, 1, WHvAdviseGpaRangeCodePopulate, &adv, sizeof(adv));
        if (timed)
            QueryPerformanceCounter(&t1);
        InterlockedIncrement64(&pop_calls);
        InterlockedAdd64(&pop_bytes, (LONG64) len);
        InterlockedAdd64(&pop_ticks, t1.QuadPart - t0.QuadPart);
        return;
    }
}

static bool vp_set(int vp, const WHV_REGISTER_NAME *n, const WHV_REGISTER_VALUE *v, unsigned c) {
    CK(p_WHvSetVirtualProcessorRegisters(part, vp, n, c, v));
    return true;
fail:
    return false;
}

static DWORD WINAPI vp_thread(void *arg) {
    struct vp *v = arg;
    for (;;) {
        WaitForSingleObject(v->go, INFINITE);
        if (v->step_out) {
            WHV_REGISTER_NAME n = WHvX64RegisterRip;
            WHV_REGISTER_VALUE r = {.Reg64 = v->exit.VpContext.Rip + v->exit.VpContext.InstructionLength};
            p_WHvSetVirtualProcessorRegisters(part, v->index, &n, 1, &r);
            v->step_out = false;
        }
        v->hr = p_WHvRunVirtualProcessor(part, v->index, &v->exit, sizeof(v->exit));
        while (SUCCEEDED(v->hr) && v->exit.ExitReason == WHvRunVpExitReasonMemoryAccess &&
                v->exit.MemoryAccess.AccessInfo.GpaUnmapped && v->vm != NULL &&
                v->exit.MemoryAccess.Gpa - SLOT_GPA(v->vm->slot) < 4 * GB &&
                map_chunk(v->vm, (v->exit.MemoryAccess.Gpa - SLOT_GPA(v->vm->slot)) / CHUNK))
            v->hr = p_WHvRunVirtualProcessor(part, v->index, &v->exit, sizeof(v->exit));
        v->step_out = v->exit.ExitReason == WHvRunVpExitReasonX64IoPortAccess &&
                      v->exit.IoPortAccess.PortNumber == PORT_WAIT;
        if (v->step_out)
            InterlockedIncrement64(&stub_stops);
        InterlockedExchange(&v->running, 0);
        SetEvent(v->stopped);
    }
    return 0;
}

static const WHV_X64_SEGMENT_REGISTER user_cs = {.Base = 0, .Limit = 0xffffffff, .Selector = 0x33, .Attributes = 0xa0fb};
static const WHV_X64_SEGMENT_REGISTER user_ds = {.Base = 0, .Limit = 0xffffffff, .Selector = 0x2b, .Attributes = 0xc0f3};

// A processor in 64-bit user mode, syscall going to its stub
static bool vp_make(int vp) {
    struct vp *p = &vps[vp];
    if (vp == 0)
        px_boot_mark("vm: making processor 0");
    CK(p_WHvCreateVirtualProcessor(part, vp, 0));
    WHV_REGISTER_NAME n[20];
    WHV_REGISTER_VALUE v[20];
    memset(v, 0, sizeof(v));
    unsigned c = 0;
    n[c] = WHvX64RegisterCr0; v[c++].Reg64 = 0x80010033;   // PG WP NE ET MP PE
    n[c] = WHvX64RegisterCr4; v[c++].Reg64 = 0x620;        // OSXMMEXCPT OSFXSR PAE
    n[c] = WHvX64RegisterEfer; v[c++].Reg64 = 0xd01;       // NXE LMA LME SCE
    // sysret: cs 0x33, ss 0x2b; syscall: cs 0x10, ss 0x18 (no GDT is read)
    n[c] = WHvX64RegisterStar; v[c++].Reg64 = (0x23ull << 48) | (0x10ull << 32);
    n[c] = WHvX64RegisterLstar; v[c++].Reg64 = HV_VA + HV_VP(vp);
    n[c] = WHvX64RegisterSfmask; v[c++].Reg64 = 0x47700;   // TF DF IF NT AC
    n[c] = WHvX64RegisterCs; v[c++].Segment = user_cs;
    n[c] = WHvX64RegisterSs; v[c++].Segment = user_ds;
    n[c] = WHvX64RegisterDs; v[c++].Segment = user_ds;
    n[c] = WHvX64RegisterEs; v[c++].Segment = user_ds;
    n[c] = WHvX64RegisterTr;
    v[c].Segment.Selector = 0x40; v[c].Segment.Limit = 0x67; v[c++].Segment.Attributes = 0x8b;
    n[c] = WHvX64RegisterLdtr;
    v[c].Segment.Limit = 0xffff; v[c++].Segment.Attributes = 0x82;
    if (!vp_set(vp, n, v, c))
        return false;
    p->go = CreateEventW(NULL, FALSE, FALSE, NULL);
    p->stopped = CreateEventW(NULL, FALSE, FALSE, NULL);
    HANDLE t = CreateThread(NULL, 64 << 10, vp_thread, p, 0, NULL);
    if (p->go == NULL || p->stopped == NULL || t == NULL)
        return false;
    CloseHandle(t);
    return true;
fail:
    return false;
}

// A processor for a thread of vm's, the FPU and SSE as a new Linux process has
// them; -1 when there are no more
int px_vm_vcpu(struct px_vm *vm) {
    int vp = -1;
    for (;;) {
        AcquireSRWLockExclusive(&lock);
        for (int i = 0; i < VM_VPS && vp < 0; i++) {
            struct vp *p = &vps[i];
            if (!p->made || p->in_use || p->running)
                continue;
            p->in_use = true;
            p->vm = vm;
            vp = i;
            vp_claims++;
        }
        // none spare: one made here (or the maker's, when it is done)
        int make = vp < 0 ? vp_to_make() : -1;
        bool wake = maker_wake != NULL && vp_spare() < vp_want();
        ReleaseSRWLockExclusive(&lock);
        if (wake)
            SetEvent(maker_wake);
        if (vp >= 0)
            break;
        if (make >= 0) {
            bool ok = vp_make(make);
            vp_made(make, ok);
            if (!ok)
                return -1;
            continue;
        }
        // all made or being made: in use, but for those being made
        bool waiting = false;
        AcquireSRWLockShared(&lock);
        for (int i = 0; i < VM_VPS; i++)
            waiting |= vps[i].making;
        ReleaseSRWLockShared(&lock);
        if (!waiting)
            return -1;
        SwitchToThread();
    }
    struct px_vm_mailbox *mb = px_vm_mailbox(vm, vp);
    mb->state = PX_MB_IDLE;
    mb->asleep = 0;
    mb->flush = 0;
    mb->park = 0;
    WHV_REGISTER_NAME n[3] = {WHvX64RegisterCr3, WHvX64RegisterFpControlStatus, WHvX64RegisterXmmControlStatus};
    WHV_REGISTER_VALUE v[3];
    memset(v, 0, sizeof(v));
    v[0].Reg64 = HV_PT(vm->slot);
    vps[vp].loaded_gen = vm->pt_gen;
    v[1].FpControlStatus.FpControl = 0x37f;
    v[2].XmmControlStatus.XmmStatusControl = 0x1f80;
    v[2].XmmControlStatus.XmmStatusControlMask = 0xffff;
    vp_set(vp, n, v, 3);
    return vp;
}

// the thread that had it is gone
void px_vm_vcpu_release(struct px_vm *vm, int vp) {
    struct vp *v = &vps[vp];
    vp_stop(v);
    AcquireSRWLockExclusive(&lock);
    if (v->in_use && v->vm == vm)
        vp_free(v);
    ReleaseSRWLockExclusive(&lock);
}

struct px_vm_mailbox *px_vm_mailbox(struct px_vm *vm, int vp) {
    (void) vm;
    return (struct px_vm_mailbox *) (hv + HV_VP(vp) + 0x1000);
}

void px_vm_go(struct px_vm *vm, int vp) {
    struct vp *v = &vps[vp];
    LONG gen = vm->pt_gen;
    if (v->loaded_gen != gen) {
        // entries went: what the processor's TLB has of them goes too
        WHV_REGISTER_NAME n = WHvX64RegisterCr3;
        WHV_REGISTER_VALUE r = {.Reg64 = HV_PT(vm->slot)};
        p_WHvSetVirtualProcessorRegisters(part, vp, &n, 1, &r);
        v->loaded_gen = gen;
    }
    InterlockedExchange(&v->running, 1);
    SetEvent(v->go);
}

static bool in_stub(struct vp *v) {
    return v->exit.VpContext.Rip >= HV_VA;
}

static void fill_exit(struct vp *v, struct px_vm_exit *x) {
    WHV_RUN_VP_EXIT_CONTEXT *e = &v->exit;
    memset(x, 0, sizeof(*x));
    x->reason = e->ExitReason;
    switch (e->ExitReason) {
    case WHvRunVpExitReasonException:
        x->kind = PX_VM_EXCEPTION;
        x->vector = e->VpException.ExceptionType;
        x->error = e->VpException.ErrorCode;
        x->addr = e->VpException.ExceptionParameter;
        break;
    case WHvRunVpExitReasonMemoryAccess:
        if (v->vm == NULL) {   // detached meanwhile (px_vm_wait)
            x->kind = PX_VM_CANCELED;
            break;
        }
        x->kind = PX_VM_MEMORY;
        x->addr = e->MemoryAccess.Gpa - SLOT_GPA(v->vm->slot);   // the guest address
        x->is_write = e->MemoryAccess.AccessInfo.AccessType == WHvMemoryAccessWrite;
        break;
    case WHvRunVpExitReasonCanceled:
        x->kind = PX_VM_CANCELED;
        break;
    default:
        x->kind = PX_VM_OTHER;
        break;
    }
}

// Until there is a syscall in the mailbox (the processor may still be
// running) or the processor stopped for something else. -1 when running it
// failed. A cancel can miss a processor about to run: while *poked, it is
// canceled again now and then.
int px_vm_wait(struct px_vm *vm, int vp, struct px_vm_exit *x, volatile bool *poked) {
    struct vp *v = &vps[vp];
    struct px_vm_mailbox *mb = px_vm_mailbox(vm, vp);
    for (unsigned spin = 0;; spin++) {
        if (mb->state == PX_MB_CALL) {
            MemoryBarrier();
            memset(x, 0, sizeof(*x));
            x->kind = PX_VM_SYSCALL;
            return 0;
        }
        if (!v->running && !v->in_use) {
            // its address space left the VM (px_vm_detach): back to ish,
            // which translates it from here
            memset(x, 0, sizeof(*x));
            x->kind = PX_VM_CANCELED;
            return 0;
        }
        if (!v->running) {
            if (FAILED(v->hr)) {
                fprintf(stderr, "[vm] run: 0x%08lx\n", (unsigned long) v->hr);
                return -1;
            }
            if (in_stub(v) && v->exit.ExitReason != WHvRunVpExitReasonException) {
                // stopped (canceled) on the way to posting a syscall: it
                // goes on by itself, unless held
                AcquireSRWLockShared(&vm->hold_lock);
                bool held = vm->hold;
                if (!held)
                    px_vm_go(vm, vp);
                ReleaseSRWLockShared(&vm->hold_lock);
                if (held) {
                    memset(x, 0, sizeof(*x));
                    x->kind = PX_VM_CANCELED;
                    return 0;
                }
                continue;
            }
            fill_exit(v, x);
            return 0;
        }
        if (spin < HOST_SPIN) {
            YieldProcessor();
            continue;
        }
        // the stopped event may be stale: the loop looks again. A syscall
        // posted from now on stops the processor to wake us (both sides
        // fence between their store and their look at the other's).
        mb->asleep = 1;
        MemoryBarrier();
        if (mb->state != PX_MB_CALL && v->running &&
                WaitForSingleObject(v->stopped, 10) == WAIT_TIMEOUT && *poked)
            p_WHvCancelRunVirtualProcessor(part, vp, 0);
        mb->asleep = 0;
    }
}

void px_vm_answer(struct px_vm *vm, int vp, uint64_t rax) {
    struct px_vm_mailbox *mb = px_vm_mailbox(vm, vp);
    LONG gen = vm->pt_gen;
    if (vps[vp].loaded_gen != gen) {
        mb->flush = 1;   // the stub reloads cr3
        vps[vp].loaded_gen = gen;
    }
    mb->gpr[0] = rax;
    mb->park = 0;
    MemoryBarrier();
    mb->state = PX_MB_ANSWER;
    // if it stopped waiting, run it to take it; if it stops now, before
    // seeing it, px_vm_wait runs it again
    if (!vps[vp].running)
        px_vm_go(vm, vp);
}

// Stopped in the stub with the mailbox's fxsave loaded, for px_vm_set
void px_vm_answer_full(struct px_vm *vm, int vp) {
    struct vp *v = &vps[vp];
    struct px_vm_mailbox *mb = px_vm_mailbox(vm, vp);
    mb->park = 0;
    MemoryBarrier();
    mb->state = PX_MB_FULL;
    for (;;) {
        while (v->running)
            YieldProcessor();
        if (v->exit.ExitReason == WHvRunVpExitReasonX64IoPortAccess && v->exit.IoPortAccess.PortNumber == PORT_FULL)
            break;
        px_vm_go(vm, vp);   // stopped short of it: waiting, or canceled
    }
    mb->state = PX_MB_IDLE;
    v->kernel_segs = true;
}

static const WHV_REGISTER_NAME gpr_names[] = {
    WHvX64RegisterRax, WHvX64RegisterRcx, WHvX64RegisterRdx, WHvX64RegisterRbx,
    WHvX64RegisterRsp, WHvX64RegisterRbp, WHvX64RegisterRsi, WHvX64RegisterRdi,
    WHvX64RegisterR8, WHvX64RegisterR9, WHvX64RegisterR10, WHvX64RegisterR11,
    WHvX64RegisterR12, WHvX64RegisterR13, WHvX64RegisterR14, WHvX64RegisterR15,
    WHvX64RegisterRip, WHvX64RegisterRflags,
};

bool px_vm_get(struct px_vm *vm, int vp, struct px_vm_regs *r) {
    (void) vm;
    WHV_REGISTER_VALUE v[18];
    if (FAILED(p_WHvGetVirtualProcessorRegisters(part, vp, gpr_names, 18, v)))
        return false;
    for (int i = 0; i < 16; i++)
        r->gpr[i] = v[i].Reg64;
    r->rip = v[16].Reg64;
    r->rflags = v[17].Reg64;
    r->fs_base = vps[vp].fs;
    r->gs_base = vps[vp].gs;
    return true;
}

bool px_vm_set(struct px_vm *vm, int vp, const struct px_vm_regs *r) {
    (void) vm;
    struct vp *p = &vps[vp];
    WHV_REGISTER_VALUE v[22];
    WHV_REGISTER_NAME n[22];
    memset(v, 0, sizeof(v));
    for (int i = 0; i < 16; i++)
        v[i].Reg64 = r->gpr[i];
    v[16].Reg64 = r->rip;
    v[17].Reg64 = r->rflags;
    memcpy(n, gpr_names, sizeof(gpr_names));
    unsigned c = 18;
    if (!p->seg_set || p->fs != r->fs_base || p->gs != r->gs_base) {
        n[c] = WHvX64RegisterFs;
        v[c].Segment = user_ds;
        v[c++].Segment.Base = r->fs_base;
        n[c] = WHvX64RegisterGs;
        v[c].Segment = user_ds;
        v[c++].Segment.Base = r->gs_base;
        p->fs = r->fs_base;
        p->gs = r->gs_base;
        p->seg_set = true;
    }
    if (p->kernel_segs) {
        n[c] = WHvX64RegisterCs;
        v[c++].Segment = user_cs;
        n[c] = WHvX64RegisterSs;
        v[c++].Segment = user_ds;
        p->kernel_segs = false;
    }
    return vp_set(vp, n, v, c);
}

static const WHV_REGISTER_NAME sse_names[17] = {
    WHvX64RegisterXmm0, WHvX64RegisterXmm1, WHvX64RegisterXmm2, WHvX64RegisterXmm3,
    WHvX64RegisterXmm4, WHvX64RegisterXmm5, WHvX64RegisterXmm6, WHvX64RegisterXmm7,
    WHvX64RegisterXmm8, WHvX64RegisterXmm9, WHvX64RegisterXmm10, WHvX64RegisterXmm11,
    WHvX64RegisterXmm12, WHvX64RegisterXmm13, WHvX64RegisterXmm14, WHvX64RegisterXmm15,
    WHvX64RegisterXmmControlStatus,
};

bool px_vm_get_sse(struct px_vm *vm, int vp, void *xmm16, uint32_t *mxcsr) {
    (void) vm;
    WHV_REGISTER_VALUE v[17];
    if (FAILED(p_WHvGetVirtualProcessorRegisters(part, vp, sse_names, 17, v)))
        return false;
    for (int i = 0; i < 16; i++)
        memcpy((uint8_t *) xmm16 + 16 * i, &v[i].Reg128, 16);
    *mxcsr = v[16].XmmControlStatus.XmmStatusControl;
    return true;
}

bool px_vm_set_sse(struct px_vm *vm, int vp, const void *xmm16, uint32_t mxcsr) {
    (void) vm;
    WHV_REGISTER_VALUE v[17];
    memset(v, 0, sizeof(v));
    for (int i = 0; i < 16; i++)
        memcpy(&v[i].Reg128, (const uint8_t *) xmm16 + 16 * i, 16);
    v[16].XmmControlStatus.XmmStatusControl = mxcsr;
    v[16].XmmControlStatus.XmmStatusControlMask = 0xffff;
    return vp_set(vp, sse_names, v, 17);
}

void px_vm_cancel(struct px_vm *vm, int vp) {
    (void) vm;
    p_WHvCancelRunVirtualProcessor(part, vp, 0);
}

// how often a syscall waited so long that the stub stopped
uint64_t px_vm_stub_stops(void) {
    return stub_stops;
}

// The hypervisor's own counts over the processors made: nested page faults
// (the first touches of pages it hadn't filled) and the time they took (us),
// and all intercepts
void px_vm_counters(uint64_t *nested_faults, uint64_t *nested_us, uint64_t *intercepts, uint64_t *run_us) {
    *nested_faults = *nested_us = *intercepts = *run_us = 0;
    if (p_WHvGetVirtualProcessorCounters == NULL)
        return;
    for (int i = 0; i < VM_VPS; i++) {
        if (!vps[i].made)
            continue;
        struct WHV_PROCESSOR_INTERCEPT_COUNTERS c;
        UINT32 got;
        if (FAILED(p_WHvGetVirtualProcessorCounters(part, i, WHvProcessorCounterSetIntercepts, &c, sizeof(c), &got)))
            continue;
        WHV_PROCESSOR_RUNTIME_COUNTERS rt;
        if (SUCCEEDED(p_WHvGetVirtualProcessorCounters(part, i, WHvProcessorCounterSetRuntime, &rt, sizeof(rt), &got)))
            *run_us += rt.TotalRuntime100ns / 10;
        *nested_faults += c.NestedPageFaultIntercepts.Count;
        *nested_us += c.NestedPageFaultIntercepts.Time100ns / 10;
        *intercepts += c.PageInvalidations.Count + c.ControlRegisterAccesses.Count + c.IoInstructions.Count +
                       c.HaltInstructions.Count + c.CpuidInstructions.Count + c.MsrAccesses.Count +
                       c.OtherIntercepts.Count + c.PendingInterrupts.Count + c.EmulatedInstructions.Count +
                       c.DebugRegisterAccesses.Count + c.PageFaultIntercepts.Count +
                       c.NestedPageFaultIntercepts.Count + c.Hypercalls.Count + c.RdpmcInstructions.Count;
    }
}

// the processor stopped waiting for the syscall's answer (it took long)
bool px_vm_stopped_waiting(struct px_vm *vm, int vp) {
    (void) vm;
    return !vps[vp].running && vps[vp].step_out;
}
