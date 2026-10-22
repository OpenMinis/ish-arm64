// Windows, where the machine can (vm_wanted; with the guest windows:
// ISH_FASTMEM=2): address spaces run in hypervisor VMs (platform/windows/whp.c)
// instead of being translated.
// Their code runs as is, each thread on a processor of its own. A syscall
// comes back through the processor's mailbox without leaving the VM, and goes
// back the same way when all it changed is rax; otherwise (a signal
// delivered, sigreturn, execve) the processor stops for every register to be
// set. What leaves the VM comes back as the interrupt the translated code
// would have raised: a fault on a page the window doesn't let through yet
// (resolved here, as a TLB miss would be), other exceptions, a poke. Threads
// past a VM's processors are translated as before.
//
// Mappings outside the window (MAP_SHARED) aren't in the VM: an address space
// that touches one is translated from then on (vm_leave).
#include <sys/mman.h>
#include "emu/arch/x86/cpuid.h"
#include "kernel/task.h"
static int vm_state;   // 0: not looked, 1: on, -1: off (no hypervisor)
#define FM_GUEST_PAGES (1ull << (32 - PAGE_BITS))   // the window: the low 4GB
#define FAULT_AROUND 64   // pages mapped with one faulted on above the window

// An address space's slot in the VM. They are kept for the next address space
// when theirs goes away, and never freed: a thread can tell from gen whether
// the one its processor came from is still the same. (No slot left: that
// address space is translated.)
struct vm_space {
    struct px_vm *vm;
    struct mmu *mmu;               // NULL: free
    uint64_t gen;                  // bumped when it is detached from mmu
    // whose each processor is (compared, never followed: the thread may be
    // gone), and a poke for it
    struct cpu_state *owner[64];
    volatile bool poked[64];
    bool off;                      // its address space is translated from now on
    struct vm_space *next;
};
static struct vm_space *vm_spaces;
static lock_t vm_lock = LOCK_INITIALIZER;   // vm_spaces, attaching and detaching

// this thread's processor
struct vm_mine {
    struct vm_space *sp;
    uint64_t gen;
    int vp;                        // -1: none yet, -2: none to have
};
static __thread struct vm_mine *vm_me;
static __thread bool vm_pending;   // a syscall in the mailbox waits for its answer
static __thread struct px_vm_mailbox *vm_parkable;   // its mailbox, while ish handles it
static __thread struct px_vm_regs vm_at_call;   // the registers it was made with
static pthread_key_t vm_key;
static pthread_once_t vm_key_once = PTHREAD_ONCE_INIT;
static _Atomic uint64_t vm_exits[5], vm_full, vm_made, vm_left, vm_high_faults, vm_shootdowns;
static _Atomic uint64_t vm_ns_new, vm_ns_attach, vm_ns_vcpu, vm_ns_high, vm_ns_mem, vm_ns_first;
static _Atomic uint64_t vm_long[512];   // syscalls the stub stopped waiting for, by number   // time making VMs, attaching, getting processors
static uint64_t vm_now_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t) t.tv_sec * 1000000000ull + (uint64_t) t.tv_nsec;
}
// what the memory exits were for: a file page not read in yet, a write to a
// page shared with a fork, nothing mapped there, anything else
static _Atomic uint64_t vm_mem_lazy, vm_mem_cow, vm_mem_none, vm_mem_other;

