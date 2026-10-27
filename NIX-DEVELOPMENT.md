# Development on NixOS

Enter this repository's environment with:

```sh
nix develop path:.
```

The explicit `path:.` includes the Nix files while they are still untracked. Once
they are committed, plain `nix develop` works. `flake.lock` pins nixpkgs, so the
toolchain does not move under the project. Entering the shell starts no services.

The shell supplies gcc 15, cmake, ninja, gmake, gdb, pkg-config, SQLite 3.51 and
CRIU 4.1.1. CRIU also provides `compel`, `libcompel` and its headers. Python 3.13
is there for the experiment scripts.

```sh
cmake -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
```

Both optional dependencies are found, so this configuration builds the restorer
and the profile store. Exercise the degraded builds with:

```sh
cmake -B build-nocompel -G Ninja -DAR_FORCE_NO_COMPEL=ON
cmake -B build-nosql -G Ninja -DAR_FORCE_NO_SQLITE=ON
```

## Address space randomisation

`nix develop` sets `ADDR_NO_RANDOMIZE` on the shell and on everything it starts.
`arctl restore` forks a stub and retries until the stub's TLS block misses the
image. With randomisation off every retry lands in the same place, so restore
fails with:

```
restore: could not place a stub whose TLS clears the image after 8 attempts
```

Run restore and the restore tests with the flag cleared. `setarch` resets the
personality before it execs the command:

```sh
setarch x86_64 ctest --test-dir build --output-on-failure
setarch x86_64 ./build/arctl restore --image run.arimg --policy eager --verify
```

The built binaries carry their store paths, so they also run from a normal shell
outside `nix develop`.

## The cost-model gate

`tests/integration/test_cost_model_gate.cpp` compares the adaptive policy against
every fixed policy. The model constants come from
`docs/phase4-calibration-report.md` and were measured on one machine, so another
machine can rank the policies differently. Set `AR_GATE_REPORT_ONLY=1` to print
the medians without asserting:

```sh
AR_GATE_REPORT_ONLY=1 setarch x86_64 ./build/tests/test_cost_model_gate
```

CI builds and tests with this gate excluded from the pass/fail run, then runs it
once in report-only mode.
