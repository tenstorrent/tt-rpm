#!/usr/bin/env python3

# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: 2026 Tenstorrent USA, Inc.

"""Top-down performance breakdown of RPM core-model runs.

Reads the Sparta statistics report of a run (`--report-all FILE` in text form,
or `--report-all FILE json`, optionally bzip2-compressed as written by the
Slurm runner) and prints the top-down breakdown of every core in it:

    Retiring           uops that retired
    Bad Speculation    uops dispatched but squashed, plus dispatch slots lost
                       while recovering from a misprediction or pipeline flush
      Wasted Work
      Recovery
    Frontend Bound     slots the frontend left empty
      Latency          ... in cycles in which no uop was delivered
      Bandwidth        ... in cycles in which some uops were delivered
    Backend Bound      slots lost because a backend resource stalled dispatch
      Memory Bound     ... with a load/store queue full or a load at the ROB head
      Core Bound       ... for any other resource

All categories are fractions of the dispatch slots (cycles x dispatch width)
counted by the rename unit, and the four level-1 categories add up to 100%.

Usage:
    topdown.py report.txt
    topdown.py run_a.json run_b.json --csv topdown.csv
    topdown.py <study_dir>              # every *.json / *.json.bz2 under it
"""
from __future__ import annotations

import argparse
import bz2
import csv
import json
import re
import sys
from pathlib import Path
from typing import Optional

REPORT_GLOBS = ("*.json", "*.json.bz2")

# Supporting counters shown as misses per kilo-instruction (unit, counter).
MPKI_COUNTERS = [
    ("Branch mispredicts", "branch_predictor", "num_mispredicted"),
    ("I-cache misses", "icache", "num_misses"),
    ("D-cache misses", "dcache", "num_misses"),
    ("L2 misses", "l2cache", "num_misses"),
]

STALL_COUNTERS = [
    ("ROB full", "dispatch_stall_rob_full"),
    ("PRF full", "dispatch_stall_prf_full"),
    ("Scheduler full", "dispatch_stall_scheduler_full"),
    ("Load queue full", "dispatch_stall_load_queue_full"),
    ("Store queue full", "dispatch_stall_store_queue_full"),
    ("Checkpoints full", "dispatch_stall_checkpoint_full"),
    ("Operands not ready", "dispatch_stall_operands_not_ready"),
]

CSV_COLUMNS = [
    "report", "core", "cycles", "instructions", "ipc",
    "retiring", "bad_speculation", "bad_spec_wasted", "bad_spec_recovery",
    "frontend_bound", "frontend_latency", "frontend_bandwidth",
    "backend_bound", "backend_memory", "backend_core",
]


# ---------------------------------------------------------------------------
# report parsing
# ---------------------------------------------------------------------------

def _open_text(path: Path) -> str:
    if path.suffix == ".bz2":
        with bz2.open(path, "rt", encoding="utf-8", errors="replace") as f:
            return f.read()
    return path.read_text(encoding="utf-8", errors="replace")


def _parse_json_report(data: dict) -> dict[str, float]:
    """Flatten a Sparta JSON report into {'top.core0.rename.num_dispatched': value}."""
    flat: dict[str, float] = {}

    def walk(node: dict, prefix: str) -> None:
        for key, val in node.items():
            if key == "ordered_keys" or not isinstance(val, dict):
                continue
            if "val" in val and not isinstance(val["val"], dict):
                flat[prefix + key] = val["val"]
            else:
                walk(val, prefix + key + ".")

    stats = data.get("Statistics", data)
    for root in stats.values():
        if isinstance(root, dict):
            walk(root, "")
    return flat


_REPORT_RE = re.compile(r'^\s*Report "([^"]+)"')
_STAT_RE = re.compile(r"^\s*([A-Za-z_]\w*)\s*=\s*([-+0-9.eE]+|nan|inf)\s*$")


def _parse_text_report(text: str) -> dict[str, float]:
    """Flatten a Sparta text report into {'top.core0.rename.num_dispatched': value}."""
    flat: dict[str, float] = {}
    node: Optional[str] = None
    for line in text.splitlines():
        m = _REPORT_RE.match(line)
        if m:
            node = m.group(1)
            continue
        m = _STAT_RE.match(line)
        if m and node:
            flat[f"{node}.{m.group(1)}"] = float(m.group(2))
    return flat


def load_report(path: Path) -> dict[str, float]:
    text = _open_text(path)
    if text.lstrip().startswith("{"):
        return _parse_json_report(json.loads(text))
    return _parse_text_report(text)


# ---------------------------------------------------------------------------
# metrics
# ---------------------------------------------------------------------------

def find_cores(stats: dict[str, float]) -> list[str]:
    """Core nodes (e.g. 'top.core0') whose rename unit reports top-down slots."""
    suffix = ".rename.topdown_slots"
    return sorted(k[: -len(suffix)] for k in stats if k.endswith(suffix))