// at exit (kernel/exit.c), with ISH_XJIT_STATS
void vm_dump_stats(void) {
    if (vm_spaces == NULL || getenv("ISH_XJIT_STATS") == NULL)
        return;
    fprintf(stderr, "[vm] syscalls %llu (%llu with every register, %llu waited too long), exits: exception %llu, "
            "memory %llu, canceled %llu, other %llu; %llu VMs made, %llu address spaces left them\n",
            (unsigned long long) vm_exits[PX_VM_SYSCALL], (unsigned long long) vm_full,
            (unsigned long long) px_vm_stub_stops(),
            (unsigned long long) vm_exits[PX_VM_EXCEPTION], (unsigned long long) vm_exits[PX_VM_MEMORY],
            (unsigned long long) vm_exits[PX_VM_CANCELED], (unsigned long long) vm_exits[PX_VM_OTHER],
            (unsigned long long) vm_made, (unsigned long long) vm_left);
    fprintf(stderr, "[vm] making VMs %.2f ms, attaching %.2f ms, getting processors %.2f ms; "
            "mapping above the window %.2f ms, memory exits %.2f ms, loading registers %.2f ms\n",
            vm_ns_new / 1e6, vm_ns_attach / 1e6, vm_ns_vcpu / 1e6, vm_ns_high / 1e6, vm_ns_mem / 1e6, vm_ns_first / 1e6);
    fprintf(stderr, "[vm] syscalls waited for too long:");
    for (int i = 0; i < 512; i++)
        if (vm_long[i] > 0)
            fprintf(stderr, " %d:%llu", i, (unsigned long long) vm_long[i]);
    fprintf(stderr, "\n");
    uint64_t nested, nested_us, intercepts, run_us;
    px_vm_counters(&nested, &nested_us, &intercepts, &run_us);
    fprintf(stderr, "[vm] hypervisor: processors ran %.1f ms; %llu nested page faults (%.1f ms), %llu intercepts in all\n",
            run_us / 1e3, (unsigned long long) nested, nested_us / 1e3, (unsigned long long) intercepts);
    fprintf(stderr, "[vm] memory exits: lazy %llu, cow %llu, unmapped %llu, other %llu; pages mapped above the window %llu, "
            "TLB shootdowns %llu\n",
            (unsigned long long) vm_mem_lazy, (unsigned long long) vm_mem_cow,
            (unsigned long long) vm_mem_none, (unsigned long long) vm_mem_other,
            (unsigned long long) vm_high_faults, (unsigned long long) vm_shootdowns);
    uint64_t pop_calls, pop_bytes, pop_us;
    px_vm_populate_stats(&pop_calls, &pop_bytes, &pop_us);
    fprintf(stderr, "[vm] filled ahead: %llu ranges, %.1f MB, %.1f ms\n", (unsigned long long) pop_calls,
            pop_bytes / 1048576.0, pop_us / 1e3);
    extern void fm_stats_print(FILE *out);   // kernel/memory.c
    fm_stats_print(stderr);
}

// the processor goes back to its VM, if that is still the same
static void vm_release(struct vm_mine *me) {
    lock(&vm_lock);
    if (me->vp >= 0 && me->sp != NULL && me->sp->gen == me->gen) {
        px_vm_vcpu_release(me->sp->vm, me->vp);
        me->sp->owner[me->vp] = NULL;
    }
    unlock(&vm_lock);
    me->sp = NULL;
    me->vp = -1;
}

static void vm_thread_gone(void *arg) {
    vm_release(arg);
    free(arg);
}

static void vm_key_init(void) {
    pthread_key_create(&vm_key, vm_thread_gone);
}

// The VM's CPUID is the one translated code sees (emu/arch/x86/cpuid.h): what
// a program learns of the processor stays true if it goes on translated.
static void vm_cpuid(void) {
    static bool done;
    if (done)
        return;
    done = true;
    static const uint32_t leaves[] = {0, 1, 2, 4, 6, 7, 0xb, 0xd, 0x80000000, 0x80000001, 0x80000002,
                                      0x80000003, 0x80000004, 0x80000005, 0x80000006, 0x80000007, 0x80000008};
    for (unsigned i = 0; i < sizeof(leaves) / sizeof(leaves[0]); i++) {
        dword_t a = leaves[i], b = 0, c = 0, d = 0;
        if (a <= 1 || a >= 0x80000000 && a <= 0x80000006)
            do_cpuid(&a, &b, &c, &d);
        else
            a = 0;   // past the highest leaf it says it has: nothing
        px_vm_cpuid(leaves[i], a, b, c, d);
    }
}

// main: with VMs wanted, the VM and a first processor are made on another
// thread while ish starts (making them takes milliseconds)
// px_about_to_block: the syscall this thread handles blocks; the processor
// waiting for it in the stub stops instead of spinning on
static void vm_about_to_block(void) {
    struct px_vm_mailbox *mb = vm_parkable;
    if (mb != NULL) {
        mb->park = 1;
        vm_parkable = NULL;
    }
}

