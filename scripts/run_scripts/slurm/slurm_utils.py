"""Thin wrappers around the Slurm CLI used by the RPM runner.

All functions shell out to squeue / sbatch / scontrol rather than linking to a
Slurm library so this file stays dependency-free.
"""
from __future__ import annotations

import re
import shutil
import subprocess
import time
from typing import Iterable, Optional


_JOB_ID_RE = re.compile(r"Submitted batch job\s+(\d+)")


def detect_cluster_name() -> str:
    """Return the ClusterName reported by `scontrol show config`."""
    out = subprocess.run(
        ["scontrol", "show", "config"],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        timeout=30,
        check=True,
    ).stdout
    for line in out.splitlines():
        key, sep, val = line.partition("=")
        if sep and key.strip() == "ClusterName":
            name = val.strip()
            if name:
                return name
    raise RuntimeError("scontrol show config did not report a ClusterName")


def parse_job_id(sbatch_output: str) -> str:
    """Pull the numeric job id out of `sbatch` stdout."""
    m = _JOB_ID_RE.search(sbatch_output)
    if not m:
        raise RuntimeError(f"Could not find a job id in sbatch output: {sbatch_output!r}")
    return m.group(1)


def task_ids_to_slurm_spec(task_ids: Iterable[int]) -> str:
    """Collapse a list of 1-based task ids into Slurm array-range notation.

    E.g. [1,2,3,4,7,9,10,11] -> "1-4,7,9-11". Empty input returns "".
    """
    ids = sorted(set(int(t) for t in task_ids))
    if not ids:
        return ""
    pieces: list[str] = []
    run_start = ids[0]
    prev = ids[0]
    for tid in ids[1:]:
        if tid == prev + 1:
            prev = tid
            continue
        pieces.append(f"{run_start}" if run_start == prev else f"{run_start}-{prev}")
        run_start = tid
        prev = tid
    pieces.append(f"{run_start}" if run_start == prev else f"{run_start}-{prev}")
    return ",".join(pieces)


def submit_sbatch(argv: list[str], *, dry_run: bool = False) -> str:
    """Run sbatch and return the parsed job id. argv should NOT start with 'sbatch'."""
    cmd = ["sbatch", *argv]
    pretty = " ".join(cmd)
    if dry_run:
        print(f"[dry-run] {pretty}")
        return "DRY-RUN"
    if shutil.which("sbatch") is None:
        raise RuntimeError("sbatch not found in PATH")
    print(f"submitting: {pretty}")
    result = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    combined = (result.stdout or "") + (result.stderr or "")
    if result.returncode != 0:
        raise RuntimeError(f"sbatch failed (exit {result.returncode}):\n{combined}")
    return parse_job_id(combined)


def job_still_queued(job_id: str, cluster: Optional[str] = None) -> bool:
    """True if `squeue` still knows about the job (pending or running)."""
    cmd = ["squeue", "--states=all", "-h", "-j", str(job_id)]
    if cluster:
        cmd.extend(["--clusters", cluster])
    result = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    if result.returncode != 0:
        # squeue emits this once the job has fully left the queue
        if "Invalid job id" in (result.stderr or ""):
            return False
        print(result.stderr, end="")
        return True
    return bool((result.stdout or "").strip())


def wait_on_job(job_id: str, cluster: Optional[str] = None, poll_seconds: int = 60) -> None:
    """Block until the given job is no longer in the Slurm queue."""
    while job_still_queued(job_id, cluster):
        time.sleep(poll_seconds)
