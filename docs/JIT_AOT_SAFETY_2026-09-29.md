# Experimental JIT/AOT bounded safety fixes — 29 September 2026

## Branch and scope

This is **`fix/jit-aot-safety`**, based on the OpenMinis AOT-family tip
`b2c768fda2e599cfd186f8eb29c94654c9fe24ec`, not a merge into ios-linuxkit's
released gadget engine. The latter stays at `1e7ad5e9` on `master`, version
2.2.1/build811, with its earlier selective CLI fix. Do not merge this entire
experimental branch into master: its base includes unrelated upstream kernel,
platform and AOT changes.

The user authorised fixing useful findings from the earlier investigation.
This tranche fixes three bounded defects, adds regressions and leaves JIT/AOT
opt-in defaults unchanged. **It does not make native host-fault retry safe.**
No version bump, tag, signed archive, device validation or performance claim.

## Changes

### Bounded code-region reservations and dumps

`asbestos/guest-arm64/jit.c:region_alloc` now reserves aligned space with a CAS
loop. Zero, absent-region, oversized and exhausted requests fail without
advancing the cursor. Oversized requests are rejected before alignment, so
`SIZE_MAX + 15` cannot wrap. The final successful reservation can exactly fill
the region; later failed requests leave its high-water mark unchanged.

Relaxed atomics provide unique reservations, **not code publication**. Emitted
code publication and cache invalidation remain separate existing mechanisms.
`jit_report` independently clamps the dump payload to the 120MiB mapping and
writes a zero payload when there is no region. Dumping while other threads are
still emitting is not a coherent snapshot guarantee. Reclamation/eviction of
code and registry metadata is not introduced.

### AOT-only pinned-state synchronisation

`jit_crash_sync` no longer requires a dynamic code region before checking linked
AOT text. Dynamic-region admission explicitly requires non-null `region`, so
low unrelated PCs cannot accidentally be classified as JIT when AOT-only is in
use. Existing canonical pinned and promoted/donor register handling is retained.

The cycle counter in w15 is synchronised as well. Its write matches the normal
exit's **32-bit store** even though `cpu_state.cycle` is a host `long`: upper
bytes of the backing field are preserved. This changes no generated instruction,
structure layout or AOT image ABI; there is no code-version bump.

This routine still assumes a recognised native-body fault with established
register conventions. It does not establish exact fault-site ownership or safe
retry on its own, and its callers still have the older replay/address ABI.

### Benchmark validity and exit status

`benchmark/aojit/guest/run_cases.py` now:

- excludes nonzero exits, timeouts and invalid durations from timing samples;
- refuses to compute a speedup if any required run failed or is missing;
- requires stable equal outputs across all rounds/modes for a speedup;
- labels volatile outputs `unverified` (`same_output: null`), retaining timings
  but neither claiming equality nor calculating a speedup;
- retains sub-millisecond timing precision internally;
- validates positive repeat/timeout and `on,off`/`off,on` or explicit `cur` modes;
- fails setup immediately; missing `/proc/ish/jit` no longer silently changes an
  A/B comparison into current-mode timing;
- returns 1 for failed runs/comparisons/setup, 2 for skips/unverified/no cases,
  and 0 for verified comparisons or explicitly requested current-mode timing.

Results are still written for failed/incomplete case runs, with per-case status
and errors. Setup/configuration failures stop before collecting timings. Existing
wrappers using `set -e` will now stop on failed/incomplete verification instead
of implying success; volatile workloads need a domain-specific output validator
before they can yield verified speedups. `cur` never yields an A/B speedup.

The runner remains best-of-N and counts AOT installs, not executed native guest
instructions. `on/off` affects new AOT installs, not existing blocks or the
native emitter. Output equality and zero exit are necessary checks, not a full
correctness proof or proof that AOT was exercised.

## Regressions and evidence

Maintained test entry points (from the repository root):

