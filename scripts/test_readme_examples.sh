#!/usr/bin/env bash
# test_readme_examples.sh - extract, compile, and run the README examples.
#
# Usage:
#   scripts/test_readme_examples.sh            # extract + compile + run (the check)
#   scripts/test_readme_examples.sh --check    # alias for the default
#   scripts/test_readme_examples.sh --help
#
# Docs-as-tests: the two fenced examples in README.md (a pure-C 22-limb raw add
# and a C++ 22-limb CKKS add) are the source of truth. This script pulls each
# marked block out of README.md, compiles it against the shipped libhaze, runs
# it through the in-process FHETCH simulator (HAZE_TARGET=local), and asserts
# exit 0 plus the expected output token. It fails loudly if a marker region is
# missing or a compile/run/assert fails, so the published code cannot rot.
# It also verifies the migrated examples/quickstart.c and examples/ckks22.cpp
# match their README marker regions byte-for-byte (README is the source of
# truth), failing loudly on drift so examples/ cannot diverge from the docs.
#
# Resolves the repo root from `git rev-parse` (falls back to the script's
# `$(dirname BASH_SOURCE)/..` outside a git checkout, e.g. inside a nix
# derivation sandbox where the source tree has no .git).
#
# Environment overrides (all have defaults resolved from the repo root):
#   BUILD_DIR                build tree name (default: build).
#   HAZE_INCLUDE_DIR         public haze headers   (default: $root/include).
#   HAZE_BRIDGE_INCLUDE_DIR  replay-bridge header  (default:
#                            $root/replay_bridge/include). The C example calls
#                            hazeReplayBridgeInitCryptoContext, declared there.
#   HAZE_LIB_DIR             dir holding libhaze.*  (default: $root/$BUILD_DIR).
#   STOCK_OPENFHE_DIR        stock OpenFHE prefix   (default:
#                            $root/vendor/lib/openfhe-stock). C++ only.
#   HAZE_RUNS_DIR            writable dir the examples run in so libnbfhetch's
#                            program_dir stays out of the source root (default:
#                            $root/$BUILD_DIR/runs).
#   CC / CXX                 compilers (default: cc / c++).
#   EXAMPLES_DIR             dir holding the migrated runnable examples
#                            (default: $root/examples).

set -euo pipefail

case "${1:-}" in
    -h | --help)
        sed -n '/^# Usage:/,/^$/p' "$0" | sed 's/^# \?//'
        exit 0
        ;;
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

readme="${README:-$root/README.md}"
build_dir="${BUILD_DIR:-build}"
haze_include_dir="${HAZE_INCLUDE_DIR:-$root/include}"
haze_bridge_include_dir="${HAZE_BRIDGE_INCLUDE_DIR:-$root/replay_bridge/include}"
haze_lib_dir="${HAZE_LIB_DIR:-$root/$build_dir}"
stock_openfhe_dir="${STOCK_OPENFHE_DIR:-$root/vendor/lib/openfhe-stock}"
runs_dir="${HAZE_RUNS_DIR:-$root/$build_dir/runs}"
cc="${CC:-cc}"
cxx="${CXX:-c++}"
examples_dir="${EXAMPLES_DIR:-$root/examples}"

[[ -f "$readme" ]] || {
    printf 'error: README not found: %s\n' "$readme" >&2
    exit 1
}
[[ -d "$haze_lib_dir" ]] || {
    printf 'error: HAZE_LIB_DIR not found: %s (build libhaze first)\n' "$haze_lib_dir" >&2
    exit 1
}

scratch=$(mktemp -d "${TMPDIR:-/tmp}/readme-examples.XXXXXX")
trap 'rm -rf "$scratch"' EXIT

