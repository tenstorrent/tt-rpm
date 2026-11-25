#!/usr/bin/env python3

# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: 2026 Tenstorrent USA, Inc.

"""Submit RPM simulation jobs to a Slurm cluster as an array of tasks.

Given one or more experiment config files and a set of benchmarks, this tool

  1. creates a timestamped study directory on the shared filesystem,
  2. snapshots each model binary + its config.yaml into the study dir,
  3. expands experiment x benchmark x workload into shell command lines,
  4. writes a Slurm array script (commands=[...] indexed by $SLURM_ARRAY_TASK_ID),
  5. submits one sbatch array job per (experiment, benchmark) pair.

Run with --dry-run to generate everything but skip sbatch. Run with --blocking
to wait on the jobs and then auto-invoke parse_slurm_results.py and perf_report.py.

Run with --help for all options. See configs/experiment_configs/core_baseline.yml
for an example experiment config and benchmarks.yml for the benchmark registry.
"""
from __future__ import annotations

import argparse
import dataclasses
import datetime
import getpass
import os
import shlex
import shutil
import subprocess
import sys
import textwrap
from pathlib import Path
from typing import Optional

import yaml

SCRIPT_DIR = Path(__file__).resolve().parent
REPO_ROOT = SCRIPT_DIR.parent.parent.parent  # scripts/run_scripts/slurm -> repo
sys.path.insert(0, str(SCRIPT_DIR))
from slurm_utils import (  # noqa: E402
    detect_cluster_name,
    submit_sbatch,
    wait_on_job,
)
PARSE_SCRIPTS = REPO_ROOT / "scripts" / "parse_scripts"
sys.path.insert(0, str(PARSE_SCRIPTS))
from rpmreport.data import write_settings  # noqa: E402
from rpmreport.metrics import SPEC_SECONDS  # noqa: E402
from rpmreport.notebook import write_notebook  # noqa: E402

DEFAULT_RESULT_ROOT = "results"
DEFAULT_QOS = "normal"
DEFAULT_MEM_PER_SLOT_GB = 8.0
DEFAULT_MAX_LOG_BYTES = "10M"


# ---------------------------------------------------------------------------
# data classes
# ---------------------------------------------------------------------------

@dataclasses.dataclass
class Experiment:
    config_name: str
    model: str
    config_files: list[str] = dataclasses.field(default_factory=list)
    model_options: list[str] = dataclasses.field(default_factory=list)
    num_cycles: Optional[int] = None
    num_instructions: Optional[int] = None

    def validate(self) -> None:
        if not self.config_name:
            raise ValueError("experiment missing 'config_name'")
        if not self.model:
            raise ValueError(f"experiment {self.config_name}: missing 'model'")
        # Both optional: when neither is set, run.py emits no -r/-i and the model's
        # own config governs the run length (e.g. the cluster's `instruction_limit`).
        if self.num_cycles is not None and self.num_instructions is not None:
            raise ValueError(
                f"experiment {self.config_name}: num_cycles and num_instructions are mutually exclusive")


@dataclasses.dataclass
class Benchmark:
    name: str
    type: str               # 'elf' | 'snapshot'
    path: Path
    workload_files: list[Path]

    def model_flag(self) -> str:
        # Everything execution-driven from a snapshot goes through --snapshot. The cluster
        # infers the Whisper topology from the snapshot's hart count: a multi-hart
        # snapshot runs one shared Whisper (shared address space); a single-hart
        # snapshot runs an independent Whisper per core.
        if self.type == "elf":
            return "--target-elf"
        return "--snapshot"


# ---------------------------------------------------------------------------
# config loading
# ---------------------------------------------------------------------------

def load_experiment_configs(paths: list[Path]) -> list[Experiment]:
    experiments: list[Experiment] = []
    seen: set[str] = set()
    for p in paths:
        with open(p) as f:
            docs = list(yaml.safe_load_all(f))
        # Flatten: each doc can be a list or a single dict.
        entries: list[dict] = []
        for doc in docs:
            if doc is None:
                continue
            if isinstance(doc, list):
                entries.extend(doc)
            elif isinstance(doc, dict):
                entries.append(doc)
            else:
                raise ValueError(f"{p}: unexpected YAML shape {type(doc).__name__}")
        for entry in entries:
            exp = Experiment(
                config_name=entry.get("config_name", ""),
                model=entry.get("model", ""),
                config_files=list(entry.get("config_files", []) or []),
                model_options=list(entry.get("model_options", []) or []),
                num_cycles=entry.get("num_cycles"),
                num_instructions=entry.get("num_instructions"),
            )
            exp.validate()
            if exp.config_name in seen:
                raise ValueError(
                    f"duplicate experiment config_name '{exp.config_name}' in {p}")
            seen.add(exp.config_name)
            experiments.append(exp)
    if not experiments:
        raise ValueError("No experiments found in the provided config files")
    return experiments


