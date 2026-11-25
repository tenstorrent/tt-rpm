// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Tenstorrent USA, Inc.

#include "Frontend/BranchPredictor/BranchPredictor.hpp"

#include <algorithm>
#include <stdexcept>
#include <type_traits>

#include "Logging.hpp"
#include "predictors/common/PredictorRegistry.hpp"

namespace frontend {

namespace {
// splitmix64 finalizer, mapped to [0, 1).
double hashUnit(uint64_t x) {
    x += 0x9E3779B97F4A7C15ULL;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
    x ^= x >> 31;
    return static_cast<double>(x >> 11) * 0x1.0p-53;
}

template <typename T>
void seed(predictors::PredictorArgs& a, const char* key, const sparta::Parameter<T>& p) {
    if constexpr (std::is_same_v<T, std::string>) {
        a.seed(key, p.getValue(), p.getLocation());
    } else {
        a.seed(key, std::to_string(p.getValue()), p.getLocation());
    }
}

// Checks `type` is `off` (no algorithm) or registered, then builds it from the seeded `a` plus the
// user's `*_args` entries, unless it is `off` or !build. Unbuilt, the seeded host knobs still get the
// range checks of `type` (or `dflt` when off), so a sweep that disables the structure hides no bad value.
template <typename Iface>
std::unique_ptr<Iface> create(const sparta::Parameter<std::string>& type, const char* off, bool build, predictors::PredictorArgs& a,
                              const sparta::Parameter<std::vector<std::string>>& user, const char* dflt = nullptr) {
    const std::string& t = type.getValue();
    const auto& registry = predictors::Registry<Iface>::instance();
    const std::vector<std::string> keys = registry.keys();
    if (t != off && std::find(keys.begin(), keys.end(), t) == keys.end()) {
        std::string choices = off;
        for (const std::string& k : keys) choices += ", " + k;
        throw std::invalid_argument(type.getLocation() + " = '" + t + "'; expected one of: " + choices);
    }
    if ((!build || t == off) && (t != off || dflt)) {
        predictors::PredictorArgs seeded = a;
        registry.create(t != off ? t : dflt, seeded);
    }
    for (const std::string& kv : user.getValue()) a.add(kv);
    std::unique_ptr<Iface> p;
    if (build && t != off) p = registry.create(t, a);
    a.checkAllUsed(p ? t : off);
    return p;
}
}  // namespace

BranchPredictor::BranchPredictor(sparta::TreeNode* node, const BranchPredictorParams* params)
    : sparta::Unit(node),
      mProbabilistic(params->predictor_type.getValue() == "probabilistic"),
      mSeed(params->rng_seed),
      mRng(params->rng_seed),
      mDist(0.0, 1.0),
      mNumCorrect(&unit_stat_set_, "num_correct", "Correct branch predictions, wrong path included", sparta::Counter::COUNT_NORMAL),
      mNumMispredicted(&unit_stat_set_, "num_mispredicted",
                       "Mispredictions of every fetched branch, wrong path included; writeback.num_retired_mispredicts is the retired count",
                       sparta::Counter::COUNT_NORMAL) {
    if (mProbabilistic) {
        mAccuracy = params->accuracy;
    } else {
        // Configs written when probabilistic was the default set accuracy; running them on a real predictor would mislead.
        sparta_assert(params->accuracy.getWriteCount() == 0,
                      params->accuracy.getLocation() << " is set, but it applies only to predictor_type probabilistic and "
                                                     << params->predictor_type.getLocation() << " is " << params->predictor_type.getValue()
                                                     << ". Drop accuracy, or select probabilistic (with btb_type and ras_type none for the old oracle).");
        params->accuracy.ignore();
    }
    predictors::PredictorArgs dir_args(params->direction_args.getLocation());
    mDirection = create<predictors::DirectionPredictorInterface>(params->predictor_type, "probabilistic", true, dir_args, params->direction_args);

    const std::string& btb_type = params->btb_type;
    predictors::PredictorArgs btb_args(params->btb_args.getLocation());
    seed(btb_args, "entries", params->btb_entries);
    seed(btb_args, "ways", params->btb_ways);
    seed(btb_args, "tag_bits", params->btb_tag_bits);
    seed(btb_args, "replacement", params->btb_replacement);
    seed(btb_args, "rng_seed", params->rng_seed);
    auto btb = create<predictors::BtbInterface>(params->btb_type, "none", true, btb_args, params->btb_args, "setassoc");

    predictors::PredictorArgs ras_args(params->ras_args.getLocation());
    seed(ras_args, "entries", params->ras_entries);
    auto ras = create<predictors::RasInterface>(params->ras_type, "none", true, ras_args, params->ras_args, "circular");

    // Without a BTB there is no fetch-time target to override.
    predictors::PredictorArgs indirect_args(params->indirect_args.getLocation());
    auto indirect = create<predictors::IndirectPredictorInterface>(params->indirect_type, "none", btb_type != "none", indirect_args, params->indirect_args);
    mTargets = TargetPredictor::create(std::move(btb), std::move(ras), std::move(indirect), &unit_stat_set_);

    // With oracle targets the prediction steers fetch the next cycle: an idealized zero-latency BTB.
    if (btb_type == "none") {
        params->taken_redirect_latency.ignore();
    } else {
        mTakenRedirectLatency = params->taken_redirect_latency;
        sparta_assert(mTakenRedirectLatency > 0, params->taken_redirect_latency.getLocation() << " must be at least 1");
    }
    addAlgorithmStats(&unit_stat_set_, "dir", mDirection.get(), mAlgorithmStats);
    predict_in.registerConsumerHandler(CREATE_SPARTA_HANDLER_WITH_DATA(BranchPredictor, receiveRequest, core::BranchPrediction));
    train_in.registerConsumerHandler(CREATE_SPARTA_HANDLER_WITH_DATA(BranchPredictor, receiveTrain, uint64_t));
    mispred_redirect_in.registerConsumerHandler(CREATE_SPARTA_HANDLER_WITH_DATA(BranchPredictor, receiveMispredRedirect, core::BranchRedirect));
}

void BranchPredictor::receiveRequest(const core::BranchPrediction& req) {
    core::BranchPrediction result = req;

    // Execution-driven: the incoming packet carries Whisper's actual outcome.
    const bool actual_taken = req.predicted_taken;

    // Non-conditional branches are taken by definition; their targets are the BTB's and RAS's job.
    const bool conditional = (req.type == predictors::BranchType::Conditional);
    bool predicted_taken = actual_taken;
    InFlightBranch& b = mInFlight[req.fetch_seq];
    b.info = {req.pc, req.type, req.inst_size};
    b.target = req.target_pc;
    b.spec_target = req.target_pc;
    if (mProbabilistic) {
        // A correct-path tag is the dynamic instruction number (flushes rewind it), so keying
        // the draw on it makes each committed branch's outcome independent of timing knobs.
        if (conditional) {
            const double u = (req.wrong_path_depth == 0) ? hashUnit(mSeed ^ req.tag) : mDist(mRng);
            if (u >= mAccuracy) predicted_taken = !actual_taken;
        }
    } else {
        if (conditional) predicted_taken = mDirection->predict(b.info).taken;
        b.dir = mDirection->snapshot();
    }
    b.predicted_taken = predicted_taken;
    b.actual_taken = actual_taken;

    result.predicted_taken = predicted_taken;
    result.mispredicted = (predicted_taken != actual_taken);
    // Without a target predictor, fetch follows the predicted direction to the oracle target.
    FetchSteer steer{predicted_taken, result.target_pc, result.mispredicted};
    if (mTargets) steer = mTargets->predict(req, predicted_taken, result, b);
    if (mDirection) mDirection->speculativeUpdate(b.info, predicted_taken, b.spec_target);

    if (!result.mispredicted) {
        ++mNumCorrect;
    } else {
        ++mNumMispredicted;
    }

    // The BTB and direction predictor answer taken_redirect_latency cycles after fetch, so a taken branch
    // costs the cycles in between. No branch in a group steers before an older one.
    const uint64_t cycle = getClock()->currentCycle();
    uint32_t steer_delay = (cycle == mGroupCycle) ? mGroupSteerDelay : 1;
    if (steer.taken && mTakenRedirectLatency > 1) {
        steer_delay = std::max(steer_delay, mTakenRedirectLatency);
        taken_bubble_out.send(mTakenRedirectLatency - 1, 0);
    }
    mGroupCycle = cycle;
    mGroupSteerDelay = steer_delay;

    if (steer.wrong) {
        // Send flush request to FlushArbiter for both predicted-taken and predicted-not-taken
        // Both cases enter speculative/wrong-path mode
        core::FlushRequest flush;
        flush.source = core::FlushSource::BranchPredictor;
        flush.branch_tag = result.tag;
        flush.branch_fetch_seq = result.fetch_seq;  // Use fetch_seq for prediction clearing
        flush.branch_pc = result.pc;
        flush.branch_depth = result.wrong_path_depth;
        flush.predicted_taken = steer.taken;

        if (steer.taken) {
            flush.target_pc = steer.target;  // Redirect to branch target
            ILOG("[bp] PREDICTED TAKEN: sending flush to FlushArbiter for tag=" << result.tag << " target_pc=0x" << std::hex << flush.target_pc << std::dec);
        } else {
            flush.target_pc = result.pc + result.inst_size;  // Continue sequential (but in wrong-path mode)
            ILOG("[bp] PREDICTED NOT-TAKEN: sending flush to FlushArbiter for tag=" << result.tag << " target_pc=0x" << std::hex << flush.target_pc
                                                                                    << std::dec);
        }
        flush_out.send(flush, steer_delay);
    }

    ILOG("[bp] pc=0x" << std::hex << result.pc << std::dec << " taken=" << result.predicted_taken << " mispred=" << result.mispredicted);

    predict_out.send(result, 0);
}

// Commit-time training: a branch retired, so train on its committed outcome and
// drop it (and any older stragglers) from the in-flight set.
void BranchPredictor::receiveTrain(const uint64_t& fetch_seq) {
    auto it = mInFlight.find(fetch_seq);
    if (it != mInFlight.end()) {
        const InFlightBranch& f = it->second;
        if (mTargets) mTargets->retire(f);
        if (mDirection && f.info.type == predictors::BranchType::Conditional) {
            mDirection->update(predictors::DirectionUpdate{f.info, f.actual_taken, f.predicted_taken != f.actual_taken, f.target, f.dir});
        }
    }
    mInFlight.erase(mInFlight.begin(), mInFlight.upper_bound(fetch_seq));
}

// Misprediction recovery: roll speculative history back to the redirecting branch,
// re-apply its correct outcome, and drop everything fetched after it.
void BranchPredictor::receiveMispredRedirect(const core::BranchRedirect& redirect) {
    const uint64_t f = redirect.branch_fetch_seq;
    if (redirect.authoritative) {
        // A trap names the last retired instruction, which may be a branch whose train has not
        // arrived; its outcome stands, so unwind to just after it and re-apply nothing.
        auto first = mInFlight.upper_bound(f);
        if (mDirection && first != mInFlight.end()) mDirection->restore(first->second.dir);
        const InFlightBranch* older = (first == mInFlight.begin()) ? nullptr : &std::prev(first)->second;
        if (mTargets) mTargets->trap(older, first == mInFlight.end() ? nullptr : &first->second);
        mInFlight.erase(first, mInFlight.end());
        return;
    }
    // The FlushArbiter drops redirects from squashed branches.
    auto it = mInFlight.find(f);
    sparta_assert(it != mInFlight.end(), "misprediction redirect for a branch not in flight: fetch_seq=" << f);
    InFlightBranch& e = it->second;
    if (predictors::indirectTarget(e.info.type)) e.spec_target = redirect.correct_target_pc;
    if (mDirection) {
        mDirection->restore(e.dir);
        mDirection->speculativeUpdate(e.info, redirect.was_taken, e.spec_target);
    }
    e.actual_taken = redirect.was_taken;
    if (mTargets) mTargets->redirect(e);
    mInFlight.erase(std::next(it), mInFlight.end());
}

}  // namespace frontend
