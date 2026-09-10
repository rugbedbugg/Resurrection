# Adaptive Region-Aware Process Restoration

> Every restore teaches the system how to make the next one faster.

A userspace checkpoint-and-restore prototype for single-threaded Linux x86-64
processes that selects a **restoration policy per memory region** — eager load,
working-set prefetch, or lazy paging — instead of applying one strategy to the
whole process. A low-overhead analytical cost model makes the per-region choice
and is calibrated against measured I/O and page-fault costs.

**Status:** Phase 2 complete. `arctl checkpoint` captures a stopped
single-threaded target into a verified `.arimg`, `arctl inspect` reads it back,
and `arctl restore --policy eager` brings the process back and resumes it at the
saved instruction. Phase 3 (lazy restore via `userfaultfd`) is next.

Restore needs `criu`/`compel` at build time; checkpoint and inspect do not, and
a build without compel is supported (it simply has no `restore`).

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

## Usage

`arctl` spawns its target rather than attaching to a running pid: `ptrace_scope`
normally forbids attaching to a non-descendant, and the target needs ASLR off
plus a curated environment for saved absolute addresses to be reproducible. Mark
the snapshot point in the workload with `ar_checkpoint_here()` from
`include/adaptive_restore/marker.h`, then:

```sh
arctl checkpoint --verify --output run.arimg -- ./build/workloads/workload_mini
arctl inspect --image run.arimg
arctl restore --image run.arimg --policy eager --verify
```

`--verify` re-reads every captured page from the live process and re-checksums
it against the first pass; a mismatch leaves no image behind. The target's
stdout and stderr are redirected to `<output>.stdout` / `<output>.stderr`.

Phase 0 feasibility probes build separately:

```sh
make -C experiments/phase0 check
```

## Requirements

- Linux x86-64, kernel with `userfaultfd` (`UFFD_USER_MODE_ONLY` is used — works
  even with `vm.unprivileged_userfaultfd=0`).
- gcc/clang with C++20, CMake ≥ 3.20, SQLite3 (Phase 4+).
- Optional (recommended for the restorer, Phase 2): `criu` / `compel`.