// ISH_WHP=1: VMs, 0: translated; unset: VMs where the machine can run them
// (px_vm_supported), translated where it can't, and quietly so. Looked at
// first by the first address space, before there are threads of guests.
// Unset means translated too where whoever embeds ish counts timer ticks
// (ish_set_timer_tick_hook): code running in a VM makes none.
static int whp_env(void) {
    const char *e = getenv("ISH_WHP");
    if (e != NULL && (e[0] == '0' || e[0] == '1'))
        return e[0] - '0';
    return ish_timer_tick_hook_set() ? 0 : -1;
}
bool vm_wanted(void) {
    static int wanted = -1;
    if (wanted < 0)
        wanted = whp_env() >= 0 ? whp_env() : px_vm_supported();
    return wanted;
}

void vm_prepare(void) {
    int env = whp_env();
    if (env == 0)
        return;
    if (env < 0)
        px_vm_verbose = getenv("ISH_VM_TRACE") != NULL;
    px_about_to_block = vm_about_to_block;
    vm_cpuid();
    // whether there is a hypervisor is found out there too (loading it takes
    // a couple of milliseconds), and the partition made right after
    px_vm_prepare(env < 0);
}

// the address space's VM, attaching one to it first if need be
static struct vm_space *vm_space_of(struct mmu *mmu) {
    struct vm_space *sp = mmu->vm;
    if (sp != NULL)
        return sp->off ? NULL : sp;
    lock(&vm_lock);
    if ((sp = mmu->vm) == NULL && vm_state > 0) {
        // a free one that had this window (it has it mapped still), or else
        // a new one while there are, so that each window keeps one; else
        // one that had another, which then unmaps it
        struct vm_space *other = NULL;
        for (sp = vm_spaces; sp != NULL; sp = sp->next) {
            if (sp->mmu != NULL)
                continue;
            if (px_vm_window(sp->vm) == mmu->fm_base)
                break;
            if (other == NULL || px_vm_window(sp->vm) == NULL)
                other = sp;
        }
        if (sp == NULL && other != NULL && px_vm_window(other->vm) == NULL)
            sp = other;
        if (sp == NULL) {
            vm_cpuid();
            uint64_t t0 = vm_now_ns();
            struct px_vm *vm = px_vm_new();
            atomic_fetch_add(&vm_ns_new, vm_now_ns() - t0);
            if (vm == NULL && !px_vm_present()) {
                if (px_vm_verbose)
                    fprintf(stderr, "[vm] no hypervisor: translating\n");
                vm_state = -1;
            } else if (vm != NULL && (sp = calloc(1, sizeof(*sp))) != NULL) {
                sp->vm = vm;
                sp->next = vm_spaces;
                __atomic_store_n(&vm_spaces, sp, __ATOMIC_RELEASE);   // vm_poke looks without the lock
                atomic_fetch_add(&vm_made, 1);
            }
            if (sp == NULL)
                sp = other;   // no slot left to make
        }
        if (sp != NULL) {
            uint64_t t0 = vm_now_ns();
            px_boot_mark("vm: attaching");
            px_vm_attach(sp->vm, mmu->fm_base);
            px_boot_mark("vm: attached");
            atomic_fetch_add(&vm_ns_attach, vm_now_ns() - t0);
            sp->mmu = mmu;
            mmu->vm = sp;
        }
    }
    unlock(&vm_lock);
    return sp;
}

static bool vm_ready(struct cpu_state *cpu) {
    if (vm_state == 0)
        vm_state = vm_wanted() ? 1 : -1;
    if (vm_state < 0 || cpu->mmu->fm_base == NULL)
        return false;
    if (vm_me == NULL) {
        pthread_once(&vm_key_once, vm_key_init);
        if ((vm_me = calloc(1, sizeof(*vm_me))) == NULL)
            return false;
        vm_me->vp = -1;
        pthread_setspecific(vm_key, vm_me);
    }
    struct vm_space *sp = vm_space_of(cpu->mmu);
    if (sp == NULL)
        return false;
    if (vm_me->sp != sp || vm_me->gen != sp->gen) {
        // execve: the processor was the old address space's
        if (vm_me->vp >= 0)
            vm_release(vm_me);
        vm_me->vp = -1;
        vm_pending = false;
    }
    if (vm_me->vp == -2)
        return false;
    if (vm_me->vp == -1) {
        lock(&vm_lock);
        uint64_t t0 = vm_now_ns();
        int vp = sp->mmu == cpu->mmu ? px_vm_vcpu(sp->vm) : -1;
        px_boot_mark("vm: got a processor");
        atomic_fetch_add(&vm_ns_vcpu, vm_now_ns() - t0);
        if (vp >= 0) {
            sp->owner[vp] = cpu;
            sp->poked[vp] = false;
        }
        vm_me->sp = sp;
        vm_me->gen = sp->gen;
        vm_me->vp = vp >= 0 ? vp : -2;
        unlock(&vm_lock);
        if (vp < 0)
            return false;
    }
    return true;
}

