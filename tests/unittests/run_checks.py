#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: 2026 Tenstorrent USA, Inc.

"""Minimal unit-test / perf-regression harness for the RPM core model.

For each entry it: builds the workload ELF (via the existing tests/Makefile),
runs it through `build/core/core`, parses the model's end-of-run stats block
from stderr, and asserts loose bounds recorded in baseline.json.

Two modes:
  --calibrate   run every workload, record observed metrics + suggested bounds to
                baseline.json (does not fail on metric values).
  (default)     run every workload, compare metrics against baseline.json, print
                PASS/FAIL, exit non-zero if anything fails.

Stdlib only -- no venv, no pip, no model changes.
"""

import argparse
import json
import os
import re
import subprocess
import sys

# ---------------------------------------------------------------------------
# Paths. This file lives at <repo>/tests/unittests/run_checks.py.
# ---------------------------------------------------------------------------
HERE = os.path.dirname(os.path.abspath(__file__))
TESTS_DIR = os.path.dirname(HERE)
REPO = os.path.dirname(TESTS_DIR)
BASELINE_PATH = os.path.join(HERE, "baseline.json")

DEFAULT_CORE = os.path.join(REPO, "build", "core", "core")
# The Makefile copies config.yaml next to the binary; fall back to the source tree.
CONFIG_CANDIDATES = [
    os.path.join(REPO, "build", "core", "config.yaml"),
    os.path.join(REPO, "models", "cpu", "src", "core", "config.yaml"),
]

# Wall-clock safety net for a single model run (workloads stop themselves at
# HTIF tohost; this only fires if a run hangs).
RUN_TIMEOUT_S = 1800

# ---------------------------------------------------------------------------
# Test entries. Absolute-minimal set: the two workloads already wired in
# tests/Makefile. `checks` names metrics parsed from the stats block; concrete
# bounds live in baseline.json (written by --calibrate) so we never hardcode
# numbers that depend on a specific build.
# ---------------------------------------------------------------------------
ENTRIES = [
    {
        "name": "dhrystone",
        "make_target": "dhrystone",
        "elf": os.path.join(TESTS_DIR, "build", "dhrystone.bare.elf"),
        "icount": 0,  # 0 => run to tohost
        # Last line the workload prints; instruction counts depend on the toolchain, so
        # reaching this line is the toolchain-independent proof the run completed.
        "done_marker": "Dhrystones per Second:",
        # metric -> check kind. `gt` uses a min bound.
        "checks": {"ipc": "gt", "bp_acc": "gt"},
    },
    {
        "name": "coremark",
        "make_target": "coremark",
        "elf": os.path.join(TESTS_DIR, "build", "coremark.bare.elf"),
        "icount": 0,
        "done_marker": "Correct operation validated.",
        "checks": {"ipc": "gt", "bp_acc": "gt"},
    },
]

# Calibration margins: how baseline.json bounds are derived from observed values.
GT_MARGIN = {"ipc": 0.80, "bp_acc": 0.90}  # min bound = margin * observed


# ---------------------------------------------------------------------------
# Stats extraction: parse the model's end-of-run block from stderr.
# Format emitted by models/cpu/src/core/Pipeline.cpp:reportStats().
# ---------------------------------------------------------------------------
_HEADER = "=== Pipeline Stats @ cycle"
_PATTERNS = {
    "ipc": re.compile(r"IPC:\s+([\d.]+)"),
    "retired": re.compile(r"\(retired=(\d+),\s*cycles=(\d+)\)"),
    "ic_hit_rate": re.compile(r"I-Cache:.*hit_rate=([\d.]+)%"),
    "dc_hit_rate": re.compile(r"D-Cache:.*hit_rate=([\d.]+)%"),
    "l2_hit_rate": re.compile(r"L2-Cache:.*hit_rate=([\d.]+)%"),
    "bp_acc": re.compile(r"BP:.*accuracy=([\d.]+)%"),
}


def parse_stats(stderr):
    """Return a dict of metrics from the LAST stats block, or None if absent.

    The model prints a heartbeat block every kReportInterval cycles; the final
    block (printed at termination) is cumulative, so we keep only the last one.
    """
    idx = stderr.rfind(_HEADER)
    if idx == -1:
        return None
    block = stderr[idx:]

    stats = {}
    m = _PATTERNS["ipc"].search(block)
    if m:
        stats["ipc"] = float(m.group(1))
    m = _PATTERNS["retired"].search(block)
    if m:
        stats["retired"] = int(m.group(1))
        stats["cycles"] = int(m.group(2))
    for key in ("ic_hit_rate", "dc_hit_rate", "l2_hit_rate", "bp_acc"):
        m = _PATTERNS[key].search(block)
        if m:
            stats[key] = float(m.group(1))
    return stats


# ---------------------------------------------------------------------------
# Build + run.
# ---------------------------------------------------------------------------
def find_config():
    for c in CONFIG_CANDIDATES:
        if os.path.isfile(c):
            return c
    return None


def build(entry):
    """Build the workload ELF via the existing tests/Makefile."""
    subprocess.run(
        ["make", "-C", TESTS_DIR, entry["make_target"]],
        check=True,
    )


