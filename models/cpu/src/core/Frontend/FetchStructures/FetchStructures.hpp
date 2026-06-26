// SPDX-FileCopyrightText: © 2026 Tenstorrent AI ULC
//
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <deque>
#include <vector>

#include "sparta/ports/DataPort.hpp"
#include "sparta/simulation/ParameterSet.hpp"
#include "sparta/simulation/Unit.hpp"
#include "sparta/statistics/Counter.hpp"

#include "Common/PipelinePacket.hpp"
#include "Common/PipelineVisualizer.hpp"

namespace cpu {
class ExecutionDriver;
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

    void tick();
    void setCache(frontend::FrontendMemoryStructures* cache) { mCache = cache; }
    void setExecutionDriver(cpu::ExecutionDriver* d) { mDriver = d; }
    void setVisualizer(core::PipelineVisualizer* v) { mVis = v; }
    void setMispredictionPenalty(uint32_t cycles) { mMispredictionPenalty = cycles; }

    uint64_t numFetched() const { return mNumFetched.get(); }
    uint64_t numBufferFullStallCycles() const { return mNumBufferFullStallCycles.get(); }
    uint64_t numRedirects() const { return mNumRedirects.get(); }

   private:
    void sendRequest(uint64_t base_pc, std::vector<core::PipelinePacket>&& packets);
    void receiveResponse(const core::FetchResponse&);
    void receiveBpRedirect_(const core::PredictedRedirect& redirect);
    void receiveMispredRedirect_(const core::BranchRedirect& redirect);
    void receiveBranchResolved_(const core::BranchResolved& resolved);

    cpu::ExecutionDriver* mDriver{nullptr};
    uint64_t mPc;
    uint32_t mFetchWidth;
    uint32_t mFetchBufferCapacity;
    bool mFirstFetch{true};
    uint32_t mBufferedBytes{0};
    frontend::FrontendMemoryStructures* mCache = nullptr;

    core::PipelineVisualizer* mVis{nullptr};

    // Wrong-path speculation tracking
    uint8_t mCurrentWrongPathDepth{0};  // Current speculation depth (0 = correct path)
    bool mOnSpeculativePath{false};     // True when fetching from BP-predicted path
    uint64_t mSpeculativePc{0};         // PC to fetch from when on speculative path
    uint64_t mCurrentSpecBranchTag{0};  // Branch tag that caused current speculation level (for BranchResolved filtering)

    // Filter state for stale BP/mispred redirects after a misprediction recovery.
    // Updated atomically by receiveMispredRedirect_ when a recovery commits.
    //
    // Why two thresholds (squash vs depth0)? Speculative recoveries (depth>0)
    // invalidate everything fetched before them, but they do NOT invalidate
    // depth=0 branches that were fetched even earlier — only depth=0 recoveries
    // can flush depth=0 branches. So depth=0 mispred filtering uses its own pair
    // (depth0_fetch_seq + depth0_branch_fetch_seq) instead of the general threshold.
    struct StaleRedirectFilter {
        // Fetch-seq threshold updated by ANY mispred recovery. Used to filter BP
        // redirects and depth>0 mispred redirects. Branches with fetch_seq < this
        // are from squashed paths.
        uint64_t squash_threshold_fetch_seq{0};

        // Fetch frontier at the most recent depth=0 recovery (upper bound of its
        // squashed window).
        uint64_t depth0_fetch_seq{0};

        // Fetch-seq of the most recent depth=0 recovery's branch (lower bound of its
        // squashed window). A depth=0 mispred is stale iff its branch was fetched
        // strictly between this and depth0_fetch_seq -- i.e. on that recovery's
        // squashed path. Monotonic and never reused (unlike the model tag).
        uint64_t depth0_branch_fetch_seq{0};
    };
    StaleRedirectFilter mStaleFilter;

    // Monotonic fetch-sequence counter (never reset, globally unique). Liveness/
    // ordering key for stale-redirect filtering; the visualizer also consumes it.
    uint64_t mFetchSeq{0};

    // Static misprediction penalty (used when wrong_path_enabled=false)
    uint32_t mMispredictionPenalty{0};  // Configured penalty cycles
    uint32_t mMispredStallCycles{0};    // Remaining stall cycles (counts down)

    sparta::Counter mNumFetched;
    sparta::Counter mNumBufferFullStallCycles;
    sparta::Counter mNumRedirects;
    sparta::Counter mNumMispredPenaltyStalls;
};

}  // namespace frontend
