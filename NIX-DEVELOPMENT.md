# Development on NixOS

Enter this repository's environment with:

```sh
nix develop
```

Use plain `nix develop`, not `nix develop path:.`. A `path:` flake copies the
whole working tree into the Nix store on every entry, including build
directories and checkpoint images. A git flake copies only tracked files. The
flake reads only `flake.nix` and `flake.lock`, so untracked files make no
difference to the shell. `flake.lock` pins nixpkgs, so the toolchain does not
move under the project. Entering the shell starts no services.

The shell supplies gcc 15, cmake, ninja, gmake, gdb, pkg-config, SQLite 3.51 and
CRIU 4.1.1. CRIU also provides `compel`, `libcompel` and its headers. Python 3.13
is there for the experiment scripts. `nix flake check` evaluates the shell.

```sh
cmake -B build -G Ninja
cmake --build build
setarch x86_64 ctest --test-dir build --output-on-failure
```

Both optional dependencies are found, so this configuration builds the restorer
and the profile store. Exercise the degraded builds with:

```sh
cmake -B build-nocompel -G Ninja -DAR_FORCE_NO_COMPEL=ON
cmake -B build-nosql -G Ninja -DAR_FORCE_NO_SQLITE=ON
```

## Build directories from outside the shell

A build directory configured outside `nix develop` records that compiler and
those library paths in its `CMakeCache.txt`. The same applies to one configured
in a different checkout path. CMake will either reuse the wrong toolchain or
refuse with:

```
CMake Error: The current CMakeCache.txt directory … is different than the directory … where CMakeCache.txt was created.
```

Delete the directory and configure it again inside the shell. To see which
compiler a build directory uses:

```sh
grep CMAKE_CXX_COMPILER: build/CMakeCache.txt
```

Inside the shell it points into `/nix/store`.

## Address space randomisation

`nix develop` sets `ADDR_NO_RANDOMIZE` on the shell and on everything it starts.
`setarch --show` prints `PER_LINUX (ADDR_NO_RANDOMIZE)` inside the shell.
`arctl restore` forks a stub and retries until the stub's TLS block misses the
image. With randomisation off every retry lands in the same place, so restore
fails with:

```
restore: could not place a stub whose TLS clears the image after 8 attempts
```

Without `setarch`, `ctest` fails the five tests that drive a real restore:
`restore_mini`, `restore_mini_lazy`, `restore_sparse`, `phase_change` and
`prefetch_observes`. Run restore and the restore tests with the flag cleared.
`setarch` resets the personality before it execs the command:

```sh
setarch x86_64 ctest --test-dir build --output-on-failure
setarch x86_64 ./build/arctl restore --image run.arimg --policy eager --verify
```

Checkpoint is unaffected. `arctl checkpoint` turns randomisation off for its
target itself, so it works with or without `setarch`.

The built binaries carry their store paths, so they also run from a normal shell
outside `nix develop`.

## The cost-model gate

`tests/integration/test_cost_model_gate.cpp` compares the adaptive policy against
every fixed policy on the `mixed_regions` workload. The model constants live in
`src/policy/calibration.cpp` and were measured on one machine, so another
machine can rank the policies differently and fail the gate. Set
`AR_GATE_REPORT_ONLY=1` to print the medians without asserting:

```sh
AR_GATE_REPORT_ONLY=1 setarch x86_64 ./build/tests/test_cost_model_gate
```

CI builds and tests with this gate excluded from the pass/fail run, then runs it
once in report-only mode.