def run_model(entry, elf, core, config):
    """Run one workload; return (stats, error_message)."""
    if not os.path.isfile(elf):
        return None, "ELF not found: %s" % elf
    cmd = [core, "-c", config, "-i", str(entry["icount"]), "--target-elf", elf]
    try:
        proc = subprocess.run(
            cmd, capture_output=True, text=True, timeout=RUN_TIMEOUT_S
        )
    except subprocess.TimeoutExpired:
        return None, "run timed out after %ds" % RUN_TIMEOUT_S
    tail = "\n".join(proc.stderr.strip().splitlines()[-15:])
    if proc.returncode != 0:
        return None, "model exited with rc=%d. tail:\n%s" % (proc.returncode, tail)
    if entry["done_marker"] not in proc.stdout:
        return None, "workload never printed %r (run ended early)" % entry["done_marker"]
    stats = parse_stats(proc.stderr)
    if stats is None:
        return None, "no stats block in model output. tail:\n%s" % tail
    return stats, None


# ---------------------------------------------------------------------------
# Checks.
# ---------------------------------------------------------------------------
def evaluate(entry, stats, bounds):
    """Return list of (metric, ok, message) for one entry."""
    results = []
    for metric, kind in entry["checks"].items():
        if metric not in stats:
            results.append((metric, False, "metric missing from stats"))
            continue
        if metric not in bounds:
            results.append((metric, False, "no baseline bound (run --calibrate)"))
            continue
        obs = stats[metric]
        if kind == "gt":
            lo = bounds[metric]
            ok = obs > lo
            results.append((metric, ok, "%.4g > %.4g" % (obs, lo)))
        else:
            results.append((metric, False, "unknown check kind %r" % kind))
    return results


def suggest_bounds(entry, stats):
    """Derive baseline bounds from observed values for --calibrate."""
    bounds = {}
    for metric, kind in entry["checks"].items():
        if metric not in stats:
            continue
        obs = stats[metric]
        if kind == "gt":
            bounds[metric] = round(GT_MARGIN.get(metric, 0.8) * obs, 4)
    return bounds


# ---------------------------------------------------------------------------
# Driver.
# ---------------------------------------------------------------------------
def main():
    ap = argparse.ArgumentParser(description="RPM minimal perf unit-tests")
    ap.add_argument("--only", help="run only this entry by name")
    ap.add_argument("--core", default=DEFAULT_CORE, help="path to core binary")
    ap.add_argument("--config", default=None, help="path to Sparta config.yaml")
    ap.add_argument("--elf-dir", default=None,
                    help="use prebuilt ELFs (same file names) from this directory instead of building")
    ap.add_argument(
        "--calibrate",
        action="store_true",
        help="record observed metrics + suggested bounds to baseline.json",
    )
    args = ap.parse_args()

    config = args.config or find_config()
    if not os.path.isfile(args.core):
        print("error: core binary not found: %s (build it first)" % args.core, file=sys.stderr)
        return 2
    if not config or not os.path.isfile(config):
        print("error: Sparta config.yaml not found (looked in %s)" % CONFIG_CANDIDATES, file=sys.stderr)
        return 2

    entries = [e for e in ENTRIES if not args.only or e["name"] == args.only]
    if not entries:
        print("error: no entry named %r" % args.only, file=sys.stderr)
        return 2

    baseline = {}
    if not args.calibrate:
        if not os.path.isfile(BASELINE_PATH):
            print(
                "error: %s missing. Run once with --calibrate to record baselines."
                % BASELINE_PATH,
                file=sys.stderr,
            )
            return 2
        with open(BASELINE_PATH) as f:
            baseline = json.load(f)

    print("core:   %s" % args.core)
    print("config: %s" % config)
    print("mode:   %s\n" % ("CALIBRATE" if args.calibrate else "CHECK"))

    all_ok = True
    new_baseline = {}
    for entry in entries:
        name = entry["name"]
        elf = entry["elf"]
        if args.elf_dir:
            elf = os.path.join(args.elf_dir, os.path.basename(elf))
        else:
            print("[%s] building..." % name)
            try:
                build(entry)
            except subprocess.CalledProcessError as e:
                print("[%s] BUILD FAILED (rc=%d)" % (name, e.returncode))
                all_ok = False
                continue

        print("[%s] running..." % name)
        stats, err = run_model(entry, elf, args.core, config)
        if err:
            print("[%s] RUN FAILED: %s" % (name, err))
            all_ok = False
            continue

        summ = "  ".join(
            "%s=%s" % (k, stats[k]) for k in ("ipc", "retired", "cycles", "bp_acc") if k in stats
        )
        print("[%s] stats: %s" % (name, summ))

        if args.calibrate:
            new_baseline[name] = suggest_bounds(entry, stats)
            print("[%s] suggested bounds: %s" % (name, new_baseline[name]))
        else:
            bounds = baseline.get(name, {})
            for metric, ok, msg in evaluate(entry, stats, bounds):
                flag = "PASS" if ok else "FAIL"
                print("[%s]   %-4s %-8s %s" % (name, flag, metric, msg))
                all_ok = all_ok and ok
        print()

    if args.calibrate:
        # Merge with any existing baseline so --only --calibrate is non-destructive.
        merged = {}
        if os.path.isfile(BASELINE_PATH):
            with open(BASELINE_PATH) as f:
                merged = json.load(f)
        merged.update(new_baseline)
        with open(BASELINE_PATH, "w") as f:
            json.dump(merged, f, indent=2, sort_keys=True)
            f.write("\n")
        print("wrote baselines to %s" % BASELINE_PATH)
        return 0

    print("RESULT: %s" % ("PASS" if all_ok else "FAIL"))
    return 0 if all_ok else 1


if __name__ == "__main__":
    sys.exit(main())