# Pull the fenced code of the readme-example region named $1 out of the README,
# stripping the ``` fence lines. Prints to stdout; empty output means the region
# was absent or malformed.
extract_block() {
    local name="$1"
    awk -v want="$name" '
        /^<!-- readme-example:begin / { active = (index($0, "name=" want " ") > 0); next }
        /^<!-- readme-example:end/    { if (active) exit; next }
        active && /^```/              { infence = !infence; next }
        active && infence             { print }
    ' "$readme"
}

extract_to() {
    local name="$1" out="$2"
    extract_block "$name" >"$out"
    [[ -s "$out" ]] || {
        printf 'error: README region name=%s is missing or empty\n' "$name" >&2
        exit 1
    }
}

# Emit a unified diff with human-readable labels in a portable way. diff(1)
# label support is not uniform: GNU diff accepts both --label and -L, but the
# BSD/macOS diff this script must also run under does not accept the --label
# long option. Feature-detect -L support once (the portable short spelling that
# GNU and BSD share) and fall back to a plain unified diff -- whose headers show
# the raw file paths -- when even -L is unavailable, so the mirror check reports
# drift identically everywhere instead of aborting on an unknown option.
_diff_labels_supported=""
mirror_diff() {
    local left_label="$1" right_label="$2" left_file="$3" right_file="$4"
    if [[ -z "$_diff_labels_supported" ]]; then
        if diff -u -L x -L y /dev/null /dev/null >/dev/null 2>&1; then
            _diff_labels_supported=yes
        else
            _diff_labels_supported=no
        fi
    fi
    if [[ "$_diff_labels_supported" == yes ]]; then
        diff -u -L "$left_label" -L "$right_label" "$left_file" "$right_file"
    else
        diff -u "$left_file" "$right_file"
    fi
}

# Assert a migrated examples/ file matches its authoritative README region
# byte-for-byte. The README marker regions are the source of truth; a drift
# here means examples/ fell out of sync and must be regenerated from README.
check_mirror() {
    local extracted="$1" mirror="$2" name="$3"
    if [[ ! -f "$mirror" ]]; then
        printf 'error: examples file missing: %s (must mirror README region name=%s)\n' \
            "$mirror" "$name" >&2
        exit 1
    fi
    if ! mirror_diff "README:name=$name" "$mirror" "$extracted" "$mirror"; then
        printf 'error: %s drifted from README region name=%s (README is the source of truth; regenerate examples/ from the README markers)\n' \
            "$mirror" "$name" >&2
        exit 1
    fi
}

extract_to quickstart "$scratch/quickstart.c"
extract_to ckks22 "$scratch/ckks22.cpp"

printf '[readme] verifying examples/ mirror the README regions (README is source of truth)\n'
check_mirror "$scratch/quickstart.c" "$examples_dir/quickstart.c" quickstart
check_mirror "$scratch/ckks22.cpp"   "$examples_dir/ckks22.cpp"   ckks22

printf '[readme] compiling C example (quickstart.c)\n'
"$cc" -std=c11 -O2 \
    -I"$haze_include_dir" -I"$haze_bridge_include_dir" \
    "$scratch/quickstart.c" \
    -L"$haze_lib_dir" -lhaze \
    -Wl,-rpath,"$haze_lib_dir" \
    -o "$scratch/quickstart"

printf '[readme] compiling C++ example (ckks22.cpp)\n'
# C++17 rather than the repo's C++23: the example is a portable consumer snippet.
"$cxx" -std=c++17 -O2 \
    -I"$haze_include_dir" -I"$haze_bridge_include_dir" \
    -isystem "$stock_openfhe_dir/include/openfhe" \
    -isystem "$stock_openfhe_dir/include/openfhe/core" \
    -isystem "$stock_openfhe_dir/include/openfhe/pke" \
    -isystem "$stock_openfhe_dir/include/openfhe/binfhe" \
    "$scratch/ckks22.cpp" \
    -L"$haze_lib_dir" -lhaze \
    -L"$stock_openfhe_dir/lib" -lOPENFHEpke -lOPENFHEbinfhe -lOPENFHEcore \
    -pthread \
    -Wl,-rpath,"$haze_lib_dir" -Wl,-rpath,"$stock_openfhe_dir/lib" \
    -o "$scratch/ckks22"

# Run $bin from the runs dir under the local simulator; assert exit 0 and that
# $token appears in its output. Prints elapsed wall seconds and the token line.
run_example() {
    local label="$1" bin="$2" token="$3"
    mkdir -p "$runs_dir"
    local start end out rc
    start=$(date +%s)
    if out="$(cd "$runs_dir" && HAZE_TARGET=local "$bin" 2>&1)"; then rc=0; else rc=$?; fi
    end=$(date +%s)
    if [[ $rc -ne 0 ]]; then
        printf '[readme] %s FAILED: exit %d\n' "$label" "$rc" >&2
        printf '%s\n' "$out" >&2
        return 1
    fi
    if ! grep -qF "$token" <<<"$out"; then
        printf '[readme] %s FAILED: token %s not found in output\n' "$label" "$token" >&2
        printf '%s\n' "$out" >&2
        return 1
    fi
    printf '[readme] %s passed in %ds wall — %s\n' \
        "$label" "$((end - start))" "$(grep -F "$token" <<<"$out" | tail -n1)"
}

run_example "C example" "$scratch/quickstart" "readme-c: OK"
run_example "C++ example" "$scratch/ckks22" "readme-cpp: OK"

printf '[readme] all README examples compiled and passed\n'
