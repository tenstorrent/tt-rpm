# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: 2026 Tenstorrent USA, Inc.

"""Configs of one or more studies as table columns, and every table the report shows."""
from __future__ import annotations

import datetime
import math
import statistics
from dataclasses import dataclass, field
from pathlib import Path
from typing import Callable, Optional, Union

from . import data, metrics
from .data import ConfigResults, Counters, warn

MEAN = "(mean)"
GEOMEAN = "(geomean)"


@dataclass
class Table:
    title: str
    columns: list[str]
    rows: list                 # row labels: str, or (benchmark, input) tuples
    cells: list[list]          # float, str or None (blank)
    footer: int = 0            # trailing mean/geomean rows
    pct: set = field(default_factory=set)       # columns holding % change vs the baseline
    better: dict = field(default_factory=dict)  # pct column -> "higher" | "lower"

    def frame(self):
        import pandas as pd
        index = pd.MultiIndex.from_tuples(self.rows) if self.rows and isinstance(self.rows[0], tuple) \
            else pd.Index(self.rows)
        df = pd.DataFrame(self.cells, index=index, columns=self.columns, dtype=object)
        for col in df.columns:
            if all(isinstance(v, int) for v in df[col]):
                df[col] = df[col].astype(int)
            elif all(v is None or isinstance(v, (int, float)) for v in df[col]):
                df[col] = df[col].astype(float)
        return df


@dataclass(eq=False)
class Column:
    study: str
    directory: Path
    suite: Optional[str]
    cfg: ConfigResults
    label: str = ""


def pct_change(v: Optional[float], base: Optional[float]) -> Optional[float]:
    return None if v is None or base is None or base == 0 else 100.0 * (v - base) / base


def mean_row(grid: list[list[Optional[float]]], width: int) -> list[Optional[float]]:
    """Per column, the mean over rows with a value in every column that has any value."""
    filled = [j for j in range(width) if any(row[j] is not None for row in grid)]
    usable = [row for row in grid if all(row[j] is not None for j in filled)]
    return [statistics.fmean(row[j] for row in usable) if usable and j in filled else None
            for j in range(width)]


def evaluate(fn: Callable, c: Counters) -> Optional[float]:
    try:
        v = fn(c)
    except ZeroDivisionError:
        return None
    return None if v is None or math.isnan(v) else float(v)


