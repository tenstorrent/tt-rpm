// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Tenstorrent USA, Inc.

#pragma once

#include <cstdint>

#include "predictors/common/PredictorComponent.hpp"
#include "predictors/common/PredictorTypes.hpp"

namespace predictors {

struct BtbEntry {
    bool hit{false};
    addr_t target{0};
    BranchType type{BranchType::NotABranch};
};

// Trained only at retire (allocate). Not checkpointed: lookup() may only touch replacement state.
class BtbInterface : public PredictorComponent {
   public:
    ~BtbInterface() override = default;

    virtual BtbEntry lookup(addr_t pc) = 0;
    virtual void allocate(addr_t pc, addr_t target, BranchType type) = 0;
};

}  // namespace predictors
