#!/usr/bin/env python3
"""Run the new targeted stress tests and print pipeline waterfall output."""
import subprocess, os, yaml, copy

REPO     = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "../.."))
CONFIG   = os.path.join(REPO, "models/cpu/src/core/config.yaml")
CORE_BIN = os.path.join(REPO, "build/core/core")
BUILD    = os.path.join(REPO, "build/core")
PROGRAMS = os.path.join(REPO, "tests/programs")
TICKS    = "2000"

# Write modified configs to the build directory so the source config is never touched.
BUILD_CONFIG = os.path.join(BUILD, "config.yaml")

def load_config():
    with open(CONFIG) as f:
        return yaml.safe_load(f)

def core_params(cfg):
    """Get the core0 params dict from the Sparta-native config."""
    return cfg['top.core0']['params']

def unit_params(cfg, unit_name):
    """Get a unit's params dict from the Sparta-native config."""
    return cfg['top.core0'][unit_name]['params']

def save_config(cfg):
    with open(BUILD_CONFIG, 'w') as f:
        yaml.dump(cfg, f, default_flow_style=False, sort_keys=False)

PIPE = os.path.join(BUILD, "pipeline.txt")

def run_test(elf_name, max_instructions=25):
    cfg = load_config()
    core_params(cfg)['visualizer_enabled'] = True
    core_params(cfg)['visualizer_max_instructions'] = max_instructions
    core_params(cfg)['visualizer_color'] = False
    core_params(cfg)['visualizer_format'] = 'waterfall'
    core_params(cfg)['visualizer_output_file'] = 'pipeline.txt'
    save_config(cfg)
    elf = os.path.join(PROGRAMS, elf_name)
    subprocess.run([CORE_BIN, '-r', TICKS, '--target-elf', elf, '-c', BUILD_CONFIG], cwd=BUILD, capture_output=True, text=True)
    import re
    with open(PIPE) as f:
        raw = f.read()
    return re.sub(r'\x1b\[[0-9;]*m', '', raw)

DIVIDER = "=" * 72

