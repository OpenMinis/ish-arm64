#ifndef KERNEL_MM_H
#define KERNEL_MM_H

#include <stdatomic.h>
#include <stdbool.h>
#include "kernel/memory.h"
#include "misc.h"

// Maximum anonymous GUEST pages across ALL processes.
// Prevents iOS app from being killed by jetsam.
// 0 = no limit. Non-zero = hard limit in guest pages (4KB each).
// Go runtime alone needs ~1.1GB for page summary reservations (PROT_NONE),
// which do NOT count here — the mmap path exempts PROT_NONE explicitly.
//
// [T-ish-anon-cap-page-units] BUDGET THE HOST, COUNT THE GUEST. This counter
// is in 4KB guest pages, but what jetsam kills us over is HOST bytes, and on
// arm64 iOS the host page is 16KB. Because every committed guest page takes a
// whole host page (see the per-page pt_map_nothing paths), 1 counted page
// costs 16KB of footprint, not 4KB. Converting a host-byte budget with the
// guest page size therefore over-permits by exactly 4x.
//
// That is not hypothetical: the 2026-08-25 device test installed a nominal
// "1953 MB" limit that actually authorised ~7.6GB of host memory, so the
// runaway compile sailed from 102MB to 3361MB — jetsam-killed with the
// counter at only ~42% of the limit and not one refusal logged. Confirmed in
// the memgraph: 116,836 VM_ALLOCATE regions, every one exactly 16KB, holding
// just 456MB worth of guest pages while consuming 1826MB of host memory.
//
// So: 131072 guest pages x 16KB host page = 2GB of actual host memory.
// Anything converting bytes→pages for this counter must divide by the HOST
// page size (getpagesize()), never by PAGE_SIZE.
//
// [T-ish-anon-cap-above-jetsam] Keep this BELOW the iOS jetsam threshold, or
// the whole mechanism is decorative. 5d8e1e1a raised it 2GB → 4GB for Node,
// which put it above the point where iOS kills the app: a runaway guest
// allocation now reached jetsam before ever reaching the cap, so the guest
// never got the ENOMEM that would have failed one command, and the user lost
// the entire app instead. Observed 2026-08-24: a guest compile grew the app
// footprint ~40MB/s from 135MB to 3.36GB over ~100s and was SIGKILLed, twice,
// with anon_page_count still far under 1048576.
//
// Node does not actually need 4GB: exec.c injects --max-old-space-size=512,
// so V8's heap is bounded at 512MB regardless of what this allows.
//
// 2GB of host memory leaves headroom under jetsam on the smallest supported
// device while staying far above any legitimate workload.
//
// [T-ish-anon-cap-dynamic] This constant is now the CEILING, not the limit.
// A fixed number cannot sit on the right side of jetsam on every device: the
// threshold scales with RAM (iPhone 8 ≈ 1.4GB foreground vs iPhone 17 Pro
// 6GB+), and 2GB is decorative on the former while over-conservative on the
// latter. The effective limit lives in `anon_page_limit` below: the host
// derives it from os_proc_available_memory() at kernel boot and installs it
// via ish_set_anon_page_limit(), which clamps to at most this ceiling.
// Builds whose host never calls the setter (tests, Linux) keep the ceiling.
#define ANON_MMAP_LIMIT_PAGES 131072

#if ANON_MMAP_LIMIT_PAGES > 0
extern _Atomic long anon_page_count;
// Effective limit in guest pages. Always in (0, ANON_MMAP_LIMIT_PAGES].
extern _Atomic long anon_page_limit;
// Install a host-derived limit, in GUEST pages. Callers converting a host
// byte budget must divide by the HOST page size — see the page-units note
// above. Values ≤ 0 are ignored; values above the ceiling are clamped.
void ish_set_anon_page_limit(long pages);
// Check-and-add `pages` against anon_page_limit. Returns false (and adds
// nothing) when the limit would be exceeded. All allocation sites that CAN
// fail gracefully must go through this instead of a bare fetch_add.
bool anon_pages_reserve(long pages);
void anon_pages_unreserve(long pages);

