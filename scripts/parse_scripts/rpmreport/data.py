# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: 2026 Tenstorrent USA, Inc.

"""Read a study directory: stat dumps, workload names, instruction counts, aggregation."""
from __future__ import annotations

import bz2
import json
import math
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import Optional

import yaml

from . import metrics

SETTINGS_FILE = "report_settings.yml"
COUNTS_SECTION = "Dynamic-Insts"
CORE = "top.core0."
RETIRED = CORE + "writeback.num_retired"
WALL_SECONDS = "scheduler.host_machine_wall_runtime_seconds"
ROOT_CYCLES = "clocks.Root.cycles"
CORE_CLOCKS = ("clocks.Root.cpu_clock_0.cycles", "clocks.Root.cpu_clock.cycles")
WEIGHT_TOLERANCE = 0.01

_SIMPOINT_SUFFIX = re.compile(r"-s(\d+)-w(\d+)$")
_INPUT_PREFIX = re.compile(r"(.+?)(?:-(?:ref|train|test))?-(inp\d+)(?=-|$)")
_OTHER_CORE = re.compile(r"top\.core[1-9]\d*\.")


def warn(msg: str) -> None:
    print(f"warning: {msg}", file=sys.stderr)


# ---------------------------------------------------------------------------
# workload names and instruction counts
# ---------------------------------------------------------------------------

@dataclass(frozen=True)
class WorkloadName:
    benchmark: str
    input: Optional[str]     # None: the benchmark's single unnamed input
    key: str                 # instruction-count file key of the input
    simpoint: int
    weight: float


def split_input(name: str) -> tuple[str, Optional[str], str]:
    """(benchmark, input, count key) of a workload name."""
    m = _INPUT_PREFIX.match(name)
    if m:
        return m.group(1), m.group(2), m.group(0)
    return name, None, name


def decode_name(filename: str) -> WorkloadName:
    name = filename
    for suffix in (".bz2", ".json", "-trace"):
        name = name.removesuffix(suffix)
    simpoint, weight = 0, 1.0
    m = _SIMPOINT_SUFFIX.search(name)
    if m:
        simpoint, weight = int(m.group(1)), int(m.group(2)) / 100000
        name = name[: m.start()]
    bench, inp, key = split_input(name)
    return WorkloadName(bench, inp, key, simpoint, weight)


def _load_yaml(path: Path):
    try:
        with open(path) as f:
            return yaml.safe_load(f)
    except OSError as e:
        raise ValueError(f"{path}: {e.strerror}") from None
    except yaml.YAMLError as e:
        mark = getattr(e, "problem_mark", None)
        raise ValueError(f"{path}: not valid YAML" + (f" (line {mark.line + 1})" if mark else "")) from None


def read_counts(paths: list[Path]) -> dict[tuple[str, Optional[str]], float]:
    """{(benchmark, input): whole-program dynamic instructions}; later files win. ValueError if unusable."""
    counts: dict[tuple[str, Optional[str]], float] = {}
    for path in paths:
        doc = _load_yaml(path)
        section = doc.get(COUNTS_SECTION) if isinstance(doc, dict) else None
        if not isinstance(section, dict):
            raise ValueError(f"{path}: not an instruction-count file (no mapping of input to count)")
        for key, n in section.items():
            if isinstance(n, bool) or not isinstance(n, (int, float)):
                raise ValueError(f"{path}: instruction count of {key!r} is not a number")
            bench, inp, _ = split_input(str(key))
            counts[(bench, inp)] = float(n)
    return counts


def read_settings(study_dir: Path) -> dict:
    """The study's settings record; a malformed one is ignored with a warning."""
    path = study_dir / SETTINGS_FILE
    if not path.is_file():
        return {}
    try:
        record = _load_yaml(path) or {}
    except ValueError as e:
        warn(f"{e}; settings record ignored")
        return {}
    if not (isinstance(record, dict) and isinstance(record.get("suite") or "", str)
            and isinstance(record.get("counts_files") or [], list)):
        warn(f"{path}: expected suite and counts_files entries; settings record ignored")
        return {}
    return record


