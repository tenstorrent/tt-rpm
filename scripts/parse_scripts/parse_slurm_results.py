#!/usr/bin/env python3

# SPDX-FileCopyrightText: © 2026 Tenstorrent AI ULC
#
# SPDX-License-Identifier: Apache-2.0

"""Aggregate RPM Slurm study results into CSVs.

Given a study directory produced by scripts/run_scripts/slurm/run.py, this
walks every `<exp>/<benchmark>/*-log.bz2`, parses retire / setup stats from
the compressed log stream, writes a per-(experiment, benchmark) stats.csv,
and a study-wide summary.csv at the root.

Usage:
    parse_slurm_results.py <study_dir>
"""
from __future__ import annotations

import argparse
import bz2
import csv
import io
import sys
from pathlib import Path

# Allow "from parse_stats import extract_stats" regardless of invocation cwd.
sys.path.insert(0, str(Path(__file__).resolve().parent))
from parse_stats import extract_stats  # noqa: E402


SUMMARY_COLUMNS = [
    "experiment",
    "benchmark",
    "workload",
    "total_cycles",
    "total_instructions",
    "ipc",
    "retire_samples",
    "setup_time_s",
    "log_file",
]


def iter_log_lines(log_path: Path):
    """Stream a possibly-bzipped log file as text lines."""
    if log_path.suffix == ".bz2":
        fh = bz2.open(log_path, "rb")
    else:
        fh = open(log_path, "rb")
    try:
        for raw in io.TextIOWrapper(fh, encoding="utf-8", errors="replace"):
            yield raw
    finally:
        fh.close()


def workload_name_from_log(log_path: Path) -> str:
    """'foo-log.bz2' -> 'foo'. Drops a trailing '-log' if present."""
    name = log_path.name
    if name.endswith(".bz2"):
        name = name[: -len(".bz2")]
    if name.endswith(".log"):
        name = name[: -len(".log")]
    if name.endswith("-log"):
        name = name[: -len("-log")]
    return name


def parse_one_log(log_path: Path) -> dict:
    return extract_stats(iter_log_lines(log_path))


def discover_logs(study_dir: Path):
    """Yield (experiment, benchmark, log_path) for every *-log.bz2 in the study.

    Layout produced by run.py:
        <study>/<experiment>/<benchmark>/<workload>-log.bz2
    """
    for exp_dir in sorted(p for p in study_dir.iterdir() if p.is_dir()):
        # Skip per-experiment scratch like 'binary' containing no bench dirs.
        for bench_dir in sorted(p for p in exp_dir.iterdir() if p.is_dir()):
            logs = sorted(bench_dir.glob("*-log.bz2")) + sorted(bench_dir.glob("*-log"))
            for log_path in logs:
                yield exp_dir.name, bench_dir.name, log_path


def write_bench_csv(rows: list[dict], out_path: Path) -> None:
    out_path.parent.mkdir(parents=True, exist_ok=True)
    with open(out_path, "w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=SUMMARY_COLUMNS)
        writer.writeheader()
        for row in rows:
            writer.writerow({k: row.get(k, "") for k in SUMMARY_COLUMNS})


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("study_dir", type=Path, help="Top-level study directory")
    args = ap.parse_args()

    study_dir: Path = args.study_dir.resolve()
    if not study_dir.is_dir():
        print(f"error: {study_dir} is not a directory", file=sys.stderr)
        return 2

    grouped: dict[tuple[str, str], list[dict]] = {}
    all_rows: list[dict] = []

    for exp_name, bench_name, log_path in discover_logs(study_dir):
        stats = parse_one_log(log_path)
        row = {
            "experiment": exp_name,
            "benchmark": bench_name,
            "workload": workload_name_from_log(log_path),
            "total_cycles": stats["total_cycles"],
            "total_instructions": stats["total_instructions"],
            "ipc": f"{stats['ipc']:.6f}",
            "retire_samples": stats["retire_samples"],
            "setup_time_s": f"{stats['setup_time_s']:.2f}",
            "log_file": str(log_path.relative_to(study_dir)),
        }
        grouped.setdefault((exp_name, bench_name), []).append(row)
        all_rows.append(row)

    if not all_rows:
        print(f"warning: no *-log.bz2 files found under {study_dir}", file=sys.stderr)
        return 1

    for (exp, bench), rows in grouped.items():
        write_bench_csv(rows, study_dir / exp / bench / "stats.csv")

    summary = study_dir / "summary.csv"
    write_bench_csv(all_rows, summary)
    print(f"parsed {len(all_rows)} log(s); summary at {summary}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
