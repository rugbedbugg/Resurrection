# Adaptive Region-Aware Process Restoration

> Every restore teaches the system how to make the next one faster.

A userspace checkpoint-and-restore prototype for single-threaded Linux x86-64
processes that selects a **restoration policy per memory region** — eager load,
working-set prefetch, or lazy paging — instead of applying one strategy to the
whole process. A low-overhead analytical cost model makes the per-region choice
and is calibrated against measured I/O and page-fault costs.

**Status:** Phase 0 complete (environment verified). Phase 1 (checkpoint image +
capture) in progress.

Design drafts, phase reports, and specs live in `docs/` — a local, untracked
working folder. This README is the only committed prose.

## Scope (Tier 1 — the committed deliverable)

| In | Out (Tier 2 / never) |
|---|---|
| Single-threaded x86-64 process C/R | Multithreaded, distributed, GPU/device state |
| GP registers + IP, VM mappings, regular-file FDs | Arbitrary sockets/pipes/terminals |
| Eager / lazy (`userfaultfd`) / static prefetch data plane | Fault-triggered read-ahead (Tier 2) |
| Region profiler + analytical cost-model controller | Bandit / neural controllers, drift detection (Tier 2) |
| 4 synthetic workloads, correctness suite, terminal visualizer | Paper-grade ablation evaluation (Tier 2) |

## Layout

```
docs/          design drafts, phase reports, specs   (local, untracked)
include/        public headers (adaptive_restore/…)
src/           checkpoint · restore · image · paging · policy · profile · metrics · cli · tui
restorer/      freestanding restore bootstrap (compel-based or from-scratch — see §9.1)
workloads/     synthetic benchmark targets
tests/         unit · integration · correctness
experiments/   phase0 probes, benchmark configs + scripts + raw results
third_party/   vendored deps (compel, if used)
```

## Build

```sh
cmake -B build -G Ninja
cmake --build build
ctest --test-dir build
```

Phase 0 feasibility probes build separately:

```sh
make -C experiments/phase0 check
```

## Requirements

- Linux x86-64, kernel with `userfaultfd` (`UFFD_USER_MODE_ONLY` is used — works
  even with `vm.unprivileged_userfaultfd=0`).
- gcc/clang with C++20, CMake ≥ 3.20, SQLite3 (Phase 4+).
- Optional (recommended for the restorer, Phase 2): `criu` / `compel`.
