# RPM Slurm runner

Submit RPM simulation jobs to a Slurm cluster as array jobs, one array per
`(experiment, benchmark)` pair.

## Prerequisites

- A built model, e.g. `scripts/build_scripts/build_model.sh --model core`.
- Access to a Slurm cluster (`sbatch`, `squeue`, `scontrol` on `$PATH`).
- Python 3 with `pyyaml`. The study notebook also needs `pandas` and Jupyter; the
  HTML report does not.
- The study dir (`--result-root`, default `results/` in the repo) and every workload
  path (including `tests/build` for `microbench`, built with `make -C tests`) on a
  filesystem the compute nodes mount. Otherwise tasks fail at once and leave no log.

## Quick start

```bash
# 1. Build the model you want to run
scripts/build_scripts/build_model.sh --model core

# 2. Submit an experiment sweep against the microbench ELFs
scripts/run_scripts/slurm/run.py my_study \
    configs/experiment_configs/core_baseline.yml \
    --benchmarks microbench --partition <partition>

# 3. When jobs finish (or pass --blocking above), aggregate stats
scripts/parse_scripts/parse_slurm_results.py \
    results/experiments/$USER/my_study/my_study_<ts>

# 4. ...and write performance_report.html into the study dir
scripts/parse_scripts/perf_report.py results/experiments/$USER/my_study/my_study_<ts>
```

For interactive analysis, open `<study>_performance_study.ipynb` in the study dir.

## Statistics dump

Every task always runs the model with `--report-all <workload>.json json`, so
Sparta writes the full `top.core0` statistic tree (plus `clocks.*`) to JSON. The
task wrapper bzip2's it to `<workload>.json.bz2` next to the log. The
performance report reads core 0's counters (`top.core0.*`) and the `clocks.*`
and `scheduler.*` statistics from these dumps; on the cluster model it ignores
the other cores. Cycles and frequency come from core 0's clock; a cluster
configured without per-core clocks has none, so the report counts cycles on the
root clock and shows no frequency.

## Output layout

```
<study>_<ts>/
  _binaries/<model>/...                  # one canonical snapshot per model
  <experiment.yml copies>
  <exp>/
    binary/{<model>, config.yaml}        # per-experiment snapshot
    <bench>/
      <workload>-log.bz2                 # per-task captured stdout+stderr
      <workload>.json.bz2                # per-task Sparta statistic tree
      stats.csv                          # produced by parse_slurm_results.py
  <exp>_<bench>_cmds.sh                  # Slurm array script (one per job)
  report_settings.yml                    # study name, --score-ref, instruction-count files
  <study>_performance_study.ipynb        # notebook over the study's results
  summary.csv                            # produced by parse_slurm_results.py
  performance_report.html                # produced by perf_report.py
```

## Performance report

```bash
scripts/parse_scripts/perf_report.py [STUDY_DIR] [--baseline CONFIG] [--score-ref spec2006|spec2017]
    [--metadata-file FILE ...] [--compare LABEL=DIR ...] [--out FILE]
```

writes one self-contained `performance_report.html` (default: in the study dir)
covering every config of the study:

- **Headline:** SPEC score per GHz per benchmark and its geometric mean or, if
  nothing is scored, IPC.
- **Summary**, **top-down** (level 1 and 2, memory bound split by the level of
  the load blocking retirement, dispatch stalls), **branch prediction**,
  **frontend**, **memory hierarchy**, **issue** and **flushes** tables, each
  per benchmark with a mean row.
- **Per input** values, and **host runtime** per benchmark with the slowest
  workloads (a workload far above its config's median may have livelocked).

| Flag | Meaning |
|---|---|
| `--baseline CONFIG` | Add % change columns against this config (or comparison column). |
| `--score-ref SUITE` | `spec2006` (CINT2006) or `spec2017` (SPECrate2017 int + fp); empty auto-detects. |
| `--metadata-file FILE` | Instruction-count file; repeatable, later files win. |
| `--no-metadata-file` | Ignore the instruction-count files recorded with the study. |
| `--compare LABEL=DIR` | Add another study's configs as columns; repeatable. The first compared study's first column is the default baseline. |
| `--out FILE` | Output path. |

Defaults for `--score-ref` and `--metadata-file` come from the study's
`report_settings.yml`; without one (e.g. a hand-made study) the report uses
only the flags. Without a suite, the report picks the one that names the most
of the study's benchmarks (a year in an instruction-count file's path breaks a
tie) and scores only that suite's benchmarks; the header says which suite was
used and how it was chosen. A recorded instruction-count file that has since
moved or become unreadable is skipped with a warning.

How the numbers are formed:

- Each dump is one workload: a SimPoint (index and weight) of one input of a
  benchmark (see "Snapshot workload sets" in the top-level README). SimPoints of an
  input are averaged by weight; the inputs of a benchmark by their whole-program
  instruction counts (equally if any count is missing). Every metric is then
  computed from these averaged counters (a ratio of means), and is blank where
  its denominator is 0.
- A benchmark is scored and compared only in configs where it is **complete**:
  every input seen in any config or listed in the instruction-count file, every
  SimPoint any config has for that input, and SimPoint weights adding up to 1
  (±0.01). Incomplete pairs are listed in the report header, blank in the
  per-benchmark tables and left out of means; their per-input values are shown.
- Score per GHz is the SPEC reference time over the benchmark's run time on a
  1 GHz core: `ref_seconds / (sum_inputs(count / IPC) / 1e9)`. It needs every
  input's instruction count. The geometric mean covers the benchmarks scored in
  every column that has scores.
- Table means average the benchmarks with a value in every column; the header
  says how many that is. Compared studies with no complete benchmark in common
  have blank mean rows.