// asbestos_free: the address space is going away, and another may get its
// address; its VM goes to the next one
static void vm_mmu_gone(struct mmu *mmu) {
    if (mmu == NULL || mmu->vm == NULL)
        return;
    lock(&vm_lock);
    struct vm_space *sp = mmu->vm;
    px_vm_detach(sp->vm);
    sp->gen++;
    sp->off = false;
    memset(sp->owner, 0, sizeof(sp->owner));
    sp->mmu = NULL;
    mmu->vm = NULL;
    unlock(&vm_lock);
}

void vm_poke_mmu(struct mmu *mmu);

// The address space is translated from now on: each of its threads in the VM
// goes on translated where it stops next (vm_ready says no to them), whatever
// it was doing there; the VM stays its own until it goes away
static void vm_leave(struct mmu *mmu) {
    struct vm_space *sp = mmu->vm;
    if (sp == NULL || sp->off)
        return;
    extern void mem_window_drop_copies(struct mem *mem);   // kernel/memory.c
    mem_window_drop_copies(container_of(mmu, struct mem, mmu));
    sp->off = true;
    vm_poke_mmu(mmu);
}

// kernel/memory.c, with mem->lock taken to write: ish's mappings of [start,
// start + pages) changed (unmapped, protection, copy on write). What the VM has
// of them above the window goes, from the TLBs of its threads running too: as
// they don't hold mem->lock there (vm_run), they are stopped for it. (In the
// window the host's memory itself changes, and the hypervisor sees to that.)
void vm_pages_changed(struct mmu *mmu, page_t start, pages_t pages) {
    struct vm_space *sp = mmu->vm;
    if (sp == NULL)
        return;
    if (px_vm_unmap_pages(sp->vm, (uint64_t) start << PAGE_BITS, (uint64_t) pages << PAGE_BITS)) {
        atomic_fetch_add_explicit(&vm_shootdowns, 1, memory_order_relaxed);
        px_vm_shootdown(sp->vm);
    }
}

// kernel/memory.c, with mem->lock taken to write: no thread of the address
// space runs in the VM until vm_hold(mmu, false), as before none did while a
// writer had the lock
void vm_hold(struct mmu *mmu, bool hold) {
    struct vm_space *sp = mmu->vm;
    if (sp != NULL)
        px_vm_hold(sp->vm, hold);
}

// Every thread of the address space in the VM stops (vm_leave)
void vm_poke_mmu(struct mmu *mmu) {
    struct vm_space *sp = mmu->vm;
    if (sp == NULL)
        return;
    for (int i = 0; i < 64; i++) {
        if (__atomic_load_n(&sp->owner[i], __ATOMIC_ACQUIRE) != NULL) {
            sp->poked[i] = true;
            px_vm_cancel(sp->vm, i);
        }
    }
}

// cpu_poke: a thread in the VM doesn't look at poked_ptr until it comes out.
// (Its address space may be going away: the VMs are looked through, which
// stay.)
static void vm_poke(struct cpu_state *cpu) {
    for (struct vm_space *sp = __atomic_load_n(&vm_spaces, __ATOMIC_ACQUIRE); sp != NULL; sp = sp->next) {
        for (int i = 0; i < 64; i++) {
            if (__atomic_load_n(&sp->owner[i], __ATOMIC_ACQUIRE) == cpu) {
                sp->poked[i] = true;
                px_vm_cancel(sp->vm, i);
            }
        }
    }
}

#define VM_FLAGS 0xcd5   // CF PF AF ZF SF DF OF

static void vm_regs_from_cpu(struct px_vm_regs *r, struct cpu_state *cpu) {
    memcpy(r->gpr, cpu->regs, sizeof(r->gpr));
    r->rip = cpu->rip;
    r->rflags = (cpu->eflags & VM_FLAGS) | 0x202;
    r->fs_base = cpu->tls_ptr;
    r->gs_base = cpu->gs_base;
}

static void vm_regs_to_cpu(struct cpu_state *cpu, const struct px_vm_regs *r) {
    memcpy(cpu->regs, r->gpr, sizeof(r->gpr));
    cpu->rip = r->rip;
    cpu->eflags = (cpu->eflags & ~VM_FLAGS) | (r->rflags & VM_FLAGS);
}