# ---------------------------------------------------------------------------
# stat dumps
# ---------------------------------------------------------------------------

@dataclass
class Counters:
    """Statistic values by path; a path a dump lacks reads as 0."""
    values: dict[str, float]
    cycles: float                 # core clock cycles
    ghz: Optional[float] = None   # None when the dump has no core clock

    def __getitem__(self, path: str) -> float:
        return self.values.get(path, 0.0)

    @property
    def instructions(self) -> float:
        return self[RETIRED]


def read_dump(path: Path) -> Counters:
    with (bz2.open(path, "rt") if path.name.endswith(".bz2") else open(path)) as f:
        stats = json.load(f)["Statistics"]
    values: dict[str, float] = {}

    def walk(node: dict, prefix: str) -> None:
        for key, child in node.items():
            if key == "ordered_keys" or not isinstance(child, dict):
                continue
            if "val" not in child:
                walk(child, f"{prefix}{key}.")
                continue
            val = child["val"]
            if isinstance(val, (int, float)) and not isinstance(val, bool) and not math.isnan(val):
                values[prefix + key] = float(val)

    for root in stats.values():
        walk(root, "")
    values = {k: v for k, v in values.items() if not _OTHER_CORE.match(k)}
    if not values:
        raise ValueError("no numeric statistics")

    clock = next((p for p in CORE_CLOCKS if p in values), None)
    cycles = values[clock] if clock else values.get(ROOT_CYCLES, 0.0)
    root = values.get(ROOT_CYCLES, 0.0)
    ghz = 1000.0 * cycles / root if clock and root and cycles != root else None
    return Counters(values, cycles, ghz)


def weighted_mean(items: list[tuple[float, Counters]]) -> Counters:
    total = sum(w for w, _ in items)
    if total <= 0:
        items = [(1.0, c) for _, c in items]
        total = float(len(items))
    keys = set().union(*(c.values for _, c in items))
    values = {k: sum(w * c.values.get(k, 0.0) for w, c in items) / total for k in keys}
    cycles = sum(w * c.cycles for w, c in items) / total
    ghz = None
    if all(c.ghz is not None for _, c in items):
        ghz = sum(w * c.ghz for w, c in items) / total
    return Counters(values, cycles, ghz)


@dataclass
class Run:
    path: Path
    name: WorkloadName
    counters: Counters

    @property
    def workload(self) -> str:
        return self.path.name.removesuffix(".bz2").removesuffix(".json")


# ---------------------------------------------------------------------------
# one config of one study
# ---------------------------------------------------------------------------

@dataclass
class ConfigResults:
    name: str
    runs: list[Run]
    # benchmark -> input -> SimPoint runs
    grouped: dict[str, dict[Optional[str], list[Run]]] = field(default_factory=dict)
    inputs: dict[str, dict[Optional[str], Counters]] = field(default_factory=dict)
    benches: dict[str, Counters] = field(default_factory=dict)
    complete: dict[str, bool] = field(default_factory=dict)
    scores: dict[str, float] = field(default_factory=dict)


def is_dump(path: Path) -> bool:
    return path.is_file() and (path.name.endswith(".json") or path.name.endswith(".json.bz2"))


def config_dirs(study_dir: Path, skipped: list[str]) -> dict[str, list[Path]]:
    """Config name -> its dump files; `<config>/binary/` holds model binaries, not results."""
    found: dict[str, list[Path]] = {}
    for d in sorted(study_dir.iterdir()):
        if not d.is_dir() or d.name.startswith(("_", ".")):
            continue
        dumps = sorted(p for p in d.rglob("*")
                       if is_dump(p) and p.relative_to(d).parts[0] != "binary")
        if dumps:
            found[d.name] = dumps
        else:
            skip(skipped, study_dir, d, "no stat dumps; config skipped")
    return found


def skip(skipped: list[str], study_dir: Path, path: Path, why: str) -> None:
    """Warn now, and keep the item for the report header."""
    warn(f"{path}: {why}")
    skipped.append(f"{path.relative_to(study_dir)}: {why}")


