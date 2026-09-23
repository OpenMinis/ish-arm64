#ifndef NATIVE_OFFLOAD_H
#define NATIVE_OFFLOAD_H

#include <stdbool.h>
#include <sys/types.h>
#include <signal.h>
struct task;

// ============================================================================
// Native Offload — bypass emulation for selected guest binaries
// ============================================================================
//
// When guest execve() matches a registered binary name, execution is
// intercepted and routed to either:
//   1. An in-process handler function (iOS + macOS)
//   2. A native host binary via posix_spawn (macOS only)
//
// The handler receives argc/argv (with guest paths translated to host paths)
// and pipe-backed stdio fds. Output written to stdout_fd/stderr_fd is
// forwarded through the guest TTY driver, so it appears in the terminal UI.
//
// --- Quick integration guide ---
//
//   // 1. Define your handler (runs on guest thread, in-process)
//   static int my_tool_main(int argc, char **argv,
//                           int stdin_fd, int stdout_fd, int stderr_fd) {
//       dprintf(stdout_fd, "hello from native handler\n");
//       return 0; // exit code
//   }
//
//   // 2. Register it (call once at app startup, before guest runs)
//   native_offload_add_handler("mytool", my_tool_main);
//
//   // 3. Done — when guest runs `execve("/usr/bin/mytool", ...)`,
//   //    my_tool_main() is called instead of emulating the ELF binary.
//
// Notes for handler authors:
//   - argv paths starting with '/' are auto-translated to host filesystem
//   - CWD is set to the host equivalent of the guest's CWD
//   - Use dprintf(stdout_fd, ...) and dprintf(stderr_fd, ...) for output
//   - stdin_fd may be -1 if guest stdin is not backed by a real host fd
//   - Return 0 for success, non-zero for failure (becomes guest exit code)
//   - Handler runs on the guest thread; do_exit() is called after return
//
// macOS CLI also supports host binary offload:
//   ish -n ffmpeg                     # auto-detect /opt/homebrew/bin/ffmpeg
//   ish -n ffprobe=/usr/local/bin/ffprobe  # explicit path
//
// ============================================================================

#define NATIVE_OFFLOAD_MAX 32

// Handler function signature. Called in-process on the guest thread.
// Receives translated argv (guest absolute paths → host paths) and
// pipe-backed stdio fds for output.
// Must return an exit code (0 = success).
typedef int (*native_handler_func)(int argc, char **argv,
                                   int stdin_fd, int stdout_fd, int stderr_fd);

// Register an in-process handler for a guest binary name.
// When guest execve() basename matches guest_name, handler is called
// instead of emulating the binary. Takes priority over host binary lookup.
// Returns 0 on success, -1 if registry is full.
int native_offload_add_handler(const char *guest_name, native_handler_func handler);

// [T-ish-offload-signal-forward] Abort callback for an in-process handler.
//
// WHY THIS EXISTS: a handler runs synchronously on the guest thread and
// `exec_handler` only calls do_exit() once it returns. A handler that never
// returns (ffmpeg wedged inside a VideoToolbox transcode is the reported case)
// therefore pins the guest task in the process table forever, and `kill -9`
// silently does nothing: send_signal queues the signal and cpu_poke()s a thread
// that is not in the CPU loop to see it.
//
// Signals cannot be delivered *to* such a handler from outside — do_exit()
// operates on the thread-local `current` and ends in pthread_exit(), so only
// the task's own thread can reap it. The only workable shape is to ask the
// handler to stop waiting and return, which is what this callback does.
//
// Contract for implementors:
//   - Called from the SIGNALLING thread, not the handler's own thread, while
//     the handler is still running. It must be async-safe with respect to the
//     handler body: signal an event, do not take a lock the handler holds.
//   - Returns true if the handler will now wind down, false if it cannot be
//     aborted (the signal then falls through to normal guest delivery).
//   - The handler is expected to return "soon" after this; whatever host work
//     it had started may keep running. Abandoning that work is the caller's
//     accepted cost, and the handler should say so in the log.
typedef bool (*native_abort_func)(int sig);

// Attach an abort callback to an already-registered in-process handler.
// Returns 0 on success, -1 if `guest_name` has no handler registered.
// Optional: a handler without one simply cannot be interrupted, which is the
// pre-existing behaviour for every offload.
int native_offload_set_abort_handler(const char *guest_name, native_abort_func abort_fn);

// Register a host binary offload (macOS CLI only, uses posix_spawn).
// spec is "name" or "name=/host/path". Returns 0 on success.
int native_offload_add(const char *spec);

// Check if a guest binary should be offloaded.
// Returns native host path, "[builtin]" for handler-only, or NULL.
// Applies the path policy (native_offload_policy.h) but not the env hatch.
const char *native_offload_lookup(const char *guest_path);

