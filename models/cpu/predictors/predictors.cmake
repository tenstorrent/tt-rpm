# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: 2026 Tenstorrent USA, Inc.

# Branch-predictor algorithm sources (interfaces are header-only), shared by the core and cluster builds.
set(PREDICTOR_SOURCES
    ${RPM_ROOT}/models/cpu/predictors/bimodal/src/Bimodal.cpp
    ${RPM_ROOT}/models/cpu/predictors/gshare/src/Gshare.cpp
    ${RPM_ROOT}/models/cpu/predictors/tage/src/TageScL.cpp
    ${RPM_ROOT}/models/cpu/predictors/tage/src/TageScLRegistration.cpp
    ${RPM_ROOT}/models/cpu/predictors/btb/src/SetAssocBtb.cpp
    ${RPM_ROOT}/models/cpu/predictors/ras/src/CircularRas.cpp
)
