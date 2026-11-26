// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Tenstorrent USA, Inc.

#pragma once

#include <deque>
#include <vector>

#include "sparta/ports/DataPort.hpp"
#include "sparta/simulation/ParameterSet.hpp"
#include "sparta/simulation/Unit.hpp"
#include "sparta/statistics/Counter.hpp"

#include "Common/PipelinePacket.hpp"
#include "Common/PipelineVisualizer.hpp"
#include "Common/SpeculationConfig.hpp"

namespace cpu {
class ExecutionDriver;
}
namespace core {
class Mmu;
}

namespace frontend {

class FrontendMemoryStructures;
class FetchStructuresParams : public sparta::ParameterSet {
   public:
    FetchStructuresParams(sparta::TreeNode* n)
        : sparta::ParameterSet(n) {}

    PARAMETER(uint64_t, initial_pc, 0x80000000, "Initial program counter")
    PARAMETER(uint32_t, fetch_width, 4, "Number of bytes fetched per cycle")
    PARAMETER(uint32_t, fetch_buffer_capacity, 64, "Fetch buffer capacity in bytes")
    PARAMETER(uint32_t, max_branches_per_fetch, 2, "Branches the predictor handles per fetch group; a group ends at the last one")
};

class FetchStructures : public sparta::Unit {
   public:
    static constexpr char name[] = "fetch";

    FetchStructures(sparta::TreeNode* node, const FetchStructuresParams* params);

    // Request to ICache (now contains embedded packets)
    sparta::DataOutPort<core::FetchRequest> request_out{&unit_port_set_, "request_out"};

    sparta::DataInPort<core::FetchResponse> response_in{&unit_port_set_, "response_in"};

    // Branch prediction request (sent in parallel with ICache request)
    sparta::DataOutPort<core::BranchPrediction> predict_request_out{&unit_port_set_, "predict_request_out"};

    // Redirect inputs from FlushArbiter
    sparta::DataInPort<core::PredictedRedirect> bp_redirect_in{&unit_port_set_, "bp_redirect_in"};
    sparta::DataInPort<core::BranchRedirect> mispred_redirect_in{&unit_port_set_, "mispred_redirect_in"};

    // Correct branch resolution - decrements depth without redirect
    sparta::DataInPort<core::BranchResolved> branch_resolved_in{&unit_port_set_, "branch_resolved_in"};

    // Cycles to stall before a predicted-taken branch steers fetch
    sparta::DataInPort<uint32_t> taken_bubble_in{&unit_port_set_, "taken_bubble_in"};

    void tick();
    void setCache(frontend::FrontendMemoryStructures* cache) { mCache = cache; }
    void setMmu(core::Mmu* mmu) { mMmu = mmu; }
    void setExecutionDriver(cpu::ExecutionDriver* d) { mDriver = d; }
    void setVisualizer(core::PipelineVisualizer* v) { mVis = v; }
    void setMispredictionPenalty(uint32_t cycles) { mMispredictionPenalty = cycles; }
    void setSpeculationConfig(const core::SpeculationConfig* cfg) { mSpecCfg.set(cfg); }
    // A target predictor redirects only where it differs from Whisper, so the wrong path
    // must take Whisper's branches too; without one it runs straight through them.
    void setFollowWrongPathBranches(bool on) { mFollowWrongPathBranches = on; }

    uint64_t numFetched() const { return mNumFetched.get(); }
    uint64_t numBufferFullStallCycles() const { return mNumBufferFullStallCycles.get(); }
    uint64_t numRedirects() const { return mNumRedirects.get(); }
    uint64_t nextFetchSeq() const { return mFetchSeq; }

   private:
    void sendRequest(uint64_t base_pc, std::vector<core::PipelinePacket>&& packets);
    void receiveResponse(const core::FetchResponse&);
    void receiveBpRedirect_(const core::PredictedRedirect& redirect);
    void receiveMispredRedirect_(const core::BranchRedirect& redirect);
    void receiveBranchResolved_(const core::BranchResolved& resolved);
    void receiveTakenBubble_(const uint32_t& cycles);
    void cancelInFlight_(uint64_t branch_tag, uint8_t branch_depth, core::FlushSource source);

    cpu::ExecutionDriver* mDriver{nullptr};
    uint64_t mPc;
    uint32_t mFetchWidth;
    uint32_t mFetchBufferCapacity;
    uint32_t mMaxBranchesPerFetch;
    bool mFirstFetch{true};
    frontend::FrontendMemoryStructures* mCache = nullptr;
    core::Mmu* mMmu = nullptr;
    // Last MSHR pre-check translation, so a stalled fetch block is not re-walked every cycle.
    uint64_t mPrecheckVa{0};
    uint64_t mPrecheckPa{0};

    core::PipelineVisualizer* mVis{nullptr};
    core::SpeculationConfigRef mSpecCfg;

    // Wrong-path speculation tracking
    uint8_t mCurrentWrongPathDepth{0};  // Current speculation depth (0 = correct path)
    bool mOnSpeculativePath{false};     // True when fetching from BP-predicted path
    uint64_t mSpeculativePc{0};         // PC to fetch from when on speculative path
    uint64_t mCurrentSpecBranchTag{0};  // Branch tag that caused current speculation level (for BranchResolved filtering)
    bool mFollowWrongPathBranches{false};

    // Monotonic fetch-sequence counter (never reset, globally unique). Liveness/
    // ordering key for stale-redirect filtering; the visualizer also consumes it.
    uint64_t mFetchSeq{0};

    // Static misprediction penalty (used when wrong_path_enabled=false)
    uint32_t mMispredictionPenalty{0};  // Configured penalty cycles
    uint32_t mMispredStallCycles{0};    // Remaining stall cycles (counts down)
    bool mAwaitingResolve{false};       // holding at a mispredicted branch until its redirect
    uint32_t mTakenBubbleCycles{0};
    uint64_t mMispredRedirectCycle{~0ULL};

    sparta::Counter mNumFetched;
    sparta::Counter mNumBufferFullStallCycles;
    sparta::Counter mNumRedirects;
    sparta::Counter mNumMispredPenaltyStalls;
    sparta::Counter mNumBranchLimitEnds;
    sparta::Counter mNumTakenBubbleCycles;
};

}  // namespace frontend
