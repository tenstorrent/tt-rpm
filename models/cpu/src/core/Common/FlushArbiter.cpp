// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Tenstorrent USA, Inc.

#include "Common/FlushArbiter.hpp"

#include "Frontend/FetchStructures/FetchStructures.hpp"
#include "Logging.hpp"

namespace core {

FlushArbiter::FlushArbiter(sparta::TreeNode* node, const FlushArbiterParams*)
    : sparta::Unit(node),
      mNumBpFlushes(&unit_stat_set_, "num_bp_flushes", "Flushes triggered by branch prediction", sparta::Counter::COUNT_NORMAL),
      mNumExeFlushes(&unit_stat_set_, "num_exe_flushes", "Flushes triggered by execute misprediction", sparta::Counter::COUNT_NORMAL),
      mNumDroppedBpFlushes(&unit_stat_set_, "num_dropped_bp_flushes", "Predictor flushes dropped: branch squashed, or fetch redirected since it was fetched",
                           sparta::Counter::COUNT_NORMAL),
      mNumDroppedExeFlushes(&unit_stat_set_, "num_dropped_exe_flushes", "Execute and Decode flushes dropped because their branch was squashed",
                            sparta::Counter::COUNT_NORMAL) {
    bp_flush_in.registerConsumerHandler(CREATE_SPARTA_HANDLER_WITH_DATA(FlushArbiter, receiveBpFlush_, FlushRequest));
    exe_flush_in.registerConsumerHandler(CREATE_SPARTA_HANDLER_WITH_DATA(FlushArbiter, receiveExeFlush_, FlushRequest));
    retire_flush_in.registerConsumerHandler(CREATE_SPARTA_HANDLER_WITH_DATA(FlushArbiter, receiveExeFlush_, FlushRequest));
    branch_retire_in.registerConsumerHandler(CREATE_SPARTA_HANDLER_WITH_DATA(FlushArbiter, receiveBranchRetire_, uint64_t));
}

namespace {
template <typename Windows>
bool squashedBy(const Windows& windows, uint64_t fetch_seq) {
    for (const auto& [branch, frontier] : windows) {
        if (fetch_seq > branch && fetch_seq < frontier) return true;
    }
    return false;
}
}  // namespace

void FlushArbiter::receiveBpFlush_(const FlushRequest& req) {
    if (req.branch_fetch_seq < mExeFrontier || squashedBy(mBpSquashed, req.branch_fetch_seq)) {
        ++mNumDroppedBpFlushes;
        ILOG("[flush_arbiter] DROPPING stale predicted flush: branch_tag=" << req.branch_tag << " fetch_seq=" << req.branch_fetch_seq);
        return;
    }
    mBpSquashed.emplace_back(req.branch_fetch_seq, mFetch->nextFetchSeq());
    ++mNumBpFlushes;
    ILOG("[flush_arbiter] cycle=" << getClock()->currentCycle() << " PREDICTED FLUSH from BranchPredictor: branch_tag=" << req.branch_tag << " branch_pc=0x"
                                  << std::hex << req.branch_pc << " target_pc=0x" << req.target_pc << std::dec);

    ILOG("[flush_arbiter] EXECUTING flush: branch_tag=" << req.branch_tag << " redirecting to 0x" << std::hex << req.target_pc << std::dec);

    // NOTE: Whisper flush is done by FetchStructures when it receives the redirect
    // This keeps Whisper state synchronized with Fetch's actual redirect timing

    // Send flush to frontend stages
    flush_to_fq_out.send(req, 0);
    flush_to_icache_out.send(req, 0);
    flush_to_decode_out.send(req, 0);
    flush_to_decode_queue_out.send(req, 0);

    // Send redirect to FetchStructures (increments wrong_path_depth). The predictor's flush already
    // carries the steer latency.
    PredictedRedirect redirect;
    redirect.branch_tag = req.branch_tag;
    redirect.branch_fetch_seq = req.branch_fetch_seq;
    redirect.branch_pc = req.branch_pc;
    redirect.target_pc = req.target_pc;
    redirect.wrong_path_depth = req.branch_depth;
    redirect_to_fetch_out.send(redirect, 0);
}

void FlushArbiter::receiveExeFlush_(const FlushRequest& req) {
    uint64_t current_cycle = getClock()->currentCycle();

    if (!req.authoritative && squashedBy(mExeSquashed, req.branch_fetch_seq)) {
        ++mNumDroppedExeFlushes;
        ILOG("[flush_arbiter] DROPPING flush from squashed branch_tag=" << req.branch_tag << " fetch_seq=" << req.branch_fetch_seq);
        return;
    }
    mExeFrontier = mFetch->nextFetchSeq();
    while (!mExeSquashed.empty() && mExeSquashed.back().first >= req.branch_fetch_seq) mExeSquashed.pop_back();
    mExeSquashed.emplace_back(req.branch_fetch_seq, mExeFrontier);
    mBpSquashed.clear();

    ++mNumExeFlushes;
    ILOG("[flush_arbiter] cycle=" << current_cycle << " MISPREDICTION FLUSH from Execute: branch_tag=" << req.branch_tag << " branch_pc=0x" << std::hex
                                  << req.branch_pc << " target_pc=0x" << req.target_pc << std::dec << " branch_depth=" << static_cast<int>(req.branch_depth));

    // NOTE: Whisper flush is done by FetchStructures when it receives the redirect
    // This keeps Whisper state synchronized with Fetch's actual redirect timing

    // Send misprediction redirect to Fetch. Arriving this cycle, before Fetch ticks, keeps Fetch from
    // sending the I-cache a group this flush has already passed; Fetch refetches next cycle.
    BranchRedirect redirect;
    redirect.branch_tag = req.branch_tag;
    redirect.branch_fetch_seq = req.branch_fetch_seq;
    redirect.branch_pc = req.branch_pc;
    redirect.correct_target_pc = req.target_pc;
    redirect.wrong_path_depth = req.branch_depth;
    redirect.was_taken = req.was_taken;
    redirect.authoritative = req.authoritative;
    mispred_redirect_to_fetch_out.send(redirect, 0);
    ILOG("[flush_arbiter] sent misprediction redirect to Fetch for tag=" << req.branch_tag << " target=0x" << std::hex << req.target_pc << std::dec);

    // Flush all pipeline stages
    flush_to_fq_out.send(req, 0);
    flush_to_icache_out.send(req, 0);
    flush_to_decode_out.send(req, 0);
    flush_to_decode_queue_out.send(req, 0);
    flush_to_issue_out.send(req, 0);
    flush_to_execute_out.send(req, 1);  // 1-cycle delay to avoid iterator invalidation
    flush_to_lsq_out.send(req, 0);
    flush_to_rename_out.send(req, 0);
    flush_to_writeback_out.send(req, 0);

    ILOG("[flush_arbiter] misprediction recovery complete for tag=" << req.branch_tag);
}

void FlushArbiter::receiveBranchRetire_(const uint64_t& fetch_seq) {
    while (!mExeSquashed.empty() && mExeSquashed.front().second <= fetch_seq) mExeSquashed.pop_front();
}

void FlushArbiter::tick() {
    // Flushes are processed immediately in receive handlers
}

}  // namespace core
