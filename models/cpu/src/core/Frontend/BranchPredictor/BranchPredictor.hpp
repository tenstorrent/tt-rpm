// SPDX-FileCopyrightText: © 2026 Tenstorrent AI ULC
//
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <random>

#include "sparta/ports/DataPort.hpp"
#include "sparta/simulation/ParameterSet.hpp"
#include "sparta/simulation/Unit.hpp"
#include "sparta/statistics/Counter.hpp"

#include "Common/PipelinePacket.hpp"

namespace frontend {

class BranchPredictorParams : public sparta::ParameterSet {
   public:
    BranchPredictorParams(sparta::TreeNode* n)
        : sparta::ParameterSet(n) {}

    PARAMETER(double, accuracy, 0.95, "Probability that prediction matches real outcome")
    PARAMETER(uint64_t, rng_seed, 0xC0FFEEULL, "Seed for the prediction RNG (deterministic across runs)")
};

class BranchPredictor : public sparta::Unit {
   public:
    static constexpr char name[] = "branch_predictor";

    BranchPredictor(sparta::TreeNode* node, const BranchPredictorParams* params);

    sparta::DataInPort<core::BranchPrediction> predict_in{&unit_port_set_, "predict_in"};

    sparta::DataOutPort<core::BranchPrediction> predict_out{&unit_port_set_, "predict_out"};

    // Flush output to FlushArbiter (for wrong-path speculation)
    sparta::DataOutPort<core::FlushRequest> flush_out{&unit_port_set_, "flush_out"};

    uint64_t numCorrect() const { return mNumCorrect.get(); }
    uint64_t numMispredicted() const { return mNumMispredicted.get(); }

   private:
    void receiveRequest(const core::BranchPrediction& req);

    double mAccuracy;
    std::mt19937_64 mRng;
    std::uniform_real_distribution<double> mDist;

    sparta::Counter mNumCorrect;
    sparta::Counter mNumMispredicted;
};

}  // namespace frontend
