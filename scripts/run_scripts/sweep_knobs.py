#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: 2026 Tenstorrent USA, Inc.

"""Smoke-test every config knob value against a single SPEC2k6 simpoint.

Each "cell" sets one knob (or a tightly-related knob group) to one value via
`-p PATH VALUE` overrides on top of a chosen base config. The run passes if
the process exits 0, retires the requested instruction count, and reports a
non-zero IPC at or below the configured `retire_width`.

Usage (--trace defaults to $RPM_SWEEP_TRACE):
  scripts/run_scripts/sweep_knobs.py --trace T             # default sweep
  scripts/run_scripts/sweep_knobs.py --instrs 50000        # bump per-cell budget
  scripts/run_scripts/sweep_knobs.py --only fetch_width    # filter cells by substring
  scripts/run_scripts/sweep_knobs.py --csv out.csv         # write a different csv

The "baseline" used for the sweep is the source config.yaml plus the overrides
in `BASELINE_OVERRIDES` below.
"""

from __future__ import annotations

import argparse
import csv
import os
import re
import subprocess
import sys
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Iterable

REPO = Path(__file__).resolve().parents[2]
SOURCE_CONFIG = REPO / "models/cpu/src/core/config.yaml"
CORE_BIN = REPO / "build/core/core"

# Baseline overrides applied on top of the source config for every cell.
BASELINE_OVERRIDES: list[tuple[str, str]] = []

# Per-knob retire_width override for the IPC ceiling check (defaults to 4).
RETIRE_WIDTH_DEFAULT = 4

SUMMARY_RE = re.compile(
    r"IPC:\s+([\d.]+).*?\(retired=(\d+),\s+cycles=(\d+)\)"
)
ILLEGAL_RE = re.compile(r"16\s+consecutive\s+illegal", re.IGNORECASE)
EXCEPTION_RE = re.compile(r"(Exception while running|CoreException::Stop|"
                           r"DEADLOCK DETECTED|ERROR:|assert.* failed)")


@dataclass
class Cell:
    """One knob value to smoke-test."""
    name: str              # short label, becomes the CSV "cell" column
    group: str             # which config section the knob belongs to
    overrides: list[tuple[str, str]] = field(default_factory=list)
    retire_width: int = RETIRE_WIDTH_DEFAULT


def kv(path: str, value) -> tuple[str, str]:
    return (path, str(value).lower() if isinstance(value, bool) else str(value))


