// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Tenstorrent USA, Inc.

#include <cstdint>
#include <vector>

#include "predictors/common/DirectionPredictorInterface.hpp"
#include "predictors/common/PredictorRegistry.hpp"

namespace predictors {

namespace {

// gshare (McFarling, "Combining Branch Predictors", WRL TN-36, 1993): 2-bit counters indexed by
// the global history of conditional outcomes XORed with the branch address, both history_bits wide.
class Gshare : public DirectionPredictorInterface {
   public:
    explicit Gshare(const PredictorArgs& a)
        : mBits(a.get<uint32_t>("history_bits", 14)) {
        if (mBits < 1 || mBits > 24) a.fail("history_bits", "must be in [1, 24]");
        mCounters.assign(size_t{1} << mBits, 1);
    }

    DirectionResult predict(const BranchInfo& b) override {
        const uint8_t c = mCounters[index(b.pc, mHistory)];
        return DirectionResult{c >= 2};
    }
    BPCheckpoint snapshot() const override { return BPCheckpoint(std::make_unique<History>(mHistory)); }
    void speculativeUpdate(const BranchInfo& b, bool taken, addr_t) override {
        if (b.type == BranchType::Conditional) mHistory = ((mHistory << 1) | taken) & mask();
    }
    void update(const DirectionUpdate& u) override {
        const History* h = u.at_predict.as<History>();
        if (!h) return;
        uint8_t& c = mCounters[index(u.b.pc, h->bits)];
        if (u.taken && c < 3) ++c;
        if (!u.taken && c > 0) --c;
    }
    void restore(const BPCheckpoint& cp) override {
        if (const History* h = cp.as<History>()) mHistory = h->bits;
    }

   private:
    struct History : BPCheckpointBase {
        explicit History(uint32_t b)
            : bits(b) {}
        uint32_t bits;
    };

    uint32_t mask() const { return (1u << mBits) - 1; }
    // RISC-V branches sit on 2-byte boundaries, so bit 0 of the pc carries no information.
    uint32_t index(addr_t pc, uint32_t history) const { return (static_cast<uint32_t>(pc >> 1) ^ history) & mask(); }

    const uint32_t mBits;
    std::vector<uint8_t> mCounters;
    uint32_t mHistory{0};
};

}  // namespace

REGISTER_PREDICTOR(DirectionPredictorInterface, "gshare", Gshare)

}  // namespace predictors
