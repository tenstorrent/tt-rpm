// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Tenstorrent USA, Inc.

#include <cstdint>
#include <vector>

#include "predictors/common/DirectionPredictorInterface.hpp"
#include "predictors/common/PredictorRegistry.hpp"

namespace predictors {

namespace {

// PC-indexed 2-bit counters (16K entries, 4KB): a history-free baseline.
class Bimodal : public DirectionPredictorInterface {
   public:
    explicit Bimodal(const PredictorArgs&)
        : mCtr(kEntries, 1) {}

    DirectionResult predict(const BranchInfo& b) override {
        const uint8_t c = mCtr[index(b.pc)];
        return DirectionResult{c >= 2};
    }
    BPCheckpoint snapshot() const override { return {}; }
    void speculativeUpdate(const BranchInfo&, bool, addr_t) override {}
    void update(const DirectionUpdate& u) override {
        uint8_t& c = mCtr[index(u.b.pc)];
        if (u.taken && c < 3) ++c;
        if (!u.taken && c > 0) --c;
    }
    void restore(const BPCheckpoint&) override {}

   private:
    static constexpr uint32_t kEntries = 1u << 14;
    static uint32_t index(addr_t pc) { return static_cast<uint32_t>((pc ^ (pc >> 14)) >> 1) & (kEntries - 1); }

    std::vector<uint8_t> mCtr;
};

}  // namespace

REGISTER_PREDICTOR(DirectionPredictorInterface, "bimodal", Bimodal)

}  // namespace predictors