TESTS = [
    ("age_matrix_order",
     "age_matrix_order.elf",
     20,
     """WHAT TO LOOK FOR — Age matrix oldest-first selection
  Assembly:
    tags 0-3: addi x1-x4, x0, imm    (independent producers, ALU lat=1)
    tags 4-7: add x5-x8, xi, xi     (consumers, RAW on x1-x4)

  EXPECTED PATTERN (age_matrix selection, issue_width=4):
   - Tags 0-3 issue together in Is1 (all independent, all ready).
   - Tags 0-3 complete in Is1+1 (ALU latency=1).
   - Tags 4-7 all become ready in same cycle (when 0-3 complete).
   - With age_matrix: all 4 dependents can issue in Is2 (oldest-first among ready).
   - KEY SIGNAL: tags 4-7 should issue in order 4→5→6→7 if width permits,
     or in batches respecting age order if width < 4."""),

    ("dep_routing",
     "dep_routing.elf",
     20,
     """WHAT TO LOOK FOR — Dependency-aware routing (partitioned mode)
  Assembly:
    tag 0: addi x1, x0, 100   (producer)
    tags 1-4: independent fillers
    tags 5-7: add chain (RAW on x1, then x2, then x3)

  EXPECTED PATTERN (with dependency_locality routing priority):
   - Tag 0 routes to some scheduler S.
   - Tags 5, 6, 7 (dependent chain) should all route to S.
   - Enable ILOG to see scheduler routing decisions.
   - KEY SIGNAL: ILOG shows "routed to scheduler X via dependency_locality"
     for dependent instructions."""),

    ("partitioned_balance",
     "partitioned_balance.elf",
     20,
     """WHAT TO LOOK FOR — Load balancing across partitioned schedulers
  Assembly:
    tags 0-7: 8 independent addi instructions

  EXPECTED PATTERN (2 schedulers, round_robin or least_occupied):
   - Instructions should be distributed ~evenly across schedulers.
   - With 2 schedulers: ~4 in each.
   - Enable ILOG to see routing decisions.
   - KEY SIGNAL: ILOG shows alternating or balanced scheduler assignments."""),

    ("selection_tiebreak",
     "selection_tiebreak.elf",
     20,
     """WHAT TO LOOK FOR — Age matrix tie-breaking
  Assembly:
    tag 0: mul (3-cycle latency)
    tags 1-6: independent ADDIs
    tag 7: add dependent on MUL result

  EXPECTED PATTERN:
   - Tags 1-6 issue and complete quickly (independent).
   - Tag 7 waits for MUL result (3 cycles + wakeup delay).
   - When tag 7 becomes ready, it should issue immediately (no younger ready).
   - KEY SIGNAL: verify tag 7 issues as soon as MUL completes."""),

    ("cross_type_routing",
     "cross_type_routing.elf",
     20,
     """WHAT TO LOOK FOR — Cross-type dependency routing
  Assembly:
    tag 0: mul x1, x0, x0   (MUL producer, lat=3)
    tags 1-3: independent ADDIs
    tags 4-6: add chain dependent on MUL result

  EXPECTED PATTERN (with cross-type routing via 'accepts' field):
   - If MUL scheduler accepts ALU types, dependent ALUs route there.
   - Reduces cross-scheduler wakeup latency.
   - KEY SIGNAL: ILOG shows ALU instructions routing to MUL scheduler
     via dependency_locality when producer is in that scheduler."""),

    ("raw_ooo_wakeup",
     "raw_ooo_wakeup.elf",
     20,
     """WHAT TO LOOK FOR — OOO RAW wakeup latency
  Assembly:
    tag 0: addi x1, x0, 1       (producer, ALU lat=1)
    tag 1: addi x2, x0, 2       (independent filler)
    tag 2: addi x3, x0, 3       (independent filler)
    tag 3: addi x4, x0, 4       (independent filler)
    tag 4: add  x5, x1, x1      (RAW on x1 = tag 0)
    tag 5: add  x6, x5, x5      (RAW on x5 = tag 4)
    tag 6: add  x7, x6, x6      (RAW on x6 = tag 5)
    tag 7: addi x8, x0, 8       (independent)

  EXPECTED PATTERN:
   - Tags 0-3 issue in same batch (no deps). All enter Execute together.
   - Tags 1/2/3 complete Execute immediately (ALU, 0 remaining).
   - Tag 4 sits in Iq for ~2 cycles after tag 0 dispatches (tag 0 needs
     1 cycle in Execute + 1 cycle on completion port → wakeup arrives 2 cycles later).
   - Each successive RAW pair (4→5, 5→6) shows the same ~2-cycle gap.
   - Key: tags 1/2/3 and tag 7 should execute WITHOUT waiting for the chain."""),

    ("mul_ooo_parallel",
     "mul_ooo_parallel.elf",
     20,
     """WHAT TO LOOK FOR — OOO MUL + ALU parallel execution
  Assembly:
    tag 0: mul x3, x0, x0       (MUL, lat=3, independent)
    tag 1: mul x4, x0, x0       (MUL, lat=3, independent)
    tag 2: addi x10, x0, 10     (independent filler)
    tag 3: addi x11, x0, 11     (independent filler)
    tag 4: addi x12, x0, 12     (independent filler — fills MUL latency window)
    tag 5: addi x13, x0, 13     (independent filler)
    tag 6: add x5, x3, x3       (RAW on x3 = MUL result, tag 0)
    tag 7: add x6, x4, x4       (RAW on x4 = MUL result, tag 1)

  EXPECTED PATTERN:
   - Tags 0 and 1 (MUL) issue together; they both enter Execute in same cycle.
   - Tags 2-5 (independent ADDIs) issue and complete Execute immediately —
     visible as a tight F/D/Rn/Is/Ex/WB pipeline while MULs are still running.
   - Tags 6 and 7 sit in Iq until MUL wakeup: ~2 cycles after MUL completes
     (3-cycle Execute + 1-cycle port delay → total 4 cycles after MUL enters Execute).
   - KEY SIGNAL: tags 2-5 should show NO Iq stall despite tags 0 and 1
     being in multi-cycle Execute. This proves OOO issue works correctly."""),

    ("fu_saturate",
     "fu_saturate.elf",
     20,
     """WHAT TO LOOK FOR — Execute FU capacity limit (int_max_in_flight=6)
  Assembly:
    tags 0-7: 8 independent mul x_i, x0, x0  (lat=3 each)

  EXPECTED PATTERN (typed granularity, int_max_in_flight=6):
   - Tags 0-3 issue in cycle Is1 (issue_width=4).
   - Tags 4-5 issue in cycle Is2 (2 more slots available since int_max_in_flight=6).
   - Tags 6-7 are BLOCKED at Issue — INT group is full (6/6 in-flight).
     They should show extended Iq time.
   - After tags 0-3 complete Execute (3 cycles after Is1), 4 slots free.
   - Tags 6-7 can then issue.
   - KEY SIGNAL: Look for tags 6 and 7 showing much longer Iq labels
     compared to tags 0-5."""),

    ("wp_converge",
     "wp_converge.elf",
     20,
     """WHAT TO LOOK FOR — Write port convergence (5 completions, 4 ports)
  Assembly:
    tag 0: mul x1, x0, x0       (MUL, lat=3)
    tag 1: addi x10, x0, 1      (filler — completes 2 cycles before MUL)
    tag 2: addi x11, x0, 2      (filler — completes 2 cycles before MUL)
    tag 3: addi x2, x0, 2       (ALU — should finish same cycle as MUL)
    tag 4: addi x3, x0, 3       (ALU — same)
    tag 5: addi x4, x0, 4       (ALU — same)
    tag 6: addi x5, x0, 5       (ALU — same)
    tag 7: addi x6, x0, 6       (ALU — extra, might also converge)

  EXPECTED PATTERN (4 write ports, unified):
   The MUL (tag 0, 3 cycles) and some ALUs should complete in the same Execute
   cycle, generating more than 4 simultaneous write candidates.
   One or more instructions should show 'Wp' stall label (denied arbiter grant).
   KEY SIGNAL: any instruction showing Ex→Wp→WB pattern instead of Ex→WB."""),

    ("branch_penalty",
     "branch_penalty.elf",
     20,
     """WHAT TO LOOK FOR — Branch misprediction fetch stall
  Assembly:
    tag 0: addi x1, x0, 4       (loop counter = 4)
    tag 1: addi x1, x1, -1      (decrement counter)
    tag 2: bne  x1, x0, -8      (branch back while x1 != 0)
    tag 3: ecall

  EXPECTED PATTERN (bp_mispred_penalty=5, accuracy=0.75):
   - With 4 loop iterations and ~75% accuracy, ~1 misprediction expected.
   - On a misprediction: fetch stalls for 5 cycles.
   - The branch instruction (tag 2 or its iteration copy) should show 'Bp' event.
   - After the stall the pipeline should refill (visible as F . . . . D... pattern)."""),

    ("dcache_miss",
     "dcache_miss.elf",
     20,
     """WHAT TO LOOK FOR — D-Cache cold miss latency
  Assembly:
    tag 0: ld x1, x0, 0         (load from addr 0x000, cold miss line 0)
    tag 1: ld x2, x0, 64        (load from addr 0x040, cold miss line 1)
    tag 2: ld x3, x0, 128       (load from addr 0x080, cold miss line 2)
    tag 3: ld x4, x0, 192       (load from addr 0x0C0, cold miss line 3)
    tag 4: add x5, x1, x2       (RAW on x1 AND x2)
    tag 5: add x6, x3, x4       (RAW on x3 AND x4)
    tag 6: add x7, x5, x6       (RAW on x5 and x6)

  EXPECTED PATTERN (dcache structural, miss_latency=10, hit_latency=1):
   - All 4 loads hit different 64-byte cache lines → 4 cold misses.
   - With mshr_capacity=16 all 4 misses are serviced in parallel.
   - Each load should show 'dM' event marker and a long Lq (LSQ) stall (~11 cycles).
   - Tags 4/5/6 (dependent adds) sit in Iq until the loads complete.
   - KEY SIGNAL: very long Lq column for loads, then Iq for the dependent adds."""),
]

