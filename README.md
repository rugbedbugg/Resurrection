# Adaptive Region-Aware Process Restoration

> Every restore teaches the system how to make the next one faster.

A userspace checkpoint and restore prototype for single-threaded Linux x86-64
processes. It picks a **restoration policy per memory region** instead of one
strategy for the whole process. The policies are eager load, working-set
prefetch and lazy paging. A low-overhead analytical cost model makes the
per-region choice. Measured I/O and page-fault costs calibrate the model.

`arctl checkpoint` captures a stopped single-threaded target into a verified
`.arimg`. `arctl inspect` reads it back. `arctl restore` brings the process back
and resumes it at the saved instruction. Restore takes one of four policies:

- `--policy eager` populates everything up front.
- `--policy lazy` registers anonymous regions with `userfaultfd` and serves
  pages on demand.
- `--policy prefetch` loads the working set recorded by earlier restores.
- `--policy cost` chooses per 256 KiB region from an analytical cost model
  calibrated against this machine.

`arctl benchmark` runs the workload suite. `arctl visualize` draws the result.

## Results

**The adaptive policy has no bad case.** That matters more than a margin on a
favourable workload. Regret is how far a policy falls behind the best policy
for a given workload. Over the Tier 1 suite (4 workloads, cold and warm, 9 reps
each) the worst-case regret is:

| policy | worst-case regret |
|---|---|
| lazy | +144% |
| eager | +103% |
| prefetch | +58% |
| **cost** | **+1.9%** |

Every fixed policy is badly wrong on at least one workload. The adaptive one is
never more than 2% off the best. That is inside the run-to-run spread.

`mixed_regions` has regions with different access behaviour. On it
`--policy cost` beat every fixed policy on 9 of 9 repetitions cold. It won 8 of
9 warm. The margin over the best fixed policy was 10-17%. Where a fixed policy
is already optimal, the adaptive policy matches it. Wrong recorded history costs
latency. It never costs correctness.

These numbers come from one machine with kernel 7.2.10 and ext4 on NVMe. The
cost model's constants in `src/policy/calibration.cpp` were calibrated there.
Other hardware can rank the policies differently. To measure your own machine,
run `arctl benchmark` and the cost-model gate in report-only mode. See
`NIX-DEVELOPMENT.md` for the gate.

The ranking also depends on transparent huge pages. Eager writes into fresh
memory. With 4 KiB pages it runs at half speed. Plain prefetch then wins
everywhere and leaves nothing for a per-region choice to win. The restorer
therefore opts eager-only mappings into THP with `MADV_HUGEPAGE`. The result
does not depend on the system-wide THP mode. Spans that hold a deferred page
keep THP off. A huge page there would make pages resident that the restore
expects to be missing.

## Scope

| In | Out |
|---|---|
| Single-threaded x86-64 process C/R | Multithreaded, distributed, GPU or device state |
| GP registers and IP, VM mappings, regular-file FDs | Arbitrary sockets, pipes and terminals |
| Eager, lazy (`userfaultfd`) and static prefetch data plane | Fault-triggered read-ahead |
| Region profiler and analytical cost-model controller | Bandit or neural controllers, drift detection |
| 4 synthetic workloads, correctness suite, terminal visualizer | Paper-grade ablation evaluation |

## Layout

```
include/       public headers (adaptive_restore/…)
src/           common | checkpoint | restore | image | paging | policy | profile | bench | cli | tui
workloads/     synthetic benchmark targets
tests/         unit | integration
experiments/   feasibility probes, spikes, the Tier 1 benchmark suite, raw/ (ignored output)
cmake/         find modules for the optional compel and SQLite dependencies
flake.nix      NixOS development shell (see NIX-DEVELOPMENT.md)
```

compel and SQLite come from the system or the Nix shell. Nothing is vendored.

## Build

```sh
cmake -B build -G Ninja
cmake --build build
ctest --test-dir build
```

On NixOS, `nix develop` supplies this toolchain. It includes `criu`, `compel`
and SQLite. That shell disables address space randomisation. Restore needs it.
Run the tests there as `setarch x86_64 ctest --test-dir build`. See
`NIX-DEVELOPMENT.md`.

## Usage

`arctl` spawns its target. It does not attach to a running pid.
`ptrace_scope` normally forbids attaching to a non-descendant. Saved absolute
addresses are reproducible only with ASLR off and a curated environment. Mark
the snapshot point in the workload with `ar_checkpoint_here()` from
`include/adaptive_restore/marker.h`. Then run:

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

This is the common path, not every option. `arctl help` lists the full surface.
It covers `checkpoint --env` and `--allow-unsupported-state`. It covers
`restore --cold-cache`, `--warm-cache` and `--no-learn`. It also covers
`profile reset|export`.

### Checkpoint

`--verify` re-reads every captured page from the live process. It checksums
each page again and compares against the first pass. A mismatch leaves no image
behind. The target's stdout and stderr go to `<output>.stdout` and
`<output>.stderr`.

Images and their `.stdout` and `.stderr` files are created owner-only (`0600`).
An image is a copy of the process's memory.

### Restore and the profile

A restore teaches the profile only from the regions it left *pure lazy*. Only
those fault on first touch. Their faults reveal what the workload used.

- `--policy lazy` teaches every eligible region.
- `--policy cost` teaches the subset it chose to leave lazy.
- `--policy prefetch` teaches nothing.

Later restores of the same checkpoint identity read that history back.
`--profile` needs SQLite. Like `compel`, SQLite is restore-side only. A build
without either still checkpoints and inspects.

Under any deferring policy, `arctl` stays alive as the target's page server.
`--record-trace` writes one row per demand fault. Under `--policy lazy` every
first touch faults. That trace is exactly the workload's touched set. Under the
other policies it holds only the touched pages not loaded up front.

### Benchmark

Run `arctl benchmark` from the repository root. The suite's paths are relative
to the working directory. They expect a configured `build/`. Images go to
`experiments/raw/images`. `arctl` creates it if missing. It must not be on
tmpfs.

### Visualize

`arctl visualize` only reads what other commands left behind. It never perturbs
a measurement. It builds in every configuration.

- `--results` draws median resume latency per policy per workload.
- `--image` draws the region map of the plan a restore *would* choose. It calls
  the same `plan_restore()` as the restorer. The picture cannot drift from the
  behaviour.
- `--trace` draws when demand faults arrived.

### Feasibility probes

The probes in `experiments/phase0` build separately:

```sh
make -C experiments/phase0 check
```

## Requirements

- Linux x86-64 with `userfaultfd`. Restore uses `UFFD_USER_MODE_ONLY`. It works
  with `vm.unprivileged_userfaultfd=0`.
- gcc or clang with C++20, CMake 3.20 or later.
- Optional: `criu` and `compel` 4.1 or 4.2. A build without them has no
  `restore`. Checkpoint and inspect do not need them.
- Optional: SQLite3. A build without it has no `--profile`. It then has no
  `prefetch` or `cost` policy and no `profile` command.
- Address space randomisation enabled for `restore`. Most systems enable it by
  default. See `NIX-DEVELOPMENT.md` for the case where it is off.
- No seccomp filter on `arctl` for `restore`. compel cannot seize a filtered
  task without `CAP_SYS_ADMIN`. Restore fails at its first step inside a
  container with a seccomp profile, such as Docker's default.
