// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Tenstorrent USA, Inc.

#pragma once

#include <memory>
#include <string>
#include <vector>

#include "sparta/statistics/Counter.hpp"
#include "sparta/statistics/ReadOnlyCounter.hpp"
#include "sparta/statistics/StatisticSet.hpp"

#include "Common/PipelinePacket.hpp"
#include "predictors/common/BtbInterface.hpp"
#include "predictors/common/IndirectPredictorInterface.hpp"
#include "predictors/common/RasInterface.hpp"

namespace frontend {

// Reports c's own counters in stats as <prefix>_<name>.
void addAlgorithmStats(sparta::StatisticSet* stats, const std::string& prefix, const predictors::PredictorComponent* c,
                       std::vector<std::unique_ptr<sparta::ReadOnlyCounter>>& out);

// The path the fetch unit follows after a branch, before Decode can check it.
struct FetchSteer {
    bool taken{false};
    uint64_t target{0};
    bool wrong{false};  // differs from Whisper's outcome, so fetch must be redirected onto it
};

// One fetched branch's predictor state, kept until it retires or is squashed. History structures
// checkpoint before their update and re-apply it on a redirect; target structures checkpoint after it.
struct InFlightBranch {
    predictors::BranchInfo info;
    uint64_t target{0};           // taken target: static, or Whisper's for an indirect jump
    uint64_t spec_target{0};      // the target speculative history saw
    bool predicted_taken{false};  // the direction predictor's call
    bool actual_taken{false};
    bool btb_hit{false};
    bool misfetch{false};
    bool mispredicted{false};
    bool target_mispredicted{false};
    bool ind_predicted{false};  // the indirect predictor was consulted
    predictors::BPCheckpoint dir;
    predictors::BPCheckpoint ind;
    predictors::BPCheckpoint ras;
};

// BTB, RAS and indirect target prediction. A 'none' BTB or RAS is an oracle for that structure;
// with both 'none' the BranchPredictor has no TargetPredictor at all.
class TargetPredictor {
   public:
    static std::unique_ptr<TargetPredictor> create(std::unique_ptr<predictors::BtbInterface> btb, std::unique_ptr<predictors::RasInterface> ras,
                                                   std::unique_ptr<predictors::IndirectPredictorInterface> indirect, sparta::StatisticSet* stats);

    TargetPredictor(std::unique_ptr<predictors::BtbInterface> btb, std::unique_ptr<predictors::RasInterface> ras,
                    std::unique_ptr<predictors::IndirectPredictorInterface> indirect, sparta::StatisticSet* stats);

    // Combines dir_taken with the BTB, RAS and indirect predictor; sets result's predicted_taken,
    // mispredicted, target_mispredicted and misfetch, and b's target fields. req carries Whisper's outcome.
    FetchSteer predict(const core::BranchPrediction& req, bool dir_taken, core::BranchPrediction& result, InFlightBranch& b);
    void retire(const InFlightBranch& b);
    // Recovers to b, whose actual_taken and spec_target are now the resolved ones.
    void redirect(const InFlightBranch& b);
    // Recovers to the state after `older` (the youngest retired branch if null); `squashed` is the
    // oldest squashed branch, if any.
    void trap(const InFlightBranch* older, const InFlightBranch* squashed);

   private:
    predictors::BPCheckpoint mRetiredRas;

    std::unique_ptr<predictors::BtbInterface> mBtb;                     // null: oracle
    std::unique_ptr<predictors::RasInterface> mRas;                     // null: oracle
    std::unique_ptr<predictors::IndirectPredictorInterface> mIndirect;  // null: the BTB's last target
    std::vector<std::unique_ptr<sparta::ReadOnlyCounter>> mAlgorithmStats;

    sparta::Counter mBtbLookups;
    sparta::Counter mBtbHits;
    sparta::Counter mBtbMisses;
    sparta::Counter mRasOverflows;
    sparta::Counter mRetiredTakenBtbMisses;
    sparta::Counter mMisfetches;
    sparta::Counter mRetiredReturns;
    sparta::Counter mReturnMispredicts;
    sparta::Counter mRetiredIndirects;
    sparta::Counter mIndirectMispredicts;
    sparta::Counter mDirectTargetMispredicts;
};

}  // namespace frontend
