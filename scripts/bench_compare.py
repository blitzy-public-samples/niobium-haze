#!/usr/bin/env python3
# Compare a Google Benchmark JSON run against the checked-in regression baseline
# (benchmark/baseline.json) for the FHE compute ops and the record->flush->replay
# path (P1b). Self-contained: standard library only, no numpy/scipy.
#
# The gate FAILS (exit 1) when either condition holds:
#   * a benchmark present in the baseline is MISSING from the current run
#     (renamed/dropped/never-ran benchmark), or
#   * a benchmark regressed beyond the tolerance threshold, i.e.
#         current_metric > baseline_metric * (1 + threshold).
#   * a current benchmark reports an error / was skipped (SkipWithError) — a
#     skipped run is not a valid measurement and must not pass the gate.
#
# The default threshold (0.50 = +50%) tolerates the variance of shared,
# multi-tenant CI hosts (see benchmark/baseline.json -> context.load_avg);
# tighten it on a dedicated, quiet benchmark runner. Full methodology and the
# tool-choice rationale live in docs/decision-log.md.
#
# Usage:
#   scripts/bench_compare.py BASELINE_JSON CURRENT_JSON
#                            [--threshold FRACTION] [--metric cpu_time|real_time]
"""Regression gate that compares a benchmark run against the baseline."""

from __future__ import annotations

import argparse
import json
import sys


def load_iteration_rows(path: str) -> dict[str, dict]:
    """Return {benchmark_name: entry} for real iteration rows in a GB JSON file.

    Aggregate rows produced by --benchmark_repetitions (run_type == "aggregate",
    e.g. name suffixes _mean/_median/_stddev) are ignored so the gate compares
    like-for-like single measurements.
    """
    with open(path, encoding="utf-8") as handle:
        doc = json.load(handle)
    rows: dict[str, dict] = {}
    for entry in doc.get("benchmarks", []):
        if entry.get("run_type", "iteration") != "iteration":
            continue
        name = entry.get("name")
        if name:
            rows[name] = entry
    return rows


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(
        description="Fail on missing benchmarks or regressions beyond a threshold."
    )
    parser.add_argument("baseline", help="path to the checked-in baseline JSON")
    parser.add_argument("current", help="path to the current run JSON")
    parser.add_argument(
        "--threshold",
        type=float,
        default=0.50,
        help="max fractional slowdown before flagging a regression (default 0.50 = +50%%)",
    )
    parser.add_argument(
        "--metric",
        choices=("cpu_time", "real_time"),
        default="cpu_time",
        help="which timing to compare (default cpu_time)",
    )
    args = parser.parse_args(argv)

    if args.threshold < 0.0:
        parser.error("--threshold must be non-negative")

    baseline = load_iteration_rows(args.baseline)
    current = load_iteration_rows(args.current)

    if not baseline:
        print(f"ERROR: no benchmarks found in baseline '{args.baseline}'", file=sys.stderr)
        return 1

    missing = sorted(name for name in baseline if name not in current)
    errored = sorted(
        name
        for name, entry in current.items()
        if entry.get("error_occurred") or entry.get("skipped")
    )

    regressions: list[tuple[str, float, float, float]] = []
    ok: list[tuple[str, float, float, float]] = []
    for name, base_entry in baseline.items():
        cur_entry = current.get(name)
        if cur_entry is None or name in errored:
            continue
        base_val = float(base_entry[args.metric])
        cur_val = float(cur_entry[args.metric])
        # A zero/negative baseline can't yield a meaningful ratio; guard it.
        ratio = (cur_val / base_val) if base_val > 0.0 else float("inf")
        limit = base_val * (1.0 + args.threshold)
        row = (name, base_val, cur_val, ratio)
        if base_val > 0.0 and cur_val > limit:
            regressions.append(row)
        else:
            ok.append(row)

    # Human-readable report.
    print(f"benchmark regression gate  (metric={args.metric}, threshold=+{args.threshold * 100:.0f}%)")
    print(f"  baseline: {args.baseline}")
    print(f"  current : {args.current}")
    print(f"{'benchmark':44s} {'baseline':>16s} {'current':>16s} {'ratio':>8s}")
    for name, base_val, cur_val, ratio in sorted(ok) + sorted(regressions):
        flag = "  REGRESSION" if (name, base_val, cur_val, ratio) in regressions else ""
        print(f"{name:44s} {base_val:>16.1f} {cur_val:>16.1f} {ratio:>7.2f}x{flag}")

    failed = False
    if missing:
        failed = True
        print("\nFAIL: baseline benchmarks missing from the current run:", file=sys.stderr)
        for name in missing:
            print(f"  - {name}", file=sys.stderr)
    if errored:
        failed = True
        print("\nFAIL: current benchmarks reported an error / were skipped:", file=sys.stderr)
        for name in errored:
            print(f"  - {name}", file=sys.stderr)
    if regressions:
        failed = True
        print(
            f"\nFAIL: {len(regressions)} benchmark(s) regressed beyond +{args.threshold * 100:.0f}%:",
            file=sys.stderr,
        )
        for name, base_val, cur_val, ratio in sorted(regressions):
            print(f"  - {name}: {base_val:.1f} -> {cur_val:.1f} ({ratio:.2f}x)", file=sys.stderr)

    if failed:
        return 1
    print("\nPASS: no missing benchmarks, no errors, no regressions beyond threshold.")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
