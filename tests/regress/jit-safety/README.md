# Isolated JIT/AOT safety regressions

See [bounded repairs](../../../docs/JIT_AOT_SAFETY_2026-09-29.md) and
[precise restart scope and evidence](../../../docs/JIT_PRECISE_RESTART_2026-09-29.md).

```sh
bun tests/regress/jit-safety/run.ts
PYTHONDONTWRITEBYTECODE=1 python3 -Werror tests/regress/jit-safety/test_benchmark.py
```

The Bun runner requires Linux/AArch64 and Clang. It generates offsets from the
current source and adapts only Apple platform declarations in a private copy of
`jit.c`, then includes real functions for O0/O2 tests. It does not enable or port
the full backend. `EVIDENCE_DIR` retains scratch output, otherwise it is removed.
The Python tests import the actual benchmark runner and mock workload execution;
no network, guest or package installation is needed.

The restart harness invokes the actual emitter, CLI signal handler and extracted
post-fiber crash policy. Linux signals are adapted to the Darwin register ABI;
return uses a test trampoline with the same saved-SP contract. 100 real-fault
cases per optimisation cover unpinned/pinned/promoted, relocated PIC/AOT-only and
cross-allocation native chaining, exact PC/address/direction, flags/cycles,
writeback, cache flush, bounded escalation and no repeated prefix effects.
Unmatched native/AOT PCs fail closed. The real version-7 ABI is rejected.

The maintained native oracle adds 8,576 integer and 2,720 memory comparisons at
each optimisation, plus SIMD filtering and family/slot checks. `preservation.c`
comes from the earlier investigation; obsolete expected-bug probes were removed
and its memory CPU storage enlarged to the real checkpoint-bearing frame.

To prove the old emitter/handler fails the same exact-PC assertion (five modes
at O0/O2; a timeout or unrelated failure is an error):

```sh
BASELINE_REV=083eca69 bun tests/regress/jit-safety/run.ts
```

The runner requires Git history for the frozen version-7 ABI fixture. It syntax
checks dispatch, but does not link/run the whole backend. There is no Darwin/iOS
record/compact/link/device validation, production trampoline execution, MMU
refill test or concurrent-invalidation proof here. These are host-adapted tests,
not permission to adopt the experimental backend into master.