// fxsave's layout: struct fpstate_ in kernel/signal.h, and the same
// conversion as its save_fpstate and restore_fpstate
static void vm_fx_to_cpu(struct cpu_state *cpu, const uint8_t *fx) {
    memcpy(&cpu->fcw, fx, 2);
    memcpy(&cpu->fsw, fx + 2, 2);
    for (int i = 0; i < 8; i++)
        memcpy(&cpu->fp[(cpu->top + i) % 8], fx + 32 + 16 * i, 10);
    memcpy(&cpu->mxcsr, fx + 24, 4);
    memcpy(cpu->xmm, fx + 160, sizeof(cpu->xmm));
}

static void vm_fx_from_cpu(uint8_t *fx, struct cpu_state *cpu) {
    memcpy(fx, &cpu->fcw, 2);
    memcpy(fx + 2, &cpu->fsw, 2);
    for (int i = 0; i < 8; i++)
        memcpy(fx + 32 + 16 * i, &cpu->fp[(cpu->top + i) % 8], 10);
    memcpy(fx + 24, &cpu->mxcsr, 4);
    memcpy(fx + 160, cpu->xmm, sizeof(cpu->xmm));
}

// Load what ish has (a fresh thread, or after an exit it handled: a signal
// may have changed anything) and run.
static void vm_load_and_go(struct px_vm *vm, int vp, struct cpu_state *cpu) {
    struct px_vm_regs r;
    vm_regs_from_cpu(&r, cpu);
    if (!px_vm_set(vm, vp, &r) || !px_vm_set_sse(vm, vp, cpu->xmm, cpu->mxcsr))
        die("vm: can't set registers");
    px_vm_go(vm, vp);
}

