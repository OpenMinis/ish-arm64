#ifndef KERNEL_HVF_H
#define KERNEL_HVF_H

// Guest code run natively, at EL0 of a Hypervisor.framework VM (macOS on
// Apple silicon). The guest and the host are both arm64, so nothing has to
// be translated: ish stays the kernel. Exceptions from EL0 go to vectors at
// EL1 that only `hvc` out to us, where a system call becomes INT_SYSCALL as
// in the JIT and a fault either gets its page table entry or becomes
// INT_GPF. One VM per ish process; every address space gets its own stage-1
// page table (and ASID) that ish fills from its own page table as pages are
// touched, and every guest thread its own vCPU.
//
// Guest memory is the host memory that already backs it: the shadow window
// (kernel/sba.h) of an address space is mapped into the VM's physical
// address space 2MB at a time, any other memory (shared mappings, the vdso)
// a data object at a time. The VM keeps the host pages it was given, so
// whatever replaces host pages in the window (resync() in kernel/sba.c)
// maps them again.
//
// On by default (macOS), ISH_HVF=0 turns it off. Needs the
// com.apple.security.hypervisor entitlement, which the meson build gives the
// CLI (tools/hvf-sign.sh); without it, or on too many threads, the JIT runs
// the guest as before.

#include "util/macopt.h"
#if ISH_HVF
#include <stdbool.h>
#include <stdint.h>
#include "emu/mmu.h"

struct cpu_state;
struct mem;
struct task;
struct data;
struct sba_region;

#define HVF_NO (-2)   // hvf_run(): not this thread; run it the other way

bool hvf_on(void);
// Run current's guest code until the kernel has something to do: an
// interrupt for handle_interrupt(), or HVF_NO.
int hvf_run(struct cpu_state *cpu);
// Make the task's vCPU leave guest code, now or as it next enters (cpu_poke()).
void hvf_kick(struct task *task);
// The thread is going away (its vCPU with it).
void hvf_thread_exit(void);
// mem write-locked: guest code must not reach [start, start + pages) the old
// way any more.
void hvf_invalidate(struct mem *mem, page_t start, pages_t pages);
void hvf_mm_destroy(struct mem *mem);
// The window's host pages at shadow offsets [a, b) were replaced.
void hvf_region_remap(struct sba_region *r, uint64_t a, uint64_t b);
void hvf_region_free(struct sba_region *r);
void hvf_data_free(struct data *data);
void hvf_stats(void);

#endif
#endif
