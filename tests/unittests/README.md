# RPM minimal perf unit-tests

A small, self-contained regression harness for the RPM core model. It builds a
minimal workload set, runs each through `build/core/core`, extracts a few metrics
from the model's end-of-run stats block, and asserts loose bounds — pass/fail with a
non-zero exit so it can gate CI.

Stdlib Python only. No venv, no pip, no model source changes.

## Files

| file | role |
|---|---|
| `run_checks.py` | the whole harness: build + run + parse stats + assert + report |
| `run_smoke.sh` | runs `run_checks.py`, then `clock_domain_test.py`; the CI hook point |
| `clock_domain_test.py` | checks per-core clock ratios on the heterogeneous cluster |
| `baseline.json` | recorded metric bounds per workload (written by `--calibrate`) |
| `perf_report_test.py` | checks `scripts/parse_scripts/perf_report.py` against the hand-made study in `perf_report_study/`; needs no model or toolchain (`python3 tests/unittests/perf_report_test.py`) |

## How it works

For each entry (currently **dhrystone** and **coremark**, the two workloads already
wired in `tests/Makefile`):

1. **build** — `make -C tests <target>` produces a bare-metal ELF in `tests/build/`.
2. **run** — `core -c <config> -i 0 --target-elf <elf>` runs to HTIF `tohost`.
3. **extract** — parse the last `=== Pipeline Stats @ cycle N ===` block from the
   model's stderr (`models/cpu/src/core/Pipeline.cpp:reportStats()`): `ipc`,
   `retired`, `cycles`, I-cache/D-cache/L2 hit rates, BP accuracy.
4. **assert** — the model exits 0 and the workload prints its last line
   (CoreMark's `Correct operation validated.`, Dhrystone's `Dhrystones per Second:`),
   which catches premature termination. Then compare against `baseline.json`:
   - `ipc`, `bp_acc`: must exceed a lower bound (gross-regression guard).

   The retired count is printed but not checked: it depends on the compiler and
   newlib build (Dhrystone retires 277,046 with gcc 10.2 and a size-optimised
   newlib, 184,286 with gcc 14.2, 177,780 with gcc 16.1).

## Prerequisites

- Bare-metal RISC-V toolchain (`riscv64-unknown-elf-gcc`). One-time:
  `bash tests/install-toolchain-conda.sh && conda activate riscv`.
- A built core model at `build/core/core` (override with `--core`).
- `git submodule update --init tests/coremark tests/riscv-tests`.

## Usage

First time — record baselines against the current, known-good build:

```
bash tests/unittests/run_smoke.sh --calibrate
```

This writes `baseline.json`. Review it, then commit it.

Thereafter — check against the recorded baselines:

```
bash tests/unittests/run_smoke.sh
```

Exit code is `0` if every check passes, non-zero otherwise.

Useful flags (forwarded to `run_checks.py`):

```
--only dhrystone        run a single entry
--core  PATH            core binary (default build/core/core)
--config PATH           Sparta config.yaml (default: build/core, else source tree)
--elf-dir DIR           run prebuilt ELFs from DIR (needs a whisper.json) instead of building
--calibrate             re-record baselines instead of checking
```

## Calibration policy

Bounds in `baseline.json` are derived from one observed run:

- `ipc`    lower bound = `0.80 × observed`
- `bp_acc` lower bound = `0.90 × observed`

These are coarse regression guards, not fine-grained drift detection. Re-run
`--calibrate` (and commit the new `baseline.json`) after an intended, reviewed change
to model behavior. Record the build/commit the baseline was taken against.

> **Status:** the committed `baseline.json` was calibrated against a Release
> `build/core/core` of this tree with the shipped `config.yaml`, on ELFs built by
> `tests/Makefile` with `riscv64-unknown-elf-gcc` 14.2.0 (dhrystone IPC 2.259,
> BP 99.85%; coremark IPC 2.045, BP 99.71%). The same bounds pass with the CI
> Dockerfile toolchain (gcc 10.2.0: dhrystone IPC 2.137, coremark 2.055) and the
> conda one (gcc 16.1.0: dhrystone 2.253, coremark 2.001).

## Scope / future work

Out of scope for now: additional directed microbenchmarks, and per-core cluster
checks. Adding a new workload is a matter of dropping a build target in
`tests/Makefile` and one entry in `run_checks.py`.
