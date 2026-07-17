# Testing

Haze ships one Catch2 v3 test binary, `haze_tests`, that holds every unit and
integration case (roughly 390 `TEST_CASE`s across 31 unit and integration
translation units plus the `test/e2e/` OpenFHE-pipeline suite). The suites are
not separate executables: they are the *same* binary sliced by Catch2 tag and by
environment (`HAZE_TARGET`, plus the optional transport setup). A second,
opt-in binary, `haze_e2e_tests`, is a black-box executable that links the
*shipped* `libhaze.so` through the public `haze*` C ABI only (no `src/`, no
internal symbols) alongside a stock OpenFHE, to prove a consumer can run its
own OpenFHE next to haze.

Because the public API exposes no getters, tests assert `HAZE_SUCCESS` and the
`hazeError_t` codes and reach internal state through test-only helper headers
(`test/allocator_test_access.hpp`, `test/integration_helpers.hpp`,
`test/integration_introspect.hpp`). Every case calls `hazeDeviceReset()`
between runs so ordering never leaks state.

The build directory follows `MODE`: `make build` defaults to `MODE=debug`
(`dbuild/`), and `MODE=release` uses `build/`. CI forces `MODE=release`.

## Running the test suites

The `Makefile` is the standalone entry point. Each target builds first, then
runs its slice:

```sh
make test-unit        # ~[integration] tag, HAZE_TARGET=local; no D2H, no FHE math (~1s)
make test-sim         # [integration] tag via the in-process FHETCH simulator
make test-e2e         # black-box haze_e2e_tests: public C ABI + stock OpenFHE, decrypt-verified
make test-readme      # compile + run the README examples through the local simulator
make test-isolation   # assert libhaze.so exports only the haze* C ABI
```

The transport suite is opt-in and needs a `niobium-compiler` checkout that
contains `build/nbcc_fhetch_replay`:

```sh
make test-transport NIOBIUM_COMPILER_ROOT=/path/to/niobium-compiler
```

Two aggregate targets compose the slices:

```sh
make test       # test-unit + test-sim + test-e2e + test-isolation (no transport dependency)
make test-all   # test + test-readme + test-transport (everything)
```

All targets honour `MODE`, so `make test MODE=release` exercises the same
optimization level CI ships. `make help` prints the full target list.

### Running cases directly

To run a single case or a tag filter, bypass `make` and call the binary. Tests
`cd` into `<build-dir>/runs/` first so `niobium::compiler()`'s `program_dir`
resolves under the build tree rather than the source root (`dbuild/runs/` by
default, `build/runs/` for release):

```sh
mkdir -p build/runs && cd build/runs
HAZE_TARGET=local ../haze_tests "[unit]"                     # every unit case
HAZE_TARGET=local ../haze_tests "[integration]"              # every integration case
HAZE_TARGET=local ../haze_tests "*hazeAdd: pointwise sum*"   # select cases by name (wildcard)
HAZE_TARGET=local ../haze_tests --list-tests                 # enumerate cases
```

`HAZE_TARGET=local` pins the in-process simulator so a bare invocation never
tries to dispatch to `nbcc_fhetch_replay`. Any other target string
(`FUNC_SIM`, `FHE_SIM`, `FPGA_TRI`, `fhetch_sim`) is forwarded over HTTP
transport and requires `NIOBIUM_COMPILER_ROOT`.

## Suites, tags, and CTest targets

Each slice also has a CTest registration, so `ctest -R <name>` runs it from a
configured build tree:

| Suite (`make`)  | Catch2 filter    | Environment                                             | CTest target                | What it validates |
| --------------- | ---------------- | ------------------------------------------------------- | --------------------------- | ----------------- |
| `test-unit`     | `~[integration]` | `HAZE_TARGET=local`                                     | `haze_unit_tests`           | State-machine and recording-only behaviour; no D2H, no FHE-math validation; fast (~1s). |
| `test-sim`      | `[integration]`  | `HAZE_TARGET=local`, in-process FHETCH simulator        | `haze_sim_tests`            | Real FHE math through `libnbfhetch`, including the `[integration][e2e]` OpenFHE-pipeline cases. |
| `test-transport`| `[integration]`  | `NIOBIUM_COMPILER_ROOT`, `HAZE_TARGET=FUNC_SIM`, HTTP   | `haze_transport_tests`      | The same `[integration]` cases shipped over HTTP to a `niobium-compiler`-built `nbcc_fhetch_replay`. |
| `test-e2e`      | black-box binary | `HAZE_TARGET=local`, stock OpenFHE 1.5.1                | `haze_e2e_tests`            | The public `haze*` C ABI end to end with real OpenFHE encrypt/decrypt around haze ops. |
| `test-readme`   | n/a (script)     | `HAZE_TARGET=local`                                     | n/a (`test_readme_examples.sh`) | The README examples compile and run against the shipped library. |
| `test-isolation`| n/a (script)     | n/a                                                     | `haze_isolated_symbol_leak` | `libhaze.so` exports only the `haze*` C ABI. |

