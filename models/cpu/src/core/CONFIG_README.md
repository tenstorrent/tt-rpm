# CPU Model Configuration Guide

This document provides comprehensive documentation for configuring the out-of-order (OOO) CPU performance model. The model is configured via a YAML file (`config.yaml`) that controls all aspects of the microarchitecture simulation.

## Table of Contents

1. [Overview](#overview)
2. [Configuration File Location](#configuration-file-location)
3. [Top-Level Structure](#top-level-structure)
4. [Execution Modes](#execution-modes)
5. [Frontend Configuration](#frontend-configuration)
   - [Fetch](#fetch)
   - [I-Cache](#i-cache)
   - [Decode](#decode)
   - [Branch Predictor](#branch-predictor)
6. [Midcore Configuration](#midcore-configuration)
   - [OOO vs In-Order Mode](#ooo-vs-in-order-mode)
   - [Rename](#rename)
   - [Issue](#issue)
   - [Execute](#execute)
   - [Load Store Queue (LSQ)](#load-store-queue-lsq)
   - [D-Cache](#d-cache)
   - [Writeback / ROB](#writeback--rob)
7. [Advanced Microarchitectural Features](#advanced-microarchitectural-features)
   - [Write Port Arbiter](#write-port-arbiter)
   - [Bypass Network](#bypass-network)
   - [Writeback Buffer](#writeback-buffer)
   - [Register File Banking](#register-file-banking)
8. [L2 Cache Configuration](#l2-cache-configuration)
9. [Visualization and Debugging](#visualization-and-debugging)
   - [Pipeline Visualizer](#pipeline-visualizer)
   - [Cache Viewer](#cache-viewer)
   - [Logging](#logging)
10. [Example Configurations](#example-configurations)

---

## Overview

The CPU model supports:
- **Out-of-Order (OOO) execution** with register renaming, issue queues, and reorder buffer
- **In-order execution** mode for simpler pipelines
- **Configurable cache hierarchy** (L1 I-cache, L1 D-cache, optional unified L2)
- **Detailed microarchitectural modeling** including bypass networks, banked register files, and write port arbitration

The model is driven by either:
- An **ELF binary** executed via the Whisper ISS
- A **pre-recorded trace file** for deterministic replay

---

## Configuration File Location

The model searches for `config.yaml` in these locations (in order):
1. Current working directory
2. Parent directory (`../config.yaml`)
3. Grandparent directory (`../../config.yaml`)
4. Executable directory
5. Executable's parent directory

If no configuration is found, built-in defaults are used.

---

## Top-Level Structure

```yaml
top:
  target_command: '/path/to/binary.elf'  # ELF to execute (empty for trace mode)
  trace_filename: ''                      # Trace file (empty for ELF mode)
  
  logging:
    enabled: false        # Enable debug logging for ALL units
  
  visualizer: { ... }     # Pipeline visualization settings
  cache_viewer: { ... }   # Cache event tracing settings
  
  frontend: { ... }       # Fetch, I-cache, Decode, Branch Predictor
  midcore: { ... }        # Rename, Issue, Execute, LSQ, D-cache, Writeback
  l2cache: { ... }        # Optional unified L2 cache
```

---

## Execution Modes

### ELF-Driven Mode
```yaml
top:
  target_command: '/path/to/program.elf'
  trace_filename: ''
```
The model runs the RISC-V ELF binary using Whisper as the golden ISS. Instructions are fetched from the binary and executed functionally by Whisper while the model tracks timing.

### Trace-Driven Mode
```yaml
top:
  target_command: ''
  trace_filename: '/path/to/trace.json'
```
The model replays a pre-recorded instruction trace. This provides deterministic, reproducible simulation for debugging and validation.

> **Note**: Command-line flags `--target` and `--trace` override the config file settings.

---

## Frontend Configuration

### Fetch

```yaml
frontend:
  fetch:
    initial_pc: 2147483648          # Starting PC (0x80000000 = RISC-V default)
    fetch_width: 32                 # Bytes fetched per cycle
    bp_mispred_penalty: 5           # Cycles lost on branch misprediction
    fetch_buffer_capacity: 128      # Fetch buffer size in bytes
    log_enabled: false              # Per-unit debug logging
```

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `initial_pc` | uint64 | 0x80000000 | Starting program counter. Overridden by ELF entry point when using ELF mode. |
| `fetch_width` | uint32 | 32 | Number of bytes fetched per cycle. Affects IPC for instruction-bandwidth-limited workloads. |
| `bp_mispred_penalty` | uint32 | 5 | Number of cycles to flush and restart fetch on branch misprediction. |
| `fetch_buffer_capacity` | uint32 | 128 | Size of the fetch buffer in bytes. Decouples fetch from decode. |

### I-Cache

```yaml
frontend:
  icache:
    cache_mode: probabilistic       # "probabilistic" or "structural"
    hit_rate: 1.0                   # Probabilistic mode: hit probability (0.0-1.0)
    hit_latency: 1                  # Cycles for cache hit
    miss_latency: 20                # Additional cycles for miss (no L2 fallback)
    mshr_capacity: 4                # Max outstanding misses
    cache_size_kb: 64               # Structural mode: cache size in KB
    cache_line_size: 64             # Line size in bytes
    cache_associativity: 8          # Set associativity
    replacement_policy: lru         # "lru" or "plru"
    rng_seed: 0xCAFE                # Seed for probabilistic-mode hit/miss RNG
    log_enabled: false
```

#### Cache Modes

**Probabilistic Mode** (`cache_mode: probabilistic`):
- Uses random sampling with configured `hit_rate` to determine hits/misses
- Fast simulation, good for early exploration
- No cache state tracking (no capacity/conflict misses)

**Structural Mode** (`cache_mode: structural`):
- Full cache simulation with tag array, sets, and replacement policy
- Accurate capacity and conflict miss modeling
- Required for realistic cache studies

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `cache_mode` | string | "probabilistic" | Simulation fidelity mode |
| `hit_rate` | float | 1.0 | Probabilistic mode hit probability |
| `hit_latency` | uint32 | 1 | Cycles to return data on hit |
| `miss_latency` | uint32 | 20 | Cycles added for miss (when no L2) |
| `mshr_capacity` | uint32 | 4 | Miss Status Holding Registers for outstanding misses |
| `cache_size_kb` | uint64 | 64 | Structural mode cache size |
| `cache_line_size` | uint64 | 64 | Cache line size in bytes |
| `cache_associativity` | uint64 | 8 | Ways per set |
| `replacement_policy` | string | "lru" | "lru" (true LRU) or "plru" (pseudo-LRU) |
| `rng_seed` | uint64 | 0xCAFE | Seed for the probabilistic-mode hit/miss RNG. |

### Decode

```yaml
frontend:
  decode:
    latencies:
      alu: 1        # ALU operations (add, sub, and, or, etc.)
      mul: 3        # Integer multiply
      div: 12       # Integer divide
      branch: 1     # Branch/jump
      load: 1       # Load address generation
      store: 1      # Store address generation  
      fp: 4         # Floating-point operations
      vec: 4        # Vector operations
      fence: 1      # Memory fences
    log_enabled: false
```

These latencies define the **execution latency** for each instruction type. The latency is the number of cycles from when an instruction enters Execute until its result is available.

| Operation | Typical Values | Notes |
|-----------|---------------|-------|
| `alu` | 1 | Simple integer ALU |
| `mul` | 3-4 | Pipelined multiplier |
| `div` | 10-30 | Iterative divider |
| `branch` | 1 | Branch resolution |
| `load` | 1 | Address computation only; cache latency is separate |
| `store` | 1 | Address computation |
| `fp` | 4-6 | FP add/mul/fma |
| `vec` | 4-8 | Vector operations |
| `fence` | 1 | Memory fence |

### Branch Predictor

```yaml
frontend:
  branch_predictor:
    accuracy: 0.95      # Branch prediction accuracy (0.0-1.0)
    rng_seed: 0xC0FFEE  # Seed for the prediction RNG (deterministic across runs)
    log_enabled: false
```

The model uses a **probabilistic branch predictor** that predicts correctly with the configured `accuracy`. This simplifies the model while allowing exploration of misprediction penalty impact.

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `accuracy` | float | 0.95 | Probability of correct prediction. 0.95 = 95% accurate. |
| `rng_seed` | uint64 | 0xC0FFEE | Seed for the prediction RNG; same seed → same sequence of (mis)predictions. |

---

## Midcore Configuration

### OOO vs In-Order Mode

```yaml
midcore:
  ooo_enabled: true     # true = OOO execution, false = in-order
```

**Out-of-Order Mode (`ooo_enabled: true`)**:
- Full register renaming with physical register file
- Issue queue with age-matrix selection (oldest-ready-first)
- Reorder buffer for in-order retirement
- Speculative execution

**In-Order Mode (`ooo_enabled: false`)**:
- Architectural scoreboard for hazard detection
- Instructions stall at Rename until operands are ready
- Simpler, faster simulation

### Rename

```yaml
midcore:
  rename:
    buffer_capacity: 16     # Rename buffer entries
    dispatch_width: 4       # Instructions renamed per cycle
    num_phys_regs: 128      # Physical registers per type (Int, FP, Vec)
    freelist_headroom: 8    # Min free regs before stalling
```

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `buffer_capacity` | uint32 | 16 | Entries in the rename buffer between Decode and Issue |
| `dispatch_width` | uint32 | 4 | Max instructions dispatched per cycle |
| `num_phys_regs` | uint32 | 128 | Number of physical registers per type (Int/FP/Vec) |
| `freelist_headroom` | uint16 | 8 | Minimum free physical registers before stalling rename |

### Issue

The Issue unit supports multiple modes for scheduler organization and selection policy.

```yaml
midcore:
  issue:
    mode: unified                   # "unified" or "partitioned"
    speculative_wakeup: true        # Enable speculative wakeup for non-memory ops
    selection: age_matrix           # "age_matrix", "fifo", or "random"
    issue_queue_capacity: 32        # Total IQ entries (unified mode)
    issue_width: 8                  # Max instructions issued per cycle
    log_enabled: false
```

#### Issue Modes

**Unified Mode** (`mode: unified`):
All instruction types share a single scheduler:
```yaml
issue:
  mode: unified
  issue_queue_capacity: 32
  issue_width: 8
```

**Partitioned Mode** (`mode: partitioned`):
Multiple schedulers with type-specific routing:
```yaml
issue:
  mode: partitioned
  schedulers:
    - name: int_sched
      capacity: 16
      types: [ALU, Mul, Div, Branch]
      accepts: [Load, Store]  # Can accept these via dependency routing
      ports:
        - name: int_p0
          types: [ALU, Mul, Div]
        - name: int_p1
          types: [ALU, Branch]
    - name: mem_sched
      capacity: 16
      types: [Load, Store]
      ports:
        - name: mem_p0
          types: [Load, Store]
    - name: fp_sched
      capacity: 8
      types: [FpOp, VecOp]
      ports:
        - name: fp_p0
          types: [FpOp, VecOp]
  
  routing:
    priorities:
      - dependency_locality    # Route to producer's scheduler
      - type_affinity          # Route to type's primary scheduler
      - least_occupied         # Load balance by occupancy
    dependency_occupancy_limit: 0.9  # Skip dep routing if >90% full
```

#### Selection Modes

| Mode | Description |
|------|-------------|
| `age_matrix` | Hardware-accurate age tracking; selects oldest ready instruction. Most realistic. |
| `fifo` | Simple queue order; lower hardware complexity model. |
| `random` | Random selection among ready instructions; useful for comparison studies. |

#### Speculative Wakeup

When `speculative_wakeup: true`, dependent instructions are woken up speculatively based on the producer's known execution latency. This hides the wakeup-to-select delay for non-memory operations.

For memory operations (loads), wakeup occurs when the cache responds since latency is unpredictable.

### Execute

The Execute unit models functional unit pools with configurable granularity.

```yaml
midcore:
  execute:
    granularity: typed    # "unified", "typed", or "functional"
    
    # Unified mode settings
    unified:
      max_in_flight: 8
    
    # Typed mode settings (4 groups)
    typed:
      int_max_in_flight: 6      # ALU, Mul, Div, Load, Store, Fence
      fp_max_in_flight: 2       # FP operations
      vec_max_in_flight: 2      # Vector operations
      branch_max_in_flight: 2   # Branch
    
    # Functional mode settings (8 groups)
    functional:
      alu_count: 2
      alu_max_in_flight: 4      # Total: 2 * 4 = 8 concurrent ALU ops
      mul_count: 1
      mul_max_in_flight: 2
      div_count: 1
      div_max_in_flight: 1
      branch_count: 1
      branch_max_in_flight: 2
      fp_count: 1
      fp_max_in_flight: 4
      vec_count: 1
      vec_max_in_flight: 4
      fence_count: 1
      fence_max_in_flight: 1
```

#### Execute Granularity

| Granularity | FU Groups | Description |
|-------------|-----------|-------------|
| `unified` | 1 | All types share one pool. Simplest model. |
| `typed` | 4 | Groups: Int, FP, Vec, Branch. Good balance of accuracy and simplicity. |
| `functional` | 8 | Per-FU-type groups: ALU, Mul, Div, Branch, LDST, FP, Vec, Fence. Most accurate. |

### Load Store Queue (LSQ)

```yaml
midcore:
  lsq:
    load_queue_capacity: 32       # Load queue entries
    store_queue_capacity: 24      # Store queue entries
    store_forwarding: true        # Enable store-to-load forwarding
    forwarding_latency: 1         # Cycles for forwarded data
    num_banks: 1                  # LSQ banks (1 = no banking)
    cache_line_size: 64           # For bank indexing
    pending_dispatch_capacity: 16 # Buffered ops when both LQ and SQ are full
    log_enabled: false
```

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `load_queue_capacity` | uint32 | 32 | Maximum in-flight loads |
| `store_queue_capacity` | uint32 | 24 | Maximum in-flight stores |
| `store_forwarding` | bool | true | Enable store-to-load forwarding from SQ |
| `forwarding_latency` | uint32 | 1 | Cycles for forwarded load to complete |
| `num_banks` | uint32 | 1 | Number of LSQ banks for parallelism |
| `pending_dispatch_capacity` | uint32 | 16 | Max ops buffered when both LQ and SQ are full before applying backpressure to Issue. |

#### Store-to-Load Forwarding

When enabled, loads check the store queue for older stores that fully cover the load's address range. Matching stores forward their data with `forwarding_latency` cycles delay, avoiding a cache access.

### D-Cache

```yaml
midcore:
  dcache:
    cache_mode: probabilistic     # "probabilistic" or "structural"
    hit_rate: 1.0                 # Probabilistic hit rate
    hit_latency: 1                # Cycles for hit
    miss_latency: 10              # Cycles for miss (no L2)
    mshr_capacity: 16             # Outstanding misses
    
    # Structural mode settings
    cache_size_kb: 64
    cache_line_size: 64
    cache_associativity: 8
    replacement_policy: lru       # "lru" or "plru"
    
    # Store buffer
    store_buffer_capacity: 8      # Entries in store buffer
    write_policy: write_back      # "write_back" or "write_through"
    
    # Banking (structural mode)
    num_banks: 1
    reads_per_bank_per_cycle: 1
    writes_per_bank_per_cycle: 1
    fill_occupies_write_port: false

    rng_seed: 0xBEEF              # Seed for probabilistic-mode hit/miss RNG
    log_enabled: false
```

#### Store Buffer

In structural mode, stores first enter the **store buffer**, allowing immediate completion from the pipeline's perspective. The store buffer drains to the cache in the background.

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `store_buffer_capacity` | uint32 | 8 | Store buffer entries |
| `write_policy` | string | "write_back" | "write_back" or "write_through" |

#### D-Cache Banking

Banking enables parallel cache accesses by partitioning the cache:

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `num_banks` | uint32 | 1 | Number of cache banks |
| `reads_per_bank_per_cycle` | uint32 | 1 | Read ports per bank |
| `writes_per_bank_per_cycle` | uint32 | 1 | Write ports per bank |
| `fill_occupies_write_port` | bool | false | Cache fills consume a write port |
| `rng_seed` | uint64 | 0xBEEF | Seed for the probabilistic-mode hit/miss RNG. |

### Writeback / ROB

```yaml
midcore:
  writeback:
    retire_width: 8         # Instructions retired per cycle
    rob_capacity: 128       # Reorder buffer entries
    log_enabled: false
```

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `retire_width` | uint32 | 4 | Max instructions retired per cycle |
| `rob_capacity` | uint32 | 128 | Reorder buffer capacity (limits in-flight instructions) |

---

## Advanced Microarchitectural Features

### Write Port Arbiter

Models contention for physical register file write ports between Execute and LSQ.

```yaml
midcore:
  write_ports:
    mode: unified           # "unified" or "mapped"
    num_ports: 4            # Number of write ports
```

#### Unified Mode
All FU groups and LSQ share all write ports with round-robin arbitration:
```yaml
write_ports:
  mode: unified
  num_ports: 4
```

#### Mapped Mode
Explicit port-to-source mapping:
```yaml
write_ports:
  mode: mapped
  mapping:
    - sources: [0, 1]       # Port 0: FU groups 0, 1
    - sources: [2, 3]       # Port 1: FU groups 2, 3
    - sources: [lsq]        # Port 2: LSQ only
    - sources: [0, 1, 2, 3, lsq]  # Port 3: all sources
```

### Bypass Network

Models result forwarding between functional units.

```yaml
midcore:
  bypass_network:
    enabled: true
    regfile_read_latency: 1   # Cycles to read from PRF without bypass
    paths:
      - producer: alu
        consumer: any         # "any" = all FU types
        latency: 0            # Same-cycle forwarding
      - producer: mul
        consumer: any
        latency: 0
      - producer: branch
        consumer: any
        latency: 0
      - producer: fp
        consumer: fp
        latency: 1            # FP-to-FP has 1 cycle bypass delay
```

#### How Bypass Works

Without bypass, a consumer must wait for:
1. Producer completes execution
2. Result written to PRF
3. Consumer reads from PRF (`regfile_read_latency` cycles)

With bypass:
1. Producer completes execution
2. Result forwarded directly to consumer (`path latency` cycles)

#### Bypass Path Configuration

| Field | Type | Description |
|-------|------|-------------|
| `producer` | string | Source FU type: `alu`, `mul`, `div`, `branch`, `fp`, `vec`, `fence`, `load` |
| `consumer` | string | Destination FU type, or `any` for all types |
| `latency` | uint8 | Cycles after producer completion until result available via bypass |

### Writeback Buffer

Decouples execution completion from register file write.

```yaml
midcore:
  writeback_buffer:
    enabled: true
    capacity: 16            # Buffer entries
    drain_width: 4          # Entries drained per cycle
    latency: 0              # Cycles in buffer before eligible for drain
```

When enabled, completed instructions enter the writeback buffer instead of immediately writing to the register file. This models the write-back pipeline stage and can help with write port contention.

### Register File Banking

Models banked register file with limited read/write ports per bank.

```yaml
midcore:
  regfile:
    banking_enabled: true
    num_banks: 4            # Number of register file banks
    reads_per_bank: 2       # Read ports per bank per cycle
    writes_per_bank: 1      # Write ports per bank per cycle
```

Physical registers are assigned to banks by `phys_reg % num_banks`. An instruction can only issue if all its source operands can be read (enough read ports available in each required bank).

---

## L2 Cache Configuration

Optional unified L2 cache serving both I-cache and D-cache.

```yaml
l2cache:
  enabled: true
  hit_latency: 10           # L2 hit latency
  miss_latency: 100         # DRAM latency
  mshr_capacity: 16         # L2 MSHRs
  cache_size_kb: 1024       # L2 size in KB
  cache_line_size: 64       # Must match L1 line size
  cache_associativity: 8
  replacement_policy: lru
  
  # Arbitration between I-cache and D-cache requests
  arb_policy: dcache_first  # "dcache_first", "icache_first", "round_robin"
  starvation_threshold: 8   # Cycles before starved source overrides priority
  queue_capacity: 4         # Pending requests per source
  
  # Inclusivity
  inclusivity_policy: nine  # "nine", "inclusive", "exclusive"
  
  log_enabled: false
```

### Arbitration Policies

| Policy | Description |
|--------|-------------|
| `dcache_first` | D-cache requests have priority, with starvation protection for I-cache |
| `icache_first` | I-cache requests have priority, with starvation protection for D-cache |
| `round_robin` | Alternates between I-cache and D-cache |

### Inclusivity Policies

| Policy | Description |
|--------|-------------|
| `nine` | Non-Inclusive, Non-Exclusive (default). L1 and L2 contents are independent. |
| `inclusive` | L2 is a superset of L1. L2 evictions trigger back-invalidation to L1. |
| `exclusive` | L1 and L2 are disjoint. Not fully implemented. |

When `inclusive` is set, L2 evictions send invalidation requests to both L1 caches to maintain inclusion.

---

## Visualization and Debugging

### Pipeline Visualizer

Generates instruction-level pipeline timing diagrams.

```yaml
visualizer:
  enabled: true
  max_instructions: 40        # Instructions to capture
  output_file: pipeline.txt   # Output file ("stderr" for console)
  format: waterfall           # "waterfall" or "table"
  color: false                # ANSI color codes
```

#### Output Formats

**Waterfall Format** (`format: waterfall`):
```
tag |  0  1  2  3  4  5  6  7  8  9
  1 |  F  D  R  I  X  W  .  .  .  .
  2 |  F  D  R  I  X  X  W  .  .  .
  3 |  F  D  R  R  I  X  W  .  .  .
```

**Table Format** (`format: table`):
Lists each instruction with cycle timestamps for each pipeline stage.

### Cache Viewer

Traces cache events for detailed analysis.

```yaml
cache_viewer:
  enabled: true
  format: waterfall           # "waterfall" or "log"
  output_file: cache.log      # Output file
  max_events: 100000          # Max events to capture (0 = unlimited)
  icache: true                # Trace I-cache
  dcache: true                # Trace D-cache
  l2cache: true               # Trace L2 cache
```

#### Events Captured
- Cache requests (hit/miss)
- MSHR allocations and coalescing
- Cache fills
- Evictions and writebacks
- L2 arbitration decisions

### Logging

Enable detailed debug logging per-unit or globally:

```yaml
top:
  logging:
    enabled: true   # Enable logging for ALL units
```

Or per-unit:
```yaml
frontend:
  fetch:
    log_enabled: true
  icache:
    log_enabled: true
```

Logs are written to stderr with unit-specific prefixes like `[fetch]`, `[issue]`, `[lsq]`, etc.

---

## Example Configurations

### High-Performance OOO Core

```yaml
top:
  target_command: '/path/to/benchmark.elf'
  
  frontend:
    fetch:
      fetch_width: 64
      fetch_buffer_capacity: 256
    icache:
      cache_mode: structural
      cache_size_kb: 64
      cache_associativity: 8
    branch_predictor:
      accuracy: 0.98
  
  midcore:
    ooo_enabled: true
    rename:
      dispatch_width: 8
      num_phys_regs: 256
    issue:
      mode: partitioned
      speculative_wakeup: true
      selection: age_matrix
      schedulers:
        - name: int
          capacity: 32
          types: [ALU, Mul, Div, Branch]
          ports:
            - types: [ALU, Mul, Div, Branch]
            - types: [ALU, Mul, Div, Branch]
        - name: mem
          capacity: 32
          types: [Load, Store]
          ports:
            - types: [Load, Store]
            - types: [Load, Store]
        - name: fp
          capacity: 16
          types: [FpOp, VecOp]
          ports:
            - types: [FpOp, VecOp]
    execute:
      granularity: functional
      functional:
        alu_count: 4
        alu_max_in_flight: 4
    lsq:
      load_queue_capacity: 64
      store_queue_capacity: 48
      store_forwarding: true
    dcache:
      cache_mode: structural
      cache_size_kb: 64
    writeback:
      retire_width: 8
      rob_capacity: 256
    
    bypass_network:
      enabled: true
      paths:
        - producer: alu
          consumer: any
          latency: 0
    
    write_ports:
      mode: unified
      num_ports: 8
  
  l2cache:
    enabled: true
    cache_size_kb: 2048
    hit_latency: 12
    miss_latency: 100
```

### Simple In-Order Core

```yaml
top:
  target_command: '/path/to/test.elf'
  
  frontend:
    fetch:
      fetch_width: 16
    icache:
      cache_mode: probabilistic
      hit_rate: 0.99
    branch_predictor:
      accuracy: 0.90
  
  midcore:
    ooo_enabled: false
    rename:
      dispatch_width: 2
    execute:
      granularity: unified
      unified:
        max_in_flight: 4
    lsq:
      load_queue_capacity: 8
      store_queue_capacity: 8
    dcache:
      cache_mode: probabilistic
      hit_rate: 0.95
    writeback:
      retire_width: 2
```

### Debugging Configuration

```yaml
top:
  target_command: '/path/to/test.elf'
  
  logging:
    enabled: true             # Enable all logging
  
  visualizer:
    enabled: true
    max_instructions: 100
    output_file: pipeline.txt
    format: waterfall
  
  cache_viewer:
    enabled: true
    output_file: cache.log
    format: waterfall
  
  frontend:
    icache:
      cache_mode: probabilistic
      hit_rate: 1.0           # Perfect I-cache for debugging
  
  midcore:
    ooo_enabled: true
    dcache:
      cache_mode: probabilistic
      hit_rate: 1.0           # Perfect D-cache for debugging
```

---

## Parameter Reference Quick List

### Frontend Parameters
| Section | Parameter | Default | Range/Values |
|---------|-----------|---------|--------------|
| fetch | fetch_width | 32 | 4-128 bytes |
| fetch | bp_mispred_penalty | 5 | 1-20 cycles |
| icache | cache_mode | probabilistic | probabilistic, structural |
| icache | hit_rate | 1.0 | 0.0-1.0 |
| icache | cache_size_kb | 64 | 1-256 KB |
| decode | lat_alu | 1 | 1-4 cycles |
| decode | lat_mul | 3 | 2-8 cycles |
| decode | lat_div | 12 | 8-40 cycles |
| branch_predictor | accuracy | 0.95 | 0.0-1.0 |
| branch_predictor | rng_seed | 0xC0FFEE | any uint64 |
| icache | rng_seed | 0xCAFE | any uint64 |

### Midcore Parameters
| Section | Parameter | Default | Range/Values |
|---------|-----------|---------|--------------|
| midcore | ooo_enabled | true | true, false |
| rename | num_phys_regs | 128 | 64-512 |
| rename | dispatch_width | 4 | 1-16 |
| issue | mode | unified | unified, partitioned |
| issue | selection | age_matrix | age_matrix, fifo, random |
| issue | speculative_wakeup | false | true, false |
| execute | granularity | unified | unified, typed, functional |
| lsq | load_queue_capacity | 32 | 8-128 |
| lsq | store_forwarding | true | true, false |
| lsq | pending_dispatch_capacity | 16 | 0-256 |
| dcache | cache_mode | probabilistic | probabilistic, structural |
| dcache | rng_seed | 0xBEEF | any uint64 |
| writeback | rob_capacity | 128 | 32-512 |
| writeback | retire_width | 4 | 1-16 |

### Advanced Feature Parameters
| Section | Parameter | Default | Description |
|---------|-----------|---------|-------------|
| write_ports | mode | unified | unified, mapped |
| write_ports | num_ports | 4 | PRF write ports |
| bypass_network | enabled | false | Enable result forwarding |
| bypass_network | regfile_read_latency | 1 | PRF read latency without bypass |
| regfile | banking_enabled | false | Enable PRF banking |
| regfile | num_banks | 4 | Number of PRF banks |

### L2 Cache Parameters
| Parameter | Default | Description |
|-----------|---------|-------------|
| enabled | false | Enable L2 cache |
| hit_latency | 10 | L2 hit cycles |
| miss_latency | 100 | DRAM latency |
| cache_size_kb | 1024 | L2 size in KB |
| arb_policy | dcache_first | I$/D$ arbitration |
| inclusivity_policy | nine | L1-L2 relationship |