def build_cells() -> list[Cell]:
    """Enumerate every (knob, value) cell we want to smoke-test.

    Cells are mostly single-knob; a few co-vary tightly coupled knobs
    (e.g. structural-mode icache requires its structural params; mapped
    write_port mode requires a mapping value).
    """
    cells: list[Cell] = []

    # ---------------- top.core0.params ----------------
    g = "top"
    for v in (True, False):
        cells.append(Cell(f"ooo_enabled={str(v).lower()}", g, [
            kv("top.core0.params.ooo_enabled", v),
        ]))
    for d in (1, 8, 32):
        cells.append(Cell(f"wrong_path=true,depth={d}", g, [
            kv("top.core0.params.wrong_path_enabled", True),
            kv("top.core0.params.wrong_path_max_depth", d),
        ]))
    for p in (0, 5, 15):
        cells.append(Cell(f"wrong_path=false,penalty={p}", g, [
            kv("top.core0.params.wrong_path_enabled", False),
            kv("top.core0.params.misprediction_penalty_cycles", p),
        ]))
    cells.append(Cell("bypass_queues=true",  g, [kv("top.core0.params.bypass_queues", True)]))
    cells.append(Cell("bypass_queues=false", g, [kv("top.core0.params.bypass_queues", False)]))

    # ---------------- fetch ----------------
    g = "fetch"
    for w in (4, 8, 16, 32):
        cells.append(Cell(f"fetch_width={w}", g, [kv("top.core0.fetch.params.fetch_width", w)]))
    for c in (16, 64, 256):
        cells.append(Cell(f"fetch_buffer_capacity={c}", g,
                          [kv("top.core0.fetch.params.fetch_buffer_capacity", c)]))

    # ---------------- icache ----------------
    g = "icache"
    cells.append(Cell("icache_mode=probabilistic", g,
                      [kv("top.core0.icache.params.cache_mode", "probabilistic")]))
    for hr in (0.5, 0.9, 1.0):
        cells.append(Cell(f"icache_prob_hit_rate={hr}", g, [
            kv("top.core0.icache.params.cache_mode", "probabilistic"),
            kv("top.core0.icache.params.hit_rate", hr),
        ]))
    for sz in (16, 64, 256):
        cells.append(Cell(f"icache_struct_size_kb={sz}", g, [
            kv("top.core0.icache.params.cache_mode", "structural"),
            kv("top.core0.icache.params.cache_size_kb", sz),
        ]))
    for assoc in (1, 4, 8):
        cells.append(Cell(f"icache_assoc={assoc}", g, [
            kv("top.core0.icache.params.cache_mode", "structural"),
            kv("top.core0.icache.params.cache_associativity", assoc),
        ]))
    for pol in ("lru", "plru"):
        cells.append(Cell(f"icache_replacement={pol}", g, [
            kv("top.core0.icache.params.cache_mode", "structural"),
            kv("top.core0.icache.params.replacement_policy", pol),
        ]))
    # A prefetcher needs an MSHR beyond prefetch_mshr_reserve (1 here); smaller caches run without one.
    for m in (1, 2, 8):
        cells.append(Cell(f"icache_mshr={m}", g, [kv("top.core0.icache.params.mshr_capacity", m)] +
                          ([kv("top.core0.icache.params.prefetcher_type", "none")] if m <= 1 else [])))
    for lat in (1, 10, 50):
        cells.append(Cell(f"icache_miss_lat={lat}", g, [kv("top.core0.icache.params.miss_latency", lat)]))

    # ---------------- fetch_queue / decode_queue ----------------
    for c in (1, 16, 64):
        cells.append(Cell(f"fetch_queue_capacity={c}",  "fetch_queue",
                          [kv("top.core0.fetch_queue.params.capacity", c)]))
        cells.append(Cell(f"decode_queue_capacity={c}", "decode_queue",
                          [kv("top.core0.decode_queue.params.capacity", c)]))

    # ---------------- decode latencies ----------------
    g = "decode"
    decode_defaults = {
        "lat_alu": 1, "lat_mul": 3, "lat_div": 12, "lat_branch": 1,
        "lat_load": 1, "lat_store": 1, "lat_fp": 4, "lat_vec": 4,
        "lat_fence": 1, "lat_fp_add": 3, "lat_fp_mul": 4, "lat_fp_fma": 4,
        "lat_fp_cvt": 4, "lat_fp_cmp": 2, "lat_fp_move": 2,
    }
    for k, dflt in decode_defaults.items():
        for v in {1, dflt, 30}:
            cells.append(Cell(f"{k}={v}", g, [kv(f"top.core0.decode.params.{k}", v)]))

    # ---------------- branch_predictor ----------------
    g = "bp"
    bp = "top.core0.branch_predictor.params"
    for a in (0.0, 0.5, 0.9, 1.0):
        # accuracy is an error unless the oracle is selected.
        cells.append(Cell(f"bp_accuracy={a}", g,
                          [kv(f"{bp}.predictor_type", "probabilistic"),
                           kv(f"{bp}.btb_type", "none"),
                           kv(f"{bp}.ras_type", "none"),
                           kv(f"{bp}.accuracy", a)]))

    # ---------------- rename ----------------
    g = "rename"
    for w in (1, 4, 8):
        cells.append(Cell(f"dispatch_width={w}", g,
                          [kv("top.core0.rename.params.dispatch_width", w)]))
    for n in (64, 128, 256):
        # num_phys_regs has to track issue.num_phys_regs to avoid mismatch
        cells.append(Cell(f"num_phys_regs={n}", g, [
            kv("top.core0.rename.params.num_phys_regs", n),
            kv("top.core0.issue.params.num_phys_regs", n),
        ]))
    for h in (0, 8, 32):
        cells.append(Cell(f"freelist_headroom={h}", g,
                          [kv("top.core0.rename.params.freelist_headroom", h)]))
    for c in (1, 8, 32):
        cells.append(Cell(f"max_branch_checkpoints={c}", g,
                          [kv("top.core0.rename.params.max_branch_checkpoints", c)]))
    for nb in (2, 4):
        cells.append(Cell(f"regfile_banking,banks={nb}", g, [
            kv("top.core0.rename.params.regfile_banking_enabled", True),
            kv("top.core0.rename.params.regfile_num_banks", nb),
        ]))

    # ---------------- issue ----------------
    g = "issue"
    cells.append(Cell("issue_mode=unified", g, [kv("top.core0.issue.params.mode", "unified"),
                                                kv("top.core0.issue.params.issue_ports", "[]")]))
    cells.append(Cell("issue_mode=partitioned", g, [
        kv("top.core0.issue.params.mode", "partitioned"),
        kv("top.core0.issue.params.issue_ports", "[]"),
        kv("top.core0.issue.params.scheduler_configs",
           "[int:16:ALU+Mul+Div+Branch+Fence,mem:16:Load+Store,fp:8:FpOp+VecOp]"),
    ]))
    for sel in ("age_matrix", "fifo", "random"):
        cells.append(Cell(f"issue_selection={sel}", g,
                          [kv("top.core0.issue.params.selection", sel)]))
    for r in (0, 1, 2):
        cells.append(Cell(f"regfile_read_latency={r}", g,
                          [kv("top.core0.issue.params.regfile_read_latency", r)]))
    for n in (0, 2):
        cells.append(Cell(f"div_units={n}", g,
                          [kv("top.core0.issue.params.int_div_units", n),
                           kv("top.core0.issue.params.fp_div_units", n)]))
    for c in (8, 32, 128):
        cells.append(Cell(f"issue_queue_capacity={c}", g,
                          [kv("top.core0.issue.params.issue_queue_capacity", c)]))
    for w in (1, 4, 8):
        cells.append(Cell(f"issue_width={w}", g,
                          [kv("top.core0.issue.params.issue_width", w),
                           kv("top.core0.issue.params.issue_ports", "[]")]))

    # ---------------- execute ----------------
    g = "execute"
    cells.append(Cell("execute_granularity=unified", g, [
        kv("top.core0.execute.params.granularity", "unified"),
        kv("top.core0.execute.params.unified_max_in_flight", 8),
    ]))
    cells.append(Cell("execute_granularity=typed", g, [
        kv("top.core0.execute.params.granularity", "typed"),
    ]))
    cells.append(Cell("execute_granularity=functional", g, [
        kv("top.core0.execute.params.granularity", "functional"),
    ]))
    for wp in (1, 4, 8):
        cells.append(Cell(f"write_port_count={wp}", g, [
            kv("top.core0.execute.params.write_port_mode", "unified"),
            kv("top.core0.execute.params.write_port_count", wp),
        ]))
    # Note: write_port_mode=mapped requires a mapping config; deferred for now.
    for v in (False, True):
        cells.append(Cell(f"writeback_buffer_enabled={v}", g, [
            kv("top.core0.execute.params.writeback_buffer_enabled", v),
        ]))
    for cap in (4, 16, 64):
        cells.append(Cell(f"writeback_buffer_capacity={cap}", g, [
            kv("top.core0.execute.params.writeback_buffer_enabled", True),
            kv("top.core0.execute.params.writeback_buffer_capacity", cap),
        ]))

    # ---------------- lsq ----------------
    g = "lsq"
    for c in (8, 32, 128):
        cells.append(Cell(f"load_queue_capacity={c}", g,
                          [kv("top.core0.lsq.params.load_queue_capacity", c)]))
        cells.append(Cell(f"store_queue_capacity={c}", g,
                          [kv("top.core0.lsq.params.store_queue_capacity", c)]))
    for v in (False, True):
        cells.append(Cell(f"store_forwarding={v}", g,
                          [kv("top.core0.lsq.params.store_forwarding", v)]))
    for lat in (0, 1, 4):
        cells.append(Cell(f"forwarding_latency={lat}", g, [
            kv("top.core0.lsq.params.store_forwarding", True),
            kv("top.core0.lsq.params.forwarding_latency", lat),
        ]))

    # ---------------- dcache ----------------
    g = "dcache"
    cells.append(Cell("dcache_mode=probabilistic", g,
                      [kv("top.core0.dcache.params.cache_mode", "probabilistic")]))
    for hr in (0.5, 0.9, 1.0):
        cells.append(Cell(f"dcache_prob_hit_rate={hr}", g, [
            kv("top.core0.dcache.params.cache_mode", "probabilistic"),
            kv("top.core0.dcache.params.hit_rate", hr),
        ]))
    for sz in (16, 64, 256):
        cells.append(Cell(f"dcache_struct_size_kb={sz}", g, [
            kv("top.core0.dcache.params.cache_mode", "structural"),
            kv("top.core0.dcache.params.cache_size_kb", sz),
        ]))
    for assoc in (1, 4, 8):
        cells.append(Cell(f"dcache_assoc={assoc}", g, [
            kv("top.core0.dcache.params.cache_mode", "structural"),
            kv("top.core0.dcache.params.cache_associativity", assoc),
        ]))
    for pol in ("lru", "plru"):
        cells.append(Cell(f"dcache_replacement={pol}", g, [
            kv("top.core0.dcache.params.cache_mode", "structural"),
            kv("top.core0.dcache.params.replacement_policy", pol),
        ]))
    # As for the icache: the dcache prefetch_mshr_reserve is 2.
    for m in (1, 2, 8):
        cells.append(Cell(f"dcache_mshr={m}", g, [kv("top.core0.dcache.params.mshr_capacity", m)] +
                          ([kv("top.core0.dcache.params.prefetcher_type", "none")] if m <= 2 else [])))
    for c in (1, 8, 32):
        cells.append(Cell(f"store_buffer_capacity={c}", g,
                          [kv("top.core0.dcache.params.store_buffer_capacity", c)]))
    for nb in (1, 2, 4):
        cells.append(Cell(f"dcache_num_banks={nb}", g,
                          [kv("top.core0.dcache.params.num_banks", nb)]))
    for n in (1, 2):
        cells.append(Cell(f"dcache_reads_per_bank={n}", g,
                          [kv("top.core0.dcache.params.reads_per_bank_per_cycle", n)]))
        cells.append(Cell(f"dcache_writes_per_bank={n}", g,
                          [kv("top.core0.dcache.params.writes_per_bank_per_cycle", n)]))
    for v in (False, True):
        cells.append(Cell(f"fill_occupies_write_port={v}", g,
                          [kv("top.core0.dcache.params.fill_occupies_write_port", v)]))

    # ---------------- writeback ----------------
    g = "writeback"
    for w in (1, 4, 8):
        cells.append(Cell(f"retire_width={w}", g,
                          [kv("top.core0.writeback.params.retire_width", w)],
                          retire_width=w))
    for c in (32, 128, 256):
        cells.append(Cell(f"rob_capacity={c}", g,
                          [kv("top.core0.writeback.params.rob_capacity", c)]))

    # ---------------- l2cache ----------------
    cells.append(Cell("l2cache=disabled", "l2cache", [kv("top.core0.l2cache.params.enabled", False)]))
    cells.append(Cell("l2cache=enabled",  "l2cache", [kv("top.core0.l2cache.params.enabled", True)]))

    return cells