static int vm_run(struct cpu_state *cpu) {
    struct vm_space *sp = vm_me->sp;
    struct px_vm *vm = sp->vm;
    int vp = vm_me->vp;
    struct px_vm_mailbox *mb = px_vm_mailbox(vm, vp);
    collapse_flags(cpu);
    if (vm_pending) {
        // the syscall's answer: rax alone if nothing else changed
        vm_pending = false;
        vm_parkable = NULL;
        if (px_vm_stopped_waiting(vm, vp) && vm_at_call.gpr[0] < 512)
            atomic_fetch_add_explicit(&vm_long[vm_at_call.gpr[0]], 1, memory_order_relaxed);
        struct px_vm_regs now;
        vm_regs_from_cpu(&now, cpu);
        now.gpr[0] = vm_at_call.gpr[0];
        if (memcmp(&now, &vm_at_call, sizeof(now)) == 0) {
            px_vm_answer(vm, vp, cpu->rax);
        } else {
            atomic_fetch_add_explicit(&vm_full, 1, memory_order_relaxed);
            vm_fx_from_cpu(mb->fx, cpu);
            px_vm_answer_full(vm, vp);
            vm_regs_from_cpu(&now, cpu);
            if (!px_vm_set(vm, vp, &now))
                die("vm: can't set registers");
            px_vm_go(vm, vp);
        }
    } else {
        uint64_t t0 = vm_now_ns();
        vm_load_and_go(vm, vp, cpu);
        atomic_fetch_add_explicit(&vm_ns_first, vm_now_ns() - t0, memory_order_relaxed);
    }
    // a poke since: taken (as translated code takes it), and the guest
    // stopped to see to it
    if (__atomic_exchange_n(cpu->poked_ptr, false, __ATOMIC_ACQ_REL))
        sp->poked[vp] = true;
    if (sp->poked[vp])
        px_vm_cancel(vm, vp);

    const uint8_t *window = (const uint8_t *) cpu->mmu->fm_base;
    struct mem *mem = container_of(cpu->mmu, struct mem, mmu);
    uint64_t last = ~0ull;
    unsigned same = 0;
    int interrupt;
    for (;;) {
        // While the guest runs, mem->lock is not held: a thread changing the
        // mappings (mmap, munmap, mprotect) doesn't wait for this one, or stop
        // it, unless the VM's page tables change (vm_pages_changed)
        struct px_vm_exit x;
        read_wrunlock(&mem->lock);
        int err = px_vm_wait(vm, vp, &x, &sp->poked[vp]);
        read_wrlock(&mem->lock);
        if (err < 0)
            die("vm: run failed");
        atomic_fetch_add_explicit(&vm_exits[x.kind], 1, memory_order_relaxed);
        if (x.kind == PX_VM_SYSCALL) {
            // as the CPU leaves it: rcx the rip after it, r11 rflags
            struct px_vm_regs *r = &vm_at_call;
            memcpy(r->gpr, mb->gpr, sizeof(r->gpr));
            r->rip = r->gpr[1];
            r->rflags = (r->gpr[11] & VM_FLAGS) | 0x202;
            r->fs_base = cpu->tls_ptr;
            r->gs_base = cpu->gs_base;
            vm_regs_to_cpu(cpu, r);
            vm_fx_to_cpu(cpu, mb->fx);
            vm_pending = true;
            vm_parkable = mb;
            interrupt = INT_SYSCALL;
            break;
        }
        // stopped in user mode
        struct px_vm_regs r;
        if (!px_vm_get(vm, vp, &r))
            die("vm: can't get registers");
        uint64_t t_exit = vm_now_ns();
        if (x.kind == PX_VM_MEMORY) {
            // as a TLB miss: map it in (lazy file pages, growing the stack,
            // copy on write), or it is the guest's fault
            addr_t page = x.addr & ~(addr_t) (PAGE_SIZE - 1);
            struct pt_entry *why = mem_pt(container_of(cpu->mmu, struct mem, mmu), PAGE(page));
            atomic_fetch_add_explicit(why == NULL ? &vm_mem_none : (why->flags & P_LAZY) ? &vm_mem_lazy :
                                      (why->flags & P_COW) ? &vm_mem_cow : &vm_mem_other, 1, memory_order_relaxed);
            same = x.addr == last ? same + 1 : 0;
            last = x.addr;
            void *p = same < 64 ? mmu_translate(cpu->mmu, page, x.is_write ? MEM_WRITE : MEM_READ) : NULL;
            if (p == NULL) {
                cpu->segfault_addr = x.addr;
                cpu->segfault_was_write = x.is_write;
                interrupt = INT_GPF;
                goto out;
            }
            if ((const uint8_t *) p != window + page) {
                // a page ish keeps elsewhere (the vdso, MAP_SHARED): the VM's
                // page tables map it where it is, instead of the window
                struct pt_entry *pe = mem_pt(container_of(cpu->mmu, struct mem, mmu), PAGE(page));
                if (pe != NULL && px_vm_map_page(vm, page, p, pe->data->data, pe->data->size,
                                                 P_WRITABLE(pe->flags), pe->flags & P_EXEC)) {
                    px_vm_go(vm, vp);
                    continue;
                }
                if (pe != NULL && !(pe->flags & (P_WRITE | P_SHARED)) && !x.is_write) {
                    // in ish's image (the vdso), which a VM can't map: a copy
                    // in the window, as it doesn't change. (Committed, then
                    // copied: no thread runs meanwhile, which the lock to
                    // write and vm_hold see to.)
                    uint8_t *w = (uint8_t *) window + page;
                    read_wrunlock(&mem->lock);
                    write_wrlock(&mem->lock);
                    vm_hold(cpu->mmu, true);
                    pe = mem_pt(mem, PAGE(page));
                    if (pe != NULL && !pe->data->window && !px_fm_committed(w)) {
                        px_fm_commit(w, PAGE_SIZE, PROT_READ | PROT_WRITE);
                        memcpy(w, (char *) pe->data->data + pe->offset, PAGE_SIZE);
                        px_fm_protect(w, PAGE_SIZE, PROT_READ);
                    }
                    vm_hold(cpu->mmu, false);
                    write_wrunlock(&mem->lock);
                    read_wrlock(&mem->lock);
                    px_vm_go(vm, vp);
                    continue;
                }
                // out of page tables: only translated code reaches it
                if (getenv("ISH_VM_TRACE") != NULL)
                    fprintf(stderr, "[vm] an address space leaves the VM: can't map %llx (flags %x)\n",
                            (unsigned long long) x.addr, pe != NULL ? pe->flags : 0);
                atomic_fetch_add(&vm_left, 1);
                vm_leave(cpu->mmu);
                interrupt = INT_TIMER;   // as it is, where it was
                goto out;
            }
            atomic_fetch_add_explicit(&vm_ns_mem, vm_now_ns() - t_exit, memory_order_relaxed);
            px_vm_go(vm, vp);
            continue;
        }
        if (x.kind == PX_VM_CANCELED) {
            if (!sp->poked[vp] && !sp->off) {
                // for a TLB shootdown: on, with the page tables loaded again
                px_vm_go(vm, vp);
                continue;
            }
            sp->poked[vp] = false;
            interrupt = INT_TIMER;
            goto out;
        }
        if (x.kind != PX_VM_EXCEPTION) {
            fprintf(stderr, "[vm] exit 0x%x at %llx\n", x.reason, (unsigned long long) r.rip);
            interrupt = INT_GPF;
            goto out;
        }
        static int trace = -1;
        if (trace < 0)
            trace = getenv("ISH_VM_TRACE") != NULL;
        if (trace)
            fprintf(stderr, "[vm] exception %u error %x addr %llx at rip %llx rsp %llx\n", x.vector, x.error,
                    (unsigned long long) x.addr, (unsigned long long) r.rip, (unsigned long long) r.gpr[4]);
        if (x.vector == 14 && x.addr >= ((uint64_t) FM_GUEST_PAGES << PAGE_BITS)) {
            // above the window: the page goes in the VM's page tables, as a
            // TLB miss would (lazy pages, growing the stack, copy on write)
            // or it is the guest's fault
            bool write = x.error & 2, fetch = x.error & 0x10;
            addr_t page = x.addr & ~(addr_t) (PAGE_SIZE - 1);
            same = x.addr == last ? same + 1 : 0;
            last = x.addr;
            struct mem *mem = container_of(cpu->mmu, struct mem, mmu);
            void *p = same < 64 ? mmu_translate(cpu->mmu, page, write ? MEM_WRITE : MEM_READ) : NULL;
            struct pt_entry *pe = p != NULL ? mem_pt(mem, PAGE(page)) : NULL;
            if (pe != NULL && !(fetch && !(pe->flags & P_EXEC))) {
                atomic_fetch_add_explicit(&vm_high_faults, 1, memory_order_relaxed);
                if (px_vm_map_page(vm, page, p, pe->data->data, pe->data->size,
                                   P_WRITABLE(pe->flags), pe->flags & P_EXEC)) {
                    // and the pages around it ish has already, of the same
                    // mapping: an exit each would cost far more
                    page_t at = PAGE(page);
                    for (page_t q = at > FAULT_AROUND / 2 ? at - FAULT_AROUND / 2 : 0; q < at + FAULT_AROUND / 2; q++) {
                        struct pt_entry *qe = q != at ? mem_pt(mem, q) : NULL;
                        if (qe == NULL || qe->data != pe->data || (qe->flags & P_LAZY))
                            continue;
                        px_vm_map_page(vm, (uint64_t) q << PAGE_BITS, (char *) qe->data->data + qe->offset,
                                       qe->data->data, qe->data->size, P_WRITABLE(qe->flags), qe->flags & P_EXEC);
                    }
                    atomic_fetch_add_explicit(&vm_ns_high, vm_now_ns() - t_exit, memory_order_relaxed);
                    px_vm_go(vm, vp);
                    continue;
                }
                // out of page tables, or where our pages are: only translated
                // code reaches it
                if (trace)
                    fprintf(stderr, "[vm] an address space leaves the VM: can't map %llx\n",
                            (unsigned long long) x.addr);
                atomic_fetch_add(&vm_left, 1);
                vm_leave(cpu->mmu);
                interrupt = INT_TIMER;   // as it is, where it was
                goto out;
            }
            cpu->segfault_addr = x.addr;
            cpu->segfault_was_write = write;
            interrupt = INT_GPF;
            goto out;
        }
        switch (x.vector) {
        case 14:
            cpu->segfault_addr = x.addr;
            cpu->segfault_was_write = (x.error & 2) != 0;
            interrupt = INT_GPF;
            break;
        case INT_GPF:
            cpu->segfault_addr = 0;
            cpu->segfault_was_write = false;
            interrupt = INT_GPF;
            break;
        default:
            interrupt = x.vector;
            break;
        }
    out:
        vm_regs_to_cpu(cpu, &r);
        if (!px_vm_get_sse(vm, vp, cpu->xmm, &cpu->mxcsr))
            die("vm: can't get registers");
        break;
    }
    expand_flags(cpu);
    return interrupt;
}
