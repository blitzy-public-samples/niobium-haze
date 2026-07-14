# Decision Log

> Single source of truth for design rationale on the Haze (`libhaze`) initiative,
> maintained per **Rule 1 (Explainability)** of the Agent Action Plan.

## How this log works

This document records the rationale behind every non-trivial implementation
decision on the Haze initiative. Per Rule 1, rationale lives **here and only
here** — it is deliberately kept out of code comments so that code stays
declarative while the reasoning, alternatives, and risks live in one auditable
place.

- **Scope.** Every non-trivial decision, and every deviation from a literal
  reading of the requirements, has a row in the [decision table](#2-decision-table).
  Unexplained deviations are treated as defects.
- **Traceability.** Because this initiative includes migrations and refactors
  (graph capture on top of the epoch subsystem; README examples mirrored into
  `examples/`), the log also carries a bidirectional
  [traceability matrix](#3-traceability-matrix) with 100% coverage.
- **Organization.** The log is append-oriented and organized by work item /
  priority tier (`P1a`, `P1b`, `P2a` … and Rules 1–3). Each pull request cites
  the decision IDs it implements in its description.
- **Stable IDs.** Decision IDs (`D-01`, `D-02`, …) never change or get
  renumbered; new decisions take the next free ID.

### Referenced paths

The decisions and matrices below reference these files. Paths are relative to
this document (`docs/decision-log.md`).

- Grounding sources: [`../README.md`](../README.md), [`../CMakeLists.txt`](../CMakeLists.txt), [`../Makefile`](../Makefile), [`../flake.nix`](../flake.nix), [`../src/core/epoch.hpp`](../src/core/epoch.hpp).
- Companion documentation (this initiative): [`./index.md`](./index.md), [`./architecture.md`](./architecture.md), [`./building.md`](./building.md), [`./testing.md`](./testing.md), [`./observability/README.md`](./observability/README.md), [`./presentation/executive-summary.html`](./presentation/executive-summary.html).
- Rationale pointers elsewhere in the tree resolve back here: [`../CONTRIBUTING.md`](../CONTRIBUTING.md), [`../README.md`](../README.md), [`../CMakeLists.txt`](../CMakeLists.txt), [`../Makefile`](../Makefile), [`../flake.nix`](../flake.nix).

## 2. Decision table

Columns follow the Rule 1 contract: what was decided, what alternatives existed,
why the choice was made, and what risks it carries (with mitigations). The final
column ties each decision to its work item or rule.

| ID | Decision | Alternatives considered | Rationale | Risks / Mitigations | Work item(s) |
|----|----------|-------------------------|-----------|---------------------|--------------|
| <a id="d-01"></a>D-01 | Use **Google Benchmark** for the microbenchmark harness. | Catch2's built-in benchmarking; nanobench; hand-rolled `std::chrono` timing loops. | Industry-standard `for (auto _ : state)` loop with `benchmark::DoNotOptimize()` to defeat dead-code elimination; native JSON output feeds the CI regression gate against a checked-in baseline; available from the pinned nixpkgs as `gbenchmark` (1.9.5); builds as a separate executable linking `haze_objs`, never absorbed into `libhaze`. | Risk: adds a developer-only build dependency. Mitigation: confined to the dev shell and a standalone benchmark executable, so the shipped `libhaze`, its ABI, and the symbol-leak audit are unaffected. | P1b |
| <a id="d-02"></a>D-02 | Measure coverage with **Clang source-based coverage** (`llvm-cov` + `llvm-profdata`) rather than gcov + lcov-gcc. | GCC `gcov` with `lcov`/`genhtml`; a hosted third-party coverage service. | The project standardizes on Clang 19 (the supported floor), so `llvm-cov`/`llvm-profdata` already ship with the toolchain and add no new runtime dependency; `-fprofile-instr-generate -fcoverage-mapping` plus `llvm-cov export -format=lcov` scopes cleanly to `src/core/` and `src/api/` for the 80% target. | Risk: a Clang-only workflow. Mitigation: Clang 19 is the supported compiler floor and the instrumentation is gated behind a Clang-guarded `HAZE_COVERAGE` option, so ordinary builds are unaffected. | P2c |
| <a id="d-03"></a>D-03 | Implement graph capture by **snapshotting the recorded epoch trace** into a new `src/core/graph.{hpp,cpp}` module, not by inventing a new IR. | A dedicated graph IR; overloading `EpochState` to retain multiple replays; leaving the stubs at `HAZE_ERROR_NOT_SUPPORTED`. | `EpochState` records an op sequence once and resets after a single `replay_and_populate()` (state is dropped via `clear_state_locked()` at materialize time), so it cannot express record-once/replay-many alone; cloning the recorded FHETCH op-sequence and input bindings into `GraphState`/`GraphExec` yields deterministic replay with `DevAddr` operand stability, matching CUDA stream-capture / instantiate / launch semantics without a new IR. | Risk: snapshot lifetime and device-memory aliasing across replays. Mitigation: the snapshot must neither leak nor alias device memory between replays; the pointer-stability invariant is exercised by `test/test_graph_capture.cpp`. | P1a |
| <a id="d-04"></a>D-04 | **Reinterpret the Observability rule for a C++ shared library with a C ABI** (explicit deviation from the network-service literal reading). | A literal HTTP metrics endpoint and network tracing (not applicable to a library); or skipping observability (non-compliant). | Haze ships no network surface, so: structured logging with correlation IDs lives in `src/common/log.*` keyed by epoch/stream; the "metrics endpoint" is the `hazeGetPerformanceCounters` query surface; "distributed tracing across service boundaries" becomes op/epoch span tracing across the record -> flush -> replay path and the `replay_bridge/` / OpenFHE boundary; health/readiness maps to lifecycle and config-state introspection; the dashboard template ships under `docs/observability/`. The three documented no-ops stay pure and uninstrumented. | Risk: the reinterpretation could be read as non-compliance. Mitigation: recorded as an explicit, justified deviation (see [Deviations](#41-deviations)) with a one-to-one mapping from each literal requirement to its library-context equivalent. | Rule 2 |
| <a id="d-05"></a>D-05 | Keep the **README example markers authoritative** while additionally shipping byte-consistent mirrors under `examples/`. | Move the canonical examples into `examples/` and have the README transclude them; maintain two independently edited copies. | `scripts/test_readme_examples.sh` extracts and compiles the fenced blocks from `README.md` by HTML markers; keeping the README authoritative preserves the docs-as-tests guarantee unchanged, while `examples/quickstart.c` and `examples/ckks22.cpp` give users standalone files. README-as-source-of-truth with mirrored examples. | Risk: drift between the README and `examples/`. Mitigation: the docs-as-tests CI check keeps the README blocks compiling and running; the sync strategy (mirror from the README, do not hand-edit) is recorded here. | P3b |
| <a id="d-06"></a>D-06 | **Sequence the 80% coverage gate after** the P2d backfill and P2e hardening. | Enable the gate immediately; never gate. | A logical (not schedule) dependency — establish a baseline, raise coverage, then enable the gate — so the branch is never gated red before the coverage exists to clear it on `src/core/` + `src/api/`. | Risk: none material. Mitigation: the gate is enabled only once measured coverage clears 80% on the two scoped trees. | P2c (after P2d/P2e) |
| <a id="d-07"></a>D-07 | Expose performance counters via an **additive `hazePerformanceCounters` struct** populated through the retained `hazeGetPerformanceCounters(void*)` signature. | Add new per-counter getter symbols; return counters through several primitive out-parameters; change the existing signature. | The ABI signature is retained, so adding a struct is purely additive and introduces no new exported symbol; the struct is appended after the existing types with no reordering, preserving ABI stability. | Risk: future counter additions could tempt a layout change. Mitigation: treat the struct as append-only — never reorder or resize existing fields. | P2b |
| <a id="d-08"></a>D-08 | Build the **benchmark harness and coverage build as separate executables/configurations**, never absorbed into the shipped `libhaze`. | Compile benchmarks or coverage instrumentation into the library or its default build. | Keeps the shipped `libhaze` byte-identical (apart from the additive struct) and keeps the symbol-leak audit green by construction; internal C++ symbols mangle to `_ZN4haze…` and are localized by the existing version script. | Risk: two extra build configurations to maintain. Mitigation: both are gated behind opt-in CMake options (`HAZE_BUILD_BENCHMARKS`, `HAZE_COVERAGE`) and are exercised only in their dedicated CI jobs. | P1b, P2c |
| <a id="d-09"></a>D-09 | Implement peer access for the **simulator-representable portions only**, flagging physical multi-chip steps as human follow-up. | Implement full multi-chip peer access now; leave the stubs at `HAZE_ERROR_NOT_SUPPORTED`. | The record-and-replay simulator can model peer topology and same-process peer copies, but physical multi-chip transfer needs hardware absent from CI; implementing the representable portion advances the API while being honest about the hardware boundary. | Risk: hardware-only paths cannot be validated in CI. Mitigation: hardware-only assertions in `test/test_peer_access.cpp` sit behind a dedicated tag so the default suite stays green; the human follow-up is called out in the PR description. | P2a |
| <a id="d-10"></a>D-10 | Add benchmark, coverage, and sanitizer CI as **new workflow files** (`benchmark.yml`, `coverage.yml`, `sanitizers.yml`) rather than editing the seven existing workflows. | Extend `build-test.yml` with additional jobs. | Isolating new jobs keeps the seven established workflows intact and green, simplifies review, and lets each job be enabled or made required independently; `sanitizers.yml` is delivered as optional. | Risk: more workflow files to track. Mitigation: each file is small and single-purpose, named for the work item it serves. | P1b, P2c, P2e |
| <a id="d-11"></a>D-11 | Leave `linker/haze_exports.map` and `linker/haze_exports.sym` **unchanged**. | Add explicit entries for the newly-filled functions. | The graph, peer, and counter functions are already exported stubs; the version script matches the `haze*` C ABI by wildcard, so filling the stubs adds no new public symbol and needs no script change — the symbol-leak audit stays green by construction. | Risk: a future non-`haze*` export would slip the wildcard. Mitigation: `scripts/check_symbol_leak.sh` fails on any exported symbol outside `haze*`. | P1a, P2a, P2b |
| <a id="d-12"></a>D-12 | **Embed the Blitzy reveal.js theme inline** in `docs/presentation/executive-summary.html`. | Reference the canonical `blitzy-deck/references/blitzy-reveal-theme.css` by path; fabricate a theme. | The canonical theme file is not present in the checkout and the deck must be a single self-contained HTML file, so the theme is embedded inline; the canonical file is treated as an external reference and its contents are not fabricated. | Risk: the inline theme could drift from the canonical one. Mitigation: the inline copy is clearly marked as embedded; if the canonical file is later added to the checkout, the deck can be re-synced from it. | Rule 3 |

## 3. Traceability matrix

The initiative includes migrations and refactors, so Rule 1 requires a
bidirectional matrix. The forward matrix maps each requirement to the files that
satisfy it and the tests that validate it; the backward matrix maps each
created or modified file group back to the requirement it serves. All paths are
target paths from the Agent Action Plan file-transformation mapping.

### 3.1 Forward matrix (requirement -> implementation)

| Work item | Requirement (short) | Target file(s) | Test(s) / Validation |
|-----------|---------------------|----------------|----------------------|
| P1a | Graph capture: record-once, replay-many | `src/api/graph.cpp`, `src/core/graph.hpp`, `src/core/graph.cpp`, `src/core/epoch.hpp`, `src/core/epoch.cpp`, `include/haze/haze.h` | `test/test_graph_capture.cpp`, `test/test_build.cpp` |
| P1b | Benchmark harness + regression gate; `make bench` | `benchmark/CMakeLists.txt`, `benchmark/bench_compute.cpp`, `benchmark/bench_record_replay.cpp`, `benchmark/baseline.json`, `CMakeLists.txt`, `Makefile`, `flake.nix`, `.github/workflows/benchmark.yml` | CI benchmark job compares JSON output against `benchmark/baseline.json` |
| P2a | Multi-device / peer access (simulator) | `src/api/device.cpp`, `src/api/memory.cpp`, `src/core/device.hpp`, `src/core/device.cpp`, `include/haze/haze.h` | `test/test_peer_access.cpp` (hardware-gated tag), `test/test_build.cpp` |
| P2b | Performance counters | `include/haze/haze_types.h`, `src/api/device.cpp`, `src/core/metrics.hpp`, `src/core/metrics.cpp`, `src/core/epoch.cpp`, `src/core/allocator.cpp`, `include/haze/haze.h` | `test/test_performance_counters.cpp` |
| P2c | Coverage measurement + 80% gate | `CMakeLists.txt`, `Makefile`, `scripts/coverage.sh`, `flake.nix`, `.github/workflows/coverage.yml` | CI coverage job enforces 80% line coverage on `src/core/` + `src/api/` |
| P2d | Backfill tests for the remaining public functions | `CMakeLists.txt` (test registration) | `test/test_stream_event_lifecycle.cpp`, `test/test_async_ops.cpp`, `test/test_device_api.cpp`, `test/test_host_memory.cpp`, `test/test_introspection.cpp`, `test/test_documented_noops.cpp` |
| P2e | Error-path and edge-case hardening | `CMakeLists.txt` (test registration), `.github/workflows/sanitizers.yml` | `test/test_error_paths.cpp`, `test/test_unflushed_reads.cpp`, `test/test_allocator_limits.cpp`, `test/test_error_semantics.cpp`, `test/test_config_errors.cpp` (run under ASan/UBSan/TSan) |
| P3a | Complete CONTRIBUTING with a marked CLA placeholder | `CONTRIBUTING.md` | Markdown review; internal and external links resolve |
| P3b | `docs/` + `examples/`; keep docs-as-tests green | `docs/index.md`, `docs/architecture.md`, `docs/building.md`, `docs/testing.md`, `examples/quickstart.c`, `examples/ckks22.cpp`, `examples/README.md`, `examples/CMakeLists.txt`, `README.md`, `scripts/test_readme_examples.sh` | `scripts/test_readme_examples.sh` (docs-as-tests CI) |
| Rule 1 | Explainability: decision log + traceability | `docs/decision-log.md` | This document; the coverage assertion in [section 3.3](#33-coverage-assertion) |
| Rule 2 | Observability (library-context reinterpretation) | `src/common/log.hpp`, `src/common/log.cpp`, `docs/observability/dashboard-template.json`, `docs/observability/README.md` | `hazeGetPerformanceCounters` query surface; locally-verified structured logs and dashboard template |
| Rule 3 | Executive presentation | `docs/presentation/executive-summary.html` | Self-contained reveal.js deck opens offline; 12–18 slides (target 16); pinned CDNs |

### 3.2 Backward matrix (implementation -> requirement)

| File (or file group) | Work item(s) it satisfies | Notes |
|----------------------|---------------------------|-------|
| `src/api/graph.cpp` | P1a | Fill the seven graph-capture stubs. |
| `src/core/graph.hpp`, `src/core/graph.cpp` | P1a | New `GraphState` / `GraphExec` snapshot module (see [D-03](#d-03)). |
| `src/core/epoch.hpp`, `src/core/epoch.cpp` | P1a, P2b | Trace-snapshot primitive without post-replay reset; op-count and flush-timing metric hooks. |
| `src/api/device.cpp` | P2a, P2b | Fill peer-access stubs and the performance-counter stub. |
| `src/api/memory.cpp` | P2a | Fill `hazeMemcpyPeerAsync`. |
| `src/core/device.hpp`, `src/core/device.cpp` | P2a | Simulator peer-topology model (see [D-09](#d-09)). |
| `src/core/metrics.hpp`, `src/core/metrics.cpp` | P2b | New counter aggregator (op counts, bytes moved, flush timings). |
| `src/core/allocator.cpp` | P2b | Bytes-moved counter hooks. |
| `include/haze/haze_types.h` | P2b | Additive `hazePerformanceCounters` struct (see [D-07](#d-07)). |
| `include/haze/haze.h` | P1a, P2a, P2b | Documentation-comment updates only; no signature change. |
| `src/common/log.hpp`, `src/common/log.cpp` | Rule 2 | Structured logging with correlation IDs (see [D-04](#d-04)). |
| `CMakeLists.txt` | P1a, P1b, P2b, P2c, P2d, P2e | Register new core sources in `haze_objs`; add `HAZE_BUILD_BENCHMARKS` and `HAZE_COVERAGE`; register all new test files. |
| `Makefile` | P1b, P2c | Add `make bench` and coverage targets. |
| `flake.nix` | P1b, P2c | Add `gbenchmark` (and optional `lcov`) to the dev shell. |
| `benchmark/CMakeLists.txt`, `benchmark/bench_compute.cpp`, `benchmark/bench_record_replay.cpp`, `benchmark/baseline.json` | P1b | Harness and checked-in regression baseline (see [D-01](#d-01), [D-08](#d-08)). |
| `scripts/coverage.sh` | P2c | `llvm-profdata merge` + scoped `llvm-cov export -format=lcov` + 80% gate (see [D-02](#d-02)). |
| `scripts/test_readme_examples.sh` | P3b | Keep docs-as-tests green across README <-> `examples/` (see [D-05](#d-05)). |
| `.github/workflows/benchmark.yml` | P1b | Benchmark regression job (see [D-10](#d-10)). |
| `.github/workflows/coverage.yml` | P2c | Coverage report + 80% gate (see [D-10](#d-10)). |
| `.github/workflows/sanitizers.yml` | P2e | Optional ASan/UBSan + TSan job (see [D-10](#d-10)). |
| `test/test_build.cpp` | P1a, P2a | Update the graph and peer `NOT_SUPPORTED` assertions; redirect the last-error helper (ripple). |
| `test/test_graph_capture.cpp` | P1a | Capture / instantiate / launch / update / destroy and replay-determinism tests. |
| `test/test_peer_access.cpp` | P2a | Peer tests behind a hardware-gated tag. |
| `test/test_performance_counters.cpp` | P2b | Assert non-zero counters after a workload. |
| `test/test_stream_event_lifecycle.cpp`, `test/test_async_ops.cpp`, `test/test_device_api.cpp`, `test/test_host_memory.cpp`, `test/test_introspection.cpp`, `test/test_documented_noops.cpp` | P2d | Backfill coverage for the remaining public functions and documented no-op smoke tests. |
| `test/test_error_paths.cpp`, `test/test_unflushed_reads.cpp`, `test/test_allocator_limits.cpp`, `test/test_error_semantics.cpp`, `test/test_config_errors.cpp` | P2e | Negative and boundary tests across the nine hardening categories. |
| `CONTRIBUTING.md` | P3a | Full contributor guidelines with a clearly marked CLA placeholder. |
| `README.md` | P3b | Add graph/peer/counter status, benchmarking, coverage, and observability sections; link `docs/` and `examples/`. |
| `docs/index.md`, `docs/architecture.md`, `docs/building.md`, `docs/testing.md` | P3b | New documentation tree. |
| `examples/quickstart.c`, `examples/ckks22.cpp`, `examples/README.md`, `examples/CMakeLists.txt` | P3b | Migrated runnable examples mirrored from the README markers. |
| `docs/decision-log.md` | Rule 1 | This file — the single source of truth for rationale. |
| `docs/observability/dashboard-template.json`, `docs/observability/README.md` | Rule 2 | Dashboard template and the counters -> metrics / health / tracing mapping. |
| `docs/presentation/executive-summary.html` | Rule 3 | Self-contained reveal.js executive deck (see [D-12](#d-12)). |
| `src/api/stream.cpp` (`hazeStreamSynchronize`, `hazeStreamWaitEvent`); `src/api/device.cpp` (`hazeDeviceSynchronize`) | Out of scope | The three documented no-ops are correct-by-design and intentionally NOT modified; behavior is unchanged and only smoke-tested (in `test/test_documented_noops.cpp`). |

### 3.3 Coverage assertion

This matrix is **100%-covering**: every Agent Action Plan work item (P1a, P1b,
P2a, P2b, P2c, P2d, P2e, P3a, P3b) and every rule (Rules 1–3) appears in the
forward matrix, and every created or modified in-scope file group appears in the
backward matrix. The three documented no-ops (`hazeStreamSynchronize`,
`hazeStreamWaitEvent`, `hazeDeviceSynchronize`) are the only intentionally
excluded functions and are recorded above as out-of-scope, smoke-tested only.


## 4. Deviations and assumptions

### 4.1 Deviations

Each deviation from a literal reading of the requirements is logged here and
linked to its decision-table entry, per Rule 1.

- **Observability reinterpreted for a library context.** The rule's
  network-service vocabulary (metrics endpoint, distributed tracing, health
  checks) is mapped to library equivalents rather than implemented literally.
  See [D-04](#d-04).
- **Executive summary embeds its theme inline.** The canonical
  `blitzy-deck/references/blitzy-reveal-theme.css` is absent from the checkout,
  so the deck is delivered as a single self-contained HTML file with the theme
  embedded; the canonical file's contents are not fabricated. See [D-12](#d-12).
- **`sanitizers.yml` is delivered as optional.** The ASan/UBSan + TSan workflow
  is additive and not one of the seven required-green workflows, so it is marked
  optional to avoid changing the existing merge-gate contract. See [D-10](#d-10).
- **Coverage gate deferred behind backfill/hardening.** Rather than enabling the
  80% gate immediately, it is sequenced after P2d/P2e raise coverage — a
  deviation from a literal "add the gate now" reading, justified as a logical
  dependency. See [D-06](#d-06).

### 4.2 Assumptions

- **Toolchain availability.** Google Benchmark 1.9.5 resolves from the pinned
  nixpkgs as `gbenchmark`, and `llvm-cov` / `llvm-profdata` are provided by the
  Clang 19 toolchain with no additional runtime dependency. See [D-01](#d-01),
  [D-02](#d-02).
- **Parallel integration.** Sibling work items land the files referenced here
  (for example `../src/core/graph.hpp` and `./observability/README.md`); the
  relative links in this log are written to resolve in the integrated tree.
- **Pointer stability.** `DevAddr` operands remain stable across graph replays;
  this is the correctness invariant underpinning graph capture. See
  [D-03](#d-03).
- **Export contract.** The `haze*` version-script wildcard remains the sole
  export contract, so filling the already-exported stubs needs no change to
  `linker/haze_exports.{map,sym}`. See [D-11](#d-11).
