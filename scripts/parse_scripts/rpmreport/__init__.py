# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: 2026 Tenstorrent USA, Inc.

"""Notebook API for RPM study results: tables come back as pandas DataFrames.

    import rpmreport
    r = rpmreport.load("results/experiments/me/s/s_<ts>", baseline="base")
    rpmreport.show(r.headline())
    rpmreport.show(r.custom(lambda c: c["top.core0.lsq.num_loads"] / c.cycles, "Loads per cycle"))

pandas is needed only for the DataFrame methods; `write_html` and the CLI work without it.
"""
from __future__ import annotations

from pathlib import Path
from typing import Callable, Optional, Union

from . import metrics
from .data import Counters
from .results import Results

__all__ = ["load", "show", "Report", "Counters", "metrics"]


def _frames(tables) -> dict:
    return {t.title: t.frame() for t in tables}


class Report(Results):
    def metric(self, key: str, configs=None, benchmarks=None):
        """Benchmarks x configs for a key of `metrics.METRICS`, with mean row and baseline deltas."""
        return self.metric_table(key, configs, benchmarks).frame()

    def custom(self, fn: Callable[[Counters], Optional[float]], title: str = "custom",
               better: Optional[str] = None, configs=None, benchmarks=None):
        """Same table for fn(aggregated counters); counters are indexed by full statistic path."""
        return self.per_benchmark(fn, title, better, configs, benchmarks).frame()

    def headline(self):
        return self.headline_table().frame()

    def summary(self) -> dict:
        return _frames(self.summary_table(c) for c in self.cols)

    def topdown(self) -> dict:
        return _frames(t for c in self.cols for t in self.topdown_tables(c))

    def group(self, name: str, configs=None, benchmarks=None) -> dict:
        """One of `metrics.GROUPS`: a table per metric."""
        return _frames(self.group_tables(name, configs, benchmarks))

    def per_input(self) -> dict:
        return _frames(self.input_table(c) for c in self.cols)

    def runtime(self) -> dict:
        return _frames([self.runtime_table(c) for c in self.cols] + [self.slowest_table()])

    def comparison(self) -> dict:
        return _frames(self.comparison_tables())

    def counters(self, config: str, benchmark: str) -> Counters:
        return self.column(config).cfg.benches[benchmark]

    def input_counters(self, config: str, benchmark: str) -> dict[Optional[str], Counters]:
        return self.column(config).cfg.inputs[benchmark]

    def compare(self, study_dir: Union[str, Path], label: str, suite: Optional[str] = None,
                counts=None, baseline: Optional[str] = None) -> "Report":
        """Add another study's configs as columns labelled `<label>/<config>`, or `<label>` if it has one
        config; `<label>/<config>` and the first study's plain config names resolve either way."""
        self.add_dir(study_dir, label, suite, counts)
        if baseline is not None:
            self.set_baseline(baseline)
        return self

    def write_html(self, path: Union[str, Path, None] = None) -> Path:
        from .html import page
        path = Path(path) if path else self.studies[0][1] / "performance_report.html"
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(page(self))
        return path


def load(study_dir: Union[str, Path] = ".", suite: Optional[str] = None, counts=None,
         baseline: Optional[str] = None) -> Report:
    """suite: "spec2006"/"spec2017", "" to auto-detect, None for the study's settings record.
    counts: instruction-count file(s); None for the settings record's."""
    r = Report()
    r.add_dir(study_dir, None, suite, counts)
    r.set_baseline(baseline)
    return r


def show(obj) -> None:
    """Display a DataFrame or a {title: DataFrame} dict, in Jupyter or on a terminal."""
    try:
        from IPython.display import Markdown, display
    except ImportError:
        display, Markdown = print, str
    for title, df in (obj.items() if isinstance(obj, dict) else [(None, obj)]):
        if title:
            display(Markdown(f"**{title}**"))
        display(df)