// [T-ish-anon-count-negative] Charge/uncharge must stay symmetric or the
// counter drifts negative and the cap silently grants free headroom.
//
// The single source of truth for "does this mapping count": PROT_NONE
// reservations occupy address space but no physical memory, so they are
// neither charged when mapped nor decremented when unmapped. pt_map_nothing
// and pt_unmap both gate on this — keep them agreeing.
static inline bool anon_page_is_charged(unsigned flags) {
    return (flags & (P_READ | P_WRITE | P_EXEC)) != 0;
}
// Charge `pages` that were actually mapped, minus anything the caller already
// reserved via anon_pages_reserve(). Called by pt_map_nothing only.
void anon_pages_charge_mapped(long pages);
// Park a reservation for the mapping that immediately follows on this thread.
void anon_pages_precharged(long pages);
// Read the parked reservation without consuming it (retry paths).
long anon_pages_precharge_peek(void);

// [T-ish-footprint-brake] Footprint-based memory governor, stage 1.
//
// Once the host starts feeding live memory status through
// ish_set_memory_status(), the ledger above STOPS being the admission
// control and becomes accounting only (meminfo, diagnostics, dmesg). The
// admission decision moves to the number jetsam actually kills on: the
// app's physical footprint against its live limit.
//
// Why: the ledger charges COMMITMENT, jetsam charges DIRTY pages. Node
// commits 640MB of V8 cage at startup while dirtying a few tens of MB —
// the ledger refused a workload the device could easily run. Conversely,
// the ledger cannot see the app's own memory (WebViews, transcripts), so
// staying under it never guaranteed safety. The footprint feed measures the
// truth instead of modelling it; the whole class of charge/uncharge
// asymmetry bugs stops being safety-critical.
//
// States: OK admits everything; BRAKE refuses NEW anonymous commitments
// (mmap/brk ENOMEM, reservation mprotect-commits ENOMEM, lazy fault commits
// SIGSEGV — the same failure Linux gives when overcommitted memory cannot
// be backed). Enter BRAKE below 10% headroom, leave above 15% (hysteresis),
// and a critical memory-pressure event forces it regardless. A stale feed
// (no update for >2s) also reads as BRAKE: a dead sampler must fail closed.
//
// Two things stay admitted even under BRAKE, deliberately:
//   * the growsdown fault page (a refused stack page corrupts the frame;
//     callers shrink to 1 page and map it regardless — pre-existing rule);
//   * COW breaks (the guest already owns that memory; refusing the copy
//     would SIGSEGV a legitimate write to committed memory. Runaway dirtying
//     through COW is a stage-2 / guest-OOM-killer concern).
//
// Builds whose host never calls the setter (tests, Linux CLI) never enter
// footprint mode and keep the legacy ledger cap unchanged.
enum ish_mem_state { ISH_MEM_OK = 0, ISH_MEM_BRAKE = 1 };
// Host sampler feed. `limit_bytes` = phys_footprint + available (the live
// jetsam allowance); `avail_bytes` = os_proc_available_memory();
// `pressure_critical` = the OS sent a critical memory-pressure event.
void ish_set_memory_status(uint64_t limit_bytes, uint64_t avail_bytes, bool pressure_critical);
// True once the host feed has been installed (footprint mode active).
bool ish_footprint_mode(void);
// Admission check for `bytes` of new anonymous commitment. In legacy mode
// always true (the ledger check in anon_pages_reserve decides); in
// footprint mode false when braked, stale, or the single request exceeds
// the whole limit (an absurd ask deserves a clean upfront ENOMEM rather
// than a mid-memset death).
bool ish_mem_commit_ok(uint64_t bytes);
#endif

// uses mem.lock instead of having a lock of its own
struct mm {
    atomic_uint refcount;
    struct mem mem;

    addr_t vdso; // immutable
    addr_t start_brk; // immutable
    addr_t brk;

    // crap for procfs
    addr_t argv_start;
    addr_t argv_end;
    addr_t env_start;
    addr_t env_end;
    addr_t auxv_start;
    addr_t auxv_end;
    addr_t stack_start;
    struct fd *exefile;

    // Main executable load bias + entry point (ARM64 only — used to
    // precisely identify V8's self-abort BRK site in node at signal time).
    addr_t exe_bias;
    addr_t exe_entry;
};

// Create a new address space
struct mm *mm_new(void);
// Clone (COW) the address space
struct mm *mm_copy(struct mm *mm);
// Increment the refcount
void mm_retain(struct mm *mem);
// Decrement the refcount, destroy everything in the space if 0
void mm_release(struct mm *mem);

#endif