def run_test_with_dcache_structural(elf_name, max_instructions=25):
    """Run a test with dcache forced into structural mode (cold-miss capable)."""
    import re
    cfg = load_config()
    core_params(cfg)['visualizer_enabled'] = True
    core_params(cfg)['visualizer_max_instructions'] = max_instructions
    core_params(cfg)['visualizer_color'] = False
    core_params(cfg)['visualizer_format'] = 'waterfall'
    core_params(cfg)['visualizer_output_file'] = 'pipeline.txt'
    unit_params(cfg, 'dcache')['cache_mode'] = 'structural'
    save_config(cfg)
    elf = os.path.join(PROGRAMS, elf_name)
    subprocess.run([CORE_BIN, '-r', TICKS, '--target-elf', elf, '-c', BUILD_CONFIG], cwd=BUILD, capture_output=True, text=True)
    # Restore probabilistic mode
    unit_params(cfg, 'dcache')['cache_mode'] = 'probabilistic'
    save_config(cfg)
    with open(PIPE) as f:
        raw = f.read()
    return re.sub(r'\x1b\[[0-9;]*m', '', raw)


def run_test_with_ports(elf_name, num_ports, max_instructions=25):
    """Run a test with a specific write port count temporarily."""
    import re
    cfg = load_config()
    core_params(cfg)['visualizer_enabled'] = True
    core_params(cfg)['visualizer_max_instructions'] = max_instructions
    core_params(cfg)['visualizer_color'] = False
    core_params(cfg)['visualizer_format'] = 'waterfall'
    core_params(cfg)['visualizer_output_file'] = 'pipeline.txt'
    unit_params(cfg, 'execute')['write_port_mode'] = 'unified'
    unit_params(cfg, 'execute')['write_port_count'] = num_ports
    save_config(cfg)
    elf = os.path.join(PROGRAMS, elf_name)
    subprocess.run([CORE_BIN, '-r', TICKS, '--target-elf', elf, '-c', BUILD_CONFIG], cwd=BUILD, capture_output=True, text=True)
    unit_params(cfg, 'execute')['write_port_count'] = 4
    save_config(cfg)
    with open(PIPE) as f:
        raw = f.read()
    return re.sub(r'\x1b\[[0-9;]*m', '', raw)


