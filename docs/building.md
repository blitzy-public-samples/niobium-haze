# Building

How to build, test, and configure Haze (`libhaze`) from source. The
standalone `Makefile` flow is the first-class path and needs no nix; the
[nix flake](#with-the-nix-flake) is an opt-in convenience for contributors who
already use nix. For the day-to-day testing workflow see
[`./testing.md`](./testing.md); for the layering the build produces see
[`./architecture.md`](./architecture.md).

## Prerequisites

- clang >= 19 (C++23 is required by `CMakeLists.txt`).
- cmake >= 3.22.
- Catch2 3.x.
- git (submodule init).

Clang 19 is the supported floor. `-Wthread-safety` (the canonical enforcement
path for the lock contracts in `src/common/thread_safety.hpp`) only fires under
clang, so clang is required rather than merely recommended. The nix flake (and
therefore CI) tracks nixpkgs-unstable's default clang, which is currently newer;
pick the most recent clang available from your package manager when possible.

```sh
# macOS (Homebrew) — `llvm` is unversioned and tracks Homebrew's current
# release; pin to `llvm@19` only if you need a specific version.
brew install cmake catch2 llvm

# Debian / Ubuntu (Catch2 3.x may need a backport or source build) — bump
# clang-19 / llvm-19-dev to a newer apt suffix where available.
sudo apt install cmake catch2 clang-19 llvm-19-dev
```

## Standalone (non-nix) build

```sh
git submodule update --init --recursive   # or: make sync
make build MODE=release                    # Release into build/ (the default)
make build MODE=debug                      # Debug   into dbuild/
```

`MODE` defaults to `release`, so a bare `make build` is equivalent to
`make build MODE=release`. The same `MODE=` selector applies to every target
that produces or consumes build artefacts (`config`, `build`, `test`,
`test-unit`, `test-sim`, `test-transport`, `test-all`, `clean`).

The top-level `Makefile` builds OpenFHE (vendored at
`vendor/niobium-fhetch/vendor/openfhe`), installs it under
`vendor/niobium-fhetch/vendor/lib/openfhe`, then builds `libhaze` and
`haze_tests` against `Niobium::fhetch`. The first build is slow because OpenFHE
is compiled from source; subsequent invocations skip it.

To skip the OpenFHE build entirely (for example when a parent project already
installed it):

```sh
EXTERNAL_OPENFHE=1 OPENFHE_INSTALL_DIR=/path/to/openfhe make build MODE=release
```

## Make targets

```
Build:
  sync              Init vendor/niobium-fhetch (recursive).
  config            Configure haze (uses MODE).
  build             Build haze (uses MODE).
  config-openfhe    Configure OpenFHE.
  build-openfhe     Build and install OpenFHE locally.

Test:
  test-unit         Unit suite (HAZE_TARGET=local; no FHE math).
  test-sim          Sim suite via the in-process FHETCH simulator
                    (HAZE_TARGET=local). Validates FHE math.
  test-e2e          E2E suite (public C ABI + stock OpenFHE, decrypt).
  test-readme       Compile + run the README examples (C + C++).
  test-transport    [integration] suite via nbcc_fhetch_replay
                    (opt-in; requires NIOBIUM_COMPILER_ROOT).
  test-isolation    Assert libhaze exports only the haze* C ABI.
  test              Default: test-unit + test-sim + test-e2e + test-isolation.
  test-all          test + test-readme + test-transport.

Benchmark:
  bench             Build + run the Google Benchmark suite
                    (configures -DHAZE_BUILD_BENCHMARKS=ON).

Cleanup:
  clean-runs        Remove test runs/ artifacts.
  clean             Remove all build artifacts (refuses to touch
                    external trees pointed at via
                    NIOBIUM_HAZE_FHETCH_DIR or EXTERNAL_OPENFHE).
```

`make help` prints the same list at runtime.

### Benchmarks and coverage

`make bench` configures with `-DHAZE_BUILD_BENCHMARKS=ON`, builds, and runs the
Google Benchmark suite under `benchmark/`. The benchmarks build as a **separate
executable** that links the compiled haze objects; the shipped `libhaze` is
byte-for-byte unaffected, so benchmarking never changes the library ABI. The run
emits JSON that CI compares against the checked-in `benchmark/baseline.json` to
flag regressions. This target is live today.

> **Coverage is deferred.** A `make coverage` target and the `HAZE_COVERAGE`
> CMake option are **not yet wired** in this tree (`make coverage` currently
> fails and `make help` does not list it). The coverage *driver*
> [`../scripts/coverage.sh`](../scripts/coverage.sh) is present, and the intended
> flow is a `-DHAZE_COVERAGE=ON` Clang source-based instrumentation build
> (`-fprofile-instr-generate -fcoverage-mapping`) that runs the tests, then drives
> `scripts/coverage.sh` (`llvm-profdata merge` followed by
> `llvm-cov export -format=lcov`) scoped to `src/core/` and `src/api/`, with a CI
> gate requiring **80% line coverage** across those two trees. `llvm-cov` /
> `llvm-profdata` already ship with clang, so no new dependency is needed once the
> option, target, and workflow are implemented and verified. See
> [`./testing.md`](./testing.md) for the intended coverage workflow and
> [`./decision-log.md`](./decision-log.md) (D-06, D-10) for the sequencing
> rationale.

## Override knobs

Make variables and / or environment:

| Variable                  | Purpose                                                                                                     | Default                                   |
| ------------------------- | ----------------------------------------------------------------------------------------------------------- | ----------------------------------------- |
| `MODE`                    | `debug` or `release`. Selects `dbuild`/`build` and CMake `Debug`/`Release`.                                 | `release`                                 |
| `NUM_CPUS`                | Build parallelism.                                                                                          | Auto (`sysctl -n hw.ncpu` / `nproc`).     |
| `NIOBIUM_HAZE_FHETCH_DIR` | External `niobium-fhetch` source tree to use instead of `vendor/niobium-fhetch`.                            | unset (vendor submodule).                 |
| `OPENFHE_INSTALL_DIR`     | Where OpenFHE is installed (libs + headers).                                                                | `<fhetch>/vendor/lib/openfhe`.            |
| `EXTERNAL_OPENFHE`        | `1` skips the OpenFHE build chain (caller supplies it via `OPENFHE_INSTALL_DIR`).                           | `0`.                                      |
| `JSON_INCLUDE_DIR`        | `nlohmann/json` single-include directory.                                                                   | unset (use niobium-fhetch's vendor copy). |
| `NIOBIUM_COMPILER_ROOT`   | Path to a `niobium-compiler` checkout containing `build/nbcc_fhetch_replay`. Required for `test-transport`. | unset.                                    |

## Runtime target selector

Consumed by `libhaze` itself at run time, not by the Makefile:

| Variable      | Purpose                                                                                                                                                                                                                                                     |
| ------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `HAZE_TARGET` | Replay target: `local` (default; in-process simulator) or one of `FHE_SIM`, `FUNC_SIM`, `FPGA_TRI`, `fhetch_sim` (HTTP transport to `nbcc_fhetch_replay`). Read at the first `hazeFlush()`. See [`../include/haze/haze.h`](../include/haze/haze.h) for the full table. |

## CMake-level toggles

Passed to the configure step as `-D<name>=<value>`:

| Option                                     | Default | Effect                                                               |
| ------------------------------------------ | ------- | -------------------------------------------------------------------- |
| `HAZE_SANITIZERS`                          | `OFF`   | ASAN + UBSAN.                                                        |
| `HAZE_TSAN`                                | `OFF`   | TSAN. Mutually exclusive with `HAZE_SANITIZERS`.                     |
| `HAZE_FBC_REDUCED_NOISE`                   | `ON`    | Test oracle uses OpenFHE's `ReducedNoise` FBC variant.               |
| `NIOBIUM_CLIENT_HAZE_WITH_TRANSPORT_TESTS` | `OFF`   | Register `haze_transport_tests` as a ctest entry (parent-build use). |
| `HAZE_BUILD_BENCHMARKS`                    | `OFF`   | Build the Google Benchmark suite as a separate executable (never absorbed into `libhaze`). |
| `HAZE_COVERAGE` *(deferred)*               | —       | Planned Clang source-based coverage instrumentation (`-fprofile-instr-generate -fcoverage-mapping`, Clang only). **Not yet present** in `CMakeLists.txt`. |

`make bench` sets `HAZE_BUILD_BENCHMARKS`, so you rarely pass it by hand; it is
listed here for direct `cmake` invocations and for parent build graphs. The
`HAZE_COVERAGE` option (and the `make coverage` target that would set it) are
deferred — see the coverage note above.

## As a `niobium-client` submodule

When haze is checked out under `niobium-client/vendor/niobium-haze`, the parent
owns the build graph and haze's own `Makefile` is not invoked. The parent
exposes wrapper targets in the `##@ Haze` section of its top-level `Makefile`.

Building (from the niobium-client root):

```sh
make build-release          # full client build; produces the haze targets
                            # via the parent CMake graph. NIOBIUM_CLIENT_WITH_HAZE=ON
                            # is the default.
make build-haze-release     # rebuild only the haze targets without touching
                            # the rest of the client (handy while iterating).
```

Testing (from the niobium-client root):

```sh
make test-haze-unit-release        # = standalone `make test-unit`
                                   # (haze_tests "~[integration]", HAZE_TARGET=local).
make test-haze-integration-release \
    NIOBIUM_COMPILER_ROOT=/path/to/niobium-compiler
                                   # transport round trip through the in-tree
                                   # forwarder/server pair, HAZE_TARGET=FUNC_SIM.
make test-haze-release             # both of the above.
make clean-haze                    # drop build/vendor/niobium-haze/runs/ only.
```

Artefact paths differ from the standalone build:

| Artifact      | Parent-driven                          | Standalone         |
| ------------- | -------------------------------------- | ------------------ |
| `haze_tests`  | `build/vendor/niobium-haze/haze_tests` | `build/haze_tests` |
| Test runs dir | `build/vendor/niobium-haze/runs/`      | `build/runs/`      |

See the [`../README.md`](../README.md) "As a `niobium-client` submodule" section
for the full description of the parent-driven flow.

## With the nix flake

The standalone Makefile flow above is the first-class path; the flake is an
opt-in convenience for contributors who already use nix. It provides three
distinct surfaces. See [`../README.md`](../README.md) for the complete detail.

### Dev shell — fastest iteration

`nix develop` provisions the toolchain (clang as `cc`/`c++`, cmake, Catch2 3,
clang-tools, jujutsu, nixfmt) and, for this initiative, also `gbenchmark`
(Google Benchmark) plus optional `lcov`/`genhtml`; `llvm-cov`/`llvm-profdata`
already ship with clang. From inside the shell the Makefile flow (`make build`,
`make test`, `make bench`, ...) works unchanged against the live worktree. (The
`make coverage` target is deferred — see the coverage note above.)

```sh
nix develop                          # interactive
nix develop --command make build     # one-shot
```

### `nix run` apps — make targets without entering the shell

Each app re-enters the dev shell so cmake's setup hooks fire, then runs the
Makefile target against the caller's worktree.

```sh
nix run .#test-unit                  # = nix develop --command make test-unit
nix run .#test-sim
nix run .#test
nix run .#build
```

### Hermetic packages — cached, reproducible, slow first time

Each layer caches independently in `/nix/store`.

```sh
nix build .#openfhe                  # ~20-30 min cold; cached afterwards
nix build .#niobium-fhetch           # depends on openfhe; reuses cache
nix build .#haze                     # = .#default; libhaze + haze_tests
nix flake check                      # devshell + fmt + haze build + tests
```

## macOS SDK / ABI mismatch trap

> **Caution (macOS).** Do not mix nix and non-nix OpenFHE / `libnbfhetch`
> builds in the same link closure. The trap triggers only when **mixing** the
> two (for example, OpenFHE built from a host shell and haze built inside
> `nix develop`, or vice versa); a pure-nix or pure-host workflow is unaffected.
> The symptom is a clean build linking with `LC_BUILD_VERSION` `minos`
> mismatch warnings followed by non-deterministic segfaults inside calls that
> should be no-ops. The fix is to rebuild every dylib in the link graph in the
> same shell so each carries the same SDK version. See
> [`../CLAUDE.md`](../CLAUDE.md) ("macOS SDK / ABI mismatch trap") for the full
> recipe and verification steps.

## See also

- [`./testing.md`](./testing.md) — running the suites, sanitizers, the live
  benchmark workflow, and the deferred coverage workflow.
- [`./architecture.md`](./architecture.md) — the layering the build produces.
- [`./index.md`](./index.md) — documentation landing page.
- [`./decision-log.md`](./decision-log.md) — rationale for build-tooling
  choices (Google Benchmark, Clang source-based coverage, and others).
- [`../README.md`](../README.md) — project overview and the authoritative
  build reference.
