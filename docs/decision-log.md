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
| <a id="d-13"></a>D-13 | **Decouple reveal.js initialization from the Mermaid CDN load** so the deck comes up styled even when the diagram CDN is unreachable, and correct the earlier "opens offline" claim. | Keep the static top-level `import mermaid …` (any CDN miss aborts the whole module and reveal never initializes, leaving an unstyled vertical dump); bundle/inline Mermaid to make the deck truly offline (breaks the Rule 3 pinned-CDN mandate). | reveal.js and Lucide load via classic `<script>` tags and initialize first; Mermaid is then loaded with a dynamic `import()` inside `try/catch` after reveal init, so a diagram-CDN failure is confined and degrades to per-node captioned fallbacks instead of blanking the deck. The pinned Mermaid 11.4.0 URL is retained, honoring Rule 3's pinned-CDN mandate; the inaccurate "opens offline" wording is corrected to "requires network for pinned CDN assets; degrades gracefully offline". | Risk: diagrams are unavailable with no network. Mitigation: each `.mermaid` node is replaced with a styled fallback caption and stays hidden until processed, so no raw source flashes and no slide is blank; the deck remains fully navigable. | Rule 3 |
| <a id="d-14"></a>D-14 | Render the slide-10 gauge center label and the slide-11 progress labels as **HTML (via `<foreignObject>` / flanking `<div>`s) rather than SVG `<text>`**. | Keep SVG `<text>` with `dominant-baseline`; only convert the unitless font-size to `px`. | SVG `<text>` mis-paints under reveal.js fractional transform scaling when the text is gradient-filled or sits in an extreme-aspect viewBox — the glyphs rasterize near their unscaled size and drift or clip (observed at narrow breakpoints). HTML inside `<foreignObject>` scales with the SVG viewBox exactly like the vector shapes, and a gradient applied via `background-clip:text` rasterizes correctly, so the label stays centered and crisp at every breakpoint. | Risk: `<foreignObject>` / `background-clip:text` are comparatively less common. Mitigation: both are well-supported in evergreen browsers; the result was verified by screenshot across 1920/1366/768/390 widths, since `getBoundingClientRect` reported correct geometry even while the raster was wrong. | Rule 3 |
| <a id="d-15"></a>D-15 | Force reveal.js **scroll view onto the dark brand theme and disable its narrow-width auto-activation** (`scrollActivationWidth: null`). | Leave reveal.js defaults (scroll view auto-activates below ~435px with a white background); restyle only after detecting scroll view at runtime. | On a fresh narrow load reveal.js can open in scroll view, whose layers default to white in `reveal.css` and render the light brand text near-invisible (a contrast failure). Setting `scrollActivationWidth: null` keeps the deck in letterboxed slide view at every width, and dark-theme overrides on `.reveal-viewport` / `.scroll-page` / `.scroll-page-content` guarantee the brand background and legible text if scroll view is ever entered. | Risk: an additive config key plus a few `!important` overrides. Mitigation: the key is purely additive and the mandated config keys are untouched; contrast was re-verified via a Lighthouse audit with color-contrast passing. | Rule 3 |
| <a id="d-16"></a>D-16 | **Emit each `log_error` diagnostic as a single composed line written with one `std::fwrite` to `stderr`** (line-atomic), refining the correlation-ID logging recorded under [D-04](#d-04). | Perform several independent writes/insertions per record (one per field); serialize every emission behind a process-global `std::mutex`; use `std::osyncstream`; switch to `std::print` / `std::format`. | Composing the whole `[haze] [cid=<id>] <tag>: <body>\n` line into one `std::string` and emitting it with a single `std::fwrite(record.data(), 1, record.size(), stderr)` maps to one underlying stdio write, so records do not interleave under concurrent multi-threaded logging — satisfying the structured/parseable-diagnostics goal of Rule 2 and the checkpoint's "line integrity, concurrency isolation" property. Line integrity is additionally guaranteed deterministically by `append_escaped`, which replaces every newline, C0/C1 control, DEL, and non-ASCII byte in the tag and body with a printable `\xNN` (or `\\`) escape, so no field can inject a framing break; this escaping invariant is what the executable evidence in `test/test_documented_noops.cpp` verifies rather than a timing-dependent thread race. A global mutex adds lock-ordering surface and contention to a `noexcept` sink; `std::osyncstream` / `std::print` / `std::format` add include and allocation churn — so the single-`fwrite` approach fixes the defect with the smallest blast radius. | Risk: composing a `std::string` allocates, and an allocation failure inside a `noexcept` function would call `std::terminate`. Mitigation: the composition and single-shot write sit inside a `try/catch (...)` whose handler emits a fixed, allocation-free generic notice via `std::fputs("[haze] log sink dropped a record (formatting or write failure)\n", stderr)` — a dropped-record notice, not a re-emission of the formatted record — so `noexcept` holds and no exception can cross the C ABI boundary. The added `<string>` include is a standard-library header only; no new symbol is exported, so the symbol-leak audit is unaffected. | Rule 2 (refines [D-04](#d-04)) |
| <a id="d-17"></a>D-17 | **Defer `test/test_allocator_limits.cpp` to a subsequent PR** rather than creating it in this test-specification checkpoint. | Author a placeholder allocator-limits TU now; fold its cases into `test/test_error_paths.cpp`. | The file was not part of the processed checkpoint and does not exist in the tree; per the per-PR delivery model (AAP §0.8.1) the P2e hardening is delivered incrementally, and the pool-exhaustion / zero / oversized boundary cases depend on allocator introspection best exercised alongside their own PR. Crediting a non-existent file in the traceability matrix would be a false coverage claim (the defect R3 flags), so it is instead recorded explicitly as deferred in both matrices. | Risk: the deferral could be read as dropped scope. Mitigation: it is tracked as an explicit deferral in §3.1/§3.2/§3.3 and called out as human follow-up in the PR description; the remaining eight hardening categories are delivered this checkpoint. | P2e (deferred) |
| <a id="d-18"></a>D-18 | Deliver the **epoch/allocator lock-order concurrency test inside `test/test_error_paths.cpp` behind the hidden `[.][concurrency]` tag**, and scope it to independent per-thread allocation churn. | Put it in the deferred `test_allocator_limits.cpp`; drive concurrent op-recording through `compute`/`flush`; omit a lock-order test entirely (the R3 gap). | A concurrency test addressing the epoch->allocator lock-order surface (CWE-667/833) is delivered now so the hardening categories are not left without one. It uses a `std::barrier` to release eight threads simultaneously, each performing malloc->memset->free on thread-owned pointers, maximizing allocator-lock contention under TSan without violating the single-writer record-and-replay invariant. Deeper *nested* epoch->allocator ordering through concurrent op recording is deferred, because record-and-replay is single-writer by design and cannot record ops from multiple threads concurrently. The case sits behind `[.]` so it is opt-in (run under the sanitizer job) and does not perturb the default suite. | Risk: the test cannot exercise concurrent op recording. Mitigation: the single-writer limitation is documented here as a deviation from a literal "epoch/allocator lock-order under concurrency" reading; the delivered test still exercises the allocator lock under contention. | P2e |
| <a id="d-19"></a>D-19 | **Smoke-test the three documented no-ops** (`hazeStreamSynchronize`, `hazeStreamWaitEvent`, `hazeDeviceSynchronize`) for `HAZE_SUCCESS` and, in addition, assert their **exclusion from telemetry** with executable evidence. | Assert only the return code; instrument the no-ops (prohibited by AAP §0.3.2). | The AAP forbids changing no-op behavior, so the tests assert `HAZE_SUCCESS` without modifying them; to satisfy the Observability requirement that the no-ops stay *uninstrumented*, `test/test_documented_noops.cpp` snapshots all seven performance counters and the thread-local correlation ID before and after the three calls and asserts every value is unchanged — turning "not instrumented" into a checked invariant rather than a claim. | Risk: none — the functions are unchanged. Mitigation: the evidence is read-only observation of the public counter surface and the correlation getter. | P2d, Rule 2 |
| <a id="d-20"></a>D-20 | Prove **no C++ exception crosses the `noexcept` C ABI boundary using a fork-subprocess death-test** in `test/test_error_semantics.cpp`. | `REQUIRE_NOTHROW` around the ABI calls (cannot observe a `noexcept` violation — the process has already `std::terminate`d); add a throw-injection seam to the sink. | `errors.hpp` exposes no throw-injection seam, and a throw escaping a `noexcept` function calls `std::terminate` before any in-process matcher can react, so `REQUIRE_NOTHROW` gives false assurance (the ES1 defect). The child process drives eight pathological `noexcept`-ABI calls with pinned error codes and `_exit(rc)`; the parent asserts `WIFEXITED` and `WEXITSTATUS==0`, so a `std::terminate` would surface as an abnormal child exit and fail the test deterministically. | Risk: fork-based tests are POSIX-only. Mitigation: the supported platforms are all POSIX (x86_64/aarch64 Linux, aarch64 Darwin); the wait-status macros are annotated with the repo-standard `// NOLINTNEXTLINE(misc-include-cleaner)` precedent. | P2e |
| <a id="d-21"></a>D-21 | Write `test/test_config_errors.cpp` against the **actual `Config::set_modulus` sparse-write/contiguity + post-configure-immutability contract**, deviating from the review's suggested "assert index 64 is rejected after populating 0–63". | Implement the reviewer's literal suggestion (assert a hard 64-index cap). | An empirical probe proved `Config::set_modulus` enforces only `idx>=0`, `modulus!=0`, contiguity (`idx>size` gap -> `INVALID_VALUE`; `idx==size` append; `idx<size` overwrite), and post-`configureDevice` immutability (a differing value or new index -> `NotConfigured`/`CONFIGERR`); it has **no** hard 64-index cap. Filling slots 0–63 and then writing index 64 (and 65) *succeeds*. The `kMaxCiphertextModuli=64` limit applies to MRP *allocation count*, not to `set_modulus`. Asserting a nonexistent cap would test a false contract, so the test exercises the real one. | Risk: the test diverges from the review's literal wording. Mitigation: this deviation is logged per Rule 1 with the probe evidence; the real contiguity/immutability contract is exercised across eleven cases, which is stronger than the single-bound assertion originally suggested. | P2e |

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
| P2e | Error-path and edge-case hardening | `CMakeLists.txt` (test registration), `.github/workflows/sanitizers.yml` | Delivered this checkpoint: `test/test_error_paths.cpp` (includes the epoch/allocator lock-order concurrency test under the `[.][concurrency]` tag — see [D-18](#d-18)), `test/test_unflushed_reads.cpp`, `test/test_error_semantics.cpp`, `test/test_config_errors.cpp`. Deferred: `test/test_allocator_limits.cpp` (not created this checkpoint — see [D-17](#d-17)). Sanitizer runs (ASan/UBSan/TSan) execute in CI once the files are registered. |
| P3a | Complete CONTRIBUTING with a marked CLA placeholder | `CONTRIBUTING.md` | Markdown review; internal and external links resolve |
| P3b | `docs/` + `examples/`; keep docs-as-tests green | `docs/index.md`, `docs/architecture.md`, `docs/building.md`, `docs/testing.md`, `examples/quickstart.c`, `examples/ckks22.cpp`, `examples/README.md`, `examples/CMakeLists.txt`, `README.md`, `scripts/test_readme_examples.sh` | `scripts/test_readme_examples.sh` (docs-as-tests CI) |
| Rule 1 | Explainability: decision log + traceability | `docs/decision-log.md` | This document; the coverage assertion in [section 3.3](#33-coverage-assertion) |
| Rule 2 | Observability (library-context reinterpretation) | `src/common/log.hpp`, `src/common/log.cpp`, `docs/observability/dashboard-template.json`, `docs/observability/README.md` | `hazeGetPerformanceCounters` query surface; locally-verified structured logs and dashboard template |
| Rule 3 | Executive presentation | `docs/presentation/executive-summary.html` | Single self-contained reveal.js deck (one HTML file); pinned CDN assets require network and degrade to captioned fallbacks offline (see [D-13](#d-13)); 12–18 slides (target 16) |

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
| `src/common/log.hpp`, `src/common/log.cpp` | Rule 2 | Structured logging with correlation IDs (see [D-04](#d-04)); line-atomic single-insertion emission (see [D-16](#d-16)). |
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
| `test/test_error_paths.cpp`, `test/test_unflushed_reads.cpp`, `test/test_error_semantics.cpp`, `test/test_config_errors.cpp` | P2e | Negative and boundary tests across eight of the nine hardening categories; `test_error_paths.cpp` also carries the epoch/allocator lock-order concurrency test behind the hidden `[.][concurrency]` tag (see [D-18](#d-18)). |
| `test/test_allocator_limits.cpp` | P2e (deferred) | Allocator pool-exhaustion / zero / oversized boundary tests — **not created this checkpoint**; deferred to a subsequent PR (see [D-17](#d-17)). |
| `CONTRIBUTING.md` | P3a | Full contributor guidelines with a clearly marked CLA placeholder. |
| `README.md` | P3b | Add graph/peer/counter status, benchmarking, coverage, and observability sections; link `docs/` and `examples/`. |
| `docs/index.md`, `docs/architecture.md`, `docs/building.md`, `docs/testing.md` | P3b | New documentation tree. |
| `examples/quickstart.c`, `examples/ckks22.cpp`, `examples/README.md`, `examples/CMakeLists.txt` | P3b | Migrated runnable examples mirrored from the README markers. |
| `docs/decision-log.md` | Rule 1 | This file — the single source of truth for rationale. |
| `docs/observability/dashboard-template.json`, `docs/observability/README.md` | Rule 2 | Dashboard template and the counters -> metrics / health / tracing mapping. |
| `docs/presentation/executive-summary.html` | Rule 3 | Self-contained reveal.js executive deck (see [D-12](#d-12)). |
| `src/api/stream.cpp` (`hazeStreamSynchronize`, `hazeStreamWaitEvent`); `src/api/device.cpp` (`hazeDeviceSynchronize`) | Out of scope | The three documented no-ops are correct-by-design and intentionally NOT modified; behavior is unchanged and only smoke-tested (in `test/test_documented_noops.cpp`). |

### 3.3 Coverage assertion

**Scope of this assertion.** The forward and backward matrices above are
**plan-level** maps: every Agent Action Plan work item (P1a, P1b, P2a, P2b, P2c,
P2d, P2e, P3a, P3b) and every rule (Rules 1–3) appears in the forward matrix,
and every planned in-scope file group appears in the backward matrix. That
plan-level mapping is complete; it is **not** a claim that every work item is
*delivered* in this checkpoint. Per the per-PR delivery model (AAP §0.8.1), this
checkpoint is a **test-specification milestone**: it lands the Catch2
translation units as source-ready specifications, while the runtime, build, and
CI work they will eventually exercise is delivered incrementally in later PRs.
The earlier unqualified "100%-covering" wording is withdrawn because it did not
distinguish plan coverage from delivered coverage and is not true at the
semantic-requirement (per-test-case) level; §3.4 gives the delivered
per-test-case detail.

**Delivered in this checkpoint:** the thirteen new `test/*.cpp` translation
units plus the byte-exact revert of `test/test_build.cpp` and `CMakeLists.txt`
to the parent baseline. **Deferred to subsequent PRs** (each tracked by a
decision entry): the graph/peer/counter *runtime* (P1a/P2a/P2b implementation
files such as `src/core/graph.*` and `src/core/metrics.*`, which do not yet
exist), the benchmark harness and its gate (P1b), the coverage instrumentation
and 80% gate (P2c), the new CI workflows, and the `test/test_allocator_limits.cpp`
translation unit (P2e). None of these deferrals are silently credited above:
`test_allocator_limits.cpp` is explicitly marked *deferred* in both matrices,
and the runtime files are listed as *targets* rather than as delivered artifacts.

**Intentional exclusions.** The three documented no-ops
(`hazeStreamSynchronize`, `hazeStreamWaitEvent`, `hazeDeviceSynchronize`) are the
only intentionally excluded *functions* and are recorded above as out-of-scope,
smoke-tested only (see [D-19](#d-19)).

### 3.4 Delivered per-test-case coverage (test-specification checkpoint)

This section records the requirement/test-case granularity R3 requires for the
translation units actually delivered in this checkpoint. Each row names the
specific semantic requirement each file exercises so coverage can be audited at
the test-case level rather than only at the file level.

| Delivered test file | Semantic requirements exercised (per-case intent) |
|---------------------|---------------------------------------------------|
| `test/test_graph_capture.cpp` | Begin/end-capture state machine (nested-begin rejected `INVALID_VALUE`; end-without-begin `INVALID_VALUE` distinct from empty-capture `SOURCE_UNAVAILABLE`); instantiate/launch/update/destroy lifetime; same-topology update relaunches and changes results; genuine topology-mismatch rejection; exhaustive per-arg null/invalid/destroyed-handle validation with output-zeroing, last-error, and state recovery. Asserts the **final implemented** contract (runtime deferred). |
| `test/test_peer_access.cpp` | query -> enable -> authorization contract; disabled/enabled states; negative and out-of-range device/copy ordinals; simulator peer copy (tag/materialize/D2H/byte-compare); hardware readback behind the hidden `[.]` tag. Asserts the **final implemented** contract (runtime deferred). |
| `test/test_performance_counters.cpp` | Exact before/after deltas per op (not `>=`); one-flush `flush_count==1` and `total==last`; two-flush exact totals; `offsetof` layout assertions for all seven fields; dirty-then-reset; failed-op no-delta; `hazeWriteProgram` no-flush. Asserts the **final implemented** contract (runtime deferred). |
| `test/test_stream_event_lifecycle.cpp` | Nonzero-flag contract; direct null-output rejection for `*WithPriority`/`*WithFlags`; null-handle destroy/record. |
| `test/test_async_ops.cpp` | Default-stream malloc/free/memset; invalid source/kind/size paths; allocator-only `[unit]` retag. |
| `test/test_device_api.cpp` | `GetDeviceCount`/`SetDevice`/`GetDevice`/`GetDeviceProperties`; negative-device properties output + last-error. |
| `test/test_host_memory.cpp` | `HostAlloc`/`FreeHost` acceptance contract; safe negative paths; post-free reclassification. |
| `test/test_introspection.cpp` | `PointerGetAttributes` exact fields/device association; null and post-free classification; MRP device-classification. |
| `test/test_documented_noops.cpp` | The three no-ops return `HAZE_SUCCESS` without behavior change; **executable observability evidence** — all seven counters and the correlation ID are snapshotted before/after the no-ops and asserted unchanged, and a composed log line is asserted single-line and escaped (Rule 2, see [D-16](#d-16)). |
| `test/test_error_paths.cpp` | Null/invalid handles, use-after-free, double-free, wrong allocator with correctly-sized buffers and a sentinel-unchanged assertion; the epoch/allocator lock-order concurrency test under `[.][concurrency]` (see [D-18](#d-18)). |
| `test/test_unflushed_reads.cpp` | `NOT_FLUSHED` before flush; correct readback after tag+flush; device-vs-shadow pointer classification. |
| `test/test_error_semantics.cpp` | Explicit 18-entry `{internal, expected_public}` mapping asserted for exact equality; a fork-subprocess death-test proving no C++ exception crosses the `noexcept` C ABI boundary (see [D-20](#d-20)). |
| `test/test_config_errors.cpp` | The **actual** sparse-write/contiguity + post-configure-immutability contract of `Config::set_modulus` (see [D-21](#d-21)); ring-dim/device configuration errors. |


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
- **"Opens offline" claim corrected; CDN load made resilient.** The traceability
  matrix previously described the deck as opening offline, but it references
  pinned CDN assets (reveal.js, Lucide, Mermaid) and therefore requires network
  for full fidelity. The claim is corrected to state the deck is a single
  self-contained file that requires network for its pinned CDN assets, and
  reveal.js initialization is decoupled from the Mermaid CDN so a diagram-CDN
  outage degrades to captioned fallbacks rather than blanking the deck. See
  [D-13](#d-13).
- **Diagnostic lines are composed then emitted with one `std::fwrite`.** Rather
  than performing several independent per-field writes (which would interleave
  under concurrent multi-threaded logging), each record is composed into a
  single `std::string` and written with one `std::fwrite(..., stderr)` to
  satisfy the line-integrity / concurrency-isolation property of Rule 2. Field
  contents are neutralized deterministically by `append_escaped` (newlines,
  control bytes, and non-ASCII rendered as `\xNN`), so framing integrity does
  not depend on a timing-sensitive thread race. On any formatting/write failure
  the `catch (...)` handler emits a fixed, allocation-free generic dropped-record
  notice via `std::fputs` — not a re-emission of the record — preserving
  `noexcept`. See [D-16](#d-16).
- **Peer access is simulator-only and validates device ordinals.** Physical
  multi-chip peer transfer needs hardware absent from CI, so only the
  simulator-representable query -> enable -> copy path is implemented, and the
  peer entry points validate device ordinals (rejecting negative and
  out-of-range indices) rather than accepting them unchecked as the superseded
  comment implied. Hardware-only assertions sit behind a hidden tag and the
  multi-chip step is flagged as human follow-up. See [D-09](#d-09).
- **`test/test_config_errors.cpp` exercises the real `set_modulus` contract, not a
  nonexistent index cap.** A literal reading of the review suggested asserting
  that index 64 is rejected after populating 0–63; an empirical probe proved
  `Config::set_modulus` has no hard 64-index cap (writing 64 and 65 succeeds
  after a contiguous fill), so the test exercises the actual sparse-write /
  contiguity / post-configure-immutability contract instead. See [D-21](#d-21).
- **`test/test_allocator_limits.cpp` is deferred.** The allocator pool-exhaustion
  / zero / oversized boundary TU was not part of the processed checkpoint and is
  deferred to a subsequent PR rather than credited as delivered. See
  [D-17](#d-17).
- **The lock-order concurrency test is scoped to independent per-thread
  allocation churn.** A literal "epoch/allocator lock-order under concurrent op
  recording" test is not expressible because record-and-replay is single-writer,
  so the delivered `[.][concurrency]` test in `test/test_error_paths.cpp`
  exercises the allocator lock under eight-thread contention while the nested
  epoch-path ordering is documented as deferred. See [D-18](#d-18).
- **The no-exception-across-the-ABI proof uses a fork death-test.** Because a
  throw escaping a `noexcept` function terminates the process before any
  in-process matcher can observe it, `REQUIRE_NOTHROW` cannot prove the property;
  a fork-subprocess death-test is used instead. See [D-20](#d-20).
- **White-box test access is used only where a public getter does not exist.**
  The public C ABI intentionally exposes no state getters, so a few tests reach
  internal `haze::` symbols (for example `haze::to_public_error` for the ES2
  mapping table, and the `AllocatorTestAccess` helper already used by the
  baseline `test/test_memory.cpp` for MRP-allocation negatives). This is a
  deliberate white-box choice for assertions that have no black-box surface;
  MRP-allocation negative cases remain owned by `test/test_memory.cpp` and are
  not duplicated in the new introspection tests.


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