def compute_topdown(stats: dict[str, float], core: str) -> dict[str, float]:
    def get(unit: str, name: str) -> float:
        return float(stats.get(f"{core}.{unit}.{name}", 0.0))

    slots = get("rename", "topdown_slots")
    cycles = get("rename", "topdown_cycles")
    dispatched = get("rename", "num_dispatched")
    retired = get("writeback", "num_retired")
    if slots <= 0:
        raise ValueError(f"{core}: no top-down slots in report")

    wasted = max(dispatched - retired, 0.0)
    recovery = get("rename", "topdown_recovery_slots")
    frontend = get("rename", "topdown_frontend_bound_slots")
    frontend_latency = get("rename", "topdown_frontend_latency_slots")
    backend = get("rename", "topdown_backend_bound_slots")
    backend_memory = get("rename", "topdown_backend_memory_slots")

    m = {
        "cycles": cycles,
        "instructions": retired,
        "ipc": retired / cycles if cycles else 0.0,
        "dispatch_width": slots / cycles if cycles else 0.0,
        "retiring": retired / slots,
        "bad_spec_wasted": wasted / slots,
        "bad_spec_recovery": recovery / slots,
        "frontend_bound": frontend / slots,
        "frontend_latency": frontend_latency / slots,
        "frontend_bandwidth": (frontend - frontend_latency) / slots,
        "backend_bound": backend / slots,
        "backend_memory": backend_memory / slots,
        "backend_core": (backend - backend_memory) / slots,
    }
    m["bad_speculation"] = m["bad_spec_wasted"] + m["bad_spec_recovery"]

    for label, unit, name in MPKI_COUNTERS:
        key = f"{core}.{unit}.{name}"
        if key in stats and retired:
            m[f"mpki:{label}"] = 1000.0 * stats[key] / retired
    for label, name in STALL_COUNTERS:
        key = f"{core}.rename.{name}"
        if key in stats and cycles:
            m[f"stall:{label}"] = stats[key] / cycles
    return m


# ---------------------------------------------------------------------------
# output
# ---------------------------------------------------------------------------

def _bar(frac: float, width: int = 30) -> str:
    n = int(round(max(0.0, min(frac, 1.0)) * width))
    return "#" * n + "." * (width - n)


def print_topdown(report: Path, core: str, m: dict[str, float]) -> None:
    print(f"{report} [{core}]")
    print(f"  cycles={m['cycles']:,.0f}  instructions={m['instructions']:,.0f}  "
          f"IPC={m['ipc']:.3f}  dispatch_width={m['dispatch_width']:.0f}")
    rows = [
        ("Retiring", m["retiring"], 0),
        ("Bad Speculation", m["bad_speculation"], 0),
        ("Wasted Work", m["bad_spec_wasted"], 1),
        ("Recovery", m["bad_spec_recovery"], 1),
        ("Frontend Bound", m["frontend_bound"], 0),
        ("Latency", m["frontend_latency"], 1),
        ("Bandwidth", m["frontend_bandwidth"], 1),
        ("Backend Bound", m["backend_bound"], 0),
        ("Memory Bound", m["backend_memory"], 1),
        ("Core Bound", m["backend_core"], 1),
    ]
    for label, frac, level in rows:
        name = ("  " * level + label).ljust(20)
        bar = _bar(frac) if level == 0 else ""
        print(f"    {name} {100.0 * frac:6.2f}%  {bar}".rstrip())

    stalls = [(k[len("stall:"):], v) for k, v in m.items() if k.startswith("stall:") and v > 0]
    if stalls:
        print("  Dispatch stall cycles by backend resource (% of cycles):")
        for label, frac in stalls:
            print(f"    {label.ljust(20)} {100.0 * frac:6.2f}%")
    mpki = [(k[len("mpki:"):], v) for k, v in m.items() if k.startswith("mpki:")]
    if mpki:
        print("  Events per 1000 instructions:")
        for label, v in mpki:
            print(f"    {label.ljust(20)} {v:8.2f}")
    print()


def collect_reports(paths: list[Path]) -> list[Path]:
    reports: list[Path] = []
    for p in paths:
        if p.is_dir():
            found = sorted({f for g in REPORT_GLOBS for f in p.rglob(g)})
            if not found:
                print(f"warning: no *.json / *.json.bz2 reports under {p}", file=sys.stderr)
            reports.extend(found)
        elif p.exists():
            reports.append(p)
        else:
            print(f"error: {p} not found", file=sys.stderr)
            sys.exit(1)
    return reports


def main() -> int:
    ap = argparse.ArgumentParser(description="Top-down breakdown of RPM core-model runs")
    ap.add_argument("reports", nargs="+", type=Path,
                    help="Sparta report files (text or JSON, optionally .bz2) or directories to search")
    ap.add_argument("--csv", type=Path, default=None, help="Also write one row per report and core to this CSV")
    ap.add_argument("--quiet", action="store_true", help="Do not print the per-report breakdown")
    args = ap.parse_args()

    rows: list[dict] = []
    for report in collect_reports(args.reports):
        try:
            stats = load_report(report)
        except (OSError, ValueError) as e:
            print(f"warning: cannot read {report}: {e}", file=sys.stderr)
            continue
        cores = find_cores(stats)
        if not cores:
            print(f"warning: {report} has no top-down counters (rename.topdown_slots)", file=sys.stderr)
            continue
        for core in cores:
            m = compute_topdown(stats, core)
            if not args.quiet:
                print_topdown(report, core, m)
            rows.append({"report": str(report), "core": core, **{k: m[k] for k in CSV_COLUMNS[2:]}})

    if args.csv:
        with open(args.csv, "w", newline="") as f:
            writer = csv.DictWriter(f, fieldnames=CSV_COLUMNS)
            writer.writeheader()
            for row in rows:
                writer.writerow({k: (f"{v:.6f}" if isinstance(v, float) else v) for k, v in row.items()})
        print(f"wrote {len(rows)} row(s) to {args.csv}")

    return 0 if rows else 1


if __name__ == "__main__":
    sys.exit(main())
