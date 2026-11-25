// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Tenstorrent USA, Inc.

#pragma once

#include <map>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "sparta/ports/DataPort.hpp"
#include "sparta/simulation/ParameterSet.hpp"
#include "sparta/simulation/Unit.hpp"
#include "sparta/statistics/Counter.hpp"

#include "Common/PipelinePacket.hpp"
#include "Frontend/BranchPredictor/TargetPredictor.hpp"
#include "predictors/common/DirectionPredictorInterface.hpp"

namespace frontend {

class BranchPredictorParams : public sparta::ParameterSet {
   public:
    BranchPredictorParams(sparta::TreeNode* n)
        : sparta::ParameterSet(n) {}

    PARAMETER(std::string, btb_type, "setassoc", "BTB algorithm ('none' = oracle targets)")
    PARAMETER(uint32_t, btb_entries, 2048, "BTB entries")
    PARAMETER(uint32_t, btb_ways, 2, "BTB associativity")
    PARAMETER(uint32_t, btb_tag_bits, 0, "BTB tag width in bits (0 = full tag)")
    PARAMETER(std::string, btb_replacement, "lru", "BTB replacement policy: lru (recency refreshed on every lookup hit) or random")
    PARAMETER(std::vector<std::string>, btb_args, std::vector<std::string>{}, "BTB algorithm knobs as key=value")
    PARAMETER(std::string, ras_type, "circular", "Return-address-stack algorithm ('none' = oracle return targets)")
    PARAMETER(uint32_t, ras_entries, 32, "RAS entries")
    PARAMETER(std::vector<std::string>, ras_args, std::vector<std::string>{}, "RAS algorithm knobs as key=value")
    PARAMETER(std::string, indirect_type, "none", "Indirect-target predictor algorithm; only 'none' (the BTB's last target) ships")
    PARAMETER(std::vector<std::string>, indirect_args, std::vector<std::string>{}, "Indirect-target predictor algorithm knobs as key=value")
    PARAMETER(uint32_t, taken_redirect_latency, 2,
              "Cycle after fetch in which a predicted-taken branch steers fetch, costing taken_redirect_latency - 1 bubble cycles (ignored with btb_type none)")
    PARAMETER(std::string, predictor_type, "tage_sc_l_64kb", "Direction predictor algorithm ('probabilistic' = accuracy oracle)")
    PARAMETER(std::vector<std::string>, direction_args, std::vector<std::string>{}, "Direction predictor algorithm knobs as key=value")
    PARAMETER(double, accuracy, 0.95, "Probability that a conditional branch is predicted correctly (probabilistic mode)")
    PARAMETER(uint64_t, rng_seed, 0xC0FFEEULL, "Seed for the prediction RNG (probabilistic mode) and BTB random replacement")
};

class BranchPredictor : public sparta::Unit {
   public:
    static constexpr char name[] = "branch_predictor";

    BranchPredictor(sparta::TreeNode* node, const BranchPredictorParams* params);

    sparta::DataInPort<core::BranchPrediction> predict_in{&unit_port_set_, "predict_in"};

    // Commit-time training: fetch_seq of a retiring branch (from Writeback).
    sparta::DataInPort<uint64_t> train_in{&unit_port_set_, "train_in"};

    // Misprediction recovery: restore speculative history on flush (from FlushArbiter).
    sparta::DataInPort<core::BranchRedirect> mispred_redirect_in{&unit_port_set_, "mispred_redirect_in"};

    sparta::DataOutPort<core::BranchPrediction> predict_out{&unit_port_set_, "predict_out"};

    // Flush output to FlushArbiter (for wrong-path speculation)
    sparta::DataOutPort<core::FlushRequest> flush_out{&unit_port_set_, "flush_out"};

    // Fetch cycles lost before a predicted-taken branch steers fetch.
    sparta::DataOutPort<uint32_t> taken_bubble_out{&unit_port_set_, "taken_bubble_out"};

    uint64_t numCorrect() const { return mNumCorrect.get(); }
    uint64_t numMispredicted() const { return mNumMispredicted.get(); }
    bool predictsTargets() const { return mTargets != nullptr; }

   private:
    void receiveRequest(const core::BranchPrediction& req);
    void receiveTrain(const uint64_t& fetch_seq);
    void receiveMispredRedirect(const core::BranchRedirect& redirect);

    std::map<uint64_t, InFlightBranch> mInFlight;  // keyed by fetch_seq

    bool mProbabilistic{true};
    std::unique_ptr<predictors::DirectionPredictorInterface> mDirection;

    double mAccuracy{0};
    uint64_t mSeed;
    std::mt19937_64 mRng;  // wrong-path branches only
    std::uniform_real_distribution<double> mDist;

    sparta::Counter mNumCorrect;
    sparta::Counter mNumMispredicted;

    std::unique_ptr<TargetPredictor> mTargets;  // null: oracle targets

    uint32_t mTakenRedirectLatency{1};  // 1: a taken branch steers the next cycle (oracle targets)
    uint64_t mGroupCycle{~0ULL};        // cycle of the last request: one fetch group
    uint32_t mGroupSteerDelay{0};
    std::vector<std::unique_ptr<sparta::ReadOnlyCounter>> mAlgorithmStats;
};

}  // namespace frontend