def load_benchmarks_yaml(registry_path: Path, repo_root: Path) -> dict[str, dict]:
    with open(registry_path) as f:
        data = yaml.safe_load(f) or {}
    if not isinstance(data, dict):
        raise ValueError(f"{registry_path}: top-level must be a mapping")
    # Resolve relative paths against the repo root.
    for name, spec in data.items():
        raw = spec.get("path", "")
        if not raw:
            raise ValueError(f"benchmark '{name}' missing 'path'")
        resolved = Path(raw)
        if not resolved.is_absolute():
            resolved = (repo_root / raw).resolve()
        spec["path"] = resolved
        spec.setdefault("glob", "*")
        spec.setdefault("type", "elf")
        if spec["type"] not in ("elf", "trace", "snapshot"):
            raise ValueError(
                f"benchmark '{name}': type must be 'elf', 'trace', or 'snapshot', "
                f"got {spec['type']!r}")
    return data


def materialize_benchmarks(
    registry: dict[str, dict],
    names: list[str],
    workload_prefixes: Optional[list[str]],
) -> list[Benchmark]:
    out: list[Benchmark] = []
    for name in names:
        if name not in registry:
            raise ValueError(
                f"benchmark '{name}' not in registry (known: {sorted(registry)})")
        spec = registry[name]
        path: Path = spec["path"]
        if not path.is_dir():
            raise FileNotFoundError(f"benchmark '{name}' path {path} does not exist")
        files = sorted(path.glob(spec["glob"]))
        if workload_prefixes:
            files = [f for f in files if any(f.name.startswith(p) for p in workload_prefixes)]
        if not files:
            raise FileNotFoundError(
                f"benchmark '{name}' has no files matching {spec['glob']} under {path}")
        out.append(Benchmark(name=name, type=spec["type"], path=path, workload_files=files))
    return out


def materialize_ad_hoc_benchmark(
    kind: str, directory: Path, workload_prefixes: Optional[list[str]]
) -> Benchmark:
    if kind == "elf":
        patterns = ("*.elf",)
        bench_name = "elfs"
    else:
        patterns = ("*-trace.csv.zst", "*.csv.zst", "*.csv")
        bench_name = "traces"
    files: list[Path] = []
    for pat in patterns:
        files.extend(directory.glob(pat))
    files = sorted(set(files))
    if workload_prefixes:
        files = [f for f in files if any(f.name.startswith(p) for p in workload_prefixes)]
    if not files:
        raise FileNotFoundError(f"no {kind} files found under {directory}")
    return Benchmark(name=bench_name, type=kind, path=directory, workload_files=files)


# ---------------------------------------------------------------------------
# study directory + binary snapshots
# ---------------------------------------------------------------------------

def build_result_dir(args, study_name: str) -> Path:
    if args.result_dir:
        base = Path(args.result_dir).expanduser().resolve()
    else:
        user = getpass.getuser() or "unknown"
        root = Path(args.result_root).expanduser()
        if not root.is_absolute():
            root = REPO_ROOT / root  # relative result-root is relative to the repo
        base = (root / "experiments" / user / study_name).resolve()
    ts = datetime.datetime.now().strftime("%Y%m%d_%H%M%S")
    return base / f"{study_name}_{ts}"


def find_model_binary(repo_root: Path, model: str) -> Path:
    """Locate a built model binary. Single-core lands at build/<model>/<model>;
    the cluster (a `top` composite) lands at build/top/<model>/<model>."""
    candidates = [
        repo_root / "build" / model / model,
        repo_root / "build" / "top" / model / model,
    ]
    for src in candidates:
        if src.is_file() and os.access(src, os.X_OK):
            return src
    raise FileNotFoundError(
        f"model binary '{model}' not found/executable in any of "
        f"{[str(c) for c in candidates]} "
        f"(run scripts/build_scripts/build_model.sh --model {model} first)")