def run_test_with_selection(elf_name, selection_mode, max_instructions=25):
    """Run a test with a specific selection mode (age_matrix, fifo, random)."""
    import re
    cfg = load_config()
    core_params(cfg)['visualizer_enabled'] = True
    core_params(cfg)['visualizer_max_instructions'] = max_instructions
    core_params(cfg)['visualizer_color'] = False
    core_params(cfg)['visualizer_format'] = 'waterfall'
    core_params(cfg)['visualizer_output_file'] = 'pipeline.txt'
    unit_params(cfg, 'issue')['selection'] = selection_mode
    save_config(cfg)
    elf = os.path.join(PROGRAMS, elf_name)
    result = subprocess.run([CORE_BIN, '-r', TICKS, '--target-elf', elf, '-c', BUILD_CONFIG], cwd=BUILD, capture_output=True, text=True)
    # restore selection to age_matrix
    unit_params(cfg, 'issue')['selection'] = 'age_matrix'
    save_config(cfg)
    with open(PIPE) as f:
        raw = f.read()
    return re.sub(r'\x1b\[[0-9;]*m', '', raw), result.stderr


def run_test_with_partitioned(elf_name, num_schedulers=2, routing_priorities=None, max_instructions=25):
    """Run a test with partitioned issue queue mode."""
    import re
    cfg = load_config()
    core_params(cfg)['visualizer_enabled'] = True
    core_params(cfg)['visualizer_max_instructions'] = max_instructions
    core_params(cfg)['visualizer_color'] = False
    core_params(cfg)['visualizer_format'] = 'waterfall'
    core_params(cfg)['visualizer_output_file'] = 'pipeline.txt'

    iss = unit_params(cfg, 'issue')
    iss['mode'] = 'partitioned'
    iss['selection'] = 'age_matrix'

    ALL_TYPES = 'ALU+Mul+Div+Branch+Load+Store+FpOp+VecOp+Fence'
    base_capacity = iss['issue_queue_capacity'] // num_schedulers

    # Encode as string params: "name:capacity:types"
    sched_configs = []
    for i in range(num_schedulers):
        sched_configs.append(f"sched{i}:{base_capacity}:{ALL_TYPES}")
    iss['scheduler_configs'] = sched_configs

    if routing_priorities:
        iss['routing_priorities'] = routing_priorities
        iss['routing_dep_occupancy_limit'] = 0.8
        iss['routing_overflow_threshold'] = 0.9

    save_config(cfg)
    elf = os.path.join(PROGRAMS, elf_name)
    result = subprocess.run([CORE_BIN, '-r', TICKS, '--target-elf', elf, '-c', BUILD_CONFIG], cwd=BUILD, capture_output=True, text=True)

    # Restore to unified mode
    iss['mode'] = 'unified'
    iss['scheduler_configs'] = []
    iss.pop('routing_priorities', None)
    save_config(cfg)

    with open(PIPE) as f:
        raw = f.read()
    return re.sub(r'\x1b\[[0-9;]*m', '', raw), result.stderr


