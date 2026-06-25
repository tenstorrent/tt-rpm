#!/usr/bin/env python3
"""Submit RPM simulation jobs to a Slurm cluster as an array of tasks.

Given one or more experiment config files and a set of benchmarks, this tool

  1. creates a timestamped study directory on the shared filesystem,
  2. snapshots each model binary + its config.yaml into the study dir,
  3. expands experiment x benchmark x workload into shell command lines,
  4. writes a Slurm array script (commands=[...] indexed by $SLURM_ARRAY_TASK_ID),
  5. submits one sbatch array job per (experiment, benchmark) pair.

Run with --dry-run to generate everything but skip sbatch. Run with --blocking
to wait on the jobs and then auto-invoke parse_slurm_results.py.

See scripts/run_scripts/slurm/README.md for full usage.
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
    task_ids_to_slurm_spec,  # noqa: F401  (kept for downstream/debug use)
    wait_on_job,
)

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
        if self.num_cycles is None and self.num_instructions is None:
            raise ValueError(
                f"experiment {self.config_name}: set either num_cycles or num_instructions")
        if self.num_cycles is not None and self.num_instructions is not None:
            raise ValueError(
                f"experiment {self.config_name}: num_cycles and num_instructions are mutually exclusive")


@dataclasses.dataclass
class Benchmark:
    name: str
    type: str               # 'elf'
    path: Path
    workload_files: list[Path]

    def model_flag(self) -> str:
        return "--target-elf"


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
        if spec["type"] != "elf":
            raise ValueError(
                f"benchmark '{name}': type must be 'elf', got {spec['type']!r}")
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
    directory: Path, workload_prefixes: Optional[list[str]]
) -> Benchmark:
    files: list[Path] = []
    for pat in ("*.elf",):
        files.extend(directory.glob(pat))
    files = sorted(set(files))
    if workload_prefixes:
        files = [f for f in files if any(f.name.startswith(p) for p in workload_prefixes)]
    if not files:
        raise FileNotFoundError(f"no ELF files found under {directory}")
    return Benchmark(name="elfs", type="elf", path=directory, workload_files=files)


# ---------------------------------------------------------------------------
# study directory + binary snapshots
# ---------------------------------------------------------------------------

def build_result_dir(args, study_name: str) -> Path:
    if args.result_dir:
        base = Path(args.result_dir).expanduser().resolve()
    else:
        user = getpass.getuser() or "unknown"
        base = Path(args.result_root).expanduser() / "experiments" / user / study_name
    ts = datetime.datetime.now().strftime("%Y%m%d_%H%M%S")
    return base / f"{study_name}_{ts}"


def snapshot_binary(repo_root: Path, model: str, dest: Path) -> Path:
    src_binary = repo_root / "build" / model / model
    src_config = repo_root / "build" / model / "config.yaml"
    if not src_binary.is_file() or not os.access(src_binary, os.X_OK):
        raise FileNotFoundError(
            f"model binary not found or not executable at {src_binary} "
            f"(run scripts/build_scripts/build_model.sh --model {model} first)")
    dest.mkdir(parents=True, exist_ok=True)
    shutil.copy2(src_binary, dest / model)
    (dest / model).chmod(0o755)
    if src_config.is_file():
        shutil.copy2(src_config, dest / "config.yaml")
    return dest


# ---------------------------------------------------------------------------
# performance-study notebook + report
# ---------------------------------------------------------------------------

NOTEBOOK_SRC = REPO_ROOT / "scripts" / "parse_scripts" / "notebook"
NOTEBOOK_TEMPLATE = NOTEBOOK_SRC / "performance_study_notebook.ipynb"
# study_report.py is both the analysis library and the report CLI; it carries
# the study placeholders, so it is templated rather than copied verbatim.
REPORT_SCRIPT = "study_report.py"
# Low-level parser library copied verbatim into each study root.
PARSER_PACKAGE = ("json_stats_parsing.py", "common.py", "core_profiles.py")


def _substitute_study_params(
    text: str, study_name: str, base_exp_name: str,
    metadata_file: Optional[Path], core: str = "rpm",
) -> str:
    """Fill the shared notebook/report placeholders."""
    text = text.replace("$base_exp_name", base_exp_name or "")
    text = text.replace("$path_to_metadata_file/metadata.yml",
                        str(metadata_file) if metadata_file else "")
    text = text.replace("$core", core or "rpm")
    return text.replace("<NAME>", study_name)


def instantiate_notebook(
    study_dir: Path, study_name: str,
    base_exp_name: str, metadata_file: Optional[Path], core: str = "rpm",
) -> None:
    """Copy the parser library into the study dir and drop the study tools.

    Both the notebook and ``study_report.py`` (run as the report CLI) parse
    `os.getcwd()`, so they must live alongside the parser library in the study
    root. study_report.py carries the study placeholders (score reference +
    metadata path), so it is templated rather than copied verbatim.
    """
    for item in PARSER_PACKAGE:
        src = NOTEBOOK_SRC / item
        if src.is_file():
            shutil.copy2(src, study_dir / item)

    report_src = NOTEBOOK_SRC / REPORT_SCRIPT
    if report_src.is_file():
        text = _substitute_study_params(
            report_src.read_text(), study_name, base_exp_name, metadata_file, core)
        out = study_dir / REPORT_SCRIPT
        out.write_text(text)
        out.chmod(0o755)
        print(f"  report library/CLI: {out}")

    if not NOTEBOOK_TEMPLATE.is_file():
        print(f"  [notebook] template missing at {NOTEBOOK_TEMPLATE}; skipping",
              file=sys.stderr)
        return
    text = _substitute_study_params(
        NOTEBOOK_TEMPLATE.read_text(), study_name, base_exp_name, metadata_file, core)
    out = study_dir / f"{study_name}_performance_study.ipynb"
    out.write_text(text)
    print(f"  notebook: {out}")


def generate_html_report(study_dir: Path) -> int:
    """Run the study's study_report.py CLI to emit performance_report.html."""
    report_script = study_dir / REPORT_SCRIPT
    if not report_script.is_file():
        print(f"  [report] {REPORT_SCRIPT} missing; skipping", file=sys.stderr)
        return 0
    print("generating HTML report")
    rc = subprocess.call([sys.executable, str(report_script)], cwd=str(study_dir))
    if rc != 0:
        print(f"  [report] {REPORT_SCRIPT} exited {rc}", file=sys.stderr)
    return rc


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
    `<stats_json>.bz2` after the run.
    """
    run = f"cd {shlex.quote(str(binary_dir))} && {inner_cmd}"
    sj = shlex.quote(str(stats_json))
    return (
        f"/usr/bin/time -v bash -o pipefail -c {shlex.quote(run)} "
        f"2>&1 | head --bytes={max_log_bytes} | bzip2 > {shlex.quote(str(log_path))}; "
        f"bzip2 -f {sj}"
    )


def workload_base(workload: Path) -> str:
    base = workload.name
    # strip the .elf extension to form the workload's base name
    for suffix in (".elf",):
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
    p.add_argument("--slurmtest", action="store_true",
                   help="Submit to the <partition>-test queue")
    p.add_argument("--cluster", default=None,
                   help="Slurm cluster name (default: auto-detected via scontrol)")
    p.add_argument("--max-log-bytes", default=DEFAULT_MAX_LOG_BYTES,
                   help="Cap on per-task log bytes before bzip2 (default: %(default)s)")
    p.add_argument("--score-ref", default="",
                   help="Score reference table for the study notebook (e.g. 'spec2006'); "
                        "empty auto-detects the suite from the benchmark names / "
                        "metadata path, falling back to IPC-only")
    p.add_argument("--metadata-file", type=Path, default=None,
                   help="metadata.yml with Dynamic-Insts for the notebook "
                        "(auto-detected from the workload directory when omitted)")
    p.add_argument("--core", default="rpm",
                   help="Core profile selecting the counter map for the study "
                        "notebook/report (e.g. rpm); "
                        "default: %(default)s")
    p.add_argument("--result-root", default=DEFAULT_RESULT_ROOT,
                   help="Base directory for default --result-dir")
    p.add_argument("--result-dir", type=Path, default=None,
                   help="Override base result directory (else <result-root>/experiments/<user>/<study>)")
    p.add_argument("--dry-run", action="store_true",
                   help="Generate the study dir + scripts but skip sbatch")
    p.add_argument("--blocking", action="store_true",
                   help="Wait for submitted jobs, then run parse_slurm_results.py")
    return p.parse_args(argv)


def select_benchmarks(args: argparse.Namespace) -> list[Benchmark]:
    prefixes = [s.strip() for s in args.benches.split(",")] if args.benches else None
    ad_hoc: list[Benchmark] = []
    if args.elf_dir:
        ad_hoc.append(materialize_ad_hoc_benchmark(args.elf_dir.resolve(), prefixes))
    if args.benchmarks:
        names = [s.strip() for s in args.benchmarks.split(",") if s.strip()]
        registry = load_benchmarks_yaml(args.benchmarks_yml, REPO_ROOT)
        ad_hoc.extend(materialize_benchmarks(registry, names, prefixes))
    if not ad_hoc:
        raise SystemExit(
            "error: pick workloads via --benchmarks NAME[,NAME...] or --elf-dir DIR")
    return ad_hoc


def main(argv: list[str]) -> int:
    args = parse_args(argv)

    cluster = args.cluster or detect_cluster_name()
    partition = f"{args.partition}-test" if args.slurmtest else args.partition

    experiments = load_experiment_configs(args.configs)
    benchmarks = select_benchmarks(args)

    study_dir = build_result_dir(args, args.study_name)
    study_dir.mkdir(parents=True, exist_ok=True)
    print(f"study directory: {study_dir}")

    # Preserve the experiment configs alongside the results.
    for cfg in args.configs:
        shutil.copy2(cfg, study_dir / cfg.name)

    # Drop a ready-to-run performance-study notebook into the study root.
    # With no explicit --metadata-file, leave the path empty: study_report.py
    # (and the notebook) derive the metadata.yml(s) from the study's own
    # workload paths, which covers studies spanning several workload sets (a
    # single pinned file would only score one suite and break the others).
    metadata_file = args.metadata_file
    if args.metadata_file and not args.metadata_file.is_file():
        print(f"warning: --metadata-file {args.metadata_file} not found", file=sys.stderr)
    instantiate_notebook(study_dir, args.study_name, args.score_ref, metadata_file, args.core)

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
        # Per-experiment binary copy so each run is fully self-contained.
        for item in binary_dirs[exp.model].iterdir():
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
                "-p", partition,
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
        generate_html_report(study_dir)
        print(f"report: {study_dir / 'performance_report.html'}")
    else:
        print(f"results will land in {study_dir}")
        print("when jobs complete, run:")
        print(f"  {REPO_ROOT}/scripts/parse_scripts/parse_slurm_results.py {study_dir}")
        print(f"  (cd {study_dir} && python {REPORT_SCRIPT})   # -> performance_report.html")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