def snapshot_binary(repo_root: Path, model: str, dest: Path) -> Path:
    src_binary = find_model_binary(repo_root, model)
    dest.mkdir(parents=True, exist_ok=True)
    shutil.copy2(src_binary, dest / model)
    (dest / model).chmod(0o755)
    # Copy the binary's sibling config files so the run is self-contained. The
    # single-core model reads config.yaml; the cluster additionally reads its
    # core_config.yaml (referenced from config.yaml) and whisper.json. Grab every
    # .yaml/.json alongside the binary, skipping editor/backup/crash artifacts.
    for sib in sorted(src_binary.parent.iterdir()):
        if sib.suffix in (".yaml", ".json") and not sib.name.endswith(".bak"):
            shutil.copy2(sib, dest / sib.name)
    return dest


def instruction_count_files(args, benchmarks: list[Benchmark]) -> list[Path]:
    """--metadata-file, else any count file found beside the workload set."""
    if args.metadata_file:
        missing = [str(p) for p in args.metadata_file if not p.is_file()]
        if missing:
            raise SystemExit(f"error: --metadata-file not found: {', '.join(missing)}")
        return [p.resolve() for p in args.metadata_file]
    found = {w.parent.parent / "metadata.yml" for b in benchmarks for w in b.workload_files}
    return sorted(p.resolve() for p in found if p.is_file())


# ---------------------------------------------------------------------------
# command-line generation
# ---------------------------------------------------------------------------

def model_cli_for_workload(
    exp: Experiment, bench: Benchmark, workload: Path, binary_dir: Path,
    stats_json: Path,
) -> str:
    """Build the inner `./<model> ...` command (executed inside binary_dir).

    Always appends `--report-all <stats_json> json` so Sparta dumps the full
    `top.core0` statistic tree; the task wrapper compresses it to
    `<stats_json>.bz2` afterwards.
    """
    parts: list[str] = [f"./{exp.model}"]
    if exp.num_cycles is not None:
        parts.extend(["-r", str(exp.num_cycles)])
    if exp.num_instructions is not None:
        parts.extend(["-i", str(exp.num_instructions)])
    for cfg in exp.config_files:
        parts.extend(["-c", cfg])
    for opt in exp.model_options:
        parts.append(opt)
    parts.extend(["--report-all", shlex.quote(str(stats_json)), "json"])
    parts.extend([bench.model_flag(), str(workload)])
    return " ".join(parts)


def wrap_task_cmd(
    inner_cmd: str, binary_dir: Path, log_path: Path, max_log_bytes: str,
    stats_json: Path,
) -> str:
    """Wrap the model invocation with /usr/bin/time, size cap, and bz2 log.

    The raw JSON the model writes to `stats_json` is bzip2'd to
    `<stats_json>.bz2` after the run. The task exits with the model's status.
    Past the cap the log is drained, not closed: closing it kills the model with SIGPIPE.
    """
    run = f"cd {shlex.quote(str(binary_dir))} && {inner_cmd}"
    sj = shlex.quote(str(stats_json))
    return (
        f"/usr/bin/time -v bash -o pipefail -c {shlex.quote(run)} "
        f"2>&1 | {{ head --bytes={max_log_bytes}; cat > /dev/null; }} "
        f"| bzip2 > {shlex.quote(str(log_path))}; "
        f"rc=${{PIPESTATUS[0]}}; bzip2 -f {sj}; exit $rc"
    )


def workload_base(workload: Path) -> str:
    base = workload.name
    # drop obvious trace extensions but keep ELFs intact
    for suffix in (".csv.zst", ".csv", ".elf"):
        if base.endswith(suffix):
            base = base[: -len(suffix)]
            break
    return base


def log_path_for(workload: Path, bench_dir: Path) -> Path:
    return bench_dir / f"{workload_base(workload)}-log.bz2"


def stats_json_path_for(workload: Path, bench_dir: Path) -> Path:
    """Uncompressed path the model writes via --report-all; wrapper bz2's it."""
    return bench_dir / f"{workload_base(workload)}.json"


