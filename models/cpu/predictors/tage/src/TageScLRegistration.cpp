// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Tenstorrent USA, Inc.

#include "predictors/common/PredictorRegistry.hpp"
#include "predictors/tage/src/TageScL.hpp"

namespace predictors::tage {

namespace {

struct TageScL64KB : TageScL {
    explicit TageScL64KB(const PredictorArgs&)
        : TageScL(config64KB(), "tage_sc_l_64kb") {}
};

struct TageScL8KB : TageScL {
    explicit TageScL8KB(const PredictorArgs&)
        : TageScL(config8KB(), "tage_sc_l_8kb") {}
};

}  // namespace

REGISTER_PREDICTOR(DirectionPredictorInterface, "tage_sc_l_64kb", TageScL64KB)
REGISTER_PREDICTOR(DirectionPredictorInterface, "tage_sc_l_8kb", TageScL8KB)

}  // namespace predictors::tage
