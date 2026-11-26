// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Tenstorrent USA, Inc.

#pragma once

#include "common/AlgorithmRegistry.hpp"
#include "predictors/common/PredictorArgs.hpp"

namespace predictors {

template <typename Iface>
using Registry = rpm::Registry<Iface, PredictorArgs>;

}  // namespace predictors

#define REGISTER_PREDICTOR(Iface, KEY, Type) RPM_REGISTER_ALGORITHM(Iface, predictors::PredictorArgs, KEY, Type)