def write_array_script(path: Path, commands: list[str]) -> None:
    body = ["#!/usr/bin/env bash", "set -u", "ulimit -c 0", "commands=("]
    for cmd in commands:
        body.append(shlex.quote(cmd))
    body.append(")")
    body.append('idx=$((SLURM_ARRAY_TASK_ID - 1))')
    body.append('eval "${commands[$idx]}"')
    path.write_text("\n".join(body) + "\n")
    path.chmod(0o755)


# ---------------------------------------------------------------------------
# main
# ---------------------------------------------------------------------------

def parse_args(argv: list[str]) -> argparse.Namespace:
    p = argparse.ArgumentParser(
        description=textwrap.dedent(__doc__ or "").strip(),
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    p.add_argument("study_name", help="Label for this study; used in the output dir name")
    p.add_argument("configs", nargs="+", type=Path,
                   help="One or more experiment config YAML files")
    p.add_argument("--benchmarks", default=None,
                   help="Comma-separated names from benchmarks.yml")
    p.add_argument("--benchmarks-yml", type=Path, default=SCRIPT_DIR / "benchmarks.yml",
                   help="Path to benchmarks.yml (default: alongside run.py)")
    p.add_argument("--elf-dir", type=Path, default=None,
                   help="Ad-hoc sweep of ELFs in a directory (skips benchmarks.yml)")
    p.add_argument("--trace-dir", type=Path, default=None,
                   help="Ad-hoc sweep of traces in a directory (skips benchmarks.yml)")
    p.add_argument("--benches", default=None,
                   help="Comma-separated workload-name prefixes to filter within a benchmark")
    p.add_argument("--mem-per-slot", type=float, default=DEFAULT_MEM_PER_SLOT_GB,
                   help="Per-task memory reservation in GB (default: %(default)s)")
    p.add_argument("--extra-gbytes", type=float, default=0.0,
                   help="Extra GB added on top of --mem-per-slot")
    p.add_argument("--partition", required=True,
                   help="Slurm partition to submit to")
    p.add_argument("--qos", default=DEFAULT_QOS,
                   help="Slurm QOS (default: %(default)s)")
    p.add_argument("--cluster", default=None,
                   help="Slurm cluster name (default: auto-detected via scontrol)")
    p.add_argument("--max-log-bytes", default=DEFAULT_MAX_LOG_BYTES,
                   help="Cap on per-task log bytes before bzip2 (default: %(default)s)")
    p.add_argument("--score-ref", default=None, choices=sorted(SPEC_SECONDS),
                   help="SPEC suite the report scores against (default: auto-detect)")
    p.add_argument("--metadata-file", action="append", type=Path,
                   help="Per-input dynamic instruction counts (YAML) for report scores; repeatable")
    p.add_argument("--result-root", default=DEFAULT_RESULT_ROOT,
                   help="Base directory for default --result-dir")
    p.add_argument("--result-dir", type=Path, default=None,
                   help="Override base result directory (else <result-root>/experiments/<user>/<study>)")
    p.add_argument("--dry-run", action="store_true",
                   help="Generate the study dir + scripts but skip sbatch")
    p.add_argument("--blocking", action="store_true",
                   help="Wait for submitted jobs, then run parse_slurm_results.py and perf_report.py")
    return p.parse_args(argv)


def select_benchmarks(args: argparse.Namespace) -> list[Benchmark]:
    prefixes = [s.strip() for s in args.benches.split(",")] if args.benches else None
    ad_hoc: list[Benchmark] = []
    if args.elf_dir:
        ad_hoc.append(materialize_ad_hoc_benchmark("elf", args.elf_dir.resolve(), prefixes))
    if args.trace_dir:
        ad_hoc.append(materialize_ad_hoc_benchmark("trace", args.trace_dir.resolve(), prefixes))
    if args.benchmarks:
        names = [s.strip() for s in args.benchmarks.split(",") if s.strip()]
        registry = load_benchmarks_yaml(args.benchmarks_yml, REPO_ROOT)
        ad_hoc.extend(materialize_benchmarks(registry, names, prefixes))
    if not ad_hoc:
        raise SystemExit(
            "error: pick workloads via --benchmarks NAME[,NAME...], --elf-dir DIR, or --trace-dir DIR")
    return ad_hoc


def main(argv: list[str]) -> int:
    sys.stdout.reconfigure(line_buffering=True)  # --blocking is usually backgrounded into a log
    args = parse_args(argv)

    cluster = args.cluster or ("DRY-RUN" if args.dry_run else detect_cluster_name())

    experiments = load_experiment_configs(args.configs)
    benchmarks = select_benchmarks(args)
    counts = instruction_count_files(args, benchmarks)

    study_dir = build_result_dir(args, args.study_name)
    study_dir.mkdir(parents=True, exist_ok=True)
    print(f"study directory: {study_dir}")

    # Preserve the experiment configs alongside the results.
    for cfg in args.configs:
        shutil.copy2(cfg, study_dir / cfg.name)

    write_settings(study_dir, args.study_name, args.score_ref, counts)
    write_notebook(study_dir, args.study_name, args.score_ref, counts, REPO_ROOT)

    # Snapshot each distinct model binary into the study dir.
    binary_dirs: dict[str, Path] = {}
    for exp in experiments:
        if exp.model in binary_dirs:
            continue
        binary_dirs[exp.model] = snapshot_binary(
            REPO_ROOT, exp.model, study_dir / "_binaries" / exp.model)

    mem_mb = int(round((args.mem_per_slot + args.extra_gbytes) * 1024))
    total_tasks = 0
    submitted_job_ids: list[str] = []

    for exp in experiments:
        exp_dir = study_dir / exp.config_name
        exp_binary_dir = exp_dir / "binary"
        exp_binary_dir.mkdir(parents=True, exist_ok=True)
        # Per-experiment binary so each run is self-contained; hardlinked, the model is ~200 MB.
        for item in binary_dirs[exp.model].iterdir():
            try:
                os.link(item, exp_binary_dir / item.name)
            except OSError:
                shutil.copy2(item, exp_binary_dir / item.name)
            if item.name == exp.model:
                (exp_binary_dir / item.name).chmod(0o755)

        for bench in benchmarks:
            bench_dir = exp_dir / bench.name
            bench_dir.mkdir(parents=True, exist_ok=True)

            commands: list[str] = []
            for workload in bench.workload_files:
                log_path = log_path_for(workload, bench_dir)
                stats_json = stats_json_path_for(workload, bench_dir)
                inner = model_cli_for_workload(exp, bench, workload, exp_binary_dir, stats_json)
                commands.append(
                    wrap_task_cmd(inner, exp_binary_dir, log_path, args.max_log_bytes, stats_json))

            script_path = study_dir / f"{exp.config_name}_{bench.name}_cmds.sh"
            write_array_script(script_path, commands)
            total_tasks += len(commands)

            comment = f"{args.study_name}_{exp.config_name}_{bench.name}"
            sbatch_args = [
                "-p", args.partition,
                "--qos", args.qos,
                "--cpus-per-task=1",
                f"--mem-per-cpu={mem_mb}M",
                "-a", f"1-{len(commands)}",
                "--clusters", cluster,
                "--chdir=/tmp",
                "--output=none",
                "--error=none",
                "--comment", comment,
                str(script_path),
            ]
            job_id = submit_sbatch(sbatch_args, dry_run=args.dry_run)
            print(f"  {exp.config_name} x {bench.name}: {len(commands)} task(s), job {job_id}")
            if not args.dry_run:
                submitted_job_ids.append(job_id)

    print()
    if args.dry_run:
        print(f"[dry-run] generated {total_tasks} task(s) across {study_dir}")
        return 0
    print(f"submitted {total_tasks} task(s) across {len(submitted_job_ids)} array job(s)")

    if args.blocking:
        print("waiting for jobs to drain from the Slurm queue...")
        for jid in submitted_job_ids:
            wait_on_job(jid, cluster)
        print("all jobs done; parsing results")
        parser_path = REPO_ROOT / "scripts" / "parse_scripts" / "parse_slurm_results.py"
        rc = subprocess.call([sys.executable, str(parser_path), str(study_dir)])
        if rc != 0:
            print(f"parse_slurm_results.py exited {rc}", file=sys.stderr)
            return rc
        rc = subprocess.call([sys.executable, str(PARSE_SCRIPTS / "perf_report.py"), str(study_dir)])
        if rc != 0:
            print(f"perf_report.py exited {rc}", file=sys.stderr)
            return rc
    else:
        print(f"results will land in {study_dir}")
        print("when jobs complete, run:")
        print(f"  {REPO_ROOT}/scripts/parse_scripts/parse_slurm_results.py {study_dir}")
        print(f"  {PARSE_SCRIPTS}/perf_report.py {study_dir}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
