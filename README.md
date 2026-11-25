# RPM — RISC-V Performance Model

A cycle-level RISC-V CPU performance simulation framework built on [Sparta](https://github.com/sparcians/map) (MAP) and [Whisper](https://github.com/tenstorrent/whisper) ISS.

## Overview

RPM is an execution-driven, cycle-level performance model for RISC-V cores. It
pairs a configurable, event-driven pipeline (built on the Sparta modeling
framework) with the Whisper instruction set simulator for functional execution,
letting you study microarchitectural trade-offs and measure performance on real
bare-metal workloads such as CoreMark and Dhrystone.

Highlights:

- **Configurable pipeline** — in-order or out-of-order execution, selectable
  issue policies, execute granularity and write-port arbitration, all driven
  from YAML config.
- **Pluggable branch prediction** — TAGE-SC-L (64 KB default, 8 KB), gshare or
  bimodal direction predictors, a set-associative BTB and a return-address
  stack, selected by name from config.
- **Execution-driven** — Whisper supplies functional results, so the model runs
  real ELF binaries to completion.
- **Modeled memory hierarchy** — probabilistic or detailed L1 I$/D$, a unified
  L2 (on by default), DRAM timing, TLBs with page walks, and prefetchers.
- **Observability** — per-unit logging, stats reports, and Konata pipeline
  visualization.

## Quick Start

```bash
git clone https://github.com/tenstorrent/tt-rpm.git
cd tt-rpm
git submodule update --init --recursive

# Build the dependencies + models with your host compiler. (The bare-metal test
# workloads are skipped here unless the RISC-V toolchain is already on PATH.)
bash scripts/build_scripts/build_all.sh

# Then build + run the bare-metal RISC-V test workloads. Install the toolchain
# in its own conda env (one-time) and activate it *after* the host build above,
# so it doesn't shadow the host g++/cmake. Run CoreMark to completion:
bash tests/install-toolchain-conda.sh   # creates conda env 'riscv'
conda activate riscv
make -C tests run_coremark
```

Or via Docker (no local dependencies needed):

```bash
bash ci/docker_build.sh --test
```

## Running Simulations

### Basic Usage

```bash
# Execution-driven: run an ELF to completion.
# -i 0 = no instruction limit; the run ends when the program exits via HTIF "tohost".
./build/core/core -i 0 -c models/cpu/src/core/config.yaml --target-elf tests/build/coremark.bare.elf

# Or bound the run: -r is a tick budget (1 tick = 1 ps, ~333 ticks per cycle at 3 GHz).
./build/core/core -r 2000000 -c models/cpu/src/core/config.yaml --target-elf tests/build/coremark.bare.elf
```

### Key Command-Line Flags

| Flag | Description |
|------|-------------|
| `-r TICKS` | Run for up to TICKS (1 tick = 1 ps, ~333 ticks/cycle at 3 GHz; omit to run until the program exits) |
| `-i INSTRS` | Stop after retiring INSTRS instructions (0 = no limit; run to program exit) |
| `--cpu-freq GHZ` | CPU clock frequency (default: 3.0 GHz) |
| `-c CONFIG.yaml` | Load a Sparta config file |
| `-p PATH VALUE` | Override a single parameter |
| `--target-elf F` | ELF binary for execution-driven mode |
| `--report-all F` | Write all stats to file F after simulation |
| `--show-tree` | Print the Sparta resource and parameter tree (units + their config params) |

### Logging

Use `-l` to enable per-unit logging:

```bash
# Log ExecutionDriver info messages
./build/core/core -r 200000 --target-elf tests/build/coremark.bare.elf \
  -l top.core0.edriver info edriver.log

# Log all writeback activity
./build/core/core -r 200000 --target-elf tests/build/coremark.bare.elf \
  -l top.core0.writeback info wb.log
```

### Stats and Reports

```bash
# Generate a stats report (run to completion, write all stats to a file)
./build/core/core -i 0 \
  -c models/cpu/src/core/config.yaml \
  --target-elf tests/build/coremark.bare.elf \
  --report-all stats.txt
```

Key stats include: `num_retired`, IPC (from heartbeat output), fetch/decode/issue/execute/writeback
pipeline counters, cache hit rates, and branch prediction accuracy.

### Top-Down Analysis

The core model classifies every dispatch slot (cycles × `rename` `dispatch_width`)
at the rename stage, which separates the frontend from the backend, following
the top-down method:

| Category | Slots |
|----------|-------|
| **Retiring** | Used by instructions that retired |
| **Bad Speculation** | Used by instructions that were squashed (*wasted work*), or left empty while recovering from a misprediction or pipeline flush (*recovery*) |
| **Frontend Bound** | Left empty because the frontend delivered no instruction: in cycles with nothing dispatched (*latency*) or with some instructions dispatched (*bandwidth*) |
| **Backend Bound** | Lost because a backend resource stopped dispatch: a full load/store queue or a load waiting for data at the ROB head (*memory*), or any other resource (*core*) |

A run of empty slots is attributed when it ends: to recovery when the next
instruction is the first one after a misprediction stall or a pipeline flush,
and to the frontend otherwise. The four level-1 categories add up to 100%.

The level-1 breakdown is printed with the stats summary (`TopDown:` line). For
the full breakdown, run `scripts/parse_scripts/topdown.py` on a stats report:

```bash
./build/core/core -i 0 -c models/cpu/src/core/config.yaml \
  --target-elf tests/build/coremark.bare.elf --report-all stats.txt
python3 scripts/parse_scripts/topdown.py stats.txt
```

```
stats.txt [top.core0]
  cycles=3,522,090  instructions=7,204,389  IPC=2.045  dispatch_width=4
    Retiring              51.14%  ###############...............
    Bad Speculation        1.16%  ..............................
      Wasted Work          0.28%
      Recovery             0.88%
    Frontend Bound        18.74%  ######........................
      Latency              7.08%
      Bandwidth           11.66%
    Backend Bound         28.96%  #########.....................
      Memory Bound        14.72%
      Core Bound          14.25%
  Dispatch stall cycles by backend resource (% of cycles):
    ...
  Events per 1000 instructions:
    ...
```

`topdown.py` accepts text and JSON reports (optionally `.bz2`) and directories,
which it searches for `*.json` / `*.json.bz2` reports, e.g. a study
directory; `--csv FILE` writes one row per run. The underlying counters are the
`topdown_*` and `dispatch_stall_*` statistics of `top.core0.rename`.

With `wrong_path_enabled: false`, wrong-path instructions are never fetched:
Fetch holds at a mispredicted branch until it resolves. Mispredictions
therefore show up as recovery slots only.

### Using run_sim.sh

```bash
# Execution-driven run (ELF); -n is a tick budget
bash scripts/run_scripts/run_sim.sh -m core -n 2000000 -e tests/build/coremark.bare.elf
```

Output is saved to `build/<model>/output/<timestamp>/`:
- `out.txt` — simulation stdout
- `ilog.txt` — simulation stderr (logs, warnings)

## Workloads

Every workload runs **execution-driven** — Whisper executes forward and drives
the model. There are two inputs:

| Input | Model flag | `core` (single-core) | `cluster` (multi-core) |
|-------|-----------|:--------------------:|:----------------------:|
| **ELF** (bare-metal) | `--target-elf` | ✅ | ✅ shared multi-hart |
| **Snapshot** — a Whisper snapshot directory | `--snapshot` | ✅ | ✅ topology inferred (see below) |

Whisper runs forward from the snapshot's restored state. On the **cluster**, the
Whisper topology is inferred from the snapshot's hart count:

- **multi-hart** snapshot (`registers0..registersN-1`) → one **shared** Whisper,
  one address space (realistic multi-core; core count must equal the hart count).
- **single-hart** snapshot (bare `registers`) → an **independent** Whisper per
  core, each loading the same state (N independent copies, no sharing).

Bare-metal ELFs are built here (`make -C tests`). On the cluster an ELF runs on
one shared Whisper with a hart per core; what harts above 0 do is up to the
program's start-up code (CoreMark parks them in a `wfi` loop, Dhrystone runs on
every hart). Snapshots are Whisper snapshot
directories; this repository does not include a tool that produces them or picks
simpoints. See "Snapshot workload sets" below for the layout the models expect.