def read_config(name: str, dumps: list[Path], study_dir: Path, skipped: list[str]) -> ConfigResults:
    cfg = ConfigResults(name, [])
    for path in dumps:
        try:
            counters = read_dump(path)
        except (OSError, EOFError, ValueError, KeyError, TypeError, AttributeError) as e:
            skip(skipped, study_dir, path, f"unreadable stat dump ({type(e).__name__}: {e}); left out")
            continue
        run = Run(path, decode_name(path.name), counters)
        sims = cfg.grouped.setdefault(run.name.benchmark, {}).setdefault(run.name.input, [])
        if any(r.name.simpoint == run.name.simpoint for r in sims):
            skip(skipped, study_dir, path, "same benchmark, input and SimPoint as an earlier dump; left out")
            continue
        sims.append(run)
        cfg.runs.append(run)
    return cfg


def aggregate(cfg: ConfigResults, counts: dict) -> None:
    for bench, by_input in cfg.grouped.items():
        cfg.inputs[bench] = {inp: weighted_mean([(r.name.weight, r.counters) for r in runs])
                             for inp, runs in by_input.items()}
        aggs = cfg.inputs[bench]
        if len(aggs) == 1:
            weights = {inp: 1.0 for inp in aggs}
        elif all((bench, inp) in counts for inp in aggs):
            weights = {inp: counts[(bench, inp)] for inp in aggs}
        else:
            warn(f"{cfg.name}/{bench}: not every input has an instruction count; "
                 "using equal input weights")
            weights = {inp: 1.0 for inp in aggs}
        cfg.benches[bench] = weighted_mean([(weights[inp], c) for inp, c in aggs.items()])


def mark_complete(configs: list[ConfigResults], counts: dict) -> None:
    benches = sorted({b for c in configs for b in c.grouped})
    for bench in benches:
        want_inputs = {i for c in configs for i in c.grouped.get(bench, {})}
        want_inputs |= {i for (b, i) in counts if b == bench}
        want_sims = {i: {r.name.simpoint for c in configs for r in c.grouped.get(bench, {}).get(i, [])}
                     for i in want_inputs}
        for c in configs:
            have = c.grouped.get(bench, {})
            c.complete[bench] = bool(have) and all(
                i in have
                and want_sims[i] <= {r.name.simpoint for r in have[i]}
                and abs(sum(r.name.weight for r in have[i]) - 1.0) <= WEIGHT_TOLERANCE + 1e-12
                for i in want_inputs)


def score(cfg: ConfigResults, suite: Optional[str], counts: dict) -> None:
    """SPEC-style score per GHz: reference seconds over the run time on a 1 GHz core."""
    if not suite:
        return
    for bench, aggs in cfg.inputs.items():
        ref = metrics.reference_time(suite, bench)
        if ref is None or not cfg.complete.get(bench):
            continue
        if not all((bench, i) in counts and c.cycles and c.instructions for i, c in aggs.items()):
            continue
        cycles = sum(counts[(bench, i)] * c.cycles / c.instructions for i, c in aggs.items())
        cfg.scores[bench] = ref / (cycles / 1e9)


@dataclass
class Loaded:
    configs: list[ConfigResults]
    suite: Optional[str]     # scoring suite; None: IPC only
    suite_note: str          # how the suite was chosen
    skipped: list[str]       # configs and dumps left out, relative to the study


def read_study(study_dir: Path, suite: Optional[str], counts_files: list[Path]) -> Loaded:
    """Parse every config of a study; suite None auto-detects."""
    counts = read_counts(counts_files)
    skipped: list[str] = []
    configs = [read_config(name, dumps, study_dir, skipped)
               for name, dumps in config_dirs(study_dir, skipped).items()]
    for c in configs:
        if not c.runs:
            skip(skipped, study_dir, study_dir / c.name, "no readable stat dump; config skipped")
    configs = [c for c in configs if c.runs]
    note = "given"
    if suite is None:
        suite, note = metrics.detect_suite({b for c in configs for b in c.grouped}, counts_files)
    for c in configs:
        aggregate(c, counts)
    mark_complete(configs, counts)
    for c in configs:
        score(c, suite, counts)
    return Loaded(configs, suite, note, skipped)
