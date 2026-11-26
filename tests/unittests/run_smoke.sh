#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: 2026 Tenstorrent USA, Inc.
#
# RPM perf unit-test smoke run. All suites gating (non-zero exit on failure):
#   - run_checks.py       core-model workload metrics vs tests/unittests/baseline.json
#   - clock_domain_test.py  heterogeneous-cluster per-core clock-ratio check
#   - accel_dma_test, accel_fabric_test  accelerator harnesses (built with the cluster)
#
# First-time setup (records core baselines against the current build):
#   bash tests/unittests/run_smoke.sh --calibrate
# Thereafter:
#   bash tests/unittests/run_smoke.sh
#
# Args are forwarded to run_checks.py (e.g. --only dhrystone, --core PATH). With
# --calibrate only the core baselines are recorded; the cluster tests are skipped.

set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"

rc=0
python3 "${HERE}/run_checks.py" "$@" || rc=$?

# The cluster tests take no baseline; skip them during --calibrate.
case " $* " in
    *" --calibrate "*) ;;
    *) echo; python3 "${HERE}/clock_domain_test.py" || rc=$?
       for t in accel/accel_dma_test cluster/accel_fabric_test; do
           echo; "${HERE}/../../build/top/${t}" || rc=$?
       done ;;
esac

exit "$rc"