### Running a workload directly

```bash
# Single-core, full-system snapshot
./build/core/core -i 20000000 -c models/cpu/src/core/config.yaml \
  --snapshot <set>/<dir>/<snapshot>

# Multi-core cluster, multi-hart snapshot (core count must match the snapshot's
# hart count -- one registers<i> file per hart). -i bounds the run.
./build/top/cluster/cluster \
  --cluster-config models/cluster/src/config_heterogeneous_3core.yaml \
  --snapshot <set>/<dir>/<snapshot> -i 500000
```

A run ends when a core's program exits (HTIF `tohost` or the exit syscall) or
commits the ROI-exit HINT `addi x0, x12, imm`; on the cluster the first core to
do so ends the whole run. A snapshot has no EOF, so unless the workload executes
the ROI-exit HINT, bound it with `-i`/`-r`. For heterogeneous cluster runs prefer
`-r` (a common tick budget) — with `-i N` the fastest core reaches the limit
first and ends the whole run.

### Snapshot workload sets

```
<set>/
  bin/whisper.json     Whisper config for the snapshotted system
  bin/fw_jump.elf      optional; passed to Whisper as --target when present
  <dir>/               any name
    <snapshot>/        one snapshot directory per workload, any name
```

A snapshot directory is what Whisper writes with `--snapshotdir` /
`--snapshotperiod` and restores with `--loadfrom`: `registers` (one hart) or
`registers0..registersN-1` (one per hart), `memory`, and Whisper's other state
files. The cluster prefers a `whisper.json` or `linux.json` inside the snapshot
directory over `bin/whisper.json`.

