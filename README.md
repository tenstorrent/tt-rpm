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
  issue policies, execute granularity, write-port arbitration, and bypass paths,
  all driven from YAML config.
- **Execution-driven** — Whisper supplies functional results, so the model runs
  real ELF binaries to completion.
- **Modeled memory hierarchy** — probabilistic or detailed L1 I$/D$ and an
  optional unified L2.
- **Observability** — per-unit logging, stats reports, and Konata pipeline
  visualization.

## Quick Start

```bash
git clone <repository-url> rpm
cd rpm
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

# Or bound the run: -r is a tick budget (~3 ticks per cycle at 3 GHz).
./build/core/core -r 2000000 -c models/cpu/src/core/config.yaml --target-elf tests/build/coremark.bare.elf
```

### Key Command-Line Flags

| Flag | Description |
|------|-------------|
| `-r TICKS` | Run for up to TICKS (~3 ticks/cycle at 3 GHz; default: 100) |
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

### Using run_sim.sh

```bash
# Execution-driven run (ELF); -n is a tick budget
bash scripts/run_scripts/run_sim.sh -m core -n 2000000 -e tests/build/coremark.bare.elf
```

Output is saved to `build/<model>/output/<timestamp>/`:
- `out.txt` — simulation stdout
- `ilog.txt` — simulation stderr (logs, warnings)

## CPU Models

| Model | Directory | Description |
|-------|-----------|-------------|
| **core** | `models/cpu/src/core/` | Configurable pipeline supporting both in-order and out-of-order execution |
| **simple** | `models/cpu/src/simple/` | Minimal single-cycle CPU model for basic validation |

### config.yaml

The **core** model is configured via `models/cpu/src/core/config.yaml`. Parameters
are organized under `top.core0`:

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
      issue_queue_capacity: 32
  execute:
    params:
      granularity: typed        # unified | typed
  writeback:
    params:
      retire_width: 4
      rob_capacity: 128
```

A preset in-order config is at `models/cpu/src/core/config_inorder.yaml`:
```bash
./build/core/core -r 1000 -c models/cpu/src/core/config_inorder.yaml --target-elf tests/build/coremark.bare.elf
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
  Memory:    L1 I$ and L1 D$ each fill from a shared L2 cache (optional, off by default)
  Engine:    ExecutionDriver (Whisper ISS) supplies functional results
             PipelineClock ticks every stage (default 3 GHz).
```

### Pipeline Stages

| Stage | Unit | Description |
|-------|------|-------------|
| **Fetch** | `FetchStructures` | Fetches instruction groups, drives ICache requests |
| **ICache** | `FrontendMemoryStructures` | Probabilistic or detailed instruction cache model |
| **Branch Predictor** | `BranchPredictor` | Configurable accuracy branch prediction |
| **Decode** | `DecodeStructures` | Decodes instructions, assigns latencies by type |
| **Rename** | `Rename` | Register renaming, physical register allocation, dispatch |
| **Issue** | `Issue` | Issue queue with age-matrix or FIFO selection; unified or partitioned |
| **Execute** | `Execute` | Unified, typed, or functional-unit execution with write-port arbitration |
| **LSQ** | `LSQ` | Load/store queue with optional store forwarding |
| **DCache** | `BackendMemoryStructures` | Probabilistic or detailed data cache model |
| **Writeback** | `Writeback` | ROB-based in-order retirement with instruction limit and stall watchdog |
| **L2Cache** | `L2Cache` | Optional unified L2 cache |
| **ExecutionDriver** | `ExecutionDriver` | Whisper ISS integration for execution-driven simulation |
| **PipelineClock** | `PipelineClock` | Drives per-cycle tick across all pipeline stages |

### Configurable Features

- **Execution mode:** Out-of-order (default) or in-order via `ooo_enabled`
- **Execute granularity:** `unified` (single pool), `typed` (INT/FP/VEC/BR groups)
- **Write ports:** `unified` (shared) or `mapped` (per-FU-group assignment)
- **Issue selection:** `age_matrix`, `fifo`, or `random`
- **Bypass network:** Configurable producer-consumer bypass paths with latency
- **Writeback buffer:** Optional buffering between execute and ROB commit
- **Register file banking:** Configurable bank count and port limits
- **Cache hierarchy:** Probabilistic or detailed ICache/DCache/L2
- **ROB stall watchdog:** Detects deadlocks (default: 10,000 cycle timeout)
- **CPU clock frequency:** Configurable via `--cpu-freq` (default: 3.0 GHz)

## Directory Structure

```
rpm/
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
│   │   ├── common/                 # Shared: ExecutionDriver, Instruction, types
│   │   └── src/
│   │       ├── core/               # Configurable in-order / OOO core model
│   │       └── simple/             # Minimal single-cycle CPU model
│   ├── cluster/                    # Cluster-level models
│   ├── fabric/                     # Interconnect fabric
│   ├── memory_model/               # Memory subsystem model
│   └── soc/                        # SoC-level model
│
├── scripts/                        # Build, run & analysis scripts
│   ├── build_scripts/
│   ├── run_scripts/
│   └── parse_scripts/
│
├── configs/                        # Simulation configurations
│
└── tests/                          # Bare-metal CoreMark / Dhrystone workloads
    ├── Makefile                    # Builds + runs the workloads in the core model
    ├── coremark/                   # CoreMark submodule (+ bare-metal patch)
    └── riscv-tests/                # riscv-tests submodule (Dhrystone source)
```

## Build Scripts

All scripts are in `scripts/build_scripts/` and accept `--help`.

| Script | Purpose |
|--------|---------|
| `setup_env.sh` | Check that all prerequisites and submodules are available |
| `build_deps.sh` | Build external dependencies (Sparta, Whisper, trace-reader) |
| `build_model.sh` | Build a specific model (`--model simple\|core`) or `--all` |
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