def run_cell(cell: Cell, base_overrides: list[tuple[str, str]],
             trace: str, instrs: int, log_dir: Path, timeout: float) -> dict:
    log_path = log_dir / f"{cell.group}__{re.sub(r'[^A-Za-z0-9._=-]+', '_', cell.name)}.log"
    cmd = [str(CORE_BIN), "-c", str(SOURCE_CONFIG),
           "-i", str(instrs), "--snapshot", trace]
    for path, value in base_overrides + cell.overrides:
        cmd += ["-p", path, value]

    t0 = time.monotonic()
    try:
        proc = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout,
                              cwd=log_dir)  # failing runs write error-dump.dbg to cwd
        wall = time.monotonic() - t0
        out = (proc.stdout or "") + (proc.stderr or "")
        log_path.write_text("CMD: " + " ".join(cmd) + "\n\n" + out)
        exit_code = proc.returncode
        timed_out = False
    except subprocess.TimeoutExpired as e:
        wall = time.monotonic() - t0
        out = (e.stdout.decode("utf-8", "replace") if e.stdout else "") + \
              (e.stderr.decode("utf-8", "replace") if e.stderr else "")
        log_path.write_text("CMD: " + " ".join(cmd) + "\n\nTIMEOUT after "
                            f"{timeout}s\n\n" + out)
        exit_code = -1
        timed_out = True

    retired = cycles = 0
    ipc = 0.0
    m = SUMMARY_RE.search(out)
    if m:
        ipc = float(m.group(1))
        retired = int(m.group(2))
        cycles = int(m.group(3))

    has_illegal = bool(ILLEGAL_RE.search(out))
    has_exception = bool(EXCEPTION_RE.search(out))

    # Pass: process exited 0, retired the requested count, IPC in (0, retire_width].
    retired_ok = retired >= int(instrs * 0.999)
    ipc_ok = 0.0 < ipc <= cell.retire_width + 0.05
    passed = (exit_code == 0 and not timed_out and not has_exception
              and not has_illegal and retired_ok and ipc_ok)

    return {
        "cell": cell.name,
        "group": cell.group,
        "pass": int(passed),
        "exit_code": exit_code,
        "timed_out": int(timed_out),
        "retired": retired,
        "cycles": cycles,
        "ipc": f"{ipc:.4f}",
        "wall_s": f"{wall:.2f}",
        "has_illegal": int(has_illegal),
        "has_exception": int(has_exception),
        "log": log_path.name,
        "overrides": ";".join(f"{p}={v}" for p, v in cell.overrides),
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--trace", default=os.environ.get("RPM_SWEEP_TRACE"),
                    help="snapshot/trace to run (default: $RPM_SWEEP_TRACE)")
    ap.add_argument("--instrs", type=int, default=20000)
    ap.add_argument("--timeout", type=float, default=120.0,
                    help="per-cell timeout in seconds")
    ap.add_argument("--csv", default=str(REPO / "build/sweep_knobs/sweep_results.csv"))
    ap.add_argument("--log-dir", default=str(REPO / "build/sweep_knobs/logs"))
    ap.add_argument("--only", default=None,
                    help="substring filter on cell name (case-insensitive)")
    ap.add_argument("--group", default=None,
                    help="filter by group name (e.g. fetch, icache)")
    ap.add_argument("--list", action="store_true", help="list cells and exit")
    args = ap.parse_args()

    cells = build_cells()
    if args.only:
        s = args.only.lower()
        cells = [c for c in cells if s in c.name.lower()]
    if args.group:
        cells = [c for c in cells if c.group == args.group]

    if args.list:
        for c in cells:
            print(f"  [{c.group:10s}] {c.name}")
        print(f"\nTotal: {len(cells)} cells")
        return
    if not args.trace:
        ap.error("--trace is required when RPM_SWEEP_TRACE is unset")

    log_dir = Path(args.log_dir)
    log_dir.mkdir(parents=True, exist_ok=True)
    csv_path = Path(args.csv)

    fieldnames = ["cell", "group", "pass", "exit_code", "timed_out", "retired",
                  "cycles", "ipc", "wall_s", "has_illegal", "has_exception",
                  "log", "overrides"]
    with open(csv_path, "w", newline="") as fh:
        w = csv.DictWriter(fh, fieldnames=fieldnames)
        w.writeheader()

        n_pass = 0
        for i, cell in enumerate(cells, 1):
            print(f"[{i:3d}/{len(cells)}] {cell.group:10s} {cell.name:40s}",
                  end="", flush=True)
            r = run_cell(cell, BASELINE_OVERRIDES, args.trace, args.instrs,
                         log_dir, args.timeout)
            w.writerow(r)
            fh.flush()
            tag = "PASS" if r["pass"] else "FAIL"
            print(f"  {tag}  retired={r['retired']:>6} ipc={r['ipc']} "
                  f"wall={r['wall_s']}s exit={r['exit_code']}")
            if r["pass"]:
                n_pass += 1

    print(f"\nResults: {n_pass}/{len(cells)} passed, csv={csv_path}")
    sys.exit(0 if n_pass == len(cells) else 1)


if __name__ == "__main__":
    main()
