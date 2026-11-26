// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Tenstorrent USA, Inc.

#include "Frontend/BranchPredictor/TargetPredictor.hpp"

namespace frontend {

namespace {

using predictors::BranchType;

struct Path {
    bool taken{false};
    uint64_t target{0};
    bool operator==(const Path& o) const { return taken == o.taken && (!taken || target == o.target); }
};

bool popsRas(BranchType t) { return t == BranchType::Return || t == BranchType::CoroutineSwap; }
bool pushesRas(BranchType t) { return t == BranchType::Call || t == BranchType::IndirectCall || t == BranchType::CoroutineSwap; }

}  // namespace

void addAlgorithmStats(sparta::StatisticSet* stats, const std::string& prefix, const predictors::PredictorComponent* c,
                       std::vector<std::unique_ptr<sparta::ReadOnlyCounter>>& out) {
    if (!c) return;
    for (const predictors::PredictorStat& s : c->stats()) {
        out.push_back(std::make_unique<sparta::ReadOnlyCounter>(stats, prefix + "_" + s.name, s.desc, sparta::Counter::COUNT_NORMAL, s.value,
                                                                sparta::InstrumentationNode::DEFAULT_VISIBILITY));
    }
}

std::unique_ptr<TargetPredictor> TargetPredictor::create(std::unique_ptr<predictors::BtbInterface> btb, std::unique_ptr<predictors::RasInterface> ras,
                                                         std::unique_ptr<predictors::IndirectPredictorInterface> indirect, sparta::StatisticSet* stats) {
    if (!btb && !ras) return nullptr;
    return std::make_unique<TargetPredictor>(std::move(btb), std::move(ras), std::move(indirect), stats);
}

TargetPredictor::TargetPredictor(std::unique_ptr<predictors::BtbInterface> btb, std::unique_ptr<predictors::RasInterface> ras,
                                 std::unique_ptr<predictors::IndirectPredictorInterface> indirect, sparta::StatisticSet* stats)
    : mBtb(std::move(btb)),
      mRas(std::move(ras)),
      mIndirect(std::move(indirect)),
      mBtbLookups(stats, "btb_lookups", "BTB lookups, wrong path included", sparta::Counter::COUNT_NORMAL),
      mBtbHits(stats, "btb_hits", "BTB hits, wrong path included", sparta::Counter::COUNT_NORMAL),
      mBtbMisses(stats, "btb_misses", "BTB misses, wrong path included", sparta::Counter::COUNT_NORMAL),
      mRasOverflows(stats, "ras_overflows", "Calls pushed onto a full RAS, wrong path included", sparta::Counter::COUNT_NORMAL),
      mRetiredTakenBtbMisses(stats, "retired_taken_btb_misses", "Retired taken branches and jumps that missed the BTB", sparta::Counter::COUNT_NORMAL),
      mMisfetches(stats, "misfetches", "Retired branches whose fetch path Decode redirected", sparta::Counter::COUNT_NORMAL),
      mRetiredReturns(stats, "retired_returns", "Retired returns and coroutine swaps", sparta::Counter::COUNT_NORMAL),
      mReturnMispredicts(stats, "return_mispredicts", "Retired returns with a wrong predicted target", sparta::Counter::COUNT_NORMAL),
      mRetiredIndirects(stats, "retired_indirects", "Retired indirect jumps and calls", sparta::Counter::COUNT_NORMAL),
      mIndirectMispredicts(stats, "indirect_mispredicts", "Retired indirect jumps and calls with a wrong predicted target", sparta::Counter::COUNT_NORMAL),
      mDirectTargetMispredicts(stats, "direct_target_mispredicts", "Retired direct branches resolved at Execute to a different taken target",
                               sparta::Counter::COUNT_NORMAL) {
    addAlgorithmStats(stats, "btb", mBtb.get(), mAlgorithmStats);
    addAlgorithmStats(stats, "ras", mRas.get(), mAlgorithmStats);
    addAlgorithmStats(stats, "ind", mIndirect.get(), mAlgorithmStats);
}

FetchSteer TargetPredictor::predict(const core::BranchPrediction& req, bool dir_taken, core::BranchPrediction& result, InFlightBranch& b) {
    const BranchType type = req.type;
    const Path actual{req.predicted_taken, req.target_pc};

    predictors::BtbEntry btb{true, req.target_pc, type};
    if (mBtb) {
        btb = mBtb->lookup(req.pc);
        ++mBtbLookups;
        ++(btb.hit ? mBtbHits : mBtbMisses);
    }
    if (mIndirect && btb.hit && (btb.type == BranchType::IndirectJump || btb.type == BranchType::IndirectCall)) {
        const predictors::IndirectResult ind = mIndirect->predict(b.info);
        if (ind.hit) btb.target = ind.target;
        b.ind_predicted = true;
    }

    const uint64_t ras_top = mRas ? mRas->predict() : req.target_pc;
    if (mRas) {
        if (popsRas(type)) mRas->pop();
        if (pushesRas(type)) {
            if (mRas->full()) ++mRasOverflows;
            mRas->push(req.pc + req.inst_size);
        }
    }

    Path fetch;  // a BTB miss falls through
    if (btb.hit) {
        if (btb.type == BranchType::Conditional) {
            fetch = {dir_taken, btb.target};
        } else {
            fetch = {true, popsRas(btb.type) ? ras_top : btb.target};
        }
    }

    // Decode knows the real type, computes direct targets and sends returns to the RAS top.
    Path decode = fetch;
    if (type == BranchType::Conditional) {
        decode = {dir_taken, req.target_pc};
    } else if (type == BranchType::DirectJump || type == BranchType::Call) {
        decode = {true, req.target_pc};
    } else if (popsRas(type)) {
        decode = {true, ras_top};
    }
    // Decode redirects only onto the correct path. If fetch is wrong too, Execute repairs fetch's
    // path; if only Decode is wrong, Execute charges the mispredict with no wrong path fetched.
    if (!(decode == actual) && !(fetch == actual)) decode = fetch;

    result.predicted_taken = decode.taken;
    result.mispredicted = !(decode == actual);
    result.target_mispredicted = decode.taken && actual.taken && decode.target != actual.target;
    result.misfetch = !(fetch == decode) && !result.mispredicted;

    if (predictors::indirectTarget(type)) {
        b.spec_target = decode.taken ? decode.target : req.pc + req.inst_size;
    }
    b.btb_hit = btb.hit;
    b.misfetch = result.misfetch;
    b.mispredicted = result.mispredicted;
    b.target_mispredicted = result.target_mispredicted;
    if (mRas) b.ras = mRas->snapshot();
    if (mIndirect) {
        b.ind = mIndirect->snapshot();
        mIndirect->speculativeUpdate(b.info, dir_taken, b.spec_target);
    }
    return {fetch.taken, fetch.target, !(fetch == actual)};
}

// Training at commit keeps wrong-path targets out of the BTB; the cost is that a new taken
// branch seen again before it retires misses again.
void TargetPredictor::retire(const InFlightBranch& b) {
    if (b.actual_taken && !b.btb_hit) ++mRetiredTakenBtbMisses;
    if (b.misfetch) ++mMisfetches;
    const BranchType type = b.info.type;
    if (popsRas(type)) {
        ++mRetiredReturns;
        if (b.mispredicted) ++mReturnMispredicts;
    } else if (type == BranchType::IndirectJump || type == BranchType::IndirectCall) {
        ++mRetiredIndirects;
        if (b.mispredicted) ++mIndirectMispredicts;
        if (mIndirect && b.ind_predicted) mIndirect->update(b.info, b.target, b.mispredicted, b.ind);
    } else if (b.target_mispredicted) {
        ++mDirectTargetMispredicts;
    }
    if (mBtb && b.actual_taken) mBtb->allocate(b.info.pc, b.target, type);
    mRetiredRas = b.ras;
}

void TargetPredictor::redirect(const InFlightBranch& b) {
    if (mIndirect) {
        mIndirect->restore(b.ind);
        mIndirect->speculativeUpdate(b.info, b.actual_taken, b.spec_target);
    }
    if (mRas) mRas->restore(b.ras);
}

void TargetPredictor::trap(const InFlightBranch* older, const InFlightBranch* squashed) {
    if (mIndirect && squashed) mIndirect->restore(squashed->ind);
    if (mRas) mRas->restore(older ? older->ras : mRetiredRas);
}

}  // namespace frontend