Pass the snapshot directory to `--snapshot`.

### Performance reports

`scripts/parse_scripts/perf_report.py` reads a study directory of JSON stat
dumps (`--report-all FILE json`, optionally bzip2-compressed) and writes a static
HTML report: SPEC score per GHz, metrics, top-down and per-input tables, with %
deltas against `--baseline`. `rpmreport` returns the same tables as pandas
DataFrames.

```
<study>/
  <config>/                    one directory per model configuration
    **/<workload>.json[.bz2]   e.g. mcf_r-ref-inp0-s1-w25000.json
  report_settings.yml          optional: suite, counts_files
```

A workload name is `<benchmark>[[-ref|-train|-test]-inp<N>][-s<simpoint>-w<weight>]`,
with the SimPoint weight scaled by 100000. A workload without a SimPoint suffix
is a whole-program run of weight 1, and one without an input id is the
benchmark's only input. SPEC scores also need each input's whole-program dynamic
instruction count (`--metadata-file`, YAML under `Dynamic-Insts:`).

```bash
scripts/parse_scripts/perf_report.py my_study --score-ref spec2017 \
  --metadata-file counts.yml --baseline base
```

## CPU Models

| Model | Directory | Description |
|-------|-----------|-------------|
| **core** | `models/cpu/src/core/` | Configurable pipeline supporting both in-order and out-of-order execution |
| **simple** | `models/cpu/src/simple/` | Minimal single-cycle CPU model for basic validation |

### config.yaml

The **core** model is configured via `models/cpu/src/core/config.yaml`. Parameters
are organized under `top.core0` (see
[CONFIG_README.md](models/cpu/src/core/CONFIG_README.md) for the full parameter
reference):

```yaml
top.core0:
  params:
    ooo_enabled: true           # false for in-order pipeline
  fetch:
    params:
      fetch_width: 16           # bytes per fetch group
  icache:
    params:
      hit_rate: 0.9
  rename:
    params:
      dispatch_width: 4
      num_phys_regs: 128
  issue:
    params:
      mode: unified             # unified | partitioned
      issue_queue_capacity: 36
  execute:
    params:
      granularity: unified      # unified | typed
  writeback:
    params:
      retire_width: 4
      rob_capacity: 128
```

A preset in-order config is at `models/cpu/src/core/config_inorder.yaml`:
```bash
./build/core/core -i 0 -c models/cpu/src/core/config_inorder.yaml --target-elf tests/build/coremark.bare.elf
```

## Pipeline Architecture

