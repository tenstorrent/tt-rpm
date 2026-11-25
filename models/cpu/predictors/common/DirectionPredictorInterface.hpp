// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Tenstorrent USA, Inc.

#pragma once

#include <cstdint>

#include "predictors/common/Checkpoint.hpp"
#include "predictors/common/PredictorComponent.hpp"
#include "predictors/common/PredictorTypes.hpp"

namespace predictors {

struct DirectionResult {
    bool taken{false};
};

struct DirectionUpdate {
    BranchInfo b;
    bool taken{false};  // actual outcome
    bool mispredicted{false};
    addr_t target{0};         // taken target
    BPCheckpoint at_predict;  // snapshot taken at predict() time
};

class DirectionPredictorInterface : public PredictorComponent {
   public:
    ~DirectionPredictorInterface() override = default;

    virtual DirectionResult predict(const BranchInfo& b) = 0;  // blind to truth
    // After predict(), before speculativeUpdate(): history plus that lookup. A redirect restores it
    // and re-applies the branch with its resolved outcome.
    virtual BPCheckpoint snapshot() const = 0;
    // Every branch, conditional or not, enters speculative history here in fetch order. `target` is
    // what Decode sees: the static target of a direct branch, the predicted one of an indirect jump.
    virtual void speculativeUpdate(const BranchInfo& b, bool taken, addr_t target) = 0;
    virtual void update(const DirectionUpdate& u) = 0;  // commit-time train
    virtual void restore(const BPCheckpoint& cp) = 0;   // roll back on flush
};

}  // namespace predictors