class Results:
    """Columns are the configs of the loaded study plus those of any compared study."""

    def __init__(self) -> None:
        self.cols: list[Column] = []
        self.studies: list[tuple[str, Path]] = []
        self.suite_notes: dict[str, str] = {}   # study label -> scoring suite and how it was chosen
        self.skipped: list[tuple[str, str]] = []  # (study label, config or dump left out)
        self._baseline: Optional[Column] = None
        self._baseline_given = False
        self._present: Optional[set] = None
        self._geo: Optional[dict] = None

    # -- loading ------------------------------------------------------------

    def add_dir(self, study_dir: Union[str, Path], label: Optional[str] = None,
                suite: Optional[str] = None, counts=None) -> None:
        """suite None: the settings record's, else auto-detect; "": auto-detect."""
        directory = Path(study_dir).resolve()
        if not directory.is_dir():
            raise NotADirectoryError(f"{directory}: not a directory")
        label = label or directory.name
        if label in {s for s, _ in self.studies}:
            raise ValueError(f"study label {label!r} used twice")
        settings = data.read_settings(directory)
        source = "given"
        if suite is None:
            suite = settings.get("suite")
            source = f"from {data.SETTINGS_FILE}"
        if suite and suite not in metrics.SPEC_SECONDS:
            raise ValueError(f"unknown suite {suite!r} (known: {', '.join(metrics.SPEC_SECONDS)})")
        if counts is None:
            counts = []
            for path in map(Path, settings.get("counts_files") or []):
                try:
                    data.read_counts([path])
                    counts.append(path)
                except ValueError as e:
                    warn(f"{directory / data.SETTINGS_FILE}: recorded instruction counts unusable, "
                         f"left out: {e}")
        counts = [Path(counts)] if isinstance(counts, (str, Path)) else [Path(p) for p in counts]
        loaded = data.read_study(directory, suite or None, counts)
        if not loaded.configs:
            warn(f"{directory}: no stat dumps parsed")
        self.studies.append((label, directory))
        note = source if suite else loaded.suite_note
        self.suite_notes[label] = f"{loaded.suite or 'IPC only'} ({note})"
        self.skipped += [(label, what) for what in loaded.skipped]
        self.cols += [Column(label, directory, loaded.suite, c) for c in loaded.configs]
        multi = len(self.studies) > 1
        for col in self.cols:
            alone = sum(c.study == col.study for c in self.cols) == 1
            col.label = (col.study if alone else f"{col.study}/{col.cfg.name}") if multi else col.cfg.name
        if multi and not self._baseline_given and self._baseline is None:
            self._baseline = next((c for c in self.cols if c.study == label), None)
        self._present = self._geo = None

    def set_baseline(self, name: Optional[str]) -> None:
        self._baseline_given = name is not None
        self._baseline = None
        if name is not None:
            try:
                self._baseline = self.column(name)
            except KeyError:
                raise ValueError(f"baseline {name!r} names no column (columns: {', '.join(self.labels)})") \
                    from None

    # -- what was loaded ----------------------------------------------------

    @property
    def labels(self) -> list[str]:
        return [c.label for c in self.cols]

    @property
    def baseline(self) -> Optional[str]:
        return self._baseline.label if self._baseline else None

    @property
    def benchmarks(self) -> list[str]:
        return sorted({b for c in self.cols for b in c.cfg.grouped})

    @property
    def suites(self) -> list[str]:
        return sorted({c.suite for c in self.cols if c.suite})

    @property
    def complete(self) -> dict[str, dict[str, bool]]:
        return {c.label: {b: c.cfg.complete.get(b, False) for b in self.benchmarks} for c in self.cols}

    def missing(self) -> list[tuple[str, str]]:
        """(column, benchmark) pairs left out: incomplete or absent."""
        return [(c.label, b) for c in self.cols for b in self.benchmarks if not c.cfg.complete.get(b)]

    def column(self, name: str) -> Column:
        """By label, by `<study>/<config>` even where the label is shorter, or by the first study's
        plain config name, so names that worked before a compare() keep working."""
        first = self.studies[0][0] if self.studies else None
        for match in (lambda c: c.label == name,
                      lambda c: f"{c.study}/{c.cfg.name}" == name,
                      lambda c: c.study == first and c.cfg.name == name):
            hit = next((c for c in self.cols if match(c)), None)
            if hit:
                return hit
        raise KeyError(f"no column {name!r} (columns: {', '.join(self.labels)})")

    def present(self) -> set:
        if self._present is None:
            self._present = set().union(*(r.counters.values for c in self.cols for r in c.cfg.runs))
        return self._present

    def shown(self, m: metrics.Metric) -> bool:
        return all(p in self.present() for p in m.counters)

    def value(self, fn: Callable, col: Column, bench: str) -> Optional[float]:
        if not col.cfg.complete.get(bench):
            return None
        return evaluate(fn, col.cfg.benches[bench])

    # -- tables -------------------------------------------------------------

    def _select(self, configs=None, benchmarks=None) -> tuple[list[Column], list[str]]:
        cols = [self.column(c) for c in configs] if configs else self.cols
        return cols, list(benchmarks) if benchmarks else self.benchmarks

    def _with_deltas(self, title: str, cols: list[Column], rows: list, grid: list[list],
                     better: Optional[str], footer: int) -> Table:
        columns = [c.label for c in cols]
        cells = [list(r) for r in grid]
        pct: set = set()
        base = self._baseline
        if base in cols:
            b = cols.index(base)
            for j, col in enumerate(cols):
                if col is base:
                    continue
                name = f"Δ% {col.label}"
                columns.append(name)
                pct.add(name)
                for row in cells:
                    row.append(pct_change(row[j], row[b]))
        return Table(title, columns, rows, cells, footer, pct, {p: better for p in pct if better})

    def per_benchmark(self, fn: Callable, title: str, better: Optional[str] = None,
                      configs=None, benchmarks=None) -> Table:
        cols, benches = self._select(configs, benchmarks)
        grid = [[self.value(fn, c, b) for c in cols] for b in benches]
        return self._with_deltas(title, cols, benches + [MEAN], grid + [mean_row(grid, len(cols))], better, 1)

    def metric_table(self, key: str, configs=None, benchmarks=None) -> Table:
        m = metrics.METRICS[key]
        return self.per_benchmark(m.fn, m.label, m.better, configs, benchmarks)

    def geomeans(self) -> dict[str, Optional[float]]:
        if self._geo is None:
            scored = [c for c in self.cols if c.cfg.scores]
            common = set.intersection(*(set(c.cfg.scores) for c in scored)) if scored else set()
            dropped = sorted(set().union(*(c.cfg.scores for c in scored)) - common)
            if dropped:
                warn(f"left out of the geomean (scored in only some columns): {', '.join(dropped)}")
            self._geo = {c.label: math.exp(statistics.fmean(math.log(c.cfg.scores[b]) for b in common))
                         if c.cfg.scores and common else None for c in self.cols}
        return self._geo

    def scored(self) -> bool:
        return any(c.cfg.scores for c in self.cols)

    def headline_table(self) -> Table:
        if not self.scored():
            t = self.metric_table("ipc")
            t.title = "IPC (no benchmark scored)"
            return t
        benches = [b for b in self.benchmarks if any(b in c.cfg.scores for c in self.cols)]
        grid = [[c.cfg.scores.get(b) for c in self.cols] for b in benches]
        geo = self.geomeans()
        grid.append([geo[c.label] for c in self.cols])
        return self._with_deltas(f"SPEC score / GHz ({', '.join(self.suites)})", self.cols,
                                 benches + [GEOMEAN], grid, "higher", 1)

    def headline_values(self) -> dict[str, Optional[float]]:
        t = self.headline_table()
        return {c.label: t.cells[-1][j] for j, c in enumerate(self.cols)}

    def by_benchmark(self, title: str, col: Column, ms: list[metrics.Metric]) -> Table:
        """Benchmarks x metrics of one column; the mean covers benchmarks complete in every column."""
        grid = [[self.value(m.fn, col, b) for m in ms] for b in self.benchmarks]
        everywhere = [row for b, row in zip(self.benchmarks, grid)
                      if all(c.cfg.complete.get(b) for c in self.cols)]
        mean = []
        for j in range(len(ms)):
            vals = [row[j] for row in everywhere if row[j] is not None]
            mean.append(statistics.fmean(vals) if vals else None)
        return Table(title, [m.label.strip() for m in ms], self.benchmarks + [MEAN], grid + [mean], 1)

    def summary_table(self, col: Column) -> Table:
        ms = [metrics.METRICS[k] for k in metrics.SUMMARY if self.shown(metrics.METRICS[k])]
        return self.by_benchmark(f"Summary: {col.label}", col, ms)

    def topdown_tables(self, col: Column) -> list[Table]:
        out = []
        if metrics.TOPDOWN_COUNTER in self.present():
            out.append(self.by_benchmark(f"Top-down: {col.label}", col, metrics.TOPDOWN))
            out.append(self.by_benchmark(f"Memory bound by load level: {col.label}", col,
                                         metrics.MEMORY_SPLIT))
        stalls = [m for m in metrics.STALLS if self.shown(m)]
        if stalls:
            out.append(self.by_benchmark(f"Stall cycles (fraction of cycles): {col.label}", col, stalls))
        return out

    def group_tables(self, group: str, configs=None, benchmarks=None) -> list[Table]:
        return [self.metric_table(k, configs, benchmarks) for k in metrics.GROUPS[group]
                if self.shown(metrics.METRICS[k])]

    def input_table(self, col: Column) -> Table:
        keys = [k for k in metrics.PER_INPUT if self.shown(metrics.METRICS[k])]
        rows, grid = [], []
        for bench in sorted(col.cfg.inputs):
            for inp in sorted(col.cfg.inputs[bench], key=lambda i: i or ""):
                runs = col.cfg.grouped[bench][inp]
                c = col.cfg.inputs[bench][inp]
                rows.append((bench, inp or ""))
                grid.append([len(runs), sum(r.name.weight for r in runs)]
                            + [evaluate(metrics.METRICS[k].fn, c) for k in keys])
        return Table(f"Per input: {col.label}", ["SimPoints", "Weight sum"]
                     + [metrics.METRICS[k].label for k in keys], rows, grid)

    def runtime_table(self, col: Column) -> Table:
        grid = []
        for bench in self.benchmarks:
            secs = [r.counters.values[data.WALL_SECONDS] for runs in col.cfg.grouped.get(bench, {}).values()
                    for r in runs if data.WALL_SECONDS in r.counters.values]
            n = sum(len(runs) for runs in col.cfg.grouped.get(bench, {}).values())
            grid.append([n, statistics.median(secs) if secs else None, max(secs) if secs else None])
        return Table(f"Host runtime: {col.label}", ["Workloads", "Median s", "Max s"], self.benchmarks, grid)

    def slowest_table(self, n: int = 10) -> Table:
        rows = []
        for col in self.cols:
            timed = [r for r in col.cfg.runs if data.WALL_SECONDS in r.counters.values]
            if not timed:
                continue
            median = statistics.median(r.counters.values[data.WALL_SECONDS] for r in timed)
            for r in timed:
                s = r.counters.values[data.WALL_SECONDS]
                rows.append((s, r.workload, col.label, s / median if median else None))
        rows.sort(key=lambda x: -x[0])
        return Table(f"Slowest {n} workloads", ["Column", "Host s", "× column median"],
                     [w for _, w, _, _ in rows[:n]], [[c, s, x] for s, _, c, x in rows[:n]])

    def comparison_tables(self) -> list[Table]:
        if len(self.cols) < 2:
            return []
        base = self.baseline
        head = self.headline_values()
        ranked = sorted(self.cols, key=lambda c: -(head[c.label] or -math.inf))
        what = "geomean score / GHz" if self.scored() else "mean IPC"
        out = [Table(f"Ranking by {what}", [what] + ([f"Δ% vs {base}"] if base else []),
                     [c.label for c in ranked],
                     [[head[c.label]] + ([pct_change(head[c.label], head[base])] if base else [])
                      for c in ranked], 0, {f"Δ% vs {base}"} if base else set(),
                     {f"Δ% vs {base}": "higher"} if base else {})]

        keys = [k for k in metrics.SUMMARY if self.shown(metrics.METRICS[k])]
        means = {k: self.metric_table(k).cells[-1][:len(self.cols)] for k in keys}
        labels = [metrics.METRICS[k].label for k in keys]
        b = self.labels.index(base) if base else None
        grid = [[pct_change(means[k][j], means[k][b]) if base else means[k][j] for k in keys]
                for j in range(len(self.cols))]
        out.append(Table("Summary means" + (f", Δ% vs {base}" if base else ""), labels, self.labels, grid,
                         0, set(labels) if base else set(),
                         {metrics.METRICS[k].label: metrics.METRICS[k].better for k in keys
                          if base and metrics.METRICS[k].better}))

        for k in keys:
            m = metrics.METRICS[k]
            vals = [[self.value(m.fn, c, bench) for bench in self.benchmarks] for c in self.cols]
            out.append(Table(f"{m.label} by benchmark", self.benchmarks, self.labels, vals))
            if base:
                deltas = [[pct_change(v, vals[b][i]) for i, v in enumerate(row)] for row in vals]
                out.append(Table(f"{m.label} by benchmark, Δ% vs {base}", self.benchmarks, self.labels,
                                 deltas, 0, set(self.benchmarks),
                                 {bench: m.better for bench in self.benchmarks} if m.better else {}))

        if metrics.TOPDOWN_COUNTER in self.present():
            l1 = [n for n in metrics.TOPDOWN if n.key in metrics.TOPDOWN_L1]
            out.append(Table("Top-down level 1, mean over benchmarks complete in every column",
                             [n.label for n in l1], self.labels,
                             [self.by_benchmark("", c, l1).cells[-1] for c in self.cols]))
        return out

    def header(self) -> dict:
        common = [b for b in self.benchmarks if all(c.cfg.complete.get(b) for c in self.cols)]
        return {
            "in_means": f"{len(common)} of {len(self.benchmarks)} (complete in every column)" if common
            else "none: no benchmark is complete in every column, so mean rows are blank",
            "studies": [f"{label}: {d}" for label, d in self.studies],
            "generated": datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S"),
            "columns": self.labels,
            "benchmarks": len(self.benchmarks),
            "suite": [f"{label}: {note}" if len(self.studies) > 1 else note
                      for label, note in self.suite_notes.items()],
            "baseline": self.baseline or "none",
            "missing": self.missing(),
            "skipped": [f"{label}: {what}" if len(self.studies) > 1 else what for label, what in self.skipped],
        }
