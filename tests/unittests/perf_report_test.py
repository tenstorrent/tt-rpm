#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: 2026 Tenstorrent USA, Inc.

"""Check scripts/parse_scripts/perf_report.py against the hand-made study in perf_report_study/.

Made-up counters (I = retired instructions, C = core cycles, every dump at 2 GHz):

    config  benchmark  input  SimPoint weight   I     C     count (metadata.yml)
    base    mcf_r      inp0   0.25 / 0.75     1000  2000 / 1000   1.616e12 (= ref 1616 s x 1e9)
    fast    mcf_r      inp0   0.25 / 0.75     1000  1000 / 500
    base    x264_r     inp0   1               1000  1000          1.751e12 (= ref 1751 s x 1e9)
    fast    x264_r     inp0   1               1000   800
    base    xz_r       inp0   1               1000  2000          3e11
    base    xz_r       inp1   1               1000  1000          1e11
    fast    xz_r       inp0   1               1000  1000          (inp1 missing: incomplete)
    base    toy        -      1               2000  1000          (not a SPEC benchmark)
    fast    toy        -      1               2000   800

Since each count is the reference time x 1e9, score/GHz equals the IPC for single-input benchmarks.
Stdlib + pyyaml only. Exit 0 on pass.
"""

import bz2
import json
import math
import shutil
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent
sys.path.insert(0, str(REPO / "scripts" / "parse_scripts"))
import perf_report  # noqa: E402
from rpmreport import Report, metrics  # noqa: E402
from rpmreport.notebook import write_notebook  # noqa: E402

failures = []


def expect(what, got, want):
    ok = got is None and want is None or (
        got is not None and want is not None and math.isclose(got, want, rel_tol=1e-12, abs_tol=1e-12))
    print(f"{'ok  ' if ok else 'FAIL'} {what}: {got} (want {want})")
    if not ok:
        failures.append(what)


def row(table, label):
    return dict(zip(table.columns, table.cells[table.rows.index(label)]))


