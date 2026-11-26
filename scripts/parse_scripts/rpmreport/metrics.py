# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: 2026 Tenstorrent USA, Inc.

"""Reference times, metric and top-down definitions over aggregated core-0 counters."""
from __future__ import annotations

import re
from dataclasses import dataclass
from pathlib import Path
from typing import Callable, Optional

# SPEC reference-machine run times in seconds, as published with every result on spec.org.
SPEC_SECONDS = {
    "spec2006": {  # CINT2006
        "perlbench": 9770, "bzip2": 9650, "gcc": 8050, "mcf": 9120, "gobmk": 10490,
        "hmmer": 9330, "sjeng": 12100, "libquantum": 20720, "h264ref": 22130,
        "omnetpp": 6250, "astar": 7020, "xalancbmk": 6900,
    },
    "spec2017": {  # SPECrate2017 Integer and Floating Point
        "perlbench_r": 1592, "gcc_r": 1416, "mcf_r": 1616, "omnetpp_r": 1312,
        "xalancbmk_r": 1056, "x264_r": 1751, "deepsjeng_r": 1146, "leela_r": 1656,
        "exchange2_r": 2620, "xz_r": 1080,
        "bwaves_r": 10028, "cactuBSSN_r": 1266, "namd_r": 950, "parest_r": 2616,
        "povray_r": 2335, "lbm_r": 1054, "wrf_r": 2240, "blender_r": 1523, "cam4_r": 1749,
        "imagick_r": 2487, "nab_r": 1683, "fotonik3d_r": 3897, "roms_r": 1589,
    },
}
SUITE_PATH_TOKENS = {"spec2006": ("2006", "2k6"), "spec2017": ("2017", "2k17")}


def reference_time(suite: str, bench: str) -> Optional[float]:
    return SPEC_SECONDS[suite].get(re.sub(r"^\d+\.", "", bench))


def detect_suite(benchmarks: set[str], counts_files: list[Path]) -> tuple[Optional[str], str]:
    """(suite, how it was chosen): the suite naming the most benchmarks; a year token in a
    counts-file path breaks a tie, else the newer suite wins. Other benchmarks stay unscored."""
    cover = {s: sum(reference_time(s, b) is not None for b in benchmarks) for s in SPEC_SECONDS}
    most = max(cover.values(), default=0)
    if not most:
        return None, "no SPEC benchmark names"
    tied = [s for s in SPEC_SECONDS if cover[s] == most]
    named = [s for s in tied if any(t in str(p).lower() for p in counts_files for t in SUITE_PATH_TOKENS[s])]
    suite = (named or tied)[-1]
    how = f"auto-detected: {most} of {len(benchmarks)} benchmark names are in it"
    if most < len(benchmarks):
        how += "; only those are scored"
    return suite, how


# ---------------------------------------------------------------------------
# metrics
# ---------------------------------------------------------------------------

P = "top.core0."


def div(a: float, b: float) -> Optional[float]:
    return a / b if b else None


@dataclass(frozen=True)
class Metric:
    key: str
    label: str
    fn: Callable                   # Counters -> Optional[float]
    better: Optional[str] = None   # "higher" | "lower": colouring only
    counters: tuple[str, ...] = ()  # shown only if each occurs in some dump


def _pki(key: str, label: str, path: str, better: Optional[str] = None) -> Metric:
    return Metric(key, label, lambda c: div(1000.0 * c[P + path], c.instructions), better, (P + path,))


def _rate(key: str, label: str, a: str, b: str, better: Optional[str] = None) -> Metric:
    return Metric(key, label, lambda c: div(c[P + a], c[P + a] + c[P + b]), better, (P + a, P + b))


def _sum_defined(*xs: Optional[float]) -> Optional[float]:
    return None if any(x is None for x in xs) else sum(xs)


_BR, _MISP = P + "writeback.num_retired_branches", P + "writeback.num_retired_mispredicts"

