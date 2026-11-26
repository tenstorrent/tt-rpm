#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: 2026 Tenstorrent USA, Inc.

"""Clock-domain unit test for the heterogeneous cluster (peer of run_checks.py).

Confirms that per-core clock domains advance in proportion to their frequency. Runs the
cluster for a FIXED number of scheduler ticks (-r) with cores at different frequencies,
then checks each core's elapsed clock cycles: at a common instant,
    cycles(coreA) / cycles(coreB) == freq(A) / freq(B)
e.g. a 3 GHz core advances 3 cycles for every 2 of a 2 GHz core.

The model prints one `[clock] <name> freq_mhz=<F> cycles=<N>` line per core
(Pipeline.cpp::reportStats); we read the last line per clock (the value at the tick limit).

Stdlib only. Exit 0 on pass, non-zero on failure -- same contract as run_checks.py.
"""

import os
import re
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))     # tests/unittests
TESTS_DIR = os.path.dirname(HERE)                     # tests
REPO = os.path.dirname(TESTS_DIR)                     # repo root

CLUSTER = os.environ.get("CLUSTER_BIN", os.path.join(REPO, "build", "top", "cluster", "cluster"))
ELF = os.environ.get("CLUSTER_ELF", os.path.join(TESTS_DIR, "build", "coremark.bare.elf"))
CORE_BIG = os.path.join(REPO, "models", "cluster", "src", "core_big.yaml")
CORE_LITTLE = os.path.join(REPO, "models", "cluster", "src", "core_little.yaml")

TICKS = 3_000_000          # fixed scheduler ticks (the workload must not finish before this)
BIG_GHZ, LITTLE_GHZ = 3.0, 2.0
RATIO_TOL = 0.02           # 2% -- absorbs the +/-1 last-edge discretization
RUN_TIMEOUT_S = 600

# Cores at two frequencies behind one shared cache (cache_freq omitted -> default). Absolute
# core_config paths so the test is independent of the working directory.
CONFIG = f"""
coherence_enabled: true
cpu_types:
  big:    {{ core_config: {CORE_BIG},    cpu_freq: {BIG_GHZ} }}
  little: {{ core_config: {CORE_LITTLE}, cpu_freq: {LITTLE_GHZ} }}
cores:
  - {{ type: big,    hartids: "0-1" }}
  - {{ type: little, hartids: "2-3" }}
program: ""
"""

CLOCK_RE = re.compile(r"\[clock\]\s+(\S+)\s+freq_mhz=([\d.]+)\s+cycles=(\d+)")


def main():
    if not os.path.isfile(CLUSTER):
        print("error: cluster binary not found: %s (build it first)" % CLUSTER, file=sys.stderr)
        return 2
    # Build the workload ELF via the shared tests/Makefile if it is missing.
    if not os.path.isfile(ELF):
        try:
            subprocess.run(["make", "-C", TESTS_DIR, "coremark"], check=True)
        except subprocess.CalledProcessError as e:
            print("error: coremark build failed (rc=%d)" % e.returncode, file=sys.stderr)
            return 2
    for path, what in [(CORE_BIG, "core_big.yaml"), (CORE_LITTLE, "core_little.yaml")]:
        if not os.path.isfile(path):
            print("error: %s not found: %s" % (what, path), file=sys.stderr)
            return 2

    with tempfile.NamedTemporaryFile("w", suffix=".yaml", delete=False) as f:
        f.write(CONFIG)
        cfg_path = f.name
    try:
        proc = subprocess.run(
            [CLUSTER, "--cluster-config", cfg_path, "-r", str(TICKS), "--target-elf", ELF],
            capture_output=True, text=True, timeout=RUN_TIMEOUT_S,
        )
    finally:
        os.unlink(cfg_path)

    # Last [clock] line per clock = its value at the tick limit.
    latest = {}  # clock_name -> (freq_mhz, cycles)
    for m in CLOCK_RE.finditer(proc.stderr):
        latest[m.group(1)] = (float(m.group(2)), int(m.group(3)))

    if not latest:
        tail = "\n".join(proc.stderr.strip().splitlines()[-15:])
        print("FAIL: no [clock] lines in output (rc=%d). tail:\n%s" % (proc.returncode, tail))
        return 1

    print("core clocks at tick %d:" % TICKS)
    for name, (mhz, cyc) in sorted(latest.items()):
        print("  %-14s freq=%.0f MHz  cycles=%d  (cycles/MHz=%.5f)" % (name, mhz, cyc, cyc / mhz))

    checks = []
    # 1: cycles / freq is uniform across domains (== the shared elapsed wall time).
    per = [cyc / mhz for (mhz, cyc) in latest.values()]
    lo, hi = min(per), max(per)
    checks.append(("clocks advance proportionally to frequency (cycles/freq uniform)",
                   bool(hi) and (hi - lo) / hi <= RATIO_TOL))
    # 2: the explicit ask -- big:little cycle ratio equals the frequency ratio.
    bigs = [cyc for (mhz, cyc) in latest.values() if abs(mhz - BIG_GHZ * 1000) < 1]
    littles = [cyc for (mhz, cyc) in latest.values() if abs(mhz - LITTLE_GHZ * 1000) < 1]
    if bigs and littles:
        got = (sum(bigs) / len(bigs)) / (sum(littles) / len(littles))
        want = BIG_GHZ / LITTLE_GHZ
        checks.append(("big:little cycle ratio %.4f ~= freq ratio %.4f" % (got, want),
                       abs(got - want) / want <= RATIO_TOL))

    print()
    all_ok = True
    for desc, ok in checks:
        print("  %-4s %s" % ("PASS" if ok else "FAIL", desc))
        all_ok = all_ok and ok

    print("\nRESULT: %s" % ("PASS" if all_ok else "FAIL"))
    return 0 if all_ok else 1


if __name__ == "__main__":
    sys.exit(main())