WP_FORCED = (
    "wp_forced_2ports",
    "burst_8.elf",
    20,
    """WHAT TO LOOK FOR — Write port stalls forced (2 ports, 4 issue/cycle)
  Assembly: 8 independent addi xi, x0, i  (all ALU lat=1)
  Config override: num_write_ports=2 (config has 4 but overridden for this test)

  With issue_width=4 and ALU lat=1:
   - Batch 1 (tags 0-3): issue cycle Is, execute at Is+1, all 4 finish simultaneously.
     4 ALU candidates → 2 ports → 2 GRANTED, 2 DENIED (Wp stall).
   - The 2 denied instructions retry next cycle (only 1 port needed each time).

  EXPECTED PATTERN:
   Tags 0-3 should have some showing 'Wp' stall (Ex→Wp→WB instead of Ex→WB).
   Tags 4-7 (second batch, issued after first 4 free FU slots): similarly.
   KEY SIGNAL: 'Wp' label appears on ~2 out of every 4 simultaneous ALU completions."""
)

SELECTION_COMPARE_TESTS = [
    ("age_matrix_order_fifo",
     "age_matrix_order.elf",
     20,
     """COMPARISON: FIFO vs Age Matrix selection
  Running age_matrix_order.elf with selection=fifo for comparison.
  With FIFO, instructions issue in slot order rather than age order.
  Look for differences in issue ordering when multiple are ready."""),
]

PARTITIONED_TESTS = [
    ("partitioned_round_robin",
     "partitioned_balance.elf",
     20,
     ['round_robin'],
     """PARTITIONED TEST: Round-robin routing
  8 independent instructions with 2 schedulers.
  Should alternate: 0→S0, 1→S1, 2→S0, 3→S1, etc.
  Check ILOG for routing decisions."""),

    ("partitioned_least_occupied",
     "partitioned_balance.elf",
     20,
     ['least_occupied', 'round_robin'],
     """PARTITIONED TEST: Least-occupied routing
  8 independent instructions with 2 schedulers.
  Should balance based on occupancy (tie-break with round_robin).
  Check ILOG for routing decisions."""),

    ("partitioned_dependency",
     "dep_routing.elf",
     20,
     ['dependency_locality', 'type_affinity', 'round_robin'],
     """PARTITIONED TEST: Dependency-locality routing
  Producer-consumer chain with dependency_locality priority.
  Dependent instructions should route to producer's scheduler.
  Check ILOG for "dependency_locality" routing."""),
]


# ============================================================================
# NEW FEATURE TESTS: Bypass Network, Writeback Buffer, Register File Banking
# ============================================================================

def run_test_with_bypass(elf_name, bypass_enabled, spec_wakeup=True, prf_read_latency=1, max_instructions=25):
    """Run a test with bypass network enabled or disabled.
    
    With bypass ENABLED:
      - Speculative wakeup fires at: exec_lat + bypass_path_latency (typically 0)
      - Consumer can issue immediately after producer completes
      
    With bypass DISABLED:
      - Speculative wakeup fires at: exec_lat + prf_read_latency
      - Consumer must wait for PRF read after producer writes result
    """
    import re
    cfg = load_config()
    core_params(cfg)['visualizer_enabled'] = True
    core_params(cfg)['visualizer_max_instructions'] = max_instructions
    core_params(cfg)['visualizer_color'] = False
    core_params(cfg)['visualizer_format'] = 'waterfall'
    core_params(cfg)['visualizer_output_file'] = 'pipeline.txt'
    
    iss = unit_params(cfg, 'issue')
    iss['speculative_wakeup'] = spec_wakeup
    iss['bypass_network_enabled'] = bypass_enabled
    iss['bypass_regfile_read_latency'] = prf_read_latency
    if bypass_enabled:
        iss['bypass_paths'] = ["alu:any:0", "mul:any:0", "branch:any:0"]
    else:
        iss['bypass_paths'] = []
    save_config(cfg)

    elf = os.path.join(PROGRAMS, elf_name)
    result = subprocess.run([CORE_BIN, '-r', TICKS, '--target-elf', elf, '-c', BUILD_CONFIG], cwd=BUILD, capture_output=True, text=True)

    # Restore defaults
    iss['speculative_wakeup'] = False
    iss['bypass_network_enabled'] = False
    iss['bypass_regfile_read_latency'] = 1
    iss['bypass_paths'] = []
    save_config(cfg)
    
    with open(PIPE) as f:
        raw = f.read()
    return re.sub(r'\x1b\[[0-9;]*m', '', raw), result.stderr


