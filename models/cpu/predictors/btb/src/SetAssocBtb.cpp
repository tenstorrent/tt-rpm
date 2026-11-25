// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Tenstorrent USA, Inc.

#include "predictors/btb/src/SetAssocBtb.hpp"

#include <bit>

#include "predictors/common/PredictorRegistry.hpp"

namespace predictors {

SetAssocBtb::SetAssocBtb(const PredictorArgs& a)
    : mAssoc(a.get<uint32_t>("ways", 2)),
      mRngState(a.get<uint64_t>("rng_seed", 0) | 1) {
    const uint32_t entries = a.get<uint32_t>("entries", 2048);
    const uint32_t tag_bits = a.get<uint32_t>("tag_bits", 0);
    const std::string replacement = a.get<std::string>("replacement", "lru");
    if (replacement != "lru" && replacement != "random") a.fail("replacement", "expected one of: lru, random");
    mRandomReplacement = replacement == "random";
    if (mAssoc == 0) a.fail("ways", "must be nonzero");
    if (entries == 0 || entries % mAssoc != 0) a.fail("entries", "must be a nonzero multiple of the ways");
    mNumSets = entries / mAssoc;
    if (!std::has_single_bit(mNumSets)) a.fail("entries", std::to_string(mNumSets) + " sets; entries / ways must be a power of two");
    if (tag_bits > 64) a.fail("tag_bits", "must be at most 64");
    mIndexBits = std::countr_zero(mNumSets);
    mTagMask = (tag_bits == 0 || tag_bits == 64) ? ~0ULL : ((1ULL << tag_bits) - 1);
    mWays.resize(static_cast<size_t>(mNumSets) * mAssoc);
}

SetAssocBtb::Way* SetAssocBtb::find_(addr_t pc) {
    Way* set = set_(pc);
    const uint64_t tag = tag_(pc);
    for (uint32_t w = 0; w < mAssoc; ++w) {
        if (set[w].valid && set[w].tag == tag) return &set[w];
    }
    return nullptr;
}

BtbEntry SetAssocBtb::lookup(addr_t pc) {
    Way* w = find_(pc);
    if (!w) return {};
    w->last_use = ++mUseClock;
    return {true, w->target, w->type};
}

void SetAssocBtb::fill_(Way& w, addr_t target, BranchType type) {
    w.target = target;
    w.type = type;
    w.last_use = ++mUseClock;
}

void SetAssocBtb::allocate(addr_t pc, addr_t target, BranchType type) {
    Way* victim = find_(pc);
    if (!victim) {
        Way* set = set_(pc);
        victim = &set[0];
        for (uint32_t w = 0; w < mAssoc && victim->valid; ++w) {
            if (!set[w].valid || set[w].last_use < victim->last_use) victim = &set[w];
        }
        if (victim->valid && mRandomReplacement) {
            mRngState ^= mRngState << 13;
            mRngState ^= mRngState >> 7;
            mRngState ^= mRngState << 17;
            victim = &set[mRngState % mAssoc];
        }
        victim->valid = true;
        victim->tag = tag_(pc);
    }
    fill_(*victim, target, type);
}

}  // namespace predictors

REGISTER_PREDICTOR(predictors::BtbInterface, "setassoc", predictors::SetAssocBtb)
