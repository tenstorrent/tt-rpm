# Core Model Configuration Guide

This document describes how to configure the `core` CPU model
(`models/cpu/src/core`). Every configurable knob is a
[Sparta](https://github.com/sparcians/map) parameter attached to a unit in the
simulation tree, so the model is configured with standard Sparta YAML files and
command-line overrides.

## Table of Contents

1. [Supplying Parameters](#supplying-parameters)
2. [Simulation Tree](#simulation-tree)
3. [Core](#core)
4. [Frontend](#frontend)
   - [Fetch](#fetch)
   - [I-Cache](#i-cache)
   - [Fetch Queue and Decode Queue](#fetch-queue-and-decode-queue)
   - [Decode](#decode)
   - [Branch Predictor](#branch-predictor)
5. [Midcore](#midcore)
   - [Rename](#rename)
   - [Issue](#issue)
   - [Execute](#execute)
   - [Load Store Queue (LSQ)](#load-store-queue-lsq)
   - [D-Cache](#d-cache)
   - [MMU (TLBs and Page Walks)](#mmu-tlbs-and-page-walks)
   - [Writeback / ROB](#writeback--rob)
6. [L2 Cache](#l2-cache)
7. [Advanced Microarchitectural Features](#advanced-microarchitectural-features)
   - [Write-Port Arbiter](#write-port-arbiter)
   - [Writeback Buffer](#writeback-buffer)
   - [Wakeup and Bypass](#wakeup-and-bypass)
   - [Register File Banking](#register-file-banking)
8. [Execution Driver](#execution-driver)
9. [Visualization and Debugging](#visualization-and-debugging)
10. [Example Configurations](#example-configurations)

---

## Supplying Parameters

Parameters live at `top.core0.<unit>.params.<name>` (core-wide parameters at
`top.core0.params.<name>`). They can be set in two ways:

- **YAML config files** with `-c FILE.yaml`. `-c` may be repeated; later files
  override earlier ones, which makes small override files convenient:
  ```bash
  ./build/core/core -i 0 -c models/cpu/src/core/config.yaml -c my_overrides.yaml \
      --target-elf tests/build/coremark.bare.elf
  ```
- **Single overrides** with `-p PATH VALUE`:
  ```bash
  -p top.core0.writeback.params.retire_width 8
  -p top.core0.issue.params.issue_queue_capacity 48
  ```

The model does not read any config file implicitly; pass `-c` to use one. When
a parameter is not set, the built-in default listed in the tables below applies.
Note that the shipped `config.yaml` overrides several defaults (for example it
selects structural caches and gives each issue port its uop types).

A YAML file mirrors the tree, with parameters under a `params` key:

```yaml
top.core0:
  params:
    ooo_enabled: true
  fetch:
    params:
      fetch_width: 16
  writeback:
    params:
      retire_width: 4
      rob_capacity: 128
```

Useful Sparta options for inspecting the configuration:

| Option | Description |
|--------|-------------|
| `--help-parameters` | Print every parameter with its current and default value, then exit |
| `--show-parameters` | Print the parameter tree after configuration and continue running |
| `--write-final-config FILE` | Write the fully resolved configuration as YAML |
| `--show-tree` | Print the unit tree |

Run-control flags (`-i`, `-r`, `--target-elf`, `--cpu-freq`, `--report-all`)
are described in the top-level [README](../../../../README.md).

---

## Simulation Tree

```
top
└── core0                 (params: core-wide settings)
    ├── fetch             FetchStructures
    ├── icache            FrontendMemoryStructures
    ├── fetch_queue       FetchQueue
    ├── decode            DecodeStructures
    ├── decode_queue      DecodeQueue
    ├── branch_predictor  BranchPredictor
    ├── rename            Rename
    ├── issue             Issue
    ├── execute           Execute
    ├── lsq               LSQ
    ├── dcache            BackendMemoryStructures
    ├── mmu               Mmu (ITLB, DTLB, L2 TLB, page-table walker)
    ├── writeback         Writeback (ROB)
    ├── l2cache           L2Cache
    ├── dram              Dram
    ├── flush_arbiter     FlushArbiter
    ├── pipeline_clock    PipelineClock (no parameters)
    └── edriver           ExecutionDriver (created when --target-elf or --snapshot is given)
```

---

## Core

`top.core0.params`

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `ooo_enabled` | bool | `true` | Out-of-order pipeline (`true`) or in-order pipeline (`false`) |
| `wrong_path_enabled` | bool | `true` | Fetch and execute down mispredicted paths |
| `wrong_path_max_depth` | uint32 | `8` | Maximum number of nested speculation levels |
| `misprediction_penalty_cycles` | uint32 | `0` | Extra fetch stall after a mispredicted branch resolves; `wrong_path_enabled: false` only, must be 0 otherwise |
| `bypass_queues` | bool | `false` | Skip the FetchQueue and DecodeQueue (see below) |
| `visualizer_*` | | | Pipeline visualizer, see [Visualization and Debugging](#visualization-and-debugging) |
| `cache_viewer_*` | | | Cache viewer, see [Visualization and Debugging](#visualization-and-debugging) |

**In-order vs. out-of-order.** `ooo_enabled` selects how the pipeline is wired:
Rename → Issue → Execute in out-of-order mode, Rename → Execute (with an
architectural scoreboard in Rename) in in-order mode. `config_inorder.yaml`
sets it to `false`.

**Wrong-path speculation.** With `wrong_path_enabled: true`, a mispredicted
branch steers Fetch down the predicted (wrong) path, Rename checkpoints the RAT
for every conditional and indirect branch, and Execute flushes and redirects the pipeline when the branch
resolves. With it disabled, wrong-path instructions are never fetched. Where
the predictor would steer Fetch off the correct path, Fetch drops what it
fetched past the branch and holds until the branch resolves: at Execute for a
misprediction, or at Decode for a misfetch. It then refetches through the same
front-end stages, plus `misprediction_penalty_cycles`. A misprediction
therefore costs its resolution time plus the refill, as with wrong path on.
The hold counts in `fetch.num_mispred_penalty_stalls`, and the empty dispatch
slots it leaves count as bad speculation.

**Queue bypass.** With `bypass_queues: true`, I-cache output goes straight to
Decode and Decode output straight to Rename; the FetchQueue and DecodeQueue are
left unconnected, but Decode's input buffer keeps the FetchQueue's `capacity` as
Fetch's back-pressure.

---

## Frontend

### Fetch

`top.core0.fetch.params`

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `initial_pc` | uint64 | `0x80000000` | Initial PC. Overridden by the ELF entry point when an ELF is loaded |
| `fetch_width` | uint32 | `4` | Bytes per fetch group, a power of two no larger than an I-cache line. A group ends at the next `fetch_width`-aligned address, as a BOOM fetch packet does, or earlier at a taken branch or jump or a serializing instruction |
| `fetch_buffer_capacity` | uint32 | `64` | Fetch buffer size in bytes (currently not used by the model) |
| `max_branches_per_fetch` | uint32 | `2` | Branches the predictor handles per fetch group; the group ends after the last one (counted in `num_branch_limit_ends` when nothing else would have ended it) |

### I-Cache

`top.core0.icache.params`

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `cache_mode` | string | `probabilistic` | `probabilistic` or `structural` |
| `hit_latency` | uint32 | `2` | Hit latency in cycles |
| `miss_latency` | uint32 | `20` | Additional miss latency when no L2 is configured |
| `mshr_capacity` | uint32 | `4` | Maximum outstanding misses |
| `hit_rate` | double | `1.0` | Hit probability (probabilistic mode) |
| `rng_seed` | uint64 | `0xCAFE` | Seed of the hit/miss RNG (probabilistic mode) |
| `cache_size_kb` | uint64 | `32` | Capacity in KB (structural mode) |
| `cache_line_size` | uint64 | `64` | Line size in bytes (structural mode) |
| `cache_associativity` | uint64 | `8` | Ways per set (structural mode) |
| `replacement_policy` | string | `lru` | `lru` (true LRU) or `plru` (tree pseudo-LRU) (structural mode) |
| `prefetcher_type` | string | `next_line` | Instruction prefetcher: `none` or `next_line`, see [Prefetchers](#prefetchers) (structural mode) |
| `prefetch_degree` | uint32 | `2` | Lines requested per prefetch trigger |
| `prefetch_queue_capacity` | uint32 | `8` | Prefetch queue entries; new prefetches are dropped while it is full |
| `prefetch_mshr_reserve` | uint32 | `1` | MSHRs a prefetch may not take, kept for demand misses; must be below `mshr_capacity` |

**Cache modes.** The same two modes are available for the I-cache and the
D-cache:

- `probabilistic` draws a hit or miss for every request from `hit_rate`. Misses
  take `hit_latency + miss_latency` cycles. No tag state is kept.
- `structural` models a tag array with sets, ways and the selected replacement
  policy, MSHRs with miss coalescing, and hit-under-miss. Misses are filled from
  the L2 when it is enabled, otherwise after `hit_latency + miss_latency`
  cycles. Only structural caches can be connected to the L2 and traced by the
  cache viewer.

### Fetch Queue and Decode Queue

`top.core0.fetch_queue.params`, `top.core0.decode_queue.params`

| Parameter | Type | Default (FQ / DQ) | Description |
|-----------|------|-------------------|-------------|
| `capacity` | uint32 | `32` / `32` | Queue entries (instructions / uops); the FetchQueue's is MegaBoom's `numFetchBufferEntries` |
| `latency` | uint32 | `1` / `1` | Extra pipeline stages before the consumer can take an entry: predecode/align before Decode, a second rename stage before Rename |

Both queues are only used when `bypass_queues` is `false`, so the `latency`
stages exist only then. Cycles Fetch waits for FetchQueue space count in
`fetch.num_buffer_full_stall_cycles`. A branch at the head of the FetchQueue waits until its
branch prediction has arrived. An entry occupies its queue while it is in the
`latency` stages, so the FetchQueue must cover the whole fetch credit loop
(I-cache hit plus predecode) at full fetch width. With fewer than about 24
entries, it throttles a 4-wide machine.

**Front-end depth.** With the shipped values, a redirect reaches fetch one
cycle after the branch executes. From there, the correct path takes 2 cycles of
I-cache, 1 of predecode, Decode, 2 rename stages, and Issue, so it executes 10
cycles after the redirect and is dispatched 8 cycles after the flush. This is
in line with the 10–12 stages from redirect to execute in BOOM and XiangShan.
Eyerman et al., "Characterizing the branch misprediction penalty" (ISPASS
2006), split the full penalty into this refill plus the branch's resolution
time.

### Decode

`top.core0.decode.params`

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `decode_width` | uint32 | `4` | Instructions decoded per cycle (cycles at the limit are counted in `num_width_limited_cycles`) |
| `lat_alu` | uint32 | `1` | Execution latency of ALU uops |
| `lat_mul` | uint32 | `3` | Execution latency of integer multiplies |
| `lat_div` | uint32 | `14` | Execution latency of integer divides |
| `lat_branch` | uint32 | `1` | Execution latency of branches and jumps |
| `lat_load` | uint32 | `1` | Load address-generation latency (cache latency is added by the D-cache) |
| `lat_store` | uint32 | `1` | Store address-generation latency |
| `lat_fp` | uint32 | `4` | Execution latency of FP uops outside the classes below |
| `lat_fp_add` | uint32 | `3` | FP add and subtract |
| `lat_fp_mul` | uint32 | `4` | FP multiply |
| `lat_fp_fma` | uint32 | `4` | FP fused multiply-add (`fmadd`, `fmsub`, `fnmadd`, `fnmsub`) |
| `lat_fp_cvt` | uint32 | `4` | FP convert: to and from integer, between formats, and `fround` |
| `lat_fp_cmp` | uint32 | `2` | FP compare, min and max |
| `lat_fp_move` | uint32 | `2` | FP sign injection, `fmv`, `fli` and `fclass` |
| `lat_fp_div_s` | uint32 | `10` | Execution latency of half- and single-precision FP divides and square roots |
| `lat_fp_div_d` | uint32 | `13` | Execution latency of double-precision FP divides and square roots |
| `lat_vec` | uint32 | `4` | Execution latency of vector uops |
| `lat_fence` | uint32 | `1` | Execution latency of fence uops |

The divide and FP defaults are latencies uops.info measures for the x86 scalar
equivalents: 64-bit DIV on Ice Lake and Alder Lake-P (14, to the quotient),
ADDSS on Zen 2-4 (3), MULSS on Ice Lake (4), VFMADD231SS on Ice Lake and Zen 3/4
(4), CVTSS2SD and CVTSI2SS on Ice Lake (4), MINSS and CMPSS on Zen 4 (2), DIVSS on
Zen 2/3 (10), and DIVSD on Ice Lake, Alder Lake-P and Zen 2-4 (13). `lat_fp_cmp`
and `lat_fp_move` also match the 2-cycle defaults of the rocket-chip FPU
(`FPUParams` `fpmuLatency` for sign injection, min and max; `ifpuLatency` for
integer-to-FP moves).

Decode classifies every instruction into a uop type, which selects its latency,
its issue scheduler and its execute group:

| Uop type | Instructions |
|----------|--------------|
| `ALU` | All instructions not listed below |
| `Mul` | Mnemonics starting with `mul` |
| `Div` | Mnemonics starting with `div` or `rem` |
| `Branch` | Conditional branches, jumps, calls and returns |
| `Load` | Scalar loads |
| `Store` | Scalar stores and atomics (AMO, LR, SC) |
| `FpOp` | Instructions the execution driver reports as floating point |
| `VecOp` | Instructions the execution driver reports as vector |
| `Fence` | `fence`, `fence.tso`, `fence.i` and `sfence.vma` |

FP and vector loads and stores are `Load` and `Store` uops. A scalar `FpOp`
takes the latency of its class, read from its encoding. `fdiv`, `fsqrt`,
`vfdiv`, `vfrdiv` and `vfsqrt` take `lat_fp_div_s` or `lat_fp_div_d` instead,
chosen by precision, or by SEW for the vector forms.

### Branch Predictor

`top.core0.branch_predictor.params`

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `predictor_type` | string | `tage_sc_l_64kb` | `tage_sc_l_64kb`, `tage_sc_l_8kb`, `gshare`, `bimodal` or `probabilistic` (see below) |
| `direction_args` | string list | `[]` | Algorithm-specific direction predictor knobs as `key=value` (see below) |
| `accuracy` | double | `0.95` | Probability that a conditional branch is predicted correctly; probabilistic mode only, and an error to set otherwise |
| `rng_seed` | uint64 | `0xC0FFEE` | Seed of the prediction RNG; the same seed gives the same sequence of mispredictions (probabilistic mode). Also seeds `btb_replacement: random` |
| `btb_type` | string | `setassoc` | `none` (oracle targets), or a registered BTB: `setassoc` |
| `btb_entries` | uint32 | `2048` | BTB entries; `btb_entries / btb_ways` must be a power of two. BOOM's BTB: 128 sets x 2 ways x 4 slots x 2 banks (`BoomBTBParams`) |
| `btb_ways` | uint32 | `2` | BTB associativity |
| `btb_tag_bits` | uint32 | `0` | BTB tag width; `0` stores the full tag, fewer bits let branches alias |
| `btb_replacement` | string | `lru` | `lru` (recency refreshed on every fetch-time hit, wrong path included) or `random` |
| `btb_args` | string list | `[]` | Algorithm-specific BTB knobs as `key=value` |
| `ras_type` | string | `circular` | `none` (oracle return targets), or a registered RAS: `circular` |
| `ras_entries` | uint32 | `32` | RAS depth (BOOM `numRasEntries`); a push onto a full stack overwrites the oldest entry |
| `ras_args` | string list | `[]` | Algorithm-specific RAS knobs as `key=value` |
| `indirect_type` | string | `none` | Indirect-target predictor consulted on a BTB hit of an indirect jump or call. Only `none` (the BTB's last target) ships; the parameter selects a user-added algorithm. Unused with `btb_type: none` |
| `indirect_args` | string list | `[]` | Algorithm-specific indirect-target predictor knobs as `key=value` |
| `taken_redirect_latency` | uint32 | `2` | Cycle in which the BTB, RAS and direction predictor steer fetch; a predicted-taken branch costs `taken_redirect_latency - 1` fetch cycles. Unused with `btb_type: none` |

To add an algorithm, see [Adding a predictor](../../predictors/README.md).
Each `*_args` list holds the knobs of the selected algorithm, for example
`direction_args: [history_bits=14]`. A key the algorithm does not read is an
error that names the parameter and lists the keys it knows. The generic knobs
above (`btb_entries`, `ras_entries`, ...) cannot be repeated in an `*_args` list.
They are range-checked even when their structure is `none` (against `setassoc`
or `circular`), so a sweep that disables a structure cannot hide a bad value.
The shipped algorithms take no `*_args` keys unless listed below.

With `predictor_type: probabilistic`, the predictor flips the actual outcome of
a conditional branch with probability `1 - accuracy`. Jumps, calls and returns
are always predicted taken; their targets come from the BTB and RAS, or from
Whisper when those are `none`. The shipped config before the real predictors
became the default was `predictor_type: probabilistic`, `accuracy: 0.9`,
`btb_type: none` and `ras_type: none`.

`num_mispredicted` and `num_correct` count every prediction, wrong-path
branches included, so they overstate MPKI by a predictor-dependent factor. The
retired counts are `writeback.num_retired_mispredicts` and
`writeback.num_retired_branches`.

`tage_sc_l_64kb` and `tage_sc_l_8kb` are the two limited-size TAGE-SC-L
configurations from Seznec's CBP-5 entry (TAGE, statistical corrector, loop
predictor). CBP-5 allows each track 2,048 bits beyond its nominal size: the 8KB
geometry (section 6.1 of "TAGE-SC-L Branch Predictors Again") holds 67,356 of
67,584 bits and the 64KB one 523,374 of 526,336. They predict conditional branches only; jumps, calls and returns are
predicted taken and enter the global and path histories. History is updated
speculatively at fetch and repaired on a flush; tables train at commit.
Setting `accuracy` with them is an error; `rng_seed` only seeds
`btb_replacement: random`. `branch_predictor.dir_tagged_provider`,
`dir_loop_overrides` and `dir_sc_overrides` count the predictions a tagged
table provided and those the loop predictor or statistical corrector changed,
wrong path included.

`bimodal` is a 16K-entry table of 2-bit counters indexed by PC, trained at
commit, with no history: a floor to compare real predictors against.

`gshare` (McFarling, "Combining Branch Predictors", WRL TN-36, 1993) indexes
2-bit counters with the PC XORed with the global history of conditional
outcomes: the classic baseline between `bimodal` and TAGE. History is updated
at fetch and repaired on a flush; counters train at commit.

| `direction_args` key | Type | Default | Description |
|----------------------|------|---------|-------------|
| `history_bits` | uint32 | `14` | Global-history and index width, 1 to 24; the table has `2^history_bits` counters (14: 4 KB, as `bimodal`) |

**Target prediction.** With `btb_type` and `ras_type` both `none`, every target
comes from Whisper and only the direction is predicted. Otherwise Fetch follows
the BTB: a taken prediction needs a BTB hit, a return takes the RAS top, and an
indirect jump or call takes the last target the BTB saw. A miss falls through.
The BTB is looked up at each branch's PC, which the trace identifies; it is not
a fetch-block BTB that has to find the branches in a block.
Decode computes direct targets, so a taken conditional branch or direct jump
that missed the BTB is a *misfetch*: Decode redirects Fetch. Decode also sends
a return that fetch got wrong (a BTB miss) to the RAS top. Branch types come
from the encoding alone; RAS actions follow the RISC-V JAL/JALR hint table:
`rd` `x1` or `x5` pushes, `rs1` `x1` or `x5` pops, and both (if different) pop
then push, a coroutine swap. A wrong return or indirect target, or an indirect jump that missed
the BTB, is a misprediction resolved at Execute. The BTB is trained at retire with taken
branches only; the RAS is updated at predict and checkpointed per branch (top
pointer and top entry), so a flush repairs it. A `none` structure is an oracle
for its part, so either can be studied alone. With either one on, the wrong
path also takes the branches Whisper takes along it, redirected wherever the
prediction differs; with both `none` it runs straight through correctly
predicted taken branches, as before. The shipped config turns both on.

**Predictor latency.** With a BTB (`btb_type` not `none`), the BTB, RAS and
direction predictor answer in cycle `taken_redirect_latency` after fetch. Each
predicted-taken branch costs fetch `taken_redirect_latency - 1` cycles
(`fetch.num_taken_bubble_cycles`); the shipped 2 is BOOM's one-cycle bubble when
its BTB and TAGE redirect from the second fetch stage. A fetch block ends at its
first taken branch.

With `btb_type: none`, the direction prediction and Whisper's target steer
fetch from the next cycle, so a correctly predicted taken branch never costs a
bubble. This is an idealization (a perfect
zero-latency BTB), kept for comparison with older results.

---

## Midcore

### Rename

`top.core0.rename.params`

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `dispatch_width` | uint32 | `4` | Instructions renamed and dispatched per cycle |
| `num_phys_regs` | uint32 | `128` | Physical registers per register file (integer, FP and vector each) |
| `freelist_headroom` | uint16 | `8` | Dispatch stalls unless at least this many free registers remain after allocation |
| `max_branch_checkpoints` | uint32 | `20` | RAT checkpoints for unresolved conditional and indirect branches, freed at resolution (MegaBoom `maxBrCount`; used with wrong-path speculation, which needs at least 1) |
| `regfile_banking_enabled` | bool | `false` | See [Register File Banking](#register-file-banking) |
| `regfile_num_banks` | uint32 | `4` | See [Register File Banking](#register-file-banking) |
| `regfile_reads_per_bank` | uint32 | `2` | See [Register File Banking](#register-file-banking) |

Dispatch also stalls when the ROB is full, when the target issue scheduler (or,
in in-order mode, execute group) is full, and when no load- or store-queue entry
is available for a memory uop: the LSQ grants entries as credits at rename, in
program order. In in-order mode it additionally waits until the source operands
are ready or will meet the uop on the bypass (see [Wakeup and Bypass](#wakeup-and-bypass))
and, as the issue stage, for a free issue port and divider (below).

### Issue

`top.core0.issue.params`

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `mode` | string | `unified` | `unified` (one scheduler) or `partitioned` (several schedulers) |
| `issue_queue_capacity` | uint32 | `16` | Scheduler entries (unified mode, at most 64) |
| `issue_width` | uint32 | `4` | Issue ports. In partitioned mode they are split evenly between the schedulers (at least one each) |
| `issue_ports` | string list | `[]` | Unified mode: the uop types each port accepts, as `+`-separated lists; must have `issue_width` entries. Empty means every port accepts every type |
| `regfile_read_latency` | uint32 | `1` | Register-read stages between issue and execute. See [Wakeup and Bypass](#wakeup-and-bypass) |
| `int_div_units` | uint32 | `1` | Integer dividers, each busy `int_div_interval` cycles per divide; `0` makes divides pipelined |
| `fp_div_units` | uint32 | `1` | FP divide/square-root units for scalar and vector divides and square roots; `0` makes them pipelined |
| `int_div_interval` | uint32 | `10` | Cycles an integer divide holds its divider (its issue interval); `0` means its full latency |
| `fp_div_s_interval` | uint32 | `3` | Same for half- and single-precision FP divide and square root |
| `fp_div_d_interval` | uint32 | `4` | Same for double-precision FP divide and square root; a vector one goes by SEW, as its latency does |
| `selection` | string | `age_matrix` | Selection among ready entries: `age_matrix` (oldest first), `fifo` (lowest slot first) or `random` |
| `num_phys_regs` | uint32 | `128` | Scoreboard size; keep equal to `rename.params.num_phys_regs` |
| `scheduler_configs` | string list | `[]` | Schedulers for partitioned mode (required there), one `name:capacity:types` entry each (capacity at most 64) |
| `routing_priorities` | string list | `[]` | Routing policy chain for partitioned mode |
| `routing_dep_occupancy_limit` | double | `0.9` | Dependency routing is skipped when the producer's scheduler is fuller than this |

**Issue ports.** A ready uop issues on the free port with the fewest uop types
that accepts it, so plain ALU uops leave the multiply/divide port free. A divide
also needs a free divider, which it holds for its issue interval (the
reciprocal throughput uops.info measures for 64-bit DIV, DIVSS and DIVSD on Ice
Lake: 10, 3 and 4 cycles) while its result takes the full latency.
`num_int_div_issued` and `num_fp_div_issued` count the divides. The shipped `config.yaml` uses nine ports in the
style of the BOOM and gem5 O3 functional-unit pools: four integer (two with
branches, one with multiply and divide), two load, one store and two FP/vector.
`num_port_conflicts`, `num_int_div_busy` and `num_fp_div_busy` count the
ready uop-cycles each limit held back. In-order mode has no scheduler, but Rename
issues through the same ports and dividers.

**Partitioned mode.** Each `scheduler_configs` entry has the form
`name:capacity:types`, where `types` is a `+`-separated list of uop types
(`ALU`, `Mul`, `Div`, `Branch`, `Load`, `Store`, `FpOp`, `VecOp`, `Fence`). Every
uop type must be assigned to at least one scheduler; the first scheduler that
lists a type is that type's primary scheduler.

```yaml
top.core0:
  issue:
    params:
      mode: partitioned
      issue_width: 6
      issue_ports: []
      scheduler_configs: ["int:16:ALU+Mul+Div+Branch+Fence", "mem:16:Load+Store", "fp:8:FpOp+VecOp"]
      routing_priorities: ["dependency_locality", "type_affinity", "least_occupied"]
```

Rename routes each uop when it dispatches and reserves its entry there.
`routing_priorities` is an ordered list of policies; the first one that finds a
scheduler with a free entry wins. The fallback is the type's primary scheduler, or
the next one that accepts the type if the primary is full; dispatch stalls only
when every scheduler that accepts the type is full:

| Policy | Description |
|--------|-------------|
| `dependency_locality` | The scheduler that received the producer of one of the instruction's source operands, if it accepts the uop type and is not fuller than `routing_dep_occupancy_limit` |
| `type_affinity` | The primary scheduler of the uop type |
| `least_occupied` | The least occupied scheduler that accepts the uop type |
| `round_robin` | Rotate between the schedulers that accept the uop type |

When `routing_priorities` is empty, the chain is `dependency_locality`,
`type_affinity`, `least_occupied`.

### Execute

`top.core0.execute.params`

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `granularity` | string | `unified` | Functional-unit grouping: `unified`, `typed` or `functional` |
| `unified_max_in_flight` | uint32 | `16` | In-flight uops (unified) |
| `int_max_in_flight` | uint32 | `4` | In-flight uops in the integer group (typed) |
| `fp_max_in_flight` | uint32 | `2` | In-flight uops in the FP group (typed) |
| `vec_max_in_flight` | uint32 | `2` | In-flight uops in the vector group (typed) |
| `branch_max_in_flight` | uint32 | `2` | In-flight uops in the branch group (typed) |
| `alu_count`, `alu_per_unit_in_flight` | uint32 | `2`, `4` | ALU units and in-flight uops per unit (functional) |
| `mul_count`, `mul_per_unit_in_flight` | uint32 | `1`, `2` | Multiply units (functional) |
| `div_count`, `div_per_unit_in_flight` | uint32 | `1`, `1` | Divide units (functional) |
| `branch_count`, `branch_per_unit_in_flight` | uint32 | `1`, `2` | Branch units (functional) |
| `fp_count`, `fp_per_unit_in_flight` | uint32 | `1`, `4` | FP units (functional) |
| `vec_count`, `vec_per_unit_in_flight` | uint32 | `1`, `4` | Vector units (functional) |
| `fence_count`, `fence_per_unit_in_flight` | uint32 | `1`, `1` | Fence units (functional) |
| `write_port_mode`, `write_port_count`, `write_port_mapping` | | | See [Write-Port Arbiter](#write-port-arbiter) |
| `writeback_buffer_*` | | | See [Writeback Buffer](#writeback-buffer) |

The granularity determines the execute groups. A group accepts new uops while
its in-flight count is below its limit; the group index is also the source id
used by write-port mappings:

| Granularity | Groups (index: uop types) |
|-------------|---------------------------|
| `unified` | 0: all uop types |
| `typed` | 0: ALU, Mul, Div, Load, Store, Fence; 1: FpOp; 2: VecOp; 3: Branch |
| `functional` | 0: ALU; 1: Mul; 2: Div; 3: Branch; 4: Load, Store; 5: FpOp; 6: VecOp; 7: Fence |

In `functional` mode the limit of each group is `<unit>_count ×
<unit>_per_unit_in_flight`; the load/store group (4) uses the ALU values.
Memory uops leave Execute after address generation and complete in the LSQ.
Uops in the register-read stage do not count toward these limits. Issue ports
and dividers (see [Issue](#issue)) model per-cycle throughput; the shipped
config uses `unified` with a limit high enough not to bind.

### Load Store Queue (LSQ)

`top.core0.lsq.params`

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `load_queue_capacity` | uint32 | `32` | Load queue entries |
| `store_queue_capacity` | uint32 | `32` | Store queue entries |
| `store_forwarding` | bool | `true` | Let a load complete from an older store in the store queue that fully covers it |
| `forwarding_latency` | uint32 | `1` | Cycles for a forwarded load to complete |
| `load_ports` | uint32 | `2` | Loads per cycle that can read the D-cache or forward; an atomic also takes one |
| `store_ports` | uint32 | `1` | Retired stores moved to the D-cache store buffer per cycle |
| `mem_dep_predictor` | string | `store_sets` | How a load treats older stores whose address is unknown: `store_sets` waits for the stores the predictor names, `blind` never waits, and both replay on an ordering violation; `none` never waits and never replays (no disambiguation cost) |
| `ssit_entries` | uint32 | `4096` | Store-set ID table entries, indexed by PC |
| `lfst_entries` | uint32 | `128` | Last-fetched-store table entries, i.e. the number of store sets |
| `ssit_clear_interval` | uint64 | `1000000` | Cycles between clears of the store-set ID table; `0` never clears |
| `num_banks` | uint32 | `1` | Load/store queue banks; only `1` is supported, as rename's load/store credits are not tracked per bank |
| `cache_line_size` | uint32 | `64` | Line size used to select the bank of an address |

Stores complete towards the ROB as soon as they enter the store queue and their
address has translated; they are written to the D-cache after they retire. A store enters the queue with its address
and data, when both source registers are ready.

A load checks the older stores in the queue. If the youngest one that overlaps it
covers all its bytes, the load forwards and completes `forwarding_latency` cycles
after that store's data is ready. A partial overlap, or `store_forwarding: false`,
makes the load wait until the store has written the cache. Loads keep their entry
until they retire.

Older stores whose address is not yet known are handled by `mem_dep_predictor`. The
store-set predictor follows Chrysos and Emer, "Memory dependence prediction using
store sets" (ISCA 1998). When a store's address arrives and a younger load has
already read any of its bytes, the load is replayed: when it reaches the ROB head,
everything from it onwards is flushed and refetched, and the predictor learns the
pair.

A fence completes once every older load has its data and every older store has
drained from the store buffer; younger loads wait for it. Fences are treated as
`rw,rw`. An atomic (AMO, LR or SC) is performed at the ROB head after older stores
drain: it reads the D-cache with the line taken exclusive and leaves it Modified,
and its destination is ready when the access returns. An atomic with `aq` set
also holds younger loads until it completes.

### D-Cache

`top.core0.dcache.params`

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `cache_mode` | string | `probabilistic` | `probabilistic` or `structural`, see [Cache modes](#i-cache) |
| `hit_latency` | uint32 | `3` | Hit latency in cycles |
| `miss_latency` | uint32 | `10` | Additional miss latency when no L2 is configured |
| `mshr_capacity` | uint32 | `8` | Maximum outstanding misses |
| `hit_rate` | double | `1.0` | Load hit probability (probabilistic mode) |
| `rng_seed` | uint64 | `0xBEEF` | Seed of the hit/miss RNG (probabilistic mode) |
| `cache_size_kb` | uint64 | `32` | Capacity in KB (structural mode) |
| `cache_line_size` | uint64 | `64` | Line size in bytes (structural mode) |
| `cache_associativity` | uint64 | `8` | Ways per set (structural mode) |
| `replacement_policy` | string | `lru` | `lru` or `plru` (structural mode) |
| `store_buffer_capacity` | uint32 | `8` | Store buffer entries (structural mode) |
| `write_policy` | string | `write_back` | `write_back` (write-allocate, dirty lines written back to the L2 on eviction); the only policy modeled |
| `num_banks` | uint32 | `1` | Cache banks; `1` disables banking (structural mode) |
| `reads_per_bank_per_cycle` | uint32 | `1` | Read ports per bank (structural mode) |
| `writes_per_bank_per_cycle` | uint32 | `1` | Write ports per bank; the store buffer writes at most this many stores per cycle per bank, or in total with one bank (structural mode) |
| `fill_occupies_write_port` | bool | `false` | Line fills consume a bank write port (structural mode) |
| `prefetcher_type` | string | `ip_stride` | Data prefetcher: `none`, `ip_stride`, `next_line` or `stream`, see [Prefetchers](#prefetchers) (structural mode) |
| `prefetch_degree` | uint32 | `2` | Lines requested per prefetch trigger |
| `prefetch_distance` | uint32 | `4` | `ip_stride`: strides ahead of the trigger (lines for a sub-line stride), pulled back to stay in the page |
| `prefetch_table_entries` | uint32 | `64` | `ip_stride`: PC-indexed table entries |
| `prefetch_queue_capacity` | uint32 | `16` | Prefetch queue entries; new prefetches are dropped while it is full |
| `prefetch_mshr_reserve` | uint32 | `2` | MSHRs a prefetch may not take, kept for demand misses; must be below `mshr_capacity` |
| `prefetch_fill_level` | string | `l1` | `l1`, or `l2` to fill the L2 only (needs the L2 enabled; usefulness is then counted in the L2's stats) |
| `uncacheable_base` | uint64 | `0` | Base of an uncacheable MMIO range whose stores bypass the cache (structural mode) |
| `uncacheable_size` | uint64 | `0` | Size in bytes of the uncacheable range; `0` disables it |

In structural mode, stores go to the store buffer and drain to the cache one
per cycle. In probabilistic mode, stores always hit. With `num_banks` above 1, a
load that finds its bank's read ports used waits a cycle (`num_bank_conflicts`).

### MMU (TLBs and Page Walks)

`top.core0.mmu.params`

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `enabled` | bool | `true` | Model TLB and page-walk timing; `false` makes translation free |
| `tlb_type` | string | `setassoc` | Registered TLB algorithm used for all three TLBs |
| `itlb_entries` / `itlb_ways` | uint32 | `32` / `32` | L1 instruction TLB; ways = entries is fully associative |
| `dtlb_entries` / `dtlb_ways` | uint32 | `32` / `32` | L1 data TLB |
| `l2tlb_entries` / `l2tlb_ways` | uint32 | `1024` / `8` | Unified L2 TLB; `0` entries removes it |
| `l2tlb_latency` | uint32 | `8` | Cycles an L1 TLB miss waits for the L2 TLB lookup |
| `num_walkers` | uint32 | `2` | Concurrent page-table walks |
| `pwc_entries` | uint32 | `16` | Page-walk cache entries per non-leaf level; `0` disables it |

The L1 TLB sizes are rocket-chip's `ICacheParams`/`DCacheParams` defaults (one
set of 32 ways). L1 TLB hits cost nothing (they overlap the VIPT L1 access). An L1 miss waits
`l2tlb_latency`, and an L2 TLB hit fills the L1 TLB only then; an L2 miss then walks the page table, loading each PTE through
the D-cache. Whisper translates functionally, so the walk loads exactly the PTE
addresses of Whisper's own walk; the page-walk cache (VA-prefix tagged, one per
level) skips the upper levels it holds. TLBs hold 4 KiB, 2 MiB, 1 GiB and
512 GiB pages and honour ASIDs and the PTE G bit.

- Fetch stalls on an ITLB miss; a load waits before its D-cache access; a store
  does not complete until translated.
- M-mode, harts without S-mode and `satp` = Bare bypass the TLBs at zero cost.
- `sfence.vma` (and `sinval.vma`) flushes at retire with the granularity of its
  `rs1`/`rs2`; only `rs1` = x0 flushes the page-walk cache. A walk in flight
  across the fence fills neither the TLBs nor the page-walk cache.
- A miss joins a walk in flight for the same 4 KiB page. Two misses to
  different 4 KiB pages of one superpage walk separately, though the first
  walk's leaf would serve both.
- Wrong-path accesses may walk. A started walk completes and fills; a queued
  walk whose requesters were all squashed is dropped (`walks_cancelled`).
- Stats: `itlb_*`, `dtlb_*`, `l2tlb_*` hits/misses, `walks`, `walk_cycles`,
  `walk_pte_loads`, `pwc_hits`, `itlb_stall_cycles`, `dtlb_wait_cycles`. The run
  summary prints the MPKIs on its `TLB:` line.

### Writeback / ROB

`top.core0.writeback.params`

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `retire_width` | uint32 | `4` | Instructions retired per cycle |
| `rob_capacity` | uint32 | `128` | Reorder buffer entries |
| `retire_timeout_cycles` | uint32 | `10000` | Watchdog: abort the run when nothing retires for this many cycles |
| `retire_stall_l1_miss_age` | uint32 | `4` | `retire_stall_load_l1_miss` counts no-retire cycles whose oldest uop is an unfinished load dispatched more than this many cycles ago |
| `retire_stall_l2_miss_age` | uint32 | `16` | Same, for `retire_stall_load_l2_miss` |

---

## L2 Cache

`top.core0.l2cache.params`

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `enabled` | bool | `true` | Connect a unified L2 behind the L1 caches |
| `hit_latency` | uint32 | `4` | Array latency in cycles; an L2 hit costs 15 cycles load-to-use once the L1 and transfer cycles are added |
| `miss_latency` | uint32 | `100` | Flat latency of an L2 miss, used only when `dram.params.enabled` is false |
| `mshr_capacity` | uint32 | `32` | L2 MSHRs |
| `cache_size_kb` | uint64 | `1024` | Capacity in KB |
| `cache_line_size` | uint64 | `64` | Line size in bytes; keep equal to the L1 line size |
| `cache_associativity` | uint64 | `16` | Ways per set |
| `replacement_policy` | string | `lru` | `lru` or `plru` |
| `arb_policy` | string | `dcache_first` | Arbitration between I-cache and D-cache requests: `dcache_first`, `icache_first` or `round_robin` |
| `starvation_threshold` | uint32 | `8` | Cycles after which a waiting lower-priority request wins arbitration |
| `queue_capacity` | uint32 | `16` | Pending fill requests per source; must be at least that L1's `mshr_capacity` |
| `victim_buffer_capacity` | uint32 | `16` | Dirty victims waiting for the DRAM write queue, from the L2 or written past it by the L1; fills that would evict a dirty line stall while it is full |
| `inclusivity_policy` | string | `nine` | `nine` (non-inclusive, non-exclusive) or `inclusive` (L2 evictions back-invalidate both L1s; a dirty L1 copy is written back) |
| `prefetcher_type` | string | `stream` | Data prefetcher trained on D-side L2 accesses: `none`, `stream`, `ip_stride` or `next_line` |
| `prefetch_degree` | uint32 | `2` | Lines requested per trigger when `prefetch_feedback` is off |
| `prefetch_distance` | uint32 | `16` | `stream`: lines the prefetches run ahead of the demand when `prefetch_feedback` is off |
| `prefetch_table_entries` | uint32 | `64` | `stream`: tracked streams |
| `prefetch_queue_capacity` | uint32 | `32` | Prefetch queue entries; new prefetches are dropped while it is full |
| `prefetch_mshr_reserve` | uint32 | `4` | MSHRs a prefetch may not take, kept for demand misses; must be below `mshr_capacity` |
| `prefetch_feedback` | bool | `false` | `stream`: set degree and distance from measured accuracy, lateness and pollution |

The L2 services one request per cycle. Only structural-mode L1 caches send
their misses to the L2. Dirty L1 victims mark the L2 copy dirty, or go on to
memory when the L2 no longer holds the line. Prefetches use the tag port only
in cycles with no demand request. An L1 prefetch that misses the L1 is answered
like a fill but counted in `num_l1_prefetch_hits` / `num_l1_prefetch_misses`,
not in `num_hits` / `num_misses`; the `stream` prefetcher still trains on it, as
an L2 streamer watches the whole L1D miss stream.

Back-pressure reaches the core through every level: a request waits in the L2
queue while all L2 MSHRs are busy (`num_mshr_full_cycles`) or the DRAM read
queue is full (`num_mem_queue_full_cycles`); a fill waits while the victim
buffer is full (`num_victim_full_stalls`, or `num_l1_writeback_stalls` for an L1
fill whose dirty victim needs an entry). An L2 miss to a line still in the victim
buffer refills from it (`num_victim_hits`). A dirty line dropped by a snoop is
written back and cannot wait, so it may exceed the capacity
(`num_victim_overflows`). The waiting requests keep their L1
MSHRs, so the L1 refuses new misses and the LSQ stalls.

### Prefetchers

Each structural cache can host one prefetcher. The prefetcher trains on that
cache's demand accesses (page-walk loads excluded), and its requests wait in a bounded prefetch queue.
One request per cycle is issued into an MSHR, after demand and never into the
last `prefetch_mshr_reserve` MSHRs. A request for a line already present or in
flight is discarded. Each prefetched line carries a bit that is cleared on its
first demand use.

- `next_line`: tagged next-N-line (Smith, ACM Computing Surveys 1982). A miss,
  or the first use of a prefetched line, requests the next `prefetch_degree`
  lines in the page.
- `ip_stride`: reference prediction table (Chen and Baer, IEEE ToC 1995). It
  keeps a per-PC last address and stride with an initial / transient / steady /
  no-prediction state machine, and prefetches only in the steady state.
- `stream`: stream detection after Jouppi (ISCA 1990), Palacharla and Kessler
  (ISCA 1994), and Srinath et al. (HPCA 2007). An entry trains on two L2 misses
  in the same direction within 16 lines, then prefetches ahead of the demand
  in that direction. Each confirming access widens its lead by the degree, up
  to the distance. With `prefetch_feedback`, the (distance, degree) pair steps
  between (4,1), (8,1), (16,2), (32,4) and (64,4), starting at (16,2). The step
  is taken once per interval of evictions of half the L2's lines, using the
  paper's accuracy, lateness and pollution thresholds (0.75 / 0.40, 0.01,
  0.005).

Prefetches never cross a 4 KB page. Each host cache reports `pf_issued`,
`pf_useful` (first demand hit on a prefetched line), `pf_late` (a demand
merged into an in-flight prefetch), `pf_useless` (evicted unused),
`pf_dropped` (prefetch queue full), `pf_filtered`, `pf_mshr_stall_cycles`,
`pf_pollution_misses` and `pf_nondemand_hits` (first used by a page walk or an
upper-level prefetch: neither useful nor useless). It also derives `pf_accuracy = (useful + late) / issued`
and `pf_coverage = (useful + late) / (useful + late + demand line misses)`.

---

## DRAM

`top.core0.dram.params`

A native DDR timing model below the L2 (`MemoryHierarchy/Dram/`). The L2 talks
to it through `MemoryBackend`, so another memory model can replace it. It is
used by the single-core model; in the cluster, the L2 egresses to the shared
cluster cache instead.

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `enabled` | bool | `true` | Model DRAM below the L2; `false` restores the flat `l2cache.params.miss_latency` |
| `data_rate_mts` | uint32 | `3200` | Data rate in MT/s; the DRAM clock is half of it |
| `static_latency_ns` | double | `40` | Fixed controller, on-chip interconnect and LLC-miss latency added to every read |
| `channels` | uint32 | `2` | Channels, each with its own data bus and queues |
| `ranks` | uint32 | `1` | Ranks per channel |
| `bank_groups`, `banks_per_group` | uint32 | `4`, `4` | Banks per rank |
| `row_bytes` | uint32 | `8192` | Row size of a rank |
| `read_queue_capacity`, `write_queue_capacity` | uint32 | `32`, `64` | Queue entries per channel |
| `write_high_watermark`, `write_low_watermark` | uint32 | `54`, `32` | Write-queue occupancy that starts and ends a write drain |
| `tCL`, `tCWL`, `tRCD`, `tRP`, `tRAS`, `tRTP`, `tWR` | uint32 | `22`, `16`, `22`, `22`, `52`, `12`, `24` | Core timings in DRAM cycles |
| `tCCD_S`, `tCCD_L`, `tRRD_S`, `tRRD_L`, `tFAW`, `tWTR_S`, `tWTR_L` | uint32 | `4`, `8`, `4`, `8`, `34`, `4`, `12` | Bank-group and activate-window timings |
| `tBURST`, `tRTRS` | uint32 | `4`, `2` | Data-bus cycles per line; rank-to-rank switch |
| `tREFI`, `tRFC` | uint32 | `12480`, `560` | Refresh interval and duration; `tRFC: 0` disables refresh |

The defaults are a DDR4-3200AA (22-22-22) speed bin from JESD79-4 with x8,
8 Gb devices: two 64-bit channels, 25.6 GB/s peak each. The queue depths and
write-drain watermarks are gem5's defaults (`MemInterface` read/write buffers
of 32/64, `MemCtrl` write thresholds of 85%/50%). Every power-of-two count is
checked at start-up.

- **Policy.** Open page. Each channel issues one command per DRAM cycle and
  schedules FR-FCFS (Rixner et al., ISCA 2000): the oldest ready row hit first,
  then the oldest request whose ACT or PRE is ready; a row with queued hits is
  not closed. Reads have priority; writes drain from the high to the low
  watermark, or when no read is waiting. A read that matches a queued write is
  answered from the write queue.
- **Address map.** From the line offset up: channel, bank group, column, bank,
  rank, row. A sequential stream alternates channels and bank groups. Bank and
  bank-group bits are XORed with low row bits (Zhang, Zhu and Zhang, MICRO 2000).
- **Latency.** An idle read costs tRCD+tCL+tBURST (row closed), tCL+tBURST (row
  hit) or tRP+tRCD+tCL+tBURST (row conflict), converted to core cycles, plus
  `static_latency_ns`. At 3 GHz a pointer chase that misses the L2 costs about
  270 cycles (90 ns) per load. No LLC is modelled; `static_latency_ns`
  includes an LLC miss.
- **Statistics.** `num_reads`, `num_writes`, `num_row_hits`/`misses`/`conflicts`,
  `num_refreshes`, `num_write_drains`, `avg_read_latency` (core cycles from the
  L2 request to data back at the L2), `bandwidth_gbps`, and average read- and
  write-queue occupancy. The pipeline summary prints a `DRAM:` line and the L2
  MPKI.

---

## Advanced Microarchitectural Features

### Write-Port Arbiter

Models the limited number of physical register file write ports shared by the
execute groups and the LSQ (load results). Parameters live on Execute:

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `write_port_mode` | string | `unified` | `unified` or `mapped` |
| `write_port_count` | uint32 | `4` | Number of write ports (unified mode). `0` disables write-port arbitration |
| `write_port_mapping` | string list | `[]` | One entry per port (mapped mode): the `+`-separated sources that may use it, either execute-group indices (see [Execute](#execute)) or `lsq` |

In `unified` mode every port accepts every source; ports pick sources
round-robin. A result that loses arbitration holds its functional unit and
retries next cycle, which delays its ROB completion but not its consumers: they
were woken at issue. That is optimistic for a consumer issued after the bypass
window, which would read the register file before the write.
`execute.num_write_port_denials` counts held result-cycles and
`num_write_port_conflict_cycles` the cycles with at least one. Example of a
`mapped` configuration for the `functional` granularity:

```yaml
top.core0:
  execute:
    params:
      granularity: functional
      write_port_mode: mapped
      write_port_mapping: ["0+1", "2+3", "lsq", "0+1+2+3+4+5+6+7+lsq"]
```

### Writeback Buffer

Buffers completed uops between Execute and the write-port arbiter. It only has
an effect while the write-port arbiter is enabled. Parameters live on Execute:

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `writeback_buffer_enabled` | bool | `false` | Enable the writeback buffer |
| `writeback_buffer_capacity` | uint32 | `16` | Buffer entries; Execute holds results while it is full |
| `writeback_buffer_drain_width` | uint32 | `4` | Entries offered to the arbiter per cycle |
| `writeback_buffer_latency` | uint32 | `1` | Cycles an entry stays in the buffer before it can be drained |

### Wakeup and Bypass

A uop spends `regfile_read_latency` cycles in register read between issue and
execute. This lengthens the branch-mispredict loop but not dependent chains,
because results reach consumers on a full bypass network:

- A uop other than a load or an atomic issued at cycle `c` wakes its
  consumers at `c + latency`, so dependent 1-cycle ALU uops issue back to back
  and a chain costs exactly the producer's latency.
- An atomic (AMO, LR or SC) wakes its consumers when the LSQ completes it,
  after its access at the ROB head returns.
- A load is not woken speculatively. When its single D-cache access hits, the
  D-cache reports the cycle its data returns at tag-check time, and the load's
  consumers issue so that they reach execute with the data. Load-to-use is
  `lat_load + max(hit_latency, regfile_read_latency + 1)`: the wakeup cannot
  precede the tag check. Misses, forwarded loads and loads that cross a line
  wake at write-back. `num_load_hit_wakeups` counts the early wakeups.

In-order mode has no register-read stage: Rename dispatches straight to execute
and applies the same rules to its scoreboard, as a five-stage pipeline with full
bypassing (e.g. Rocket) does. Dependent 1-cycle ALU uops dispatch back to back,
and an L1 hit's load-to-use is `lat_load + hit_latency`.

### Register File Banking

Models a banked physical register file with a limited number of read ports per
bank. Physical register `p` lives in bank `p % regfile_num_banks`; a uop is only
selected for issue when every bank it reads from still has a free read port in
that cycle. A uop that reads more registers from one bank than the bank has
ports issues when that bank is otherwise idle and takes all its ports; the extra
read cycles are not modelled. Parameters live on Rename:

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `regfile_banking_enabled` | bool | `false` | Enable register file banking |
| `regfile_num_banks` | uint32 | `4` | Number of banks (at most 16) |
| `regfile_reads_per_bank` | uint32 | `2` | Read ports per bank per cycle |

---

## Execution Driver

`top.core0.edriver.params` (the node exists only when `--target-elf` or
`--snapshot` is given)

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `target_command` | string | `""` | ELF to execute; set from `--target-elf` |
| `whisper_flags` | string list | `[]` | Extra command-line flags passed to Whisper |
| `snapshot_foldername` | string | `""` | Whisper snapshot to start from; set from `--snapshot` |
| `enable_snapshot_usage` | bool | `false` | Load the snapshot in `snapshot_foldername` |
| `allow_early_termination` | bool | `false` | Do not treat ending the run before the program finishes as an error |
| `shared_hart_count` | uint32 | `1` | Harts in the Whisper system all cores share; adds `--harts` unless `whisper_flags` has it. Set by the cluster |
| `private_system` | bool | `false` | This core builds its own single-hart Whisper system. Set by the cluster for a single-hart snapshot |

Whisper reads its configuration from `whisper.json` in the ELF's directory,
unless `whisper_flags` contains `--configfile`/`--config`. For example:

```bash
-p top.core0.edriver.params.whisper_flags "[--configfile,/path/to/whisper.json]"
```

---

## Visualization and Debugging

### Pipeline Visualizer

Records per-instruction pipeline timestamps and writes them at the end of the
run. Parameters live on `top.core0.params`:

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `visualizer_enabled` | bool | `false` | Enable the pipeline visualizer |
| `visualizer_max_instructions` | uint32 | `200` | Instructions to record (`0` = no limit) |
| `visualizer_format` | string | `table` | `table`, `waterfall`, `log` or `kanata` |
| `visualizer_output_file` | string | `stderr` | Output file, or `stderr` |
| `visualizer_color` | bool | `false` | ANSI colors in the `waterfall` format |
| `visualizer_debug_enabled` | bool | `false` | Also write a cycle-by-cycle waterfall, in pages of 20 cycles |
| `visualizer_debug_file` | string | `pipeline_debug.txt` | Output file of the debug waterfall |
| `visualizer_debug_start_cycle` | uint64 | `0` | First cycle of the debug waterfall |
| `visualizer_debug_end_cycle` | uint64 | `0` | Last cycle of the debug waterfall (`0` = end of run) |

Formats:

- `table`: one row per instruction with the cycle of each stage and notable
  events (I-cache miss, D-cache miss, misprediction, write-port stall).
- `waterfall`: stage occupancy per cycle, in pages of 32 cycles.
- `log`: `<cycle>;<id>;<event>;<data>` records for pipetrace viewers.
- `kanata`: Kanata log for the [Konata](https://github.com/shioyadan/Konata)
  pipeline viewer (`ext/konata`).

The debug waterfall is only produced while the visualizer is enabled.

### Cache Viewer

Traces requests, hits, misses, MSHR allocation, coalescing, fills and L2
arbitration of the structural-mode L1 caches and of the L2 (when enabled).
Parameters live on `top.core0.params`:

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `cache_viewer_enabled` | bool | `false` | Enable the cache viewer |
| `cache_viewer_format` | string | `log` | `log` (event list), `waterfall` (per-request timeline) or `summary`; `log` and `waterfall` also print the summary |
| `cache_viewer_output_file` | string | `stderr` | `stderr`, `stdout` or a file path |

### Logging

Use Sparta's logging option, which enables the log messages of every unit under
a tree path:

```bash
# Writeback / retirement activity only
-l top.core0.writeback info wb.log

# Every unit
-l top info all.log
```

### Statistics and Diagnostics

- `--report-all FILE` writes every counter of the simulation tree at the end of
  the run (append `json` for JSON output).
- The pipeline clock prints a summary (IPC, simulation speed and per-unit
  counters) to stderr every 1,000,000 cycles and at the end of the run.
- When nothing retires for 10,000 cycles, the pipeline clock prints the state
  of the ROB, Issue, Execute, LSQ and Rename once to stderr.
- The ROB watchdog aborts the run after `writeback.params.retire_timeout_cycles`
  cycles without retirement.

---

## Example Configurations

The shipped configurations are a good starting point:

- `config.yaml`: out-of-order core with structural caches, a unified execute
  group behind nine typed issue ports (`issue_ports`), queues enabled and
  wrong-path speculation enabled.
- `config_inorder.yaml`: the same core with in-order execution.

The examples below are override files meant to be layered on top of
`config.yaml` with a second `-c`.

### No Wrong-Path Speculation

Fetch stalls at a mispredicted branch until it resolves, instead of running down
the wrong path:

```yaml
top.core0:
  params:
    wrong_path_enabled: false
```

### Partitioned Issue with an L2

```yaml
top.core0:
  issue:
    params:
      mode: partitioned
      issue_ports: []
      scheduler_configs: ["int:16:ALU+Mul+Div+Branch+Fence", "mem:16:Load+Store", "fp:8:FpOp+VecOp"]
      routing_priorities: ["dependency_locality", "type_affinity", "least_occupied", "round_robin"]
      selection: fifo
  l2cache:
    params:
      enabled: true
      arb_policy: icache_first
```

### Detailed Backend

```yaml
top.core0:
  params:
    wrong_path_enabled: true
  rename:
    params:
      regfile_banking_enabled: true
  execute:
    params:
      granularity: functional
      write_port_mode: mapped
      write_port_mapping: ["0+1", "2+3", "lsq", "0+1+2+3+4+5+6+7+lsq"]
  lsq:
    params:
      store_forwarding: true
  dcache:
    params:
      num_banks: 2
      fill_occupies_write_port: true
  l2cache:
    params:
      enabled: true
      inclusivity_policy: inclusive
      arb_policy: round_robin
```

### Pipeline Trace for Konata

```bash
./build/core/core -i 5000 -c models/cpu/src/core/config.yaml \
    -p top.core0.params.visualizer_enabled true \
    -p top.core0.params.visualizer_format kanata \
    -p top.core0.params.visualizer_max_instructions 0 \
    -p top.core0.params.visualizer_output_file pipeline.kanata \
    --target-elf tests/build/coremark.bare.elf
```