def run_test_with_writeback_buffer(elf_name, enabled, latency=1, max_instructions=25):
    """Run a test with writeback buffer enabled or disabled."""
    import re
    cfg = load_config()
    core_params(cfg)['visualizer_enabled'] = True
    core_params(cfg)['visualizer_max_instructions'] = max_instructions
    core_params(cfg)['visualizer_color'] = False
    core_params(cfg)['visualizer_format'] = 'waterfall'
    core_params(cfg)['visualizer_output_file'] = 'pipeline.txt'
    
    unit_params(cfg, 'execute')['writeback_buffer_enabled'] = enabled
    unit_params(cfg, 'execute')['writeback_buffer_capacity'] = 16
    unit_params(cfg, 'execute')['writeback_buffer_drain_width'] = 4
    unit_params(cfg, 'execute')['writeback_buffer_latency'] = latency
    save_config(cfg)

    elf = os.path.join(PROGRAMS, elf_name)
    result = subprocess.run([CORE_BIN, '-r', TICKS, '--target-elf', elf, '-c', BUILD_CONFIG], cwd=BUILD, capture_output=True, text=True)

    unit_params(cfg, 'execute')['writeback_buffer_enabled'] = False
    save_config(cfg)
    
    with open(PIPE) as f:
        raw = f.read()
    return re.sub(r'\x1b\[[0-9;]*m', '', raw), result.stderr


def run_test_with_banking(elf_name, enabled, num_banks=4, reads_per_bank=2, max_instructions=25):
    """Run a test with register file banking enabled or disabled."""
    import re
    cfg = load_config()
    core_params(cfg)['visualizer_enabled'] = True
    core_params(cfg)['visualizer_max_instructions'] = max_instructions
    core_params(cfg)['visualizer_color'] = False
    core_params(cfg)['visualizer_format'] = 'waterfall'
    core_params(cfg)['visualizer_output_file'] = 'pipeline.txt'
    
    unit_params(cfg, 'rename')['regfile_banking_enabled'] = enabled
    unit_params(cfg, 'rename')['regfile_num_banks'] = num_banks
    unit_params(cfg, 'rename')['regfile_reads_per_bank'] = reads_per_bank
    unit_params(cfg, 'rename')['regfile_writes_per_bank'] = 1
    save_config(cfg)

    elf = os.path.join(PROGRAMS, elf_name)
    result = subprocess.run([CORE_BIN, '-r', TICKS, '--target-elf', elf, '-c', BUILD_CONFIG], cwd=BUILD, capture_output=True, text=True)

    unit_params(cfg, 'rename')['regfile_banking_enabled'] = False
    save_config(cfg)
    
    with open(PIPE) as f:
        raw = f.read()
    return re.sub(r'\x1b\[[0-9;]*m', '', raw), result.stderr


