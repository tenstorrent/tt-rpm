#!/usr/bin/env python3

# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: 2026 Tenstorrent USA, Inc.

"""Parse RPM simulation log output and extract key performance statistics.

Usage:
    python parse_stats.py <sim.log>
    python parse_stats.py <sim.log> --csv output.csv
    python parse_stats.py <output_dir>          # looks for sim.log inside

The helpers parse_retire_lines / parse_setup_time / extract_stats can also be
imported and called with an arbitrary line iterator (see parse_slurm_results.py).
"""

import argparse
import re
import sys
from pathlib import Path
from typing import Iterable


# Per-retirement log line, only present when `-l top.core0.writeback info` is on.
RETIRE_PATTERN = re.compile(
    r"\[writeback\]\s+cycle\s+(\d+)\s+retired\s+tag=(\d+)\s+pc=(0x[0-9a-fA-F]+)"
)
# End-of-run summary line emitted unconditionally by PipelineClock::reportStats.
SUMMARY_PATTERN = re.compile(
    r"IPC:\s+([\d.]+)\s+heartbeat=[\d.]+\s+\(retired=(\d+),\s+cycles=(\d+)\)"
)
SETUP_TIME_PATTERN = re.compile(r"\[doSetupHelper\]\s+Spent:\s+([\d.]+)\s+seconds")


def extract_stats(lines: Iterable[str]) -> dict:
    """Scan a single pass over `lines` and collect retire + setup stats.

    Prefers per-retire log lines when present (gives first/last cycle bounds);
    falls back to the end-of-run summary block which is always emitted.
    """
    first_cycle = None
    last_cycle = 0
    last_tag = 0
    retire_samples = 0
    setup_time = 0.0
    summary_retired = None
    summary_cycles = None
    summary_ipc = None

    for line in lines:
        m = RETIRE_PATTERN.search(line)
        if m:
            cycle = int(m.group(1))
            tag = int(m.group(2))
            if first_cycle is None:
                first_cycle = cycle
            if cycle > last_cycle:
                last_cycle = cycle
            if tag > last_tag:
                last_tag = tag
            retire_samples += 1
            continue
        m = SUMMARY_PATTERN.search(line)
        if m:
            summary_ipc = float(m.group(1))
            summary_retired = int(m.group(2))
            summary_cycles = int(m.group(3))
            continue
        if setup_time == 0.0:
            m = SETUP_TIME_PATTERN.search(line)
            if m:
                setup_time = float(m.group(1))

    if retire_samples > 0:
        if first_cycle is None:
            first_cycle = 0
        total_cycles = last_cycle - first_cycle
        total_instructions = last_tag + 1  # tags are 0-indexed
        ipc = (total_instructions / total_cycles) if total_cycles > 0 else 0.0
    else:
        first_cycle = 0
        last_cycle = summary_cycles or 0
        total_cycles = summary_cycles or 0
        total_instructions = summary_retired or 0
        ipc = summary_ipc if summary_ipc is not None else 0.0
        last_tag = max(total_instructions - 1, 0)

    return {
        "first_cycle": first_cycle,
        "last_cycle": last_cycle,
        "last_tag": last_tag,
        "retire_samples": retire_samples,
        "total_cycles": total_cycles,
        "total_instructions": total_instructions,
        "ipc": ipc,
        "setup_time_s": setup_time,
    }


def parse_retire_lines(log_path: Path) -> dict:
    """Back-compat: extract only retire-derived stats from a file path."""
    with open(log_path) as f:
        stats = extract_stats(f)
    # preserve the pre-refactor key set
    return {k: stats[k] for k in (
        "first_cycle", "last_cycle", "last_tag", "retire_samples",
        "total_cycles", "total_instructions", "ipc",
    )}


def parse_setup_time(log_path: Path) -> float:
    """Back-compat: extract only setup time from a file path."""
    with open(log_path) as f:
        for line in f:
            m = SETUP_TIME_PATTERN.search(line)
            if m:
                return float(m.group(1))
    return 0.0


def print_stats(stats: dict, setup_time: float, log_path: Path):
    print(f"File: {log_path}")
    print(f"{'='*50}")
    print(f"  Total cycles:       {stats['total_cycles']:>15,}")
    print(f"  Total instructions: {stats['total_instructions']:>15,}")
    print(f"  IPC:                {stats['ipc']:>15.4f}")
    print(f"  Retire samples:     {stats['retire_samples']:>15,}")
    if setup_time > 0:
        print(f"  Setup time:         {setup_time:>14.2f}s")
    print()


def write_csv(stats: dict, setup_time: float, log_path: Path, csv_path: Path):
    write_header = not csv_path.exists()
    with open(csv_path, "a") as f:
        if write_header:
            f.write("log_file,total_cycles,total_instructions,ipc,retire_samples,setup_time_s\n")
        f.write(
            f"{log_path},"
            f"{stats['total_cycles']},"
            f"{stats['total_instructions']},"
            f"{stats['ipc']:.6f},"
            f"{stats['retire_samples']},"
            f"{setup_time:.2f}\n"
        )


def main():
    parser = argparse.ArgumentParser(description="Parse RPM simulation stats")
    parser.add_argument("path", help="Path to sim.log or directory containing it")
    parser.add_argument("--csv", help="Append results to a CSV file", default=None)
    args = parser.parse_args()

    p = Path(args.path)
    if p.is_dir():
        p = p / "sim.log"
    if not p.exists():
        print(f"Error: {p} not found", file=sys.stderr)
        sys.exit(1)

    stats = parse_retire_lines(p)
    setup_time = parse_setup_time(p)
    print_stats(stats, setup_time, p)

    if args.csv:
        write_csv(stats, setup_time, p, Path(args.csv))
        print(f"Appended to {args.csv}")


if __name__ == "__main__":
    main()
