# Adaptive Region-Aware Process Restoration

> Every restore teaches the system how to make the next one faster.

A userspace checkpoint-and-restore prototype for single-threaded Linux x86-64
processes that selects a **restoration policy per memory region** — eager load,
working-set prefetch, or lazy paging — instead of applying one strategy to the
whole process. A low-overhead analytical cost model makes the per-region choice
and is calibrated against measured I/O and page-fault costs.

**Status:** Phase 5 complete. `arctl checkpoint` captures a stopped
single-threaded target into a verified `.arimg`, `arctl inspect` reads it back,
and `arctl restore` brings the process back and resumes it at the saved
instruction under either `--policy eager` (populate everything up front) or
`--policy lazy` (register anonymous regions with `userfaultfd` and serve pages
on demand), `--policy prefetch` (load the working set recorded by earlier
restores), or `--policy cost` (choose per 256 KiB region from an analytical cost
model calibrated against this machine). `arctl benchmark` runs the workload
suite and `arctl visualize` draws the result.

The result worth stating is not a margin on a favourable workload, it is that
**the adaptive policy has no bad case.** Over the Tier 1 suite (4 workloads x
cold and warm x 9 reps), worst-case regret -- how far behind the best policy for
a given workload a policy ever falls -- is:

| policy | worst-case regret |
|---|---|
| lazy | +235% |
| eager | +102% |
| prefetch | +88% |
| **cost** | **+9.9%** |

Every fixed policy is catastrophically wrong on at least one workload; the
adaptive one is never more than 10% off the best, and that 10% sits inside the
run-to-run spread. On `mixed_regions`, the workload with regions of genuinely
different access behaviour, `--policy cost` beat every fixed policy on 9 of 9
repetitions with no overlap in range, and it captures ~100% of the gain an
oracle with perfect knowledge could achieve. Where a fixed policy is already
optimal it matches that policy rather than losing to it, and when its recorded
history is wrong it degrades in latency, never in correctness.

These numbers were measured on one development machine, and the cost model's
constants (`src/policy/calibration.cpp`) were calibrated on that same machine.
Other hardware can rank the policies differently. To measure your own machine,
run `arctl benchmark` (below) and the cost-model gate in report-only mode (see
`NIX-DEVELOPMENT.md`).

Restore needs `criu`/`compel` at build time; checkpoint and inspect do not, and
a build without compel is supported (it simply has no `restore`).

Design drafts, phase reports, and specs live in `docs/`, a local working folder
that is deliberately not tracked. A clone does not contain it, so references to
`docs/…` in source comments point at the maintainer's notes. Only this README,
`NIX-DEVELOPMENT.md` and `CONTRIBUTING.md` are committed prose.

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
include/       public headers (adaptive_restore/…)
src/           common | checkpoint | restore | image | paging | policy | profile | bench | cli | tui
workloads/     synthetic benchmark targets
tests/         unit | integration
experiments/   phase spike probes, the Tier 1 benchmark suite, raw/ (ignored output)
cmake/         find modules for the optional compel and SQLite dependencies
flake.nix      NixOS development shell (see NIX-DEVELOPMENT.md)
docs/          design drafts, phase reports, specs   (local only, not in a clone)
```

compel and SQLite come from the system (or the Nix shell). Nothing is vendored.

## Build

```sh
cmake -B build -G Ninja
cmake --build build
ctest --test-dir build
```

On NixOS, `nix develop` supplies this toolchain including `criu`, `compel` and
SQLite. That shell disables address space randomisation, which restore needs,
so run the tests there as `setarch x86_64 ctest --test-dir build`. See
`NIX-DEVELOPMENT.md`.

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
arctl restore --image run.arimg --policy lazy --record-trace faults.tsv --profile p.db
arctl restore --image run.arimg --policy cost --profile p.db
arctl profile show --image run.arimg --profile p.db
arctl benchmark --suite experiments/bench/tier1.suite --output experiments/raw
arctl visualize --results experiments/raw
arctl visualize --image run.arimg --profile p.db --policy cost
arctl visualize --trace faults.tsv
```

This is the common path, not every option. `arctl help` lists the full surface,
including `checkpoint --env` and `--allow-unsupported-state`, `restore
--cold-cache`/`--warm-cache` and `--no-learn`, and `profile reset|export`.

`arctl visualize` only reads what other commands left behind, so it never
perturbs a measurement and it builds in every configuration. `--results` draws
median resume latency per policy per workload; `--image` draws the region map of
the plan a restore *would* choose, from the same `plan_restore()` the restorer
calls, so the picture cannot drift from the behaviour; `--trace` draws when
demand faults arrived.

A restore teaches the profile only from the regions it left *pure lazy*, since
only those fault on first touch and so reveal what was actually used: `--policy
lazy` teaches every eligible region, `--policy cost` teaches the subset it chose
to leave lazy, and `--policy prefetch` teaches nothing. Later restores of the
same checkpoint identity read that history back. `--profile` needs SQLite,
which like `compel` is restore-side only: a build without either still
checkpoints and inspects.

Under any deferring policy, `arctl` stays alive as the target's page server for
as long as it runs. `--record-trace` writes one row per demand fault; under
`--policy lazy` every first touch faults, so that trace is exactly the
workload's touched set. Under the other policies it is only the part of the
touched set that was not already loaded up front.

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
- gcc/clang with C++20, CMake ≥ 3.20.
- Optional: `criu` / `compel` (4.1 or 4.2). Without it there is no `restore`.
- Optional: SQLite3. Without it there is no `--profile`, so no `prefetch` or
  `cost` policy and no `profile` command.
- Address space randomisation enabled for `restore` (the default on most
  systems; see `NIX-DEVELOPMENT.md` for the case where it is not).