```
                                      RPM Architecture (core0)

                    ┌────┐
               ┌───▶│ I$ │───┐
    ┌────┐     │    └────┘   │   ┌────┐   ┌────┐   ┌────┐   ┌────┐   ┌────┐
    │ F  │─────┤             ├──▶│ D  │──▶│ Rn │──▶│ Is │──▶│ Ex │──▶│ Wb │
    └────┘     │    ┌────┐   │   └────┘   └────┘   └────┘   └─┬──┘   └────┘
               └───▶│BPU │───┘                                │
                    └────┘                                    ▼
                                                           ┌─────┐   ┌────┐
                                                           │ LSQ │──▶│ D$ │
                                                           └─────┘   └────┘

  Stages:    F   Fetch (PC-select mux, steered by redirects)
             I$  L1 instruction cache
             BPU branch prediction unit (F drives I$ and BPU in parallel; both feed D)
             D   Decode
             Rn  Rename (allocates from the physical register file, PRF)
             Is  Issue — age-ordered issue queue, ready bits, load balancing, spec wakeup
             Ex  Execute (functional units)
             Wb  Writeback / ROB (in-order retire)
             LSQ load/store queue (memory ops only)
             D$  L1 data cache

  Data flow: F ─▶ {I$, BPU} ─▶ D ─▶ Rn ─▶ Is ─▶ Ex ─▶ Wb
             memory ops: Ex ─▶ LSQ ─▶ D$
  Queues:    FetchQueue / DecodeQueue buffer I$→D and D→Rn (skipped when bypass_queues=true)
  Feedback:  execution results write back to the PRF (Rn)
             LSQ ⇄ ROB for memory completion/ordering
             branch resolution / flushes (Ex, Wb, via the FlushArbiter) redirect F's PC mux
             Wb ─▶ Rn commits / frees registers
  Memory:    L1 I$ and L1 D$ each fill from a shared L2 cache (on by default)
  Engine:    ExecutionDriver (Whisper ISS) supplies functional results
             PipelineClock ticks every stage (default 3 GHz).
```

### Pipeline Stages

| Stage | Unit | Description |
|-------|------|-------------|
| **Fetch** | `FetchStructures` | Fetches instruction groups, drives ICache requests |
| **ICache** | `FrontendMemoryStructures` | Probabilistic or detailed instruction cache model |
| **Branch Predictor** | `BranchPredictor` | Pluggable direction predictor (TAGE-SC-L by default), BTB and RAS |
| **Decode** | `DecodeStructures` | Decodes instructions, assigns latencies by type |
| **Rename** | `Rename` | Register renaming, physical register allocation, dispatch |
| **Issue** | `Issue` | Issue queue with age-matrix or FIFO selection; unified or partitioned |
| **Execute** | `Execute` | Unified, typed, or functional-unit execution with write-port arbitration |
| **LSQ** | `LSQ` | Load/store queue with optional store forwarding |
| **DCache** | `BackendMemoryStructures` | Probabilistic or detailed data cache model |
| **Writeback** | `Writeback` | ROB-based in-order retirement with instruction limit and stall watchdog |
| **L2Cache** | `L2Cache` | Unified L2 cache (on by default) |
| **ExecutionDriver** | `ExecutionDriver` | Whisper ISS integration for execution-driven simulation |
| **PipelineClock** | `PipelineClock` | Drives per-cycle tick across all pipeline stages |

### Configurable Features

- **Execution mode:** Out-of-order (default) or in-order via `ooo_enabled`
- **Execute granularity:** `unified` (single pool), `typed` (INT/FP/VEC/BR groups)
- **Write ports:** `unified` (shared) or `mapped` (per-FU-group assignment)
- **Issue selection:** `age_matrix`, `fifo`, or `random`
- **Branch predictor:** `predictor_type` (`tage_sc_l_64kb`, `tage_sc_l_8kb`,
  `gshare`, `bimodal`, or `probabilistic`), `btb_type`, `ras_type`
- **Writeback buffer:** Optional buffering between execute and ROB commit
- **Register file banking:** Configurable bank count and port limits
- **Cache hierarchy:** Probabilistic or detailed ICache/DCache; L2 and DRAM
  timing each switchable with `enabled`
- **ROB stall watchdog:** Detects deadlocks (default: 10,000 cycle timeout)
- **CPU clock frequency:** Configurable via `--cpu-freq` (default: 3.0 GHz)

## Directory Structure