```sh
# Linux AArch64, Bun and Clang; -O0 and -O2, bounded child execution
bun tests/regress/jit-safety/run.ts

# Python is intentional here: test the existing Python runner directly
PYTHONDONTWRITEBYTECODE=1 python3 -Werror tests/regress/jit-safety/test_benchmark.py
```

Optional `EVIDENCE_DIR=/absolute/path` retains generated offsets, adapted source
and test executable; otherwise the first command removes its private temporary
directory. `CC` can select the compiler.

The C harness includes **actual `jit.c` functions**, with only the Apple platform
restriction/includes replaced in a temporary copy. A small adapter supplies
synthetic Darwin register contexts and timing/cache declarations; it disables
native executable allocation. This is not a full Linux backend port and does
not alter production platform restrictions. Dump writes are intercepted so the
baseline test does not actually read beyond an allocation.

On unmodified `b2c768fd`, all three source-function tests abort (134):

| Test | Frozen upstream failure | Patched result at O0 and O2 |
|---|---|---|
| Allocator | Failed reservation advances cursor past capacity | Overflow/alignment/zero/no-region/end/exhaustion tests pass; eight threads claim all 1,024 tail slots exactly once |
| AOT sync | Registered-image fault returns false when region is null | Pinned registers, promoted/donor state, low-word cycle, native region, unrelated/end PC and pin-off checks pass |
| Dump | Report writes a length larger than the region | Oversized cursor clamped; absent-region payload zero; ordinary length preserved |

The initial benchmark accounting suite fails on upstream (including a false
speedup for identical failing commands). The final **19 unittest cases pass**
with Python warnings treated as errors: failed/partial/timeout/mismatched and
nondeterministic runs, valid ratios, volatile/cur semantics, sub-millisecond
precision, invalid durations, setup failure, skips, missing JIT and bad arguments.
Argparse usage/error output in negative tests is expected, not a skipped failure.

The earlier extracted-emitter preservation suite was rerun against the patched
source at **O0 and O2**, each passing:

- 8,576 integer native-oracle comparisons;
- 2,720 memory comparisons (alignment, sign extension, pairs and writeback);
- 14 saturating SIMD rejection checks and 16 SMAX lanes;
- family exact/moved/ADR/ADRP/mutated-word and live-slot ownership checks.

The known block-replay probe was also rerun at both optimisation levels and
**still produces x0=2 instead of 1 after one fault**. Its successful reproduction
is evidence of an unresolved defect, not a backend acceptance pass.

Environment: Orange Pi 6 Plus, CIX P1/CD8180-class 12-core AArch64 SoC,
16GB-class RAM (~14GiB Linux-visible), NVMe, Debian Trixie, host-native tools.
Evidence: `/workspace/tmp/ish-jit-safety-sAEfmp/`, pointer
`/workspace/tmp/ish-jit-safety-path`; original preservation fixtures:
`/workspace/tmp/ish-jit-investigation-zYDHWD/probes/`. The prior audit is available
on ios-linuxkit master as `docs/reports/audits/JIT_AOT_INVESTIGATION_2026-09-29.md`.
A delegated review could not select an executable model; it is not counted as
independent review evidence.

## Still blocked before backend adoption

1. **Precise host-fault restart**: native-PC → guest instruction/access metadata,
   partially committed state, cross-block links, promoted prologues/epilogues and
   exact address/write classification. The existing CLI replay and x7/x10
   gadget-address reconstruction are not valid for arbitrary native faults.
2. Signal-context safety and promotion-metadata allocation failure; the existing
   signal-handler mutex and fault-site assumptions are unchanged.
3. Complete Darwin and iOS record → compact → link → execute validation, memory
   permissions/signing, AOT-only signal recovery and concurrent invalidation.
4. Sustained code/registry lifetime and reclamation design. Bounded allocation
   now falls back safely but does not make the cache unbounded or reusable.

There was no full backend/device/sanitizer run here. The earlier proc-stress exit
139 remains unattributed. Neither these experimental fixes nor the earlier
local CLI boolean fix are claimed to explain it.
