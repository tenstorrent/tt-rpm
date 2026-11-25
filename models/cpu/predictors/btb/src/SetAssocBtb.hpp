// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Tenstorrent USA, Inc.

#pragma once

#include <vector>

#include "predictors/common/BtbInterface.hpp"
#include "predictors/common/PredictorArgs.hpp"

namespace predictors {

// Set-associative BTB, trained only at commit. LRU recency is refreshed by every lookup hit, wrong path included.
class SetAssocBtb : public BtbInterface {
   public:
    explicit SetAssocBtb(const PredictorArgs& a);

    BtbEntry lookup(addr_t pc) override;
    void allocate(addr_t pc, addr_t target, BranchType type) override;

   private:
    struct Way {
        bool valid{false};
        uint64_t tag{0};
        addr_t target{0};
        BranchType type{BranchType::NotABranch};
        uint64_t last_use{0};
    };

    Way* set_(addr_t pc) { return &mWays[((pc >> 1) & (mNumSets - 1)) * mAssoc]; }
    uint64_t tag_(addr_t pc) const { return (pc >> (1 + mIndexBits)) & mTagMask; }
    Way* find_(addr_t pc);
    void fill_(Way& w, addr_t target, BranchType type);

    uint32_t mNumSets;
    uint32_t mAssoc;
    uint32_t mIndexBits;
    uint64_t mTagMask;
    bool mRandomReplacement;
    uint64_t mRngState;
    uint64_t mUseClock{0};
    std::vector<Way> mWays;  // mNumSets x mAssoc
};

}  // namespace predictors