```
tt-rpm/
├── CMakeLists.txt                  # Top-level CMake (builds all models)
├── .clang-format                   # Code formatting rules
│
├── build/                          # Out-of-source build outputs
│
├── ci/                             # CI infrastructure
│   ├── check_format.sh             # Clang-format checker
│   ├── docker_build.sh             # Local Docker build/test helper
│   └── dockerfiles/Dockerfile      # CI Docker image with all build deps
│
├── ext/                            # External dependencies (git submodules)
│   ├── map/                        # Sparta simulation framework
│   ├── whisper/                    # Whisper RISC-V ISS
│   └── konata/                     # Konata pipeline viewer
│
├── models/                         # Core simulation models
│   ├── cmake/                      # Shared CMake find-modules
│   ├── cpu/
│   │   ├── accel/                  # Accelerator models (DMA engine) for the cluster
│   │   ├── common/                 # Shared: ExecutionDriver, Instruction, types
│   │   ├── predictors/             # Branch-predictor algorithms (TAGE-SC-L, gshare, BTB, RAS)
│   │   ├── prefetch/               # Prefetcher algorithms (next-line, IP-stride, stream)
│   │   ├── tlb/                    # Set-associative TLB
│   │   └── src/
│   │       ├── core/               # Configurable in-order / OOO core model
│   │       └── simple/             # Minimal single-cycle CPU model
│   ├── analysis/                   # (reserved for future expansion) analysis models
│   ├── cluster/                    # Multi-core cluster: N cores + shared cache, coherence, multi-clock
│   ├── fabric/                     # (reserved for future expansion) interconnect fabric
│   ├── memory_model/               # (reserved for future expansion) memory subsystem model
│   ├── profiler/                   # (reserved for future expansion) profiler
│   ├── soc/                        # (reserved for future expansion) SoC-level model
│   └── sys/                        # (reserved for future expansion) system-level model
│
├── scripts/                        # Build, run & analysis scripts
│   ├── build_scripts/
│   ├── run_scripts/
│   └── parse_scripts/
│
└── tests/                          # Bare-metal workloads and regression checks
    ├── Makefile                    # Builds + runs the workloads in the core model
    ├── accel/                      # DMA doorbell microbench for the cluster
    ├── coremark/                   # CoreMark submodule (+ bare-metal patch)
    ├── dhrystone/                  # util.h shim for the bare-metal Dhrystone build
    ├── riscv-tests/                # riscv-tests submodule (Dhrystone source)
    ├── rv32/                       # Bare-metal startup + linker script (from riscv-tests)
    └── unittests/                  # Smoke checks with recorded metric bounds
```

## Build Scripts

All scripts are in `scripts/build_scripts/`; `build_deps.sh` and `build_model.sh`
accept `--help`.

| Script | Purpose |
|--------|---------|
| `setup_env.sh` | Check that all prerequisites and submodules are available |
| `build_deps.sh` | Build external dependencies (Sparta, Whisper, trace-reader) |
| `build_model.sh` | Build a specific model (`--model simple\|core\|cluster`) or `--all` |
| `build_all.sh` | One-shot: env check + deps + all models + test workloads |

```bash
# Clean rebuild in Debug mode
bash scripts/build_scripts/build_model.sh --model core --clean --type Debug

# Build with custom parallelism
bash scripts/build_scripts/build_deps.sh --jobs 8
```

## Tests

The `tests/` directory builds bare-metal **CoreMark** and **Dhrystone** ELFs
(no OS; crt0 + linker script at `0x80000000`, console/exit via HTIF `tohost`)
and runs them in the core model:

```bash
make -C tests                 # build both ELFs -> tests/build/
make -C tests run_coremark    # build + run CoreMark in the core model
make -C tests run_dhrystone   # build + run Dhrystone in the core model
make -C tests clean
```

Built ELFs land in `tests/build/` (e.g. `tests/build/coremark.bare.elf`).

Building the workloads requires the **bare-metal (newlib) RISC-V toolchain**
`riscv64-unknown-elf-gcc`. A Linux/glibc cross-compiler such as
`riscv64-unknown-linux-gnu-gcc` will **not** work — it emits glibc-linked code
(unresolved `strcmp@GLIBC`, etc.) that cannot run on the bare-metal model.
Install it into a Conda env named `riscv` and activate it before building:

```bash
bash tests/install-toolchain-conda.sh   # one-time: creates conda env 'riscv'
conda activate riscv                     # puts riscv64-unknown-elf-gcc on PATH
make -C tests run_coremark               # then build + run as above
```

