// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Tenstorrent USA, Inc.

#pragma once

#include "predictors/common/Checkpoint.hpp"
#include "predictors/common/PredictorComponent.hpp"
#include "predictors/common/PredictorTypes.hpp"

namespace predictors {

// Rollback-critical: wrong-path calls/returns corrupt the stack pointer and
// entries, so snapshot()/restore() are mandatory (not no-ops).
class RasInterface : public PredictorComponent {
   public:
    ~RasInterface() override = default;

    virtual void push(addr_t return_addr) = 0;  // on (speculative) call
    virtual addr_t predict() = 0;               // top-of-stack for a return
    virtual void pop() = 0;                     // on (speculative) return
    virtual bool full() const = 0;              // a push now overwrites the oldest entry
    // After this branch's push/pop. A redirect restores the branch's own; a trap the youngest older one's.
    virtual BPCheckpoint snapshot() const = 0;
    virtual void restore(const BPCheckpoint& cp) = 0;
};

}  // namespace predictors
