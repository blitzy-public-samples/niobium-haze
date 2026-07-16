# Contributing to Haze

Thank you for your interest in Haze (`libhaze`). This document explains what
Haze is, how to set up a development environment, how to build and test your
changes, the code-quality gates every change must pass, the architectural
invariants that must be preserved, and how changes are proposed and reviewed.

> **Current status — external contributions are gated on the CLA below.**
>
> The contribution policy and Contributor License Agreement (CLA) are still
> being finalized, so we are **not yet able to accept external contributions**.
> Until the CLA process is in place, please [contact us](https://niobium.co/contact)
> with a bug report, a feature request, or a question rather than opening a pull
> request. Watch this repository to be notified when the contribution policy
> launches. The guidelines below describe the workflow every contribution is
> expected to follow once the CLA is in effect (and that internal and authorized
> contributors follow today).

## Scope

Haze is a CUDA-shaped, record-and-replay runtime shim for the Niobium FHE
accelerator. Its public interface is a stable C ABI (`include/haze/haze.h`) that
mirrors the shape of the CUDA runtime API so that CUDA-targeting FHE code can be
retargeted to Niobium hardware by linking against `libhaze` instead of
`libcudart`.

Haze is **not** an FHE application and **not** an FHE compiler. It sits one
layer below `niobium-client`, at the polynomial level (`NTT` / `INTT` / `Add` /
`Mul` / `Automorph` / `BasisConvert`), and records an operation trace that is
materialized elsewhere. Contributions must respect that layering: new work
belongs in the runtime-shim layer, not in the FHE application or compiler
layers, and OpenFHE must stay confined to `replay_bridge/` (see
[Architectural invariants](#architectural-invariants-contributors-must-preserve)).

## Contributor License Agreement (CLA)

> **CLA — placeholder. This is not a binding agreement and contains no legal
> terms.**
>
> The Contributor License Agreement for Haze is still being finalized. This
> section is a clearly marked placeholder: it intentionally does **not** state
> any binding legal terms, grant, or waiver. Once the CLA is published, all
> external contributions will be gated on accepting it, and this section will be
> replaced with the finalized agreement and signing instructions.
>
> Until then, external contributions cannot be merged. To propose a change,
> report a problem, or ask a question, please
> [contact us](https://niobium.co/contact) and watch this repository for the
> announcement when the CLA and contribution policy go live.

## Development environment

Haze targets **Clang 19** as the supported floor, **CMake ≥ 3.22**, **C++23**,
and **Catch2 3.x**. There are two supported ways to obtain a working toolchain;
see the README ["Building"](README.md#building) section for the authoritative,
platform-specific details.

### Option A — standalone Makefile flow (first-class)

The standalone `Makefile` is the first-class path and needs only the
prerequisites installed by your system package manager:

- `clang >= 19` (C++23 is required by `CMakeLists.txt`)
- `cmake >= 3.22`
- `Catch2 3.x`
- `git` (for submodule initialization)

```sh
make sync                    # init vendor/niobium-fhetch + vendor/openfhe submodules
make build MODE=debug        # Debug into dbuild/ (the default); MODE=release builds into build/
make test                    # default suites (see "Building & testing")
```

### Option B — Nix flake dev shell (opt-in)

If you already use Nix, the flake provides a dev shell that provisions the whole
toolchain (clang, cmake, Catch2 3, clang-tools, jujutsu, nixfmt, and the
`clang-tidy-cache` wrapper CI uses):

```sh
nix develop                          # interactive shell
nix develop --command make build     # one-shot
```

Inside the dev shell the Makefile flow above works unchanged. The flake also
exposes `nix run .#build` / `nix run .#test-unit` / `nix run .#test` apps and a
hermetic `nix flake check` (devshell + formatting + build + tests). The flake is
an opt-in convenience; the Makefile flow remains the primary path.

## Building & testing

All build and test entry points are Makefile targets. `MODE` selects the build
directory and CMake configuration and defaults to `debug` (building into
`dbuild/`); pass `MODE=release` to build into `build/` instead. CI and the
`make bench`/`make coverage` flows pin `MODE=release` explicitly, so the
default is never exercised in the merge gates.

| Target | What it does |
| --- | --- |
| `make sync` | Initialize the `vendor/niobium-fhetch` (recursive) and `vendor/openfhe` submodules. |
| `make build` | Build `libhaze` and `haze_tests` (uses `MODE`). |
| `make test` | Default gate: `test-unit` + `test-sim` + `test-e2e` + `test-isolation`. |
| `make test-unit` | State-machine and recording-only coverage (no FHE math). |
| `make test-sim` | `[integration]` cases through the in-process FHETCH simulator (validates FHE math). |
| `make test-e2e` | End-to-end suite over the public C ABI with stock OpenFHE, decrypt-verified. |
| `make test-readme` | Compile and run the README examples (the docs-as-tests check). |
| `make test-isolation` | Assert `libhaze` exports only the `haze*` C ABI (the symbol-leak check). |
| `make test-all` | `make test` plus `test-readme` and the opt-in `test-transport`. |
| `make bench` | Build and run the Google Benchmark suite for the FHE ops and the record→flush→replay path. |
| `make coverage` | Build instrumented, run the suite, and enforce the 80% line-coverage gate on `src/core/` and `src/api/` (Clang source-based coverage). |

> **Coverage:** `make coverage` (and its `HAZE_COVERAGE` build option) is wired in
> this tree: it builds the instrumented tree, runs the suite, and enforces the 80%
> line-coverage gate via the coverage driver
> [`scripts/coverage.sh`](scripts/coverage.sh) — currently passing at **85.70%**.
> A [`.github/workflows/coverage.yml`](.github/workflows/coverage.yml) CI job runs
> the same gate per PR. The rationale for sequencing the gate after the
> backfill/hardening work is recorded in
> [`docs/decision-log.md`](docs/decision-log.md) (D-06, D-10).

Two of these targets guard project-wide invariants and must stay green:

- **Docs-as-tests** — `make test-readme` (driven by
  `scripts/test_readme_examples.sh`) extracts the runnable examples from
  `README.md` and compiles and runs them, so documentation cannot drift from the
  API. Keep the README example regions authoritative when you change them.
- **Symbol-leak isolation** — `make test-isolation` (driven by
  `scripts/check_symbol_leak.sh`) fails if `libhaze` exports any dynamic symbol
  outside the `haze*` C ABI, proving its statically-absorbed OpenFHE cannot
  collide with another OpenFHE in the same process.

Every change must add or extend Catch2 tests that cover the new behavior, and
update the relevant documentation (the README and, where applicable, the files
under `docs/`).

## Code style & quality gates

Formatting and linting are enforced by scripts that back the CI gates, so run
them locally before proposing a change:

- **Formatting** — `scripts/clang-format.sh` formats first-party C/C++ in place;
  `scripts/clang-format.sh --check` is the dry-run the CI gate uses. The rules
  live in [`.clang-format`](.clang-format).
- **Linting** — `scripts/clang-tidy.sh` runs `clang-tidy` with
  `--warnings-as-errors='*'` over the first-party `.cpp` files (it needs a
  `compile_commands.json`, so run `make build` first). The checks live in
  [`.clang-tidy`](.clang-tidy).

Compilation is strict. The project builds with
`-Wall -Wextra -Werror -Wpedantic -Wshadow -Wconversion`, and `-Wthread-safety`
is additionally enabled on Clang; warnings are errors, so a clean build is a
requirement, not a suggestion.

For the deeper contract, read the [`style.md`](style.md) C++ style guide (error
handling, the C-ABI boundary, RAII and ownership, the locking/TSA contract, and
the modern-C++ subset) and the reviewer-facing
[`.github/instructions/CODE_REVIEW_GUIDE.md`](.github/instructions/CODE_REVIEW_GUIDE.md).

## Architectural invariants contributors must preserve

The following invariants are load-bearing. A change that breaks any of them will
not be accepted:

- **Stable public C ABI** — Do not remove, reorder, or rename any public symbol;
  do not change `hazeError_t` enum values or existing struct layouts; additive
  changes only. Every public entry point keeps its `HAZE_API` and
  `HAZE_NOEXCEPT` markers, and no C++ appears in the interface headers.
- **Record-and-replay model** — Every compute call appends a node to an
  in-memory FHETCH trace; nothing executes until `hazeFlush()`, which is the
  **sole materialization trigger**. A device-to-host read of an address that was
  not tagged as an output and flushed returns `HAZE_ERROR_NOT_FLUSHED`. Preserve
  this single-trigger contract and the device-pointer-vs-shadow-buffer
  distinction.
- **Error handling** — Use `std::expected<T, HazeInternalError>` internally and
  translate to `hazeError_t` at the ABI boundary (via `set_internal_result` /
  `to_public_error`). **No C++ exception may cross the C ABI boundary.**
- **Lock order and thread safety** — Honor the `epoch → allocator` lock order and
  keep the Clang thread-safety (TSA) annotations intact so `-Wthread-safety`
  stays clean.
- **Symbol isolation** — Only `haze*`-prefixed symbols are exported from
  `libhaze`; internal C++ symbols stay hidden. The statically-absorbed OpenFHE
  is fully localized (enforced by `make test-isolation`).
- **OpenFHE confinement** — OpenFHE is used only inside `replay_bridge/`. Keep
  OpenFHE includes out of `libhaze`'s translation units and out of the public
  headers.
- **Intentional no-ops** — `hazeStreamSynchronize` and `hazeStreamWaitEvent`
  (`src/api/stream.cpp`) and `hazeDeviceSynchronize` (`src/api/device.cpp`) are
  correct-by-design no-ops that return `HAZE_SUCCESS` without flushing. Do not
  change their behavior; smoke tests asserting the no-op are welcome.

## Pull request process

- **Scope PRs narrowly** — Open a separate pull request per work item (or per
  priority tier). Each PR should be self-contained and easy to review.
- **Include tests** — Every PR carries Catch2 tests for the behavior it changes
  or adds, and updates the relevant README/`docs/` content.
- **Keep CI green** — All ten CI workflows must pass: the seven pre-existing
  (`build-matrix`, `build-test`, `clang-format`, `flake-check`, `openfhe-bump`,
  `PR - Claude Code Review`, `SCANOSS License Compliance`) plus the three added
  by this initiative — `benchmark`, `coverage` (the 80% line-coverage gate,
  currently passing at 85.70%), and `sanitizers` (ASan/UBSan + TSan; the
  `HAZE_SANITIZERS`/`HAZE_TSAN` build options can also be exercised locally).
  Run `make test`, `scripts/clang-format.sh --check`, and `scripts/clang-tidy.sh`
  locally first.
- **Describe the change** — Provide a short description of what was implemented
  and an explicit list of any human follow-up (for example, physical multi-chip
  hardware validation for peer access).
- **Record rationale in the decision log** — Non-trivial design decisions and any
  deviation from a literal reading of the requirements are documented in
  [`docs/decision-log.md`](docs/decision-log.md), not in code comments. The
  decision log is the single source of truth for rationale.

## Reporting bugs & requesting features

Because external contributions are currently gated on the CLA, the best way to
report a bug, request a feature, or ask a question is to
[contact us](https://niobium.co/contact) directly. Watch this repository to be
notified when the contribution policy launches and pull requests open up.