The Makefile defaults `CC` to `riscv64-unknown-elf-gcc` (resolved from the
activated env). For a toolchain installed elsewhere, override it:
`make -C tests CC=/path/to/riscv64-unknown-elf-gcc`.

## Prerequisites

Run `bash scripts/build_scripts/setup_env.sh` to verify. You need:

- **Build tools:** CMake >= 3.17, Make, g++-13 (C++23), git
- **Libraries:** Boost >= 1.74, yaml-cpp >= 0.7, RapidJSON >= 1.1, SQLite3 >= 3.19, zlib, HDF5 >= 1.10
- **Tests (optional):** bare-metal (newlib) RISC-V toolchain `riscv64-unknown-elf-gcc`, installed via `tests/install-toolchain-conda.sh` (Conda env `riscv`). A glibc `*-linux-gnu-gcc` will not work.

On Ubuntu 22.04:
```bash
sudo add-apt-repository ppa:ubuntu-toolchain-r/test
sudo apt install cmake g++-13 libboost-all-dev libyaml-cpp-dev rapidjson-dev \
    libsqlite3-dev zlib1g-dev libhdf5-dev
```

Or use the Docker image (`ci/dockerfiles/Dockerfile`) which has everything pre-installed.

## CI / Docker

The repo includes a self-contained CI setup. See `ci/` for:

- **Dockerfile** with all build dependencies
- **check_format.sh** for clang-format validation
- **docker_build.sh** for local build/test via Docker

```bash
# Local Docker workflow
bash ci/docker_build.sh              # build
bash ci/docker_build.sh --test       # build + smoke test
bash ci/docker_build.sh --format     # check formatting
bash ci/docker_build.sh --shell      # interactive shell
```

## External Dependencies

Managed as git submodules in `ext/`. Initialize with:
```bash
git submodule update --init --recursive
```

| Submodule | Description |
|-----------|-------------|
| `ext/map` | [Sparta/MAP](https://github.com/sparcians/map) — simulation framework (branch: `map_v2`) |
| `ext/whisper` | [Whisper](https://github.com/tenstorrent/whisper) — RISC-V ISS for execution driving |
| `ext/konata` | Konata pipeline viewer |

## Publication

RPM was presented at the Open-Source Computer Architecture Research
([OSCAR](https://oscar-workshop.github.io/Program_2026.html)) workshop,
co-located with ISCA 2026 (June 28, 2026, Raleigh, NC, USA):

> Sahil Gandhi, Hieu Mai, Anirudh Jain, Jiyong Yu and Vignyan Kothinti.
> **RPM: RISC-V Performance Model.** OSCAR Workshop at ISCA 2026.
> [[Poster presentation](https://oscar-workshop.github.io/files/P_7_RPM.pdf)]

If you use RPM in your research, please cite:

```bibtex
@inproceedings{rpm-oscar2026,
  title     = {{RPM}: {RISC-V} Performance Model},
  author    = {Gandhi, Sahil and Mai, Hieu and Jain, Anirudh and Yu, Jiyong and Kothinti, Vignyan},
  booktitle = {Open-Source Computer Architecture Research (OSCAR) Workshop, co-located with ISCA 2026},
  address   = {Raleigh, NC, USA},
  month     = jun,
  year      = {2026},
  url       = {https://github.com/tenstorrent/tt-rpm}
}
```

## Contributing

Contributions are welcome! Please read [CONTRIBUTING.md](CONTRIBUTING.md) for
build setup, coding conventions (including the required SPDX license headers),
and the pull-request process. All participants are expected to follow our
[Code of Conduct](CODE_OF_CONDUCT.md).

## Security

To report a security vulnerability, please follow the process described in
[SECURITY.md](SECURITY.md). Do not open public issues for security reports.

## License

This project is licensed under the Apache License, Version 2.0. See
[LICENSE](LICENSE) for the full text and [NOTICE](NOTICE) for attribution of
third-party components. See also [LICENSE_understanding.txt](LICENSE_understanding.txt).

The dependencies under `ext/` and the benchmark/test suites under `tests/`
(CoreMark, riscv-tests) are third-party components included as git submodules
and remain under their own respective licenses.
