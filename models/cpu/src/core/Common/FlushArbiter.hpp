// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Tenstorrent USA, Inc.

#pragma once

#include <deque>
#include <utility>
#include <vector>

#include "sparta/ports/DataPort.hpp"
#include "sparta/simulation/ParameterSet.hpp"
#include "sparta/simulation/Unit.hpp"
#include "sparta/statistics/Counter.hpp"

#include "Common/PipelinePacket.hpp"

namespace frontend {
class FetchStructures;
}

namespace core {

class FlushArbiterParams : public sparta::ParameterSet {
   public:
    FlushArbiterParams(sparta::TreeNode* n)
        : sparta::ParameterSet(n) {}
};

class FlushArbiter : public sparta::Unit {
   public:
    static constexpr char name[] = "flush_arbiter";

    FlushArbiter(sparta::TreeNode* node, const FlushArbiterParams* params);

    // Input ports from BP and Execute
    sparta::DataInPort<FlushRequest> bp_flush_in{&unit_port_set_, "bp_flush_in"};
    sparta::DataInPort<FlushRequest> exe_flush_in{&unit_port_set_, "exe_flush_in"};
    // Trap/interrupt redirect from Writeback (full recovery, like an Execute flush)
    sparta::DataInPort<FlushRequest> retire_flush_in{&unit_port_set_, "retire_flush_in"};
    // fetch_seq of each retiring branch
    sparta::DataInPort<uint64_t> branch_retire_in{&unit_port_set_, "branch_retire_in"};

    // Output ports to frontend units (BP flush path - lightweight redirect)
    sparta::DataOutPort<FlushRequest> flush_to_fq_out{&unit_port_set_, "flush_to_fq_out"};
    sparta::DataOutPort<FlushRequest> flush_to_icache_out{&unit_port_set_, "flush_to_icache_out"};
    sparta::DataOutPort<PredictedRedirect> redirect_to_fetch_out{&unit_port_set_, "redirect_to_fetch_out"};

    // Output ports for full misprediction recovery (Execute flush path)
    sparta::DataOutPort<FlushRequest> flush_to_decode_out{&unit_port_set_, "flush_to_decode_out"};
    sparta::DataOutPort<FlushRequest> flush_to_decode_queue_out{&unit_port_set_, "flush_to_decode_queue_out"};
    sparta::DataOutPort<FlushRequest> flush_to_issue_out{&unit_port_set_, "flush_to_issue_out"};
    sparta::DataOutPort<FlushRequest> flush_to_execute_out{&unit_port_set_, "flush_to_execute_out"};
    sparta::DataOutPort<FlushRequest> flush_to_lsq_out{&unit_port_set_, "flush_to_lsq_out"};
    sparta::DataOutPort<FlushRequest> flush_to_rename_out{&unit_port_set_, "flush_to_rename_out"};
    sparta::DataOutPort<FlushRequest> flush_to_writeback_out{&unit_port_set_, "flush_to_writeback_out"};
    sparta::DataOutPort<BranchRedirect> mispred_redirect_to_fetch_out{&unit_port_set_, "mispred_redirect_to_fetch_out"};

    // Called by PipelineClock each cycle to process pending flushes
    void tick();
    void setFetch(const frontend::FetchStructures* fetch) { mFetch = fetch; }

    uint64_t numBpFlushes() const { return mNumBpFlushes.get(); }
    uint64_t numExeFlushes() const { return mNumExeFlushes.get(); }
    uint64_t numDroppedBpFlushes() const { return mNumDroppedBpFlushes.get(); }
    uint64_t numDroppedExeFlushes() const { return mNumDroppedExeFlushes.get(); }

   private:
    void receiveBpFlush_(const FlushRequest& req);
    void receiveExeFlush_(const FlushRequest& req);
    void receiveBranchRetire_(const uint64_t& fetch_seq);

    // A flush squashes the fetch_seqs in (branch, fetch frontier); a flush from a squashed branch is dropped.
    // Predictor flushes squash only the front end, and every Execute flush supersedes them.
    const frontend::FetchStructures* mFetch{nullptr};
    std::deque<std::pair<uint64_t, uint64_t>> mExeSquashed;  // ascending; pruned as branches retire
    std::vector<std::pair<uint64_t, uint64_t>> mBpSquashed;  // since the last Execute flush
    uint64_t mExeFrontier{0};                                // fetch frontier at the last Execute flush

    sparta::Counter mNumBpFlushes;
    sparta::Counter mNumExeFlushes;
    sparta::Counter mNumDroppedBpFlushes;
    sparta::Counter mNumDroppedExeFlushes;
};

}  // namespace core