With `--compare`, every study's columns are labelled `<label>/<config>`, or just
`<label>` for a study with one config; the first study's label is its
directory name. `--baseline` (and the notebook API) also accept
`<label>/<config>` for a one-config study, and the first study's plain config
names.

The study's notebook does the same from Python. `scripts/parse_scripts/rpmreport`
returns every table as a pandas DataFrame, and `custom()` tabulates your own
function of the averaged counters:

```python
import sys; sys.path.insert(0, "<repo>/scripts/parse_scripts")
import rpmreport
r = rpmreport.load("<study_dir>", baseline="core_baseline")
rpmreport.show(r.headline())
rpmreport.show(r.custom(lambda c: c["top.core0.lsq.num_loads"] / c.cycles, "Loads per cycle"))
r.compare("<other_study_dir>", label="old")
r.write_html()
```

`tests/unittests/perf_report_test.py` checks the report against a small
hand-made study.

## Experiment YAML

Each entry describes one experiment that sweeps across every selected workload.

```yaml
- config_name: core_baseline
  model: core                         # build/core/core
  config_files: [config.yaml]         # optional extra -c files
  model_options:                      # optional -p / CLI tokens passed verbatim
    - "-p top.core0.writeback.params.retire_width 4"
  num_cycles: 20000000                # or num_instructions: 50000000
```

Multiple entries may live in a single file (YAML list or multi-document). With
neither `num_cycles` nor `num_instructions`, each workload runs until its program
ends (or the model config's own limit).

## Benchmark registry (`benchmarks.yml`)

```yaml
microbench:
  type: elf              # 'elf' -> --target-elf; 'snapshot' -> --snapshot
  path: tests/build
  glob: "*.elf"
```

`type` decides the model flag; both models run execution-driven:

| `type` | Model flag | `core` | `cluster` |
|--------|-----------|:------:|:---------:|
| `elf` | `--target-elf` | ✅ | ✅ (shared multi-hart) |
| `snapshot` | `--snapshot` (a Whisper snapshot directory) | ✅ | ✅ topology inferred from hart count |

On the cluster, a multi-hart snapshot (`registers0..N-1`) runs
one **shared** Whisper; a single-hart snapshot runs an **independent** Whisper
per core.

Snapshots never EOF, so they always honor the experiment's
`num_instructions`/`num_cycles` cap.

Ad-hoc sweeps that bypass the registry:

- `--elf-dir DIR` — run every `*.elf` under `DIR` in execution-driven mode.
  The model reads `whisper.json` from the ELF's directory, so `DIR` needs one
  (`tests/build/` gets it from `make -C tests`).
- `--trace-dir DIR` — hand every `*.csv.zst` / `*.csv` file under `DIR` to
  `--snapshot`.

`--benches prefix1,prefix2` further filters workload filenames.

## Adding a new workload (single-core and cluster)

1. **Lay out** a snapshot workload set as described under "Snapshot workload
   sets" in the top-level README: one Whisper snapshot directory per workload
   (`registers` for one hart, or one `registers<i>` per hart) in one directory.

2. **Register** it here in `benchmarks.yml` with `type: snapshot`, with a `glob`
   that matches the snapshot directories:

   ```yaml
   my_bench:                 # single-hart: core model, or independent per-core on the cluster
     type: snapshot
     path: /path/to/<set>/<dir>/
     glob: "*"

   my_bench_snap:            # multi-hart: cluster shared Whisper (realistic multi-core)
     type: snapshot
     path: /path/to/<set>/<dir>/
     glob: "*"
   ```

3. **Run** it in either model by picking the matching experiment config:

   ```bash
   # Single-core (model: core)
   run.py my_study configs/experiment_configs/core_baseline.yml \
       --benchmarks my_bench --partition <partition>

   # Multi-core cluster (model: cluster) -- for a multi-hart snapshot the cluster
   # config's core count must equal the snapshot's hart count (contiguous 0..N-1).
   run.py my_study configs/experiment_configs/cluster_3cores.yml \
       --benchmarks my_bench_snap --partition <partition>
   ```

   For heterogeneous cluster runs, bound with `num_cycles` (`-r`, a common tick
   window) rather than `num_instructions` (`-i`) — under `-i N` the fastest core
   reaches the limit first and terminates the whole run.

   A single-hart snapshot on the cluster starts one Whisper per core: size
   `--mem-per-slot` at about 3.5 GB per core (e.g. 16 for 3 cores).

## Useful flags

| Flag | Meaning |
|---|---|
| `--dry-run` | Generate the study dir + array scripts but skip `sbatch`. |
| `--blocking` | Wait for every submitted array job, then run the parser and the performance report. |
| `--mem-per-slot GB` | Per-task memory reservation (default 8). |
| `--extra-gbytes GB` | Extra memory added on top of `--mem-per-slot` (default 0). |
| `--max-log-bytes N` | Cap on each task's captured log before bzip2 (default `10M`); output past it is dropped. |
| `--partition NAME` | Slurm partition to submit to (required). |
| `--qos NAME` | Slurm QOS (default `normal`). |
| `--cluster NAME` | Override the auto-detected Slurm cluster. |
| `--result-root DIR` | Base for the study dir (default `results/` under the repo; must be on a shared FS). |
| `--result-dir DIR` | Base result directory, replacing `<result-root>/experiments/<user>/<study>`. |
| `--benchmarks-yml PATH` | Benchmark registry (default: `benchmarks.yml` next to `run.py`). |
| `--score-ref SUITE` | SPEC suite the performance report scores against, recorded in `report_settings.yml` (default: auto-detect). |
| `--metadata-file FILE` | Instruction-count file for scores, recorded in `report_settings.yml`; repeatable. |
