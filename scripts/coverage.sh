#!/usr/bin/env bash
# coverage.sh - measure Clang source-based line coverage of src/core + src/api
# and enforce the project's minimum line-coverage gate.
#
# Usage:
#   scripts/coverage.sh            # run instrumented tests, report + gate
#   scripts/coverage.sh --check    # alias for the default
#   scripts/coverage.sh --html     # also render an HTML report (needs genhtml)
#   scripts/coverage.sh --help
#
# Coverage gate: the shipped runtime lives in src/core/ and src/api/; this
# script merges the profraw emitted by the coverage-instrumented haze_tests,
# exports an lcov tracefile scoped to those two directories, computes the
# aggregate line-coverage percentage, and exits non-zero if it is below the
# threshold (default 80). Requires a build configured with -DHAZE_COVERAGE=ON
# (Clang: -fprofile-instr-generate -fcoverage-mapping); llvm-profdata / llvm-cov
# ship with the Clang 19 toolchain, so no extra runtime dependency is needed.
#
# Resolves the repo root from git rev-parse (falls back to the script's
# dirname/.. outside a git checkout, e.g. inside a nix derivation sandbox
# where the source tree has no .git).
#
# Environment overrides (all have defaults resolved from the repo root):
#   BUILD_DIR            build tree name (default: build).
#   HAZE_TEST_BIN        instrumented test binary (default:
#                        $root/$BUILD_DIR/haze_tests).
#   HAZE_RUNS_DIR        writable dir the tests run in (default:
#                        $root/$BUILD_DIR/runs).
#   COVERAGE_DIR         output dir for profraw/profdata/lcov/html (default:
#                        $root/$BUILD_DIR/coverage).
#   COVERAGE_THRESHOLD   minimum line-coverage percent (default: 80).
#   COVERAGE_SCOPE       space-separated source dirs to score (default:
#                        "src/core src/api").
#   HAZE_TARGET          transport for the run (default: local simulator).
#   LLVM_PROFDATA        llvm-profdata binary (default: llvm-profdata).
#   LLVM_COV             llvm-cov binary       (default: llvm-cov).
#   GENHTML              genhtml binary for --html (default: genhtml).

set -euo pipefail

want_html=0
case "${1:-}" in
    -h | --help)
        sed -n '/^# Usage:/,/^$/p' "$0" | sed 's/^# \?//'
        exit 0
        ;;
    --html) want_html=1 ;;
    --check | "") ;;
    *)
        printf '%s: unknown argument: %s\n' "$(basename "$0")" "$1" >&2
        exit 2
        ;;
esac

if root=$(git rev-parse --show-toplevel 2>/dev/null); then
    cd "$root"
else
    root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
    cd "$root"
fi

build_dir="${BUILD_DIR:-build}"
test_bin="${HAZE_TEST_BIN:-$root/$build_dir/haze_tests}"
runs_dir="${HAZE_RUNS_DIR:-$root/$build_dir/runs}"
coverage_dir="${COVERAGE_DIR:-$root/$build_dir/coverage}"
threshold="${COVERAGE_THRESHOLD:-80}"
scope="${COVERAGE_SCOPE:-src/core src/api}"
haze_target="${HAZE_TARGET:-local}"
llvm_profdata="${LLVM_PROFDATA:-llvm-profdata}"
llvm_cov="${LLVM_COV:-llvm-cov}"
genhtml="${GENHTML:-genhtml}"

command -v "$llvm_profdata" >/dev/null 2>&1 || {
    printf 'error: %s not found (install the Clang/LLVM toolchain)\n' "$llvm_profdata" >&2
    exit 1
}
command -v "$llvm_cov" >/dev/null 2>&1 || {
    printf 'error: %s not found (install the Clang/LLVM toolchain)\n' "$llvm_cov" >&2
    exit 1
}
if [[ ! -x "$test_bin" ]]; then
    printf 'error: instrumented test binary not found: %s\n' "$test_bin" >&2
    printf '       build it first: run make coverage, or configure with -DHAZE_COVERAGE=ON and build.\n' >&2
    exit 1
fi

rm -rf "$coverage_dir"
mkdir -p "$coverage_dir" "$runs_dir"

printf '[coverage] running instrumented tests (%s)\n' "$(basename "$test_bin")"
if ! (cd "$runs_dir" && HAZE_TARGET="$haze_target" \
        LLVM_PROFILE_FILE="$coverage_dir/haze-%p.profraw" "$test_bin"); then
    printf '[coverage] warning: test binary exited non-zero; coverage may be incomplete\n' >&2
fi

shopt -s nullglob
profraw=("$coverage_dir"/*.profraw)
shopt -u nullglob
if [[ ${#profraw[@]} -eq 0 ]]; then
    printf 'error: no .profraw produced under %s -- was the build instrumented with -DHAZE_COVERAGE=ON?\n' \
        "$coverage_dir" >&2
    exit 1
fi

profdata="$coverage_dir/haze.profdata"
lcov_file="$coverage_dir/coverage.lcov"

printf '[coverage] merging %d profraw file(s)\n' "${#profraw[@]}"
"$llvm_profdata" merge -sparse "${profraw[@]}" -o "$profdata"

read -r -a scope_dirs <<<"$scope"
printf '[coverage] exporting lcov scoped to: %s\n' "$scope"
"$llvm_cov" export "$test_bin" \
    -instr-profile="$profdata" \
    -format=lcov \
    "${scope_dirs[@]}" >"$lcov_file"

pct=$(awk -F: '
    /^LF:/ { found += $2 }
    /^LH:/ { hit   += $2 }
    END { if (found == 0) printf "0.00\n"; else printf "%.2f\n", hit * 100.0 / found }
' "$lcov_file")

if [[ $want_html -eq 1 ]]; then
    if command -v "$genhtml" >/dev/null 2>&1; then
        "$genhtml" --quiet --output-directory "$coverage_dir/html" "$lcov_file"
        printf '[coverage] HTML report: %s/html/index.html\n' "$coverage_dir"
    else
        printf '[coverage] warning: %s not found; skipping HTML report\n' "$genhtml" >&2
    fi
fi

printf '[coverage] line coverage on %s: %s%% (threshold %s%%)\n' "$scope" "$pct" "$threshold"
printf '[coverage] lcov tracefile: %s\n' "$lcov_file"

if awk -v p="$pct" -v t="$threshold" 'BEGIN { exit (p + 0 >= t + 0) ? 0 : 1 }'; then
    printf '[coverage] OK: %s%% >= %s%%\n' "$pct" "$threshold"
    exit 0
else
    printf '[coverage] FAIL: line coverage %s%% is below the %s%% threshold on %s\n' \
        "$pct" "$threshold" "$scope" >&2
    exit 1
fi