BYPASS_TESTS = [
    # (name, elf, max_inst, bypass_enabled, spec_wakeup, prf_read_latency, description)
    ("bypass_enabled_lat0",
     "bypass_chain.elf",
     25,
     True,   # bypass_enabled
     True,   # spec_wakeup enabled (required for early wake)
     1,      # prf_read_latency (not used when bypass enabled, but set anyway)
     """BYPASS NETWORK TEST: Bypass ENABLED (path latency=0)
  Assembly: 8-instruction dependency chain (each depends on previous)
  
  With bypass_network.enabled=true, bypass_path_latency=0:
   - Speculative wakeup fires at: exec_lat + bypass_lat = 1 + 0 = 1 cycle
   - Consumer issues 1 cycle after producer issues
   - Data forwarded directly from producer output
  
  EXPECTED: 1-cycle gaps between dependent instructions.
  This is the FASTEST configuration - bypass saves the PRF read."""),

    ("bypass_disabled_prf1",
     "bypass_chain.elf",
     30,
     False,  # bypass_disabled
     True,   # spec_wakeup enabled (required for early wake)
     1,      # prf_read_latency = 1
     """BYPASS NETWORK TEST: Bypass DISABLED (must read from PRF)
  Assembly: 8-instruction dependency chain (each depends on previous)
  
  With bypass_network.enabled=false, regfile_read_latency=1:
   - Speculative wakeup fires at: exec_lat + prf_read_lat = 1 + 1 = 2 cycles
   - Consumer issues 2 cycles after producer issues
   - Must wait for PRF write, then PRF read
  
  EXPECTED: 2-cycle gaps between dependent instructions.
  Compare to bypass_enabled to see the 1-cycle improvement from bypass."""),

    ("bypass_disabled_prf2",
     "bypass_chain.elf",
     40,
     False,  # bypass_disabled
     True,   # spec_wakeup enabled
     2,      # prf_read_latency = 2
     """BYPASS NETWORK TEST: Bypass DISABLED with higher PRF read latency
  Assembly: 8-instruction dependency chain (each depends on previous)
  
  With bypass_network.enabled=false, regfile_read_latency=2:
   - Speculative wakeup fires at: exec_lat + prf_read_lat = 1 + 2 = 3 cycles
   - Consumer issues 3 cycles after producer issues
   - Models a larger/slower register file
  
  EXPECTED: 3-cycle gaps between dependent instructions.
  Shows impact of PRF read latency on dependent chains."""),
]

WRITEBACK_BUFFER_TESTS = [
    ("writeback_buffer_lat1",
     "writeback_buffer.elf",
     20,
     True, 1,
     """WRITEBACK BUFFER TEST: 1-cycle buffer latency
  Assembly: Independent producers followed by dependent consumers
  
  With writeback_buffer.enabled=true, latency=1:
   - Completions sit in buffer for 1 cycle before PRF write
   - Dependent instructions see delayed wakeup
   - Ex→WB timing extended
  
  EXPECTED: Visible delay between Ex completion and WB."""),

    ("writeback_buffer_disabled",
     "writeback_buffer.elf",
     20,
     False, 0,
     """WRITEBACK BUFFER TEST: Buffer DISABLED (baseline)
  Assembly: Independent producers followed by dependent consumers
  
  With writeback_buffer.enabled=false:
   - Direct path from Execute to PRF write
   - No additional latency
  
  EXPECTED: Tight Ex→WB timing. Compare to enabled version."""),
]

BANKING_TESTS = [
    ("regfile_banking_enabled",
     "regfile_banking.elf",
     20,
     True, 4, 2,
     """REGISTER FILE BANKING TEST: Banking ENABLED (4 banks, 2 reads/bank)
  Assembly: Multiple instructions reading from same registers
  
  With regfile.banking_enabled=true, num_banks=4, reads_per_bank=2:
   - Physical registers distributed across banks (preg % num_banks)
   - Each bank has limited read ports per cycle
   - Instructions competing for same bank may stall
  
  EXPECTED: Some instructions may show extended Iq time waiting for bank reads."""),

    ("regfile_banking_disabled",
     "regfile_banking.elf",
     20,
     False, 4, 2,
     """REGISTER FILE BANKING TEST: Banking DISABLED (baseline)
  Assembly: Multiple instructions reading from same registers
  
  With regfile.banking_enabled=false:
   - Unlimited read ports (no banking constraints)
   - All ready instructions can issue immediately
  
  EXPECTED: No bank-related stalls. Compare to enabled version."""),
]


