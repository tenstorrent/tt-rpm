// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Tenstorrent USA, Inc.

#pragma once

#include <vector>

#include "predictors/common/PredictorArgs.hpp"
#include "predictors/common/RasInterface.hpp"

namespace predictors {

// Circular return-address stack: a push onto a full stack overwrites the oldest entry. A
// checkpoint holds only the top-of-stack pointer and top entry (Skadron et al., MICRO 1998),
// so wrong-path pops followed by pushes can still corrupt entries below the top.
class CircularRas : public RasInterface {
   public:
    explicit CircularRas(const PredictorArgs& a);

    void push(addr_t return_addr) override;
    addr_t predict() override { return mStack[mTop]; }
    void pop() override;
    bool full() const override { return mCount == mStack.size(); }
    BPCheckpoint snapshot() const override;
    void restore(const BPCheckpoint& cp) override;

   private:
    struct State : BPCheckpointBase {
        uint32_t top{0};
        uint32_t count{0};
        addr_t top_entry{0};
    };

    std::vector<addr_t> mStack;
    uint32_t mTop{0};
    uint32_t mCount{0};  // valid entries, saturating at the depth; only full() reads it
};

}  // namespace predictors
