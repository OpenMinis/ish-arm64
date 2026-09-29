# Isolated JIT/AOT safety regressions

See [scope and evidence](../../../docs/JIT_AOT_SAFETY_2026-09-29.md).

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

A passing suite does **not** establish correct host-fault restart. The native
block-replay defect remains an explicit blocker in the scope report.