Tags in use are `[unit]`, `[integration]`, and `[integration][e2e]`. The unit
slice runs `~[integration]` (everything *not* tagged `[integration]`), so it
picks up `[unit]` cases and any other non-integration tag. The `test/e2e/`
OpenFHE-pipeline cases carry `[integration][e2e]` and therefore run under
`haze_sim_tests` and `haze_transport_tests`; filter them with `[e2e]`. For
their `TEMPLATE_TEST_CASE`-over-scaling-mode structure and the
bit-exact-then-slot-tolerance assertion ladder, see
[`../test/e2e/README.md`](../test/e2e/README.md).

## Sanitizers

Two mutually exclusive CMake cache options add instrumentation. They are
configure-time toggles, so set them with a direct `cmake` configure into a
dedicated build tree:

```sh
# AddressSanitizer + UndefinedBehaviorSanitizer
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DHAZE_SANITIZERS=ON
cmake --build build-asan -j

# ThreadSanitizer
cmake -S . -B build-tsan -DCMAKE_BUILD_TYPE=Debug -DHAZE_TSAN=ON
cmake --build build-tsan -j
```

Setting both `HAZE_SANITIZERS` and `HAZE_TSAN` at once is a configure-time
`FATAL_ERROR`. Under `HAZE_SANITIZERS` the UBSan enum check is disabled
(`-fno-sanitize=enum`) so the C-ABI `hazeError_t` survives forward-compatible
enum extension across library versions; the rest of UBSan stays active.

A `.lsan_suppressions` file at the repository root suppresses known upstream
leaks inside `niobium-compiler`'s optimization passes so a LeakSanitizer run
surfaces only haze-side leaks:

```sh
LSAN_OPTIONS=suppressions=$(pwd)/.lsan_suppressions ./build-asan/haze_tests "[integration]"
```

The error-path and edge-case hardening tests (see below) are written to run
clean under both `HAZE_SANITIZERS` and `HAZE_TSAN`.

## Code coverage

> **Status: delivered.** The `HAZE_COVERAGE` CMake option, the `make coverage`
> target, the coverage driver [`../scripts/coverage.sh`](../scripts/coverage.sh),
> and the coverage CI gate are all wired. `make coverage` builds instrumented,
> runs the suite, and enforces the 80% line-coverage threshold on `src/core/` and
> `src/api/` — currently passing at **85.85%**. The workflow below is the live
> design.

Coverage uses Clang source-based instrumentation, gated behind the
`HAZE_COVERAGE` CMake option. The `make coverage` target configures, builds,
runs the suite, and produces the report in one step:

```sh
make coverage        # builds instrumented, runs the suite, enforces the 80% gate
```

Driven manually, the option is configured into a dedicated tree, then
`scripts/coverage.sh` run against it. `HAZE_COVERAGE` requires a Clang toolchain
(the configure step errors out under GCC, which is the default `cc`/`c++` on
many systems), so select Clang 19 explicitly:

```sh
CC=clang-19 CXX=clang++-19 \
  cmake -S . -B build-coverage -DCMAKE_BUILD_TYPE=Debug -DHAZE_COVERAGE=ON
cmake --build build-coverage -j
scripts/coverage.sh
```

`HAZE_COVERAGE` compiles the tree with `-fprofile-instr-generate
-fcoverage-mapping`. Running that instrumented `haze_tests` emits one or more
`*.profraw` files; merge them with `llvm-profdata` and export with `llvm-cov`,
passing the **same** instrumented `./build-coverage/haze_tests` binary that
produced the profiles (the profile and the binary must match), scoped to the
two trees the threshold applies to:

```sh
llvm-profdata merge -sparse *.profraw -o haze.profdata
llvm-cov export -format=lcov -instr-profile=haze.profdata \
    ./build-coverage/haze_tests src/core src/api > coverage.lcov
```

The [`../.github/workflows/coverage.yml`](../.github/workflows/coverage.yml)
coverage CI job enforces an **80% line-coverage threshold** on `src/core/` and
`src/api/`, failing the job below it (currently passing at **85.85%**). The gate
is intentionally sequenced to be enabled only after the backfill and hardening
tests raised coverage to clear it, so turning it on did not retroactively redden
the branch. An HTML report is optional via `lcov`'s `genhtml`. On lcov 2.x,
`genhtml` is stricter than `llvm-cov`'s lcov export — it aborts on the derived
function end-line and hit/line consistency checks — so pass the same
`--ignore-errors` flags `scripts/coverage.sh` uses:

```sh
genhtml coverage.lcov --output-directory coverage-html \
    --ignore-errors inconsistent,unsupported
```

The rationale for Clang source-based coverage over `gcov`, and for sequencing
the gate after the backfill, lives in [`./decision-log.md`](./decision-log.md)
(D-02, D-06, D-10).

## Docs-as-tests and symbol isolation

`make test-readme` keeps the published examples from rotting.
`scripts/test_readme_examples.sh` extracts the two fenced example blocks from
`README.md` by their HTML markers — a pure-C quickstart
(`<!-- readme-example:begin lang=c name=quickstart -->`) and a C++ CKKS example
(`<!-- readme-example:begin lang=cpp name=ckks22 -->`) — compiles each against
the shipped `libhaze`, runs it through the in-process FHETCH simulator
(`HAZE_TARGET=local`), and asserts exit 0 plus the expected output token. The
same programs are mirrored under [`../examples/`](../examples/) for readers who
want a ready-to-build copy; the README marker regions remain the source of
truth the check validates against.

`make test-isolation` runs `scripts/check_symbol_leak.sh` against the shipped
`libhaze.so` (CTest target `haze_isolated_symbol_leak`). It fails if the
library exports any defined dynamic symbol outside the `haze*` C ABI — or
exports none at all — which is the proof that the statically absorbed OpenFHE
cannot collide with another OpenFHE loaded in the same process.

## Tests added by this initiative

This initiative raised and hardened the suite. New translation units follow the
existing Catch2 v3 conventions and are registered in the
`add_executable(haze_tests ...)` list in `CMakeLists.txt`.

**Backfill coverage.** Roughly 23 of the 77 public API functions were
previously unexercised. New cases cover stream and event lifecycle, async
memory operations, the device API, host memory, and introspection:

- `test/test_stream_event_lifecycle.cpp`
- `test/test_async_ops.cpp`
- `test/test_device_api.cpp`
- `test/test_host_memory.cpp`
- `test/test_introspection.cpp`
- `test/test_documented_noops.cpp`

`test/test_documented_noops.cpp` adds **smoke tests only** for the three
documented no-op functions — `hazeStreamSynchronize`, `hazeStreamWaitEvent`,
and `hazeDeviceSynchronize`. Those functions are correct by design; the tests
assert they still return `HAZE_SUCCESS` and their behaviour is unchanged.

**Error-path and edge-case hardening.** Negative and boundary tests span nine
categories — invalid or null handles, unflushed reads, the device-versus-shadow
pointer distinction, configuration errors, allocator limits, error-state
semantics, ABI exception safety, lock ordering and concurrency, and
not-supported paths — and are exercised under the sanitizers:

- `test/test_error_paths.cpp`
- `test/test_unflushed_reads.cpp`
- `test/test_allocator_limits.cpp`
- `test/test_error_semantics.cpp`
- `test/test_config_errors.cpp`

**Feature tests.** New cases cover the implemented features:

- `test/test_graph_capture.cpp` — stream capture, instantiate, launch, update,
  and destroy, plus replay determinism.
- `test/test_peer_access.cpp` — simulator-representable peer access. Assertions
  that require physical multi-chip hardware sit behind a dedicated tag so the
  default suite stays green without hardware; physical multi-chip validation is
  human follow-up.
- `test/test_performance_counters.cpp` — asserts the counters are populated
  after a workload.

## Continuous integration

The seven pre-existing workflows in `.github/workflows/` — `build-matrix.yml`,
`build-test.yml`, `clang-format.yml`, `flake-check.yml`, `openfhe-bump.yml`,
`pr-claude-code-review.yml`, and `scanoss.yml` — remain green and unchanged.
This initiative adds three new jobs alongside them, for ten workflows total:

- `.github/workflows/benchmark.yml` — **live.** Runs the benchmark suite and
  flags regressions against the checked-in baseline.
- `.github/workflows/coverage.yml` — **live.** Builds instrumented, produces the
  coverage report, and enforces the 80% line-coverage gate on `src/core/` and
  `src/api/` via the `HAZE_COVERAGE` option and the `make coverage` target
  (currently passing at 85.85% — see the coverage note above).
- `.github/workflows/sanitizers.yml` — **live.** Runs ASan/UBSan and TSan over
  the hardening tests. The `HAZE_SANITIZERS` and `HAZE_TSAN` build options are
  exercised by this job and can also be run locally.

## See also

- [`./building.md`](./building.md) — build options, `MODE`, and toolchain.
- [`./architecture.md`](./architecture.md) — the record-and-replay design.
- [`./decision-log.md`](./decision-log.md) — rationale for the testing and
  coverage choices.
- [`./index.md`](./index.md) — documentation landing page.
- [`../README.md`](../README.md) — project overview and the authoritative
  example markers.