if __name__ == '__main__':
    for name, elf, max_inst, expected in TESTS:
        print(f"\n{DIVIDER}")
        print(f"  TEST: {name}")
        print(DIVIDER)
        print(expected)
        print(f"\n{'-'*72}")
        print(f"  ACTUAL PIPELINE WATERFALL:")
        print(f"{'-'*72}")
        if name == "dcache_miss":
            trace = run_test_with_dcache_structural(elf, max_inst)
        else:
            trace = run_test(elf, max_inst)
        print(trace)

    # Extra: forced WP stall with 2 ports
    name, elf, max_inst, expected = WP_FORCED
    print(f"\n{DIVIDER}")
    print(f"  TEST: {name}")
    print(DIVIDER)
    print(expected)
    print(f"\n{'-'*72}")
    print(f"  ACTUAL PIPELINE WATERFALL (num_write_ports=2):")
    print(f"{'-'*72}")
    trace = run_test_with_ports(elf, num_ports=2, max_instructions=max_inst)
    print(trace)

    # Selection mode comparison tests
    print(f"\n\n{'#'*72}")
    print(f"  SELECTION MODE COMPARISON TESTS")
    print(f"{'#'*72}")
    for name, elf, max_inst, expected in SELECTION_COMPARE_TESTS:
        print(f"\n{DIVIDER}")
        print(f"  TEST: {name}")
        print(DIVIDER)
        print(expected)
        print(f"\n{'-'*72}")
        print(f"  ACTUAL PIPELINE WATERFALL (selection=fifo):")
        print(f"{'-'*72}")
        trace, ilog = run_test_with_selection(elf, 'fifo', max_inst)
        print(trace)
        if ilog.strip():
            print(f"\n{'-'*72}")
            print(f"  ILOG OUTPUT (last 50 lines):")
            print(f"{'-'*72}")
            lines = ilog.strip().split('\n')
            print('\n'.join(lines[-50:]))

    # Partitioned mode tests
    print(f"\n\n{'#'*72}")
    print(f"  PARTITIONED MODE TESTS")
    print(f"{'#'*72}")
    for name, elf, max_inst, routing, expected in PARTITIONED_TESTS:
        print(f"\n{DIVIDER}")
        print(f"  TEST: {name}")
        print(DIVIDER)
        print(expected)
        print(f"\n{'-'*72}")
        print(f"  ACTUAL PIPELINE WATERFALL:")
        print(f"{'-'*72}")
        trace, ilog = run_test_with_partitioned(elf, num_schedulers=2, routing_priorities=routing, max_instructions=max_inst)
        print(trace)
        if ilog.strip():
            print(f"\n{'-'*72}")
            print(f"  ILOG OUTPUT (last 50 lines):")
            print(f"{'-'*72}")
            lines = ilog.strip().split('\n')
            print('\n'.join(lines[-50:]))

    # ============================================================================
    # NEW FEATURE TESTS
    # ============================================================================

    # Bypass Network tests
    print(f"\n\n{'#'*72}")
    print(f"  BYPASS NETWORK TESTS")
    print(f"{'#'*72}")
    for name, elf, max_inst, bypass_enabled, spec_wakeup, prf_read_lat, expected in BYPASS_TESTS:
        print(f"\n{DIVIDER}")
        print(f"  TEST: {name}")
        print(DIVIDER)
        print(expected)
        print(f"\n{'-'*72}")
        print(f"  ACTUAL PIPELINE WATERFALL (bypass={'enabled' if bypass_enabled else 'disabled'}, prf_read_lat={prf_read_lat}):")
        print(f"{'-'*72}")
        trace, ilog = run_test_with_bypass(elf, bypass_enabled, spec_wakeup, prf_read_lat, max_inst)
        print(trace)

    # Writeback Buffer tests
    print(f"\n\n{'#'*72}")
    print(f"  WRITEBACK BUFFER TESTS")
    print(f"{'#'*72}")
    for name, elf, max_inst, enabled, latency, expected in WRITEBACK_BUFFER_TESTS:
        print(f"\n{DIVIDER}")
        print(f"  TEST: {name}")
        print(DIVIDER)
        print(expected)
        print(f"\n{'-'*72}")
        print(f"  ACTUAL PIPELINE WATERFALL (wb_buffer={'enabled, lat='+str(latency) if enabled else 'disabled'}):")
        print(f"{'-'*72}")
        trace, ilog = run_test_with_writeback_buffer(elf, enabled, latency, max_inst)
        print(trace)

    # Register File Banking tests
    print(f"\n\n{'#'*72}")
    print(f"  REGISTER FILE BANKING TESTS")
    print(f"{'#'*72}")
    for name, elf, max_inst, enabled, num_banks, reads_per_bank, expected in BANKING_TESTS:
        print(f"\n{DIVIDER}")
        print(f"  TEST: {name}")
        print(DIVIDER)
        print(expected)
        print(f"\n{'-'*72}")
        print(f"  ACTUAL PIPELINE WATERFALL (banking={'enabled, '+str(num_banks)+'banks, '+str(reads_per_bank)+'reads/bank' if enabled else 'disabled'}):")
        print(f"{'-'*72}")
        trace, ilog = run_test_with_banking(elf, enabled, num_banks, reads_per_bank, max_inst)
        print(trace)

    # Restore source config in the build dir
    import shutil
    shutil.copy2(CONFIG, BUILD_CONFIG)