METRICS = {m.key: m for m in [
    Metric("ipc", "IPC", lambda c: div(c.instructions, c.cycles), "higher"),
    Metric("cycles_per_inst", "CPI", lambda c: div(c.cycles, c.instructions), "lower"),
    _pki("mispredicts_pki", "Branch mispredicts PKI", "writeback.num_retired_mispredicts", "lower"),
    Metric("branch_accuracy", "Branch prediction accuracy",
           lambda c: div(c[_BR] - c[_MISP], c[_BR]), "higher", (_BR, _MISP)),
    _pki("branch_pki", "Retired branches PKI", "writeback.num_retired_branches"),
    Metric("fetch_per_retired", "Fetched per retired instruction",
           lambda c: div(c[P + "fetch.num_fetched"], c.instructions), "lower", (P + "fetch.num_fetched",)),
    _pki("redirects_pki", "Frontend redirects PKI", "fetch.num_redirects", "lower"),
    _pki("l1i_mpki", "I-cache MPKI", "icache.num_misses", "lower"),
    _rate("l1i_hit_rate", "I-cache hit rate", "icache.num_hits", "icache.num_misses", "higher"),
    _pki("l1d_mpki", "D-cache MPKI", "dcache.num_misses", "lower"),
    _rate("l1d_hit_rate", "D-cache hit rate", "dcache.num_hits", "dcache.num_misses", "higher"),
    _pki("l2c_mpki", "L2 MPKI", "l2cache.num_misses", "lower"),
    _rate("l2c_hit_rate", "L2 hit rate", "l2cache.num_hits", "l2cache.num_misses", "higher"),
    _pki("load_pki", "Loads PKI", "lsq.num_loads"),
    _pki("store_pki", "Stores PKI", "lsq.num_stores"),
    Metric("forward_rate", "Store-to-load forward rate",
           lambda c: div(c[P + "lsq.num_forwards"], c[P + "lsq.num_loads"]), None,
           (P + "lsq.num_forwards", P + "lsq.num_loads")),
    _pki("forward_pki", "Store-to-load forwards PKI", "lsq.num_forwards"),
    _pki("issue_stall_cycles_pki", "Issue stall cycles PKI", "issue.num_stall_cycles", "lower"),
    _pki("zero_dispatch_cycles_pki", "Zero-dispatch cycles PKI", "rename.num_zero_dispatch_cycles", "lower"),
    _rate("local_route_rate", "Dependency-local routing rate",
          "issue.num_dep_local_routes", "issue.num_dep_local_fallback"),
    _rate("oldest_first_rate", "Oldest-first select rate",
          "issue.num_oldest_selected", "issue.num_not_oldest_selected"),
    _pki("predictor_flush_pki", "Branch-predictor flushes PKI", "flush_arbiter.num_bp_flushes", "lower"),
    _pki("execute_flush_pki", "Execute flushes PKI", "flush_arbiter.num_exe_flushes", "lower"),
]}
METRICS["flush_pki"] = Metric(
    "flush_pki", "All flushes PKI",
    lambda c: _sum_defined(METRICS["predictor_flush_pki"].fn(c), METRICS["execute_flush_pki"].fn(c)),
    "lower", METRICS["predictor_flush_pki"].counters + METRICS["execute_flush_pki"].counters)

SUMMARY = ["ipc", "mispredicts_pki", "branch_accuracy", "l1i_mpki", "l1d_mpki", "l2c_mpki", "l1d_hit_rate"]
GROUPS = {
    "Branch prediction": ["branch_accuracy", "mispredicts_pki", "branch_pki"],
    "Frontend": ["fetch_per_retired", "redirects_pki", "l1i_mpki", "l1i_hit_rate"],
    "Memory hierarchy": ["l1d_hit_rate", "l1d_mpki", "l2c_hit_rate", "l2c_mpki", "load_pki",
                         "store_pki", "forward_rate", "forward_pki"],
    "Issue": ["issue_stall_cycles_pki", "zero_dispatch_cycles_pki", "local_route_rate",
              "oldest_first_rate"],
    "Flushes": ["predictor_flush_pki", "execute_flush_pki", "flush_pki"],
}
PER_INPUT = ["ipc", "cycles_per_inst", "mispredicts_pki", "branch_accuracy", "l1d_mpki"]