with tempfile.TemporaryDirectory() as tmp:
    study = Path(tmp) / "study"
    shutil.copytree(HERE / "perf_report_study", study)
    # Dumps may be bzip2'd; unreadable ones and ones without numbers are left out and listed.
    plain = study / "base" / "micro" / "toy.json"
    (study / "base" / "micro" / "toy.json.bz2").write_bytes(bz2.compress(plain.read_bytes()))
    plain.unlink()
    (study / "fast" / "micro" / "broken.json").write_text('{"Statistics": {')
    (study / "fast" / "micro" / "words.json").write_text('{"Statistics": {"r": {"a": {"val": "n/a"}}}}')

    r = Report()
    r.add_dir(study, None, "spec2017", [study / "metadata.yml"])
    r.set_baseline("base")
    base, fast = r.column("base"), r.column("fast")
    if len(r.skipped) != 2 or "words" in r.benchmarks:
        failures.append(f"skipped dumps {r.skipped}")

    # SimPoint weighting: C = 0.25 * 2000 + 0.75 * 1000; mispredicts 0.25 * 4 + 0.75 * 2 of 125 branches.
    mcf = base.cfg.benches["mcf_r"]
    expect("base mcf_r cycles", mcf.cycles, 1250.0)
    expect("base mcf_r GHz", mcf.ghz, 2.0)
    expect("base mcf_r mispredicts PKI", r.value(metrics.METRICS["mispredicts_pki"].fn, base, "mcf_r"), 2.5)
    expect("base mcf_r branch accuracy", r.value(metrics.METRICS["branch_accuracy"].fn, base, "mcf_r"),
           (125 - 2.5) / 125)
    # Input weighting by instruction count, 3:1: C = (3 * 2000 + 1000) / 4.
    expect("base xz_r cycles", base.cfg.benches["xz_r"].cycles, 1750.0)

    # Completeness: fast lacks xz_r inp1, so xz_r is blank there, unscored, and out of the means.
    expect("fast xz_r complete", float(fast.cfg.complete["xz_r"]), 0.0)
    expect("base xz_r complete", float(base.cfg.complete["xz_r"]), 1.0)
    if r.missing() != [("fast", "xz_r")]:
        failures.append(f"missing pairs {r.missing()}")
    ipc = r.metric_table("ipc")
    expect("fast xz_r IPC", row(ipc, "xz_r")["fast"], None)
    expect("mean IPC base", row(ipc, "(mean)")["base"], (0.8 + 1.0 + 2.0) / 3)
    expect("mean IPC fast", row(ipc, "(mean)")["fast"], (1.6 + 1.25 + 2.5) / 3)
    expect("mcf_r IPC delta %", row(ipc, "mcf_r")["Δ% fast"], 100.0)
    expect("mean IPC delta %", row(ipc, "(mean)")["Δ% fast"], 100.0 * (5.35 / 3.8 - 1))
    # Per-input values are still reported for the incomplete benchmark.
    expect("fast xz_r inp0 IPC", row(r.input_table(fast), ("xz_r", "inp0"))["IPC"], 1.0)

    # Scores: xz_r base = 1080 / (3e11 / 0.5 + 1e11 / 1.0) * 1e9; the geomean covers mcf_r and x264_r.
    expect("base mcf_r score", base.cfg.scores["mcf_r"], 0.8)
    expect("fast mcf_r score", fast.cfg.scores["mcf_r"], 1.6)
    expect("base xz_r score", base.cfg.scores["xz_r"], 1080 / 700)
    expect("fast xz_r score", fast.cfg.scores.get("xz_r"), None)
    expect("toy score", base.cfg.scores.get("toy"), None)
    head = r.headline_table()
    expect("base geomean", row(head, "(geomean)")["base"], math.sqrt(0.8 * 1.0))
    expect("fast geomean", row(head, "(geomean)")["fast"], math.sqrt(1.6 * 1.25))
    expect("geomean delta %", row(head, "(geomean)")["Δ% fast"], 100.0 * (math.sqrt(2.5) - 1))

    # Top-down of x264_r base: 4000 slots, 1000 retired, 1000 frontend, 2000 backend (1000 memory);
    # load stalls any/L1-miss/L2-miss 100/60/20 split memory bound 40:40:20.
    td = {t.title.split(":")[0]: t for t in r.topdown_tables(base)}
    x = row(td["Top-down"], "x264_r")
    for label, want in [("Retiring", 0.25), ("Bad speculation", 0.0), ("Frontend bound", 0.25),
                        ("Backend bound", 0.5), ("memory bound", 0.25), ("core bound", 0.25)]:
        expect(f"x264_r {label}", x[label], want)
    split = row(td["Memory bound by load level"], "x264_r")
    for label, want in [("L1 latency", 0.1), ("L2", 0.1), ("Beyond L2", 0.05)]:
        expect(f"x264_r memory {label}", split[label], want)

    # Suite auto-detect: the suite naming the most benchmarks, even when that is a minority.
    suite, _ = metrics.detect_suite({"toy", "other", "mcf_r"}, [])
    if suite != "spec2017":
        failures.append(f"detected suite {suite}")

    host = row(r.runtime_table(base), "mcf_r")
    expect("mcf_r host median s", host["Median s"], 20.0)
    expect("mcf_r host max s", host["Max s"], 30.0)

    # In compare mode, plain config names still mean the first study's columns.
    both = Report()
    both.add_dir(study, None, "spec2017", [study / "metadata.yml"])
    both.add_dir(study, "again", "spec2017", [study / "metadata.yml"])
    if both.column("base").study != "study" or both.column("again/fast").study != "again":
        failures.append(f"compare-mode column names {both.labels}")

    # CLI end to end: writes the HTML report; a bad baseline fails.
    out = Path(tmp) / "report.html"
    args = [str(study), "--score-ref", "spec2017", "--metadata-file", str(study / "metadata.yml"),
            "--baseline", "base", "--out", str(out)]
    if perf_report.main(args) != 0 or "(geomean)" not in out.read_text():
        failures.append("CLI report")
    if perf_report.main(args[:-4] + ["--baseline", "nope"]) == 0:
        failures.append("CLI accepted an unknown baseline")

    nb = json.loads(write_notebook(study, "study", "spec2017", [], REPO).read_text())
    if nb["nbformat"] != 4 or len(nb["cells"]) < 10:
        failures.append("notebook")

print(f"\n{'FAILED: ' + ', '.join(failures) if failures else 'all checks passed'}")
sys.exit(1 if failures else 0)
