# Contributing to adaptive-restore

This is a Linux-only C++20 project. Read `NIX-DEVELOPMENT.md` for the Nix shell.

## Build and test

```sh
cmake -B build -G Ninja
cmake --build build
setarch x86_64 ctest --test-dir build --output-on-failure
```

Restore needs `criu` and `compel`. The profile store needs SQLite. A build
without either is supported, so configure with `-DAR_FORCE_NO_COMPEL=ON` or
`-DAR_FORCE_NO_SQLITE=ON` before changing the optional paths.

Restore needs address space randomisation to place the stub. `nix develop` turns
it off, so run the restore tests under `setarch x86_64`. Outside Nix the prefix
does no harm.

`cost_model_gate` asserts against constants calibrated on one machine and may
fail on yours. CI excludes it from the pass/fail run with `-E cost_model_gate`.
Report its medians with `AR_GATE_REPORT_ONLY=1` instead of treating a miss as a
regression, unless your change touches the policy or paging code.

## Change guidelines

- Keep `checkpoint` and `inspect` working without compel or SQLite. Only the restore and profile paths may depend on them.
- Add a unit test for new logic and an integration test for a new exit condition. Integration tests drive the real `arctl` binary against a real workload.
- Bump the version in `CMakeLists.txt` and `include/adaptive_restore/common/version.hpp` together. The build fails when they disagree.
- Keep generated output out of Git. Images (`*.arimg` and their `.stdout`/`.stderr` sidecars), profile databases (`*.db`, `*.sqlite`), fault traces (`*faults.tsv`) and `experiments/raw/` are ignored.
- Do not track `docs/`. Design notes, phase reports and specs stay local. Do not add new references to `docs/` in committed prose. A clone cannot follow them.
- Keep the docs true. When a command, flag, option or test changes, update the README usage, `arctl help` and `NIX-DEVELOPMENT.md` in the same commit, and run every command you document.
## Pull requests

State which configurations you built and which policies you ran. Include the
`ctest` result. For restore changes, say whether eager and lazy both passed.
