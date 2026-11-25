# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: 2026 Tenstorrent USA, Inc.

"""The `<study>_performance_study.ipynb` run.py writes into a study directory."""
from __future__ import annotations

import json
from pathlib import Path
from typing import Optional

from .metrics import GROUPS


def _cell(kind: str, source: str) -> dict:
    cell = {"cell_type": kind, "metadata": {}, "source": source.splitlines(keepends=True)}
    if kind == "code":
        cell.update(execution_count=None, outputs=[])
    return cell


def write_notebook(study_dir: Path, name: str, suite: Optional[str], counts_files: list[Path],
                   repo: Path) -> Path:
    code = [
        f"STUDY_DIR = {str(study_dir)!r}\n"
        f"SUITE = {suite!r}  # spec2006, spec2017, or \"\" to auto-detect\n"
        f"COUNTS_FILES = {[str(p) for p in counts_files]!r}\n"
        "BASELINE = None  # a config name, for % delta columns\n"
        f"REPO = {str(repo)!r}",
        "import sys\n"
        "sys.path.insert(0, f\"{REPO}/scripts/parse_scripts\")\n"
        "import rpmreport\n"
        "from rpmreport import show\n\n"
        "r = rpmreport.load(STUDY_DIR, suite=SUITE, counts=COUNTS_FILES, baseline=BASELINE)\n"
        "print(r.header())",
        "show(r.headline())",
        "show(r.summary())",
        "show(r.topdown())",
        *[f"show(r.group({g!r}))" for g in GROUPS],
        "show(r.per_input())",
        "# Columns of a compared study are \"other/<config>\" (\"other\" if it has one config), and its\n"
        "# first column becomes the baseline; this study's configs keep their plain names too.\n"
        "# r.compare(\"/path/to/other_study\", label=\"other\")\n"
        "# show(r.comparison())",
        "print(r.write_html())",
    ]
    nb = {
        "cells": [_cell("markdown", f"# {name}\n\nRPM results in `{study_dir}`.")]
        + [_cell("code", c) for c in code],
        "metadata": {"kernelspec": {"display_name": "Python 3", "language": "python", "name": "python3"},
                     "language_info": {"name": "python"}},
        "nbformat": 4,
        "nbformat_minor": 4,
    }
    path = study_dir / f"{name}_performance_study.ipynb"
    path.write_text(json.dumps(nb, indent=1) + "\n")
    return path
