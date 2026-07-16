#!/usr/bin/env python3
# Regression gate that compares a Google Benchmark JSON run against the
# checked-in baseline (benchmark/baseline.json) for the FHE compute ops and the
# record->flush->replay path (P1b). Self-contained: standard library only, no
# numpy/scipy. This script is the gate WIRED into .github/workflows/benchmark.yml
# (it replaced the previous inline comparator); keep the two in sync.
#
# How a run is reduced to one number per benchmark:
#   * Only real measurement rows (run_type == "iteration") are considered;
#     Google Benchmark's own aggregate rows (run_type == "aggregate", the
#     _mean/_median/_stddev/_cv suffixes emitted by --benchmark_repetitions) are
#     ignored so a run that somehow contains ONLY aggregates is treated as
#     having no measurements (and therefore fails).
#   * Benchmarks are keyed by their BASE name (the text before the first '/'),
#     so a name like "BM_HazeAdd/iterations:1000/repeats:5" and a baseline name
#     like "BM_HazeAdd/iterations:1000" refer to the same benchmark. This lets
#     the compute benchmarks add repetitions without forcing a baseline rebuild.
#   * When a benchmark has several iteration rows (e.g. ->Repetitions(N) or an
#     accidental duplicate registration), the gate compares the MEDIAN of those
#     rows. The median suppresses the per-run variance seen on shared CI hosts
#     and cannot be fooled by a duplicate "good" row appended after a "bad" one.
#
# The gate FAILS (exit 1) when any condition holds:
#   * a benchmark present in the baseline is MISSING from the current run
#     (renamed/dropped/never-ran benchmark),
#   * a current benchmark reported an error or was skipped (SkipWithError) — a
#     skipped run is not a valid measurement and must not pass the gate, or
#   * a benchmark regressed beyond the tolerance threshold, i.e.
#         median(current) > median(baseline) * (1 + threshold).
# It also fails (exit 1) when the baseline contains no measurements, and exits 2
# (argparse usage error) on a negative threshold. Extra benchmarks present only
# in the current run are reported as a warning but do not fail the gate.
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
import collections
import json
import statistics
import sys


def _load_json(path: str) -> dict:
    """Load a JSON document, exiting cleanly (code 1) on any I/O or parse error.

    A missing file or malformed JSON is a gate failure, not a crash, so callers
    get a one-line diagnostic on stderr instead of a Python traceback.
    """
    try:
        with open(path, encoding="utf-8") as handle:
            return json.load(handle)
    except FileNotFoundError:
        print(f"ERROR: benchmark JSON not found: {path}", file=sys.stderr)
        raise SystemExit(1)
    except (OSError, json.JSONDecodeError) as exc:
        print(f"ERROR: could not read benchmark JSON '{path}': {exc}", file=sys.stderr)
        raise SystemExit(1)


def load_grouped(path: str, metric: str) -> tuple[dict[str, float], set[str]]:
    """Reduce a Google Benchmark JSON file to one metric value per benchmark.

    Returns ``(medians, errored)`` where ``medians`` maps each BASE benchmark
    name to the median of ``metric`` across its non-errored iteration rows, and
    ``errored`` is the set of base names that had at least one errored/skipped
    iteration row or a row missing the requested metric. Aggregate rows and
    unnamed rows are ignored.
    """
    doc = _load_json(path)
    values: dict[str, list[float]] = collections.defaultdict(list)
    errored: set[str] = set()
    for entry in doc.get("benchmarks", []):
        if entry.get("run_type", "iteration") != "iteration":
            continue
        name = entry.get("name")
        if not name:
            continue
        base = name.split("/", 1)[0]
        if entry.get("error_occurred") or entry.get("skipped"):
            errored.add(base)
            continue
        try:
            values[base].append(float(entry[metric]))
        except (KeyError, TypeError, ValueError):
            # A measurement row without a usable metric is not trustworthy.
            errored.add(base)
    medians = {base: statistics.median(vals) for base, vals in values.items() if vals}
    return medians, errored


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(
        description="Fail on missing benchmarks, errored runs, or regressions beyond a threshold."
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

    baseline, _ = load_grouped(args.baseline, args.metric)
    current, current_errored = load_grouped(args.current, args.metric)

    if not baseline:
        print(
            f"ERROR: no benchmark measurements found in baseline '{args.baseline}'",
            file=sys.stderr,
        )
        return 1

    # A baseline benchmark is "missing" only if the current run neither measured
    # it nor attempted-and-errored it; an errored attempt is reported separately.
    current_present = set(current) | current_errored
    missing = sorted(name for name in baseline if name not in current_present)
    # Any errored/skipped current benchmark invalidates the run.
    errored = sorted(current_errored)
    # Benchmarks seen only in the current run: report but do not fail on them.
    extra = sorted(name for name in current if name not in baseline)

    regressions: list[tuple[str, float, float, float]] = []
    ok: list[tuple[str, float, float, float]] = []
    for name, base_val in baseline.items():
        if name not in current or name in current_errored:
            continue
        cur_val = current[name]
        # A zero/negative baseline can't yield a meaningful ratio; guard it.
        ratio = (cur_val / base_val) if base_val > 0.0 else float("inf")
        limit = base_val * (1.0 + args.threshold)
        row = (name, base_val, cur_val, ratio)
        if base_val > 0.0 and cur_val > limit:
            regressions.append(row)
        else:
            ok.append(row)

    # Human-readable report.
    print(
        f"benchmark regression gate  (metric={args.metric}, "
        f"threshold=+{args.threshold * 100:.0f}%, median of iteration rows)"
    )
    print(f"  baseline: {args.baseline}")
    print(f"  current : {args.current}")
    print(f"{'benchmark':44s} {'baseline':>16s} {'current':>16s} {'ratio':>8s}")
    regression_set = set(regressions)
    for name, base_val, cur_val, ratio in sorted(ok) + sorted(regressions):
        flag = "  REGRESSION" if (name, base_val, cur_val, ratio) in regression_set else ""
        print(f"{name:44s} {base_val:>16.1f} {cur_val:>16.1f} {ratio:>7.2f}x{flag}")
    if extra:
        print(f"\nWARNING: {len(extra)} benchmark(s) present only in the current run (ignored):")
        for name in extra:
            print(f"  - {name}")

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
