# Haze examples

These are the two runnable examples that ship in the top-level
[`README.md`](../README.md): a pure-C 22-limb raw add (`quickstart.c`) and a
C++ 22-limb CKKS add (`ckks22.cpp`). They live here as standalone, buildable
copies so consumers can compile and run them directly against a shipped
`libhaze`. The top-level `README.md` is the single source of truth for the
example code; the files in this directory are byte-for-byte mirrors of the
README's marked regions, and CI fails if the two ever drift (see
[Synchronization strategy](#synchronization-strategy)).

## Examples

| Example | Language | Standard | Links | Success token | Source of truth |
| --- | --- | --- | --- | --- | --- |
| `quickstart.c` | C | `c11` | `-lhaze` (no OpenFHE) | `readme-c: OK` | README region `name=quickstart` |
| `ckks22.cpp` | C++ | `c++17` | `-lhaze` + stock OpenFHE (`OPENFHEpke`, `OPENFHEbinfhe`, `OPENFHEcore`) | `readme-cpp: OK` | README region `name=ckks22` |

`quickstart.c` drives a raw 22-limb RNS add through the record-and-replay
runtime and needs only the public haze headers plus the replay-bridge header —
no OpenFHE. `ckks22.cpp` runs the same 22-limb add end-to-end through a CKKS
ciphertext, using stock OpenFHE as the encrypt/decrypt oracle, so it
additionally links the stock OpenFHE libraries. Each program exits `0` and
prints its success token on completion.

## Synchronization strategy

The top-level `README.md` marker regions are authoritative. The files in this
directory mirror them **byte-for-byte**:

- `examples/quickstart.c` mirrors the README region delimited by
  `<!-- readme-example:begin lang=c name=quickstart -->`.
- `examples/ckks22.cpp` mirrors the README region delimited by
  `<!-- readme-example:begin lang=cpp name=ckks22 -->`.

The mirror is enforced by `scripts/test_readme_examples.sh` (run via
`make test-readme`), the docs-as-tests check: it extracts each fenced block
from `README.md`, compiles and runs it under the in-process FHETCH simulator
(`HAZE_TARGET=local`), asserts exit `0` and the expected success token, and
diffs the extracted code against the corresponding file in this directory. Any
drift between the README and `examples/` fails CI.

The rationale for keeping the README authoritative while also shipping
standalone `examples/` copies is recorded in
[`docs/decision-log.md`](../docs/decision-log.md); it is not duplicated here.

## Building

The examples build as a standalone CMake project that links against an
already-built `libhaze`. Build the library first, then point the examples
project at it:

```sh
# 1. Build libhaze first (produces build/libhaze.so).
make build

# 2. Configure and build the examples against it.
cmake -S examples -B examples/build -DHAZE_LIB_DIR="$PWD/build"
cmake --build examples/build
```

`examples/CMakeLists.txt` exposes cache variables so the example project can be
pointed at non-default locations:

- `-DHAZE_LIB_DIR=<dir>` — directory holding the built `libhaze` (defaults to
  the standard build tree).
- `-DHAZE_INCLUDE_DIR=<dir>` — public haze headers (defaults to `include/`).
- `-DHAZE_BRIDGE_INCLUDE_DIR=<dir>` — replay-bridge header (defaults to
  `replay_bridge/include`).
- `-DSTOCK_OPENFHE_DIR=<dir>` — stock OpenFHE install prefix (defaults to
  `vendor/lib/openfhe-stock`).

`quickstart` compiles as C11 with `-O2`, includes `include/` and
`replay_bridge/include`, and links only `-lhaze`. `ckks22` compiles as C++17
with `-O2`, adds the stock OpenFHE headers as system includes, and links
`-lhaze` alongside `OPENFHEpke`, `OPENFHEbinfhe`, and `OPENFHEcore` plus
threads. `ckks22` is built only when stock OpenFHE is available (default
`vendor/lib/openfhe-stock`); otherwise only `quickstart` is built.

## Running

Run the built binaries under the in-process FHETCH simulator; each should exit
`0` and print its success token:

```sh
# Pure-C example: expect the line "readme-c: OK".
HAZE_TARGET=local ./examples/build/quickstart

# C++ CKKS example (requires stock OpenFHE): expect a line containing "readme-cpp: OK".
HAZE_TARGET=local ./examples/build/ckks22
```

To exercise the full docs-as-tests path in one command — extract the examples
from `README.md`, compile and run them, and verify this directory's mirror
copies — use:

```sh
make test-readme
```

## Editing these examples

Never edit the files in this directory in isolation: because they are enforced
mirrors, CI will flag any drift from `README.md`. To change an example, edit
the corresponding `README.md` marker region first, then re-mirror the updated
code into `examples/`. The top-level README stays the single source of truth.

## Formatting and lint scope

Both examples are kept `clang-format-19`-clean (with the repository
[`.clang-format`](../.clang-format)), and every Haze/replay-bridge call is
checked so a failed call fails the example loudly (the C example jumps to a
single `cleanup:` path; the C++ example throws to `main`'s catch and lets RAII
release the device groups deterministically). Running `clang-format` over either
file is a no-op.

Even so, `examples/` is intentionally **outside** the automated formatting gate
`scripts/clang-format.sh`, which globs only `src include replay_bridge test`.
These files are consumer-facing snippets whose authoritative source is the
`README.md` marker region, not first-party library code: they are held in sync
and validated by the docs-as-tests mirror check
(`scripts/test_readme_examples.sh`) — which compiles, runs, and byte-diffs them
against the README — rather than by the source lint gate. Keep both files
`clang-format`-clean when editing the README regions; the rationale for this
exclusion is recorded in [`docs/decision-log.md`](../docs/decision-log.md).