# ---------------------------------------------------------------------------
# top-down (Yasin, ISPASS 2014), level 1 and 2, as fractions of RPM's dispatch slots
# ---------------------------------------------------------------------------

def _slots(c, path: str) -> Optional[float]:
    return div(c[P + "rename." + path], c[P + "rename.topdown_slots"])


def _frac(path: str) -> Callable:
    return lambda c: _slots(c, path)


def _retiring(c) -> Optional[float]:
    return div(c.instructions, c[P + "rename.topdown_slots"])  # RPM counts uops as instructions


def _wasted(c) -> Optional[float]:
    return div(max(c[P + "rename.num_dispatched"] - c.instructions, 0.0), c[P + "rename.topdown_slots"])


def _memory_split(level: int) -> Callable:
    def fn(c) -> Optional[float]:
        bound = _slots(c, "topdown_backend_memory_slots")
        if bound is None:
            return None
        any_, l1, l2 = (c[P + f"writeback.retire_stall_load_{k}"] for k in ("any", "l1_miss", "l2_miss"))
        parts = (max(any_ - l1, 0.0), max(l1 - l2, 0.0), l2)
        return bound * parts[level] / sum(parts) if sum(parts) else 0.0
    return fn


def _minus(a: Callable, b: Callable) -> Callable:
    return lambda c: None if a(c) is None else a(c) - b(c)


_recovery = _frac("topdown_recovery_slots")
_frontend = _frac("topdown_frontend_bound_slots")
_fe_latency = _frac("topdown_frontend_latency_slots")
_backend = _frac("topdown_backend_bound_slots")
_backend_memory = _frac("topdown_backend_memory_slots")

TOPDOWN = [
    Metric("retiring", "Retiring", _retiring),
    Metric("bad_speculation", "Bad speculation", lambda c: _sum_defined(_wasted(c), _recovery(c))),
    Metric("wasted_work", "  wasted work", _wasted),
    Metric("recovery", "  recovery", _recovery),
    Metric("frontend_bound", "Frontend bound", _frontend),
    Metric("frontend_latency", "  latency", _fe_latency),
    Metric("frontend_bandwidth", "  bandwidth", _minus(_frontend, _fe_latency)),
    Metric("backend_bound", "Backend bound", _backend),
    Metric("memory_bound", "  memory bound", _backend_memory),
    Metric("core_bound", "  core bound", _minus(_backend, _backend_memory)),
]
TOPDOWN_L1 = ["retiring", "bad_speculation", "frontend_bound", "backend_bound"]
MEMORY_SPLIT = [
    Metric("load_l1", "L1 latency", _memory_split(0)),
    Metric("load_l2", "L2", _memory_split(1)),
    Metric("load_beyond_l2", "Beyond L2", _memory_split(2)),
]
STALLS = [
    Metric(f"stall_{i}", label, lambda c, p=path: div(c[p], c.cycles), None, (path,))
    for i, (label, path) in enumerate([
        ("ROB", P + "rename.dispatch_stall_rob_full"),
        ("Physical registers", P + "rename.dispatch_stall_prf_full"),
        ("Scheduler", P + "rename.dispatch_stall_scheduler_full"),
        ("Load queue", P + "rename.dispatch_stall_load_queue_full"),
        ("Store queue", P + "rename.dispatch_stall_store_queue_full"),
        ("Checkpoints", P + "rename.dispatch_stall_checkpoint_full"),
        ("Operands not ready", P + "rename.dispatch_stall_operands_not_ready"),
        ("Load at ROB head", P + "writeback.retire_stall_load_any"),
    ], start=1)
]
TOPDOWN_COUNTER = P + "rename.topdown_slots"
