// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Tenstorrent USA, Inc.

#include "predictors/ras/src/CircularRas.hpp"

#include "predictors/common/PredictorRegistry.hpp"

namespace predictors {

CircularRas::CircularRas(const PredictorArgs& a) {
    mStack.assign(a.get<uint32_t>("entries", 32), 0);
    if (mStack.empty()) a.fail("entries", "must be nonzero");
}

void CircularRas::push(addr_t return_addr) {
    mTop = (mTop + 1) % mStack.size();
    mStack[mTop] = return_addr;
    if (mCount < mStack.size()) ++mCount;
}

void CircularRas::pop() {
    mTop = (mTop + mStack.size() - 1) % mStack.size();
    if (mCount > 0) --mCount;
}

BPCheckpoint CircularRas::snapshot() const {
    auto s = std::make_unique<State>();
    s->top = mTop;
    s->count = mCount;
    s->top_entry = mStack[mTop];
    return BPCheckpoint(std::move(s));
}

void CircularRas::restore(const BPCheckpoint& cp) {
    const State* s = cp.as<State>();
    if (!s) return;
    mTop = s->top;
    mCount = s->count;
    mStack[mTop] = s->top_entry;
}

}  // namespace predictors

REGISTER_PREDICTOR(predictors::RasInterface, "circular", predictors::CircularRas)
