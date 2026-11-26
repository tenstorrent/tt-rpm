#!/usr/bin/env python3

# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: 2026 Tenstorrent USA, Inc.

"""Write performance_report.html for a study directory: <study>/<config>/**/<workload>.json[.bz2].

Scores (SPEC score per GHz), metrics, top-down breakdown, per-input values and host
runtime of every config in the study, optionally side by side with other studies.
Defaults for --score-ref and --metadata-file come from the study's report_settings.yml.

Usage:
    perf_report.py my_study --baseline base
    perf_report.py new_study --compare old=path/to/old_study
"""
from __future__ import annotations

import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from rpmreport import Report  # noqa: E402
from rpmreport.html import page  # noqa: E402


def main(argv: list[str]) -> int:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("study_dir", nargs="?", default=".", type=Path, help="study directory (default: .)")
    p.add_argument("--score-ref", default=None,
                   help="suite for scores: spec2006 or spec2017; empty string auto-detects")
    counts = p.add_mutually_exclusive_group()
    counts.add_argument("--metadata-file", action="append", type=Path,
                        help="per-input dynamic instruction counts (YAML); repeatable, later files win")
    counts.add_argument("--no-metadata-file", action="store_true",
                        help="ignore the instruction-count files recorded with the study")
    p.add_argument("--baseline", default=None, help="config (or comparison column) for %% delta columns")
    p.add_argument("--out", type=Path, default=None,
                   help="output HTML (default: performance_report.html in the study dir)")
    p.add_argument("--compare", action="append", default=[], metavar="LABEL=DIR",
                   help="add another study's configs as columns; repeatable")
    args = p.parse_args(argv)

    if not args.study_dir.is_dir():
        p.error(f"{args.study_dir}: not a directory")
    compares = []
    for item in args.compare:
        label, sep, directory = item.partition("=")
        if not sep or not label or not directory:
            p.error(f"--compare {item!r}: expected LABEL=DIR")
        compares.append((label, Path(directory)))

    counts = [] if args.no_metadata_file else args.metadata_file
    r = Report()
    try:
        r.add_dir(args.study_dir, None, args.score_ref, counts)
        for label, directory in compares:
            r.add_dir(directory, label, args.score_ref, counts)
        if args.baseline is not None:
            r.set_baseline(args.baseline)
    except (OSError, ValueError) as e:
        print(f"error: {e}", file=sys.stderr)
        return 1
    if not r.cols:
        print("error: no stat dump could be parsed", file=sys.stderr)
        return 1

    out = args.out or args.study_dir / "performance_report.html"
    try:
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_text(page(r))
    except OSError as e:
        hint = "; write it elsewhere with --out FILE" if args.out is None else ""
        print(f"error: cannot write {out}: {e.strerror}{hint}", file=sys.stderr)
        return 1
    print(out.resolve())
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
