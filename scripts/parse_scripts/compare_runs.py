#!/usr/bin/env python3
"""Compare statistics from two RPM simulation runs.

Usage:
    python compare_runs.py <run_a_dir> <run_b_dir>
    python compare_runs.py <run_a.log> <run_b.log>
    python compare_runs.py <run_a.log> <run_b.log> --threshold 5.0
"""

import argparse
import sys
from pathlib import Path

from parse_stats import parse_retire_lines, parse_setup_time


def pct_change(a: float, b: float) -> float:
    if a == 0:
        return 0.0 if b == 0 else float("inf")
    return ((b - a) / abs(a)) * 100.0


def compare(stats_a: dict, stats_b: dict, threshold: float):
    metrics = [
        ("Total cycles", "total_cycles", "less is better"),
        ("Total instructions", "total_instructions", "more is better"),
        ("IPC", "ipc", "more is better"),
    ]

    regressions = 0

    print(f"{'Metric':<25} {'Run A':>15} {'Run B':>15} {'Change':>10} {'Status':>10}")
    print("-" * 80)

    for label, key, direction in metrics:
        a = stats_a[key]
        b = stats_b[key]
        change = pct_change(a, b)

        is_regression = False
        if direction == "more is better" and change < -threshold:
            is_regression = True
        elif direction == "less is better" and change > threshold:
            is_regression = True

        if isinstance(a, float):
            a_str = f"{a:.4f}"
            b_str = f"{b:.4f}"
        else:
            a_str = f"{a:,}"
            b_str = f"{b:,}"

        status = ""
        if abs(change) > threshold:
            if is_regression:
                status = "REGRESS"
                regressions += 1
            else:
                status = "IMPROVED"

        print(f"{label:<25} {a_str:>15} {b_str:>15} {change:>+9.2f}% {status:>10}")

    return regressions


def main():
    parser = argparse.ArgumentParser(description="Compare two RPM simulation runs")
    parser.add_argument("run_a", help="Path to first run (log file or directory)")
    parser.add_argument("run_b", help="Path to second run (log file or directory)")
    parser.add_argument(
        "--threshold",
        type=float,
        default=2.0,
        help="Percent change threshold to flag regressions (default: 2.0)",
    )
    args = parser.parse_args()

    def resolve_log(p: str) -> Path:
        path = Path(p)
        if path.is_dir():
            path = path / "sim.log"
        if not path.exists():
            print(f"Error: {path} not found", file=sys.stderr)
            sys.exit(1)
        return path

    log_a = resolve_log(args.run_a)
    log_b = resolve_log(args.run_b)

    stats_a = parse_retire_lines(log_a)
    stats_b = parse_retire_lines(log_b)

    print(f"Run A: {log_a}")
    print(f"Run B: {log_b}")
    print(f"Threshold: {args.threshold}%")
    print()

    regressions = compare(stats_a, stats_b, args.threshold)

    print()
    if regressions > 0:
        print(f"RESULT: {regressions} regression(s) detected")
        sys.exit(1)
    else:
        print("RESULT: No regressions")


if __name__ == "__main__":
    main()
