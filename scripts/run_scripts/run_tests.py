#!/usr/bin/env python3
"""Run microbenchmark suite with different configs, collecting pipeline traces."""
import subprocess, os, re, sys, copy
import yaml

REPO     = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "../.."))
CONFIG   = os.path.join(REPO, "models/cpu/src/core/config.yaml")
CORE_BIN = os.path.join(REPO, "build/core/core")
BUILD    = os.path.join(REPO, "build/core")
ELF_DIR  = os.path.join(REPO, "tests/programs")
TICKS    = "500"

# Write modified configs to the build directory so the source config is never touched.
BUILD_CONFIG = os.path.join(BUILD, "config.yaml")

def load_config():
    with open(CONFIG) as f:
        return yaml.safe_load(f)

def save_config(cfg):
    # Ensure the visualizer writes a pipeline trace file for test validation
    core = cfg['top.core0']
    core['params']['visualizer_enabled'] = True
    core['params']['visualizer_output_file'] = 'pipeline.txt'
    with open(BUILD_CONFIG, 'w') as f:
        yaml.dump(cfg, f, default_flow_style=False, sort_keys=False)

def run_test(elf_name):
    elf = os.path.join(ELF_DIR, elf_name)
    subprocess.run([CORE_BIN, "-r", TICKS, "-c", BUILD_CONFIG, "--target-elf", elf],
                   cwd=BUILD, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    pipe = os.path.join(BUILD, "pipeline.txt")
    with open(pipe) as f:
        return f.read()

def print_section(title, description, expected, trace):
    print(f"\n{'='*70}")
    print(f"  {title}")
    print(f"{'='*70}")
    print(f"  Config: {description}")
    print(f"  Expected: {expected}")
    print(f"{'='*70}")
    print(trace)

def set_execute(cfg, granularity, **overrides):
    exe = cfg['top.core0']['execute']['params']
    exe['granularity'] = granularity
    for k, v in overrides.items():
        exe[k] = v
    return cfg

def set_write_ports(cfg, mode=None, num_ports=None, mapping=None):
    exe = cfg['top.core0']['execute']['params']
    if mode is None:
        exe.pop('write_port_mode', None)
        exe.pop('write_port_count', None)
        return cfg
    exe['write_port_mode'] = mode
    if num_ports is not None:
        exe['write_port_count'] = num_ports
    return cfg

# ── Tests ──

def test_A1():
    cfg = load_config()
    set_execute(cfg, 'unified', unified_max_in_flight=2)
    set_write_ports(cfg, mode=None)
    save_config(cfg)
    trace = run_test("burst_8.elf")
    print_section(
        "A1: Unified Execute, max_in_flight=2, NO write ports",
        "unified, max_in_flight=2, dispatch_width=4",
        "Only 2 instructions enter Ex per cycle (Rename stalls at 3rd).\n"
        "  Tags 0-1 execute cycle 6, tags 2-3 at cycle 7, etc.",
        trace)

def test_A2():
    cfg = load_config()
    set_execute(cfg, 'typed',
                int_max_in_flight=2,
                fp_max_in_flight=2,
                vec_max_in_flight=2,
                branch_max_in_flight=2)
    set_write_ports(cfg, mode=None)
    save_config(cfg)
    trace = run_test("alu_mul_mix.elf")
    print_section(
        "A2: Typed Execute, int_max_in_flight=2 — ALU+MUL share INT pool",
        "typed, int_max_in_flight=2, alu_mul_mix (4 ADDI + 4 MUL)",
        "ALU and MUL both map to INT group (size 2). Only 2 INT ops execute\n"
        "  per cycle, regardless of ALU vs MUL type.",
        trace)

def test_A3():
    cfg = load_config()
    # functional mode is not yet supported by the core model — use typed as a proxy
    set_execute(cfg, 'typed',
                int_max_in_flight=2,
                fp_max_in_flight=1,
                vec_max_in_flight=1,
                branch_max_in_flight=1)
    set_write_ports(cfg, mode=None)
    save_config(cfg)
    trace = run_test("alu_mul_interleaved.elf")
    print_section(
        "A3: Typed Execute, int_max_in_flight=2 — ALU+MUL interleaved",
        "typed, int_max_in_flight=2",
        "ALU and MUL both map to INT group. Up to 2 INT ops execute per cycle.",
        trace)

def test_B1():
    cfg = load_config()
    set_execute(cfg, 'unified', unified_max_in_flight=8)
    set_write_ports(cfg, mode='unified', num_ports=1)
    save_config(cfg)
    trace = run_test("burst_8.elf")
    print_section(
        "B1: 1 Write Port, Unified Execute (max=8)",
        "1 unified write port, dispatch_width=4",
        "All 4 instructions from first fetch group execute at cycle 6, but only 1\n"
        "  can WB per cycle. Instructions sit in Ex for extra cycles ('Ex Ex Ex...').\n"
        "  8 instructions drain through 1 port over ~8 WB slots.",
        trace)

def test_B2():
    cfg = load_config()
    set_execute(cfg, 'unified', unified_max_in_flight=8)
    set_write_ports(cfg, mode='unified', num_ports=2)
    save_config(cfg)
    trace = run_test("burst_8.elf")
    print_section(
        "B2: 2 Write Ports, Unified Execute (max=8)",
        "2 unified write ports, dispatch_width=4",
        "Same as B1 but 2 WB per cycle. Should drain roughly 2x faster.\n"
        "  Tags 0-1 WB at cycle 7, tags 2-3 WB at cycle 8, etc.",
        trace)

def test_B3():
    cfg = load_config()
    set_execute(cfg, 'typed',
                int_max_in_flight=4,
                fp_max_in_flight=4,
                vec_max_in_flight=4,
                branch_max_in_flight=2)
    set_write_ports(cfg, mode='unified', num_ports=2)
    save_config(cfg)
    trace = run_test("alu_load_mix.elf")
    print_section(
        "B3: 2 Write Ports, Typed Execute — ALU + load mix",
        "typed, int_max_in_flight=4, 2 unified write ports",
        "ALU and load completions share 2 write ports.\n"
        "  With 2 ports, both can WB simultaneously when ready.",
        trace)

def test_B4():
    cfg = load_config()
    set_execute(cfg, 'typed',
                int_max_in_flight=4,
                fp_max_in_flight=4,
                vec_max_in_flight=4,
                branch_max_in_flight=2)
    set_write_ports(cfg, mode='unified', num_ports=1)
    save_config(cfg)
    trace = run_test("alu_load_mix.elf")
    print_section(
        "B4: 1 Write Port, Typed Execute — ALU + load contention",
        "typed, int_max_in_flight=4, 1 unified write port",
        "ALU and load completions COMPETE for the same port.\n"
        "  More stalling than B3 — one must wait if both are ready same cycle.",
        trace)

if __name__ == '__main__':
    tests = [test_A1, test_A2, test_A3, test_B1, test_B2, test_B3, test_B4]
    for t in tests:
        t()