// [T-ish-offload-path-scope] OpenMinis#288. The exec-time lookup: path policy
// AND the env escape hatch (`envp` = the packed, double-NUL-terminated exec
// envp; may be NULL). `*generic_out` (optional) reports whether the matched
// offload shadows a real program, in which case the caller must also refuse to
// offload a file that is a #! script — that check needs the filesystem, so it
// lives at the call site in exec.c.
const char *native_offload_lookup_exec(const char *guest_path, const char *envp,
                                       bool *generic_out);

// Execute the offloaded binary (handler or posix_spawn).
// Takes over the current guest task and calls do_exit(). Does not return
// on success. Returns negative errno on failure.
int native_offload_exec(const char *native_path,
                        const char *guest_file,
                        size_t argc, const char *argv,
                        const char *envp);

// Forward a signal to the native work backing a task, covering BOTH offload
// shapes:
//   - posix_spawn proxy tasks  -> kill(native_pid, ...)
//   - in-process handler tasks -> the handler's registered abort callback
// Returns true if the signal was consumed by one of those paths.
//
// [T-ish-offload-signal-forward] The handler branch is the addition. Before it,
// this returned false for every in-process handler, so `kill` on a wedged
// ffmpeg queued a signal nobody would ever read.
bool native_offload_forward_signal(struct task *task, int sig);

// ============================================================================
// Symbol-level native offload (ARM64 guest only)
// ============================================================================
//
// The API above offloads at *binary* granularity: a whole guest command
// (matched by execve) runs as a host process or in-process main(). The API
// below offloads at *function* granularity: a single guest function (matched
// by its entry PC) runs as a native handler, skipping gadget translation for
// that call. Same "native offload" concept, finer grain.
//
// Use binary-level for "run this whole tool natively" (curl, ffmpeg).
// Use symbol-level for "run this hot function natively" (memcpy, Py compile).
//
//   // 1. Define a handler that reads guest args and writes results.
//   static enum nsym_result my_memcpy(struct nsym_ctx *c, void *user) {
//       addr_t dst = nsym_arg(c, 0), src = nsym_arg(c, 1);
//       uint64_t n = nsym_arg(c, 2);
//       // ... value-copy via nsym_read/nsym_write ...
//       nsym_set_ret(c, dst);
//       return NSYM_HANDLED;   // or NSYM_DECLINED to fall back to guest code
//   }
//   // 2. Register (binary basename + symbol name).
//   native_offload_add_symbol("libc.musl-aarch64.so.1", "memcpy",
//                             my_memcpy, NULL);
//
// Lossless: if disabled (ISH_OFFLOAD unset), unmatched, or a handler returns
// NSYM_DECLINED, the guest executes its own code unchanged.

#include <stdint.h>
#include "misc.h"   /* addr_t */
struct tlb;
struct cpu_state;

enum nsym_result {
    NSYM_HANDLED,    /* handler did the work; resume guest per ctx->resume */
    NSYM_DECLINED,   /* handler opted out; run the guest function normally */
};

enum nsym_resume {
    NSYM_RESUME_RET, /* return to link register (x30) — normal call return */
    NSYM_RESUME_PC,  /* jump to ctx->resume_pc (handler set it explicitly) */
};

// Context for a symbol-level handler: wraps guest CPU + TLB. All memory
// access is value-copy (fork/CoW safe), matching the mem* bypass discipline.
struct nsym_ctx {
    struct cpu_state *cpu;
    struct tlb *tlb;
    enum nsym_resume resume;   /* handler sets this; default RET */
    addr_t resume_pc;          /* used when resume == NSYM_RESUME_PC */
};

// AArch64 AAPCS64: integer args in x0..x7, return in x0.
uint64_t nsym_arg(struct nsym_ctx *ctx, int n);
void     nsym_set_ret(struct nsym_ctx *ctx, uint64_t v);
// Guest memory access — value-copy, fork-safe. Return false on fault.
bool nsym_read(struct nsym_ctx *ctx, addr_t gaddr, void *buf, size_t len);
bool nsym_write(struct nsym_ctx *ctx, addr_t gaddr, const void *buf, size_t len);

typedef enum nsym_result (*nsym_handler_func)(struct nsym_ctx *ctx, void *user);

// Register a symbol-level offload: (binary basename, symbol) -> handler.
// addr_hint is a fixed guest entry address for the no-ASLR case (0 = resolve
// by name later via native_offload_bind_symbol). Returns id >=0 or -1.
int native_offload_add_symbol(const char *binary, const char *symbol,
                              nsym_handler_func handler, void *user);

