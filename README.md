# RPM — RISC-V Performance Model

A cycle-level RISC-V CPU performance simulation framework built on [Sparta](https://github.com/sparcians/map) (MAP) and [Whisper](https://github.com/tenstorrent/whisper) ISS.

## Quick Start

```bash
git clone <repository-url> rpm
cd rpm
git submodule update --init --recursive
bash scripts/build_scripts/build_all.sh
./build/core/core 500 tests/programs/burst_8.elf
```

Or via Docker (no local dependencies needed):

```bash
bash ci/docker_build.sh --test
```

## Running Simulations

### Basic Usage

```bash
# ELF execution-driven (legacy positional syntax)
./build/core/core 500 tests/programs/burst_8.elf

# Named flags (equivalent)
./build/core/core -r 500 -c models/cpu/src/core/config.yaml --target-elf tests/programs/burst_8.elf
```

### SPEC Simpoint Traces

```bash
# Run a 20M-instruction SPEC simpoint (instruction limit with -i)
./build/core/core -r 100000000 -i 20000000 \
  -c models/cpu/src/core/config.yaml \
  --trace-file /path/to/traces/gcc_r-ref-inp4-i20M-k100-s758-w04251-trace.csv.zst
```

The model automatically derives the snapshot folder from the trace filename
(parses simpoint ID and interval) and loads the Whisper memory/register snapshot.

### Key Command-Line Flags

| Flag | Description |
|------|-------------|
| `-r CYCLES` | Run for up to CYCLES (default: 100) |
| `-i INSTRS` | Stop after retiring INSTRS instructions (0 = no limit) |
| `--cpu-freq GHZ` | CPU clock frequency (default: 3.0 GHz) |
| `-c CONFIG.yaml` | Load a Sparta config file |
| `-p PATH VALUE` | Override a single parameter |
| `--trace-file F` | Trace file (.csv.zst) for trace-driven mode |
| `--target-elf F` | ELF binary for execution-driven mode |
| `--report-all F` | Write all stats to file F after simulation |
| `--show-tree` | Print the device tree |

### Logging

Use `-l` to enable per-unit logging:

```bash
# Log ExecutionDriver info messages
./build/core/core -r 1000 --target-elf tests/programs/burst_8.elf \
  -l top.core0.edriver info edriver.log

# Log all writeback activity
./build/core/core -r 1000 --target-elf tests/programs/burst_8.elf \
  -l top.core0.writeback info wb.log
```

### Stats and Reports

```bash
# Generate stats report
./build/core/core -r 100000000 -i 1000000 \
  -c models/cpu/src/core/config.yaml \
  --trace-file /path/to/trace.csv.zst \
  --report-all stats.txt
```

Key stats include: `num_retired`, IPC (from heartbeat output), fetch/decode/issue/execute/writeback
pipeline counters, cache hit rates, and branch prediction accuracy.

### Using run_sim.sh

```bash
# Trace-driven run
bash scripts/run_scripts/run_sim.sh -m core -n 20000000 -t /path/to/trace.csv.zst

# ELF execution-driven run
bash scripts/run_scripts/run_sim.sh -m core -n 1000 -e tests/programs/burst_8.elf
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
./build/core/core -r 1000 -c models/cpu/src/core/config_inorder.yaml --target-elf tests/programs/burst_8.elf
```

## Pipeline Architecture

```
                              Core Pipeline (core0)
  ┌─────────────────────────────────────────────────────────────────────────────┐
  │                                                                             │
  │  ┌───────────┐    ┌────────┐    ┌────────┐    ┌────────┐    ┌────────┐     │
  │  │           │───▶│        │───▶│        │───▶│        │───▶│        │     │
  │  │   Fetch   │    │ ICache │    │ Decode │    │ Rename │    │ Issue  │     │
  │  │           │◀───│        │    │        │    │        │◀───│        │     │
  │  └─────┬─────┘    └────┬───┘    └────────┘    └───┬────┘    └───┬────┘     │
  │        │               │                          │             │          │
  │        │          ┌────┴───┐                      │        ┌────▼────┐     │
  │        ├─────────▶│ Branch │                      │        │         │     │
  │        │◀─────────│  Pred  │                      │        │ Execute │     │
  │        │          └────────┘                      │        │         │     │
  │        │                                          │        └──┬──┬───┘     │
  │        │                                          │           │  │         │
  │        │                                     ┌────▼────┐      │  │         │
  │        │                                     │  Write  │◀─────┘  │         │
  │        │                                     │  back   │         │         │
  │        │                                     │  (ROB)  │    ┌────▼────┐    │
  │        │                                     └─────────┘    │         │    │
  │        │                                                    │   LSQ   │    │
  │        │                                                    │         │    │
  │        │                                                    └────┬────┘    │
  │        │                                                         │         │
  │        │          ┌─────────┐                              ┌─────▼────┐    │
  │        │          │         │                              │          │    │
  │        └─────────▶│ L2Cache │◀─────────────────────────────│  DCache  │    │
  │                   │         │                              │          │    │
  │                   └─────────┘                              └──────────┘    │
  │                                                                            │
  │  ┌───────────────┐    ┌────────────────────┐                               │
  │  │ PipelineClock │    │ ExecutionDriver    │                               │
  │  │  (3 GHz)      │    │ (Whisper ISS)      │                               │
  │  └───────────────┘    └────────────────────┘                               │
  └─────────────────────────────────────────────────────────────────────────────┘
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
└── tests/                          # Tests
    └── programs/                   # Microbenchmark ELFs (built from source)
```

## Build Scripts

All scripts are in `scripts/build_scripts/` and accept `--help`.

| Script | Purpose |
|--------|---------|
| `setup_env.sh` | Check that all prerequisites and submodules are available |
| `build_deps.sh` | Build external dependencies (Sparta, Whisper, trace-reader) |
| `build_model.sh` | Build a specific model (`--model simple\|core`) or `--all` |
| `build_all.sh` | One-shot: env check + deps + all models + test programs |

```bash
# Clean rebuild in Debug mode
bash scripts/build_scripts/build_model.sh --model core --clean --type Debug

# Build with custom parallelism
bash scripts/build_scripts/build_deps.sh --jobs 8
```

## Prerequisites

Run `bash scripts/build_scripts/setup_env.sh` to verify. You need:

- **Build tools:** CMake >= 3.17, Make, g++-13 (C++23), git
- **Libraries:** Boost >= 1.74, yaml-cpp >= 0.7, RapidJSON >= 1.1, SQLite3 >= 3.19, zlib, HDF5 >= 1.10

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
