// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Tenstorrent USA, Inc.

#pragma once

#include <cstdint>

#include "predictors/common/Checkpoint.hpp"
#include "predictors/common/PredictorComponent.hpp"
#include "predictors/common/PredictorTypes.hpp"

namespace predictors {

struct IndirectResult {
    bool hit{false};
    addr_t target{0};
};

class IndirectPredictorInterface : public PredictorComponent {
   public:
    ~IndirectPredictorInterface() override = default;

    // On a BTB hit of an indirect jump or call; a miss keeps the BTB's last target.
    virtual IndirectResult predict(const BranchInfo& b) = 0;
    // Before speculativeUpdate(), as for the direction predictor.
    virtual BPCheckpoint snapshot() const = 0;
    // Every branch in fetch order, with the target the direction predictor's speculativeUpdate gets.
    virtual void speculativeUpdate(const BranchInfo& b, bool taken, addr_t target) = 0;
    // Only for retired branches predict() was called for, so at_predict (the snapshot taken after
    // it) holds their own lookup; `mispredicted` is the branch's final target prediction.
    virtual void update(const BranchInfo& b, addr_t actual_target, bool mispredicted, const BPCheckpoint& at_predict) = 0;
    virtual void restore(const BPCheckpoint& cp) = 0;
};

}  // namespace predictors