// Bind a registered symbol to a runtime entry address (called by the loader
// when a matching binary maps a matching symbol). Idempotent.
void native_offload_bind_symbol(const char *binary, const char *symbol, addr_t addr);

// Dispatch hook — called from the ARM64 JIT loop with the current guest PC.
// Returns true if a handler fired (guest state advanced), false to run guest
// code. Cheap on the common miss (a few address compares).
bool native_offload_sym_dispatch(addr_t pc, struct cpu_state *cpu, struct tlb *tlb);

// True when at least one symbol target is registered. The dispatch loop
// checks this to skip the scan entirely when nothing is registered — offload
// is lossless, so "registered == active" is the only gate (no env switch,
// matching binary-level offload where registration alone enables it).
bool native_offload_sym_active(void);

// Register built-in symbol targets (mem* for verification; Python compile
// when built with -DISH_OFFLOAD_PYCOMPILE). Called once from _sym_active().
// A no-op unless the relevant env/build flags are set.
void native_offload_sym_init_builtins(void);

// Register a symbol target with a fixed guest entry address (no-ASLR case),
// pre-binding it immediately instead of waiting for loader resolution.
// Prefer native_offload_add_symbol + native_offload_bind_symbol when symbol
// resolution is available.
int native_offload_add_symbol_hinted(const char *binary, const char *symbol,
                                     addr_t addr_hint, nsym_handler_func handler,
                                     void *user);

// ============================================================================
// Pre-built gadget offload (ARM64 guest only)
// ============================================================================
//
// A third offload granularity: replace a whole registered guest FUNCTION's
// block with a single "prebuilt gadget" — one native function that reproduces
// the guest function's logic, staying entirely in guest semantics (reads/
// writes cpu_state regs + guest memory via the TLB). Unlike symbol-level
// offload (which swaps in a host implementation and hits the host/guest object
// boundary), a prebuilt gadget is "the same guest logic, translated ahead of
// time to faster host code" — so it works for any function, including ones
// that return guest PyObjects. spec_fn is generated offline (hand-written now,
// clang auto-translation later) and compiled statically into iSH.
//
// spec_fn signature: void spec_fn(struct cpu_state *cpu, struct tlb *tlb).
// It must read args from cpu->regs[], do the work, write results to cpu->regs[]
// (and guest memory via tlb_read/tlb_write), and leave cpu unchanged otherwise.
// The trampoline (gadget_prebuilt_entry) resumes the guest at the return address.
typedef void (*prebuilt_fn)(struct cpu_state *cpu, struct tlb *tlb);

// Register a prebuilt-gadget target: when block compilation reaches guest_addr,
// emit a single prebuilt gadget calling spec_fn instead of the normal stream.
int native_offload_add_prebuilt(const char *binary, const char *symbol,
                              addr_t guest_addr, prebuilt_fn spec_fn);

// Look up a registered prebuilt gadget for a block starting at guest pc.
// Returns spec_fn (and sets *ret_pc to the guest return address) on hit,
// or NULL if not registered. Called from fiber_block_compile.
prebuilt_fn native_offload_prebuilt_lookup(addr_t pc);

// True if any prebuilt gadget is registered (fiber_block_compile fast-skip).
bool native_offload_prebuilt_active(void);

// Register built-in prebuilt targets. Called once, lazily. No-op unless gated.
void native_offload_prebuilt_init(void);

// --- Mixed execution: nested guest calls from a prebuilt spec_fn ---
//
// When a prebuilt spec_fn reaches a guest bl/blr, it can't translate the callee
// (another function). Instead it calls the callee "the guest way": prebuilt_call
// sets the guest LR to PREBUILT_SENTINEL, points guest PC at the target, and
// re-enters the dispatch loop. The callee runs as normal threaded-code; when it
// returns to the sentinel, the loop hands control back here. The spec_fn then
// reads the result from cpu->regs[0] and continues. This is "mixed execution":
// the specialized body runs as straight host code, but each guest call boundary
// drops back into the interpreter for that one call.
//
// PREBUILT_SENTINEL is a canonical non-zero guest address inside the low guard
// region that real code never executes; the dispatch loop treats reaching it as
// INT_PREBUILT_RET. Page-aligned, within the 48-bit guest space, high enough not
// to collide with a real entry point.
#define PREBUILT_SENTINEL 0x0000fffffffff000ULL

// Run guest function `target_pc` to completion (as threaded-code), preserving
// and restoring the caller's PC/LR, and return its result (guest x0). Args must
// already be in cpu->regs. Called from generated spec_fns at bl/blr sites.
uint64_t prebuilt_call(struct cpu_state *cpu, struct tlb *tlb, addr_t target_pc);

#endif
