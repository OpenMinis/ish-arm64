# Experimental native/AOT precise fault restart — 29 September 2026

## Scope

Follow-up to the bounded repairs at `083eca69`, on `fix/jit-aot-safety` only.
The user selected the remaining restart bug. `master` remains at `1e7ad5e9`,
version 2.2.1/build811; no backend import, version bump, tag or release.
Do not merge the whole experimental branch: its upstream base contains unrelated
kernel and platform changes.

The repair replaces native block replay with an **exact-access checkpoint**.
Host-adapted actual-fault tests now preserve prefix effects and restart at the
faulting guest instruction. This is not full Darwin/iOS backend acceptance.

## Failure and repair

Previously the CLI handler synchronised pinned registers, inferred the address
from gadget registers x7/x10, then replaced the PC with `jit_saved_pc` (block
start). Native accesses use different scratch registers. Replaying the block
with its already-committed state repeats earlier register/memory effects. A
native range alone also cannot establish canonical register state during loop
promotion, prologue or exit sequences.

`emit_guest_access` now wraps each supported native integer or SIMD/FP single
or pair access:

1. Save all currently pinned/promoted guest values into the frame. Non-pinned
   values, vector results and NZCV already use write-through stores. Promotion
   donors stay in their canonical backing slots. Save w15 to the low 32 bits of
   the cycle field, preserving the high bytes of the host `long`.
2. Record the exact guest instruction PC, guest effective address and direction.
   This happens after a successful TLB lookup and before guest result/writeback.
   PIC PC construction uses the translation's runtime slot base, not its original
   recording address or ADR/ADRP data base.
3. Use ADR to arm **one exact host instruction PC**, execute the access, then
   disarm before committing load results or base writeback. ADR remains correct
   when code is copied into a read-only AOT mapping.

Checkpoint fields are appended to `fiber_frame`, preserving existing immediate
ranges. Static assertions guard base, alignment and encoding limits. Dispatch
supplies a thread-local trusted frame around `fiber_enter` and clears the
checkpoint at entry/exit.

`jit_crash_recover` accepts only an armed exact-PC match with x1 equal to that
frame's CPU. It restores fault metadata from the checkpoint, clears stale gadget
fault-stream state and consumes the checkpoint. It does **not** try to infer
promoted registers from the fault context. The old loopmap and its signal-path
mutex/allocation are removed.

An unmatched native/AOT PC returns an explicit rejection. The CLI emits a fixed
message with `write` and exits 139 without block replay, `snprintf`, symbol lookup
or backtrace. Unrelated non-native faults retain the existing path; this is not
an audit/fix of arbitrary gadget C-helper recovery.

The CLI no longer overwrites native checkpoint metadata with block-start/gadget
register guesses. Post-fiber dispatch preserves the precise PC on
`INT_JIT_CRASH`, flushes TLB/block/return caches and retries at `cpu.pc`. The
existing bound of 16 consecutive crashes still escalates to `INT_GPF`.

## AOT compatibility

`JIT_CODE_VERSION` is bumped **7 → 8** and all four checkpoint offsets participate
in `jit_abi()`. Pre-checkpoint images must be re-recorded/rebuilt, not loaded or
patched in place. The regression computes the actual frozen version-7 ABI and
passes it through the real image-admission function alongside the new ABI:
old rejected, current accepted.

Emitted checkpoints carry no extra per-site allocation or signal-handler lookup
table. They add per-access stores and PC construction, so overhead is expected;
no speedup or acceptable-overhead claim has been measured.

## Reproducible tests

No Makefile exists on this experimental upstream-derived branch. From its root:

```sh
EVIDENCE_DIR=/absolute/evidence/candidate bun tests/regress/jit-safety/run.ts
BASELINE_REV=083eca69 EVIDENCE_DIR=/absolute/evidence/baseline \
  bun tests/regress/jit-safety/run.ts
PYTHONDONTWRITEBYTECODE=1 python3 -Werror tests/regress/jit-safety/test_benchmark.py
git diff --check
```

Requires Linux AArch64, Clang, Bun, timeout and Git history. The adapter removes
Apple-only declarations in a private copy; it does not change the production
platform guard or provide a Linux backend. See the
[test README](../tests/regress/jit-safety/README.md).

Results at **both `-O0` and `-O2`**:

- **100 real host-fault/retry cases**: ten integer/vector load/store forms × five
  modes × first/second-in-unit access. Modes are unpinned, pinned, promoted loop,
  moved PIC/AOT-only with no dynamic region, and pinned code reached by a direct
  branch from a different native allocation.
- Prefix increment and memory store happen once. Exact PC/address/direction,
  destination preservation on fault, delayed pre/post writeback, changed promoted
  value, preserved donor, NZCV and changed low-word cycle all checked. The
  handler receives a deliberately incorrect ESR direction; checkpoint wins.
- Extracted actual post-fiber policy and actual TLB flush check invalidated
  caches, precise-PC lifetime, retry and bounded escalation. Guest access is
  then retried through a fresh actual emitter sequence at the exact PC.
- Unmatched native and AOT access subprocesses exit 139, never return through
  the trampoline or retry. Function probes reject missing/untrusted frame/x1,
  unrelated/end PCs and range-only recovery.
- Actual old image ABI rejected; new ABI admitted.
- **8,576 integer + 2,720 memory native-oracle comparisons** pass. Memory cases
  include unaligned accesses, register offsets, sign extension, pairs and
  writeback, pinned and unpinned. SIMD QC-changing forms rejected 14/14; SMAX
  passes 16 lanes. Family matching and live-slot ownership checks pass.
- Existing region allocation (including eight-thread unique reservations), dump
  bounds and benchmark validity tests pass; benchmark suite has **19 tests**.
- Dispatch syntax check and diff whitespace check pass.

The frozen `083eca69` emitter/CLI/dispatch fail the exact-PC assertion (abort134)
in **all five modes at each optimisation**. The negative runner rejects timeouts,
setup failures or different assertions, rather than counting them as evidence.

Host: Orange Pi 6 Plus, CIX P1/CD8180-class 12-core AArch64 SoC, 16GB-class RAM
(~14GiB visible), NVMe, Debian Trixie, host-native tools. Evidence directory:
`/workspace/tmp/ish-jit-restart-5zgt80/`, pointer
`/workspace/tmp/ish-jit-restart-path`. A delegated source review timed out and is
not counted as independent review.

## Limits and remaining adoption gates

- Tests use the real emitter and CLI handler, with Linux signals converted to
  the Darwin context shape and a test trampoline following the same saved-SP
  ABI. They do not execute Apple's real signal return or production trampoline.
- Dispatch crash-policy code is exercised in isolation; full scheduler, MMU
  refill and chained invalidation integration is not linked/run here. TLB refill
  in the harness is explicit. Cross-page accesses still bail out to gadgets;
  native pairs tested here remain within one page.
- Relocated PIC instructions and real AOT admission are tested, **not** the full
  record → compact → link → signed image → device execution pipeline.
- Darwin/iOS device permissions, signing, real AOT-only application recovery,
  concurrent remap/invalidation, code publication and sustained metadata lifetime
  still need validation. Allocator bounds do not supply reclamation.
- No sanitizer or performance validation. No claim to fix the earlier mainline
  proc-stress exit139, which remains unattributed.

The demonstrated native replay defect is repaired within this bounded scope.
The experimental backend remains unsuitable for wholesale mainline adoption
until the platform and end-to-end gates above are met.
