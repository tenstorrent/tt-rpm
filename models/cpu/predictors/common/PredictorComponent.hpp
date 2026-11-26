// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Tenstorrent USA, Inc.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace predictors {

// A counter an algorithm keeps; the host reports it as <structure>_<name>.
struct PredictorStat {
    std::string name;
    std::string desc;
    const uint64_t* value;  // valid for the component's lifetime
};

class PredictorComponent {
   public:
    virtual ~PredictorComponent() = default;

    virtual std::vector<PredictorStat> stats() const { return {}; }
};

}  // namespace predictors
