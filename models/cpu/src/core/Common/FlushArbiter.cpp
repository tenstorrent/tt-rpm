// SPDX-FileCopyrightText: © 2026 Tenstorrent AI ULC
//
// SPDX-License-Identifier: Apache-2.0

#include "Common/FlushArbiter.hpp"

#include "Common/SpeculationConfig.hpp"
#include "Logging.hpp"

namespace core {

FlushArbiter::FlushArbiter(sparta::TreeNode* node, const FlushArbiterParams* params)
    : sparta::Unit(node),
      mLogEnabled(params->log_enabled),
      mNumBpFlushes(&unit_stat_set_, "num_bp_flushes", "Flushes triggered by branch prediction", sparta::Counter::COUNT_NORMAL),
      mNumExeFlushes(&unit_stat_set_, "num_exe_flushes", "Flushes triggered by execute misprediction", sparta::Counter::COUNT_NORMAL),
      mNumDroppedBpFlushes(&unit_stat_set_, "num_dropped_bp_flushes", "BP flushes skipped due to conflicts", sparta::Counter::COUNT_NORMAL) {
    bp_flush_in.registerConsumerHandler(CREATE_SPARTA_HANDLER_WITH_DATA(FlushArbiter, receiveBpFlush_, FlushRequest));
    exe_flush_in.registerConsumerHandler(CREATE_SPARTA_HANDLER_WITH_DATA(FlushArbiter, receiveExeFlush_, FlushRequest));
    retire_flush_in.registerConsumerHandler(CREATE_SPARTA_HANDLER_WITH_DATA(FlushArbiter, receiveExeFlush_, FlushRequest));
}

void FlushArbiter::receiveBpFlush_(const FlushRequest& req) {
    // If speculation is disabled, ignore all BP flushes - they would incorrectly squash instructions
    if (!getSpeculationConfig().enabled) {
        ILOG("[flush_arbiter] IGNORING BP flush: speculation disabled");
        return;
    }

    ++mNumBpFlushes;
    ILOG("[flush_arbiter] cycle=" << getClock()->currentCycle() << " PREDICTED FLUSH from BranchPredictor: branch_tag=" << req.branch_tag << " branch_pc=0x"
                                  << std::hex << req.branch_pc << " target_pc=0x" << req.target_pc << std::dec);

    processFlush_(req);
}

void FlushArbiter::processFlush_(const FlushRequest& req) {
    // Only process one flush per cycle - ignore subsequent ones
    uint64_t current_cycle = getClock()->currentCycle();
    if (current_cycle == mLastFlushCycle) {
        ILOG("[flush_arbiter] IGNORING flush for tag=" << req.branch_tag << " (already processed a flush this cycle)");
        ++mNumDroppedBpFlushes;
        return;
    }
    mLastFlushCycle = current_cycle;

    ILOG("[flush_arbiter] EXECUTING flush: branch_tag=" << req.branch_tag << " redirecting to 0x" << std::hex << req.target_pc << std::dec);

    // NOTE: Whisper flush is done by FetchStructures when it receives the redirect
    // This keeps Whisper state synchronized with Fetch's actual redirect timing

    // Send flush to frontend stages
    flush_to_fq_out.send(req, 0);
    flush_to_icache_out.send(req, 0);
    flush_to_decode_out.send(req, 0);
    flush_to_decode_queue_out.send(req, 0);

    // Send redirect to FetchStructures (increments wrong_path_depth)
    // 1-cycle delay: redirect takes time to propagate to frontend
    PredictedRedirect redirect;
    redirect.branch_tag = req.branch_tag;
    redirect.branch_fetch_seq = req.branch_fetch_seq;  // Unique ID for stale redirect detection
    redirect.branch_pc = req.branch_pc;
    redirect.target_pc = req.target_pc;
    redirect.wrong_path_depth = req.branch_depth;  // Include depth for stale redirect detection
    redirect_to_fetch_out.send(redirect, 1);
}

void FlushArbiter::receiveExeFlush_(const FlushRequest& req) {
    uint64_t current_cycle = getClock()->currentCycle();

    // Execute flushes always take priority - they represent confirmed mispredictions
    // Don't skip even if a BP flush happened this cycle
    mLastFlushCycle = current_cycle;

    ++mNumExeFlushes;
    ILOG("[flush_arbiter] cycle=" << current_cycle << " MISPREDICTION FLUSH from Execute: branch_tag=" << req.branch_tag << " branch_pc=0x" << std::hex
                                  << req.branch_pc << " target_pc=0x" << req.target_pc << std::dec << " branch_depth=" << static_cast<int>(req.branch_depth));

    // NOTE: Whisper flush is done by FetchStructures when it receives the redirect
    // This keeps Whisper state synchronized with Fetch's actual redirect timing

    // Send misprediction redirect to Fetch
    // 1-cycle delay: redirect takes time to propagate to frontend
    BranchRedirect redirect;
    redirect.branch_tag = req.branch_tag;
    redirect.branch_fetch_seq = req.branch_fetch_seq;  // For stale redirect filtering
    redirect.branch_pc = req.branch_pc;
    redirect.correct_target_pc = req.target_pc;
    redirect.wrong_path_depth = req.branch_depth;
    redirect.was_taken = req.was_taken;
    mispred_redirect_to_fetch_out.send(redirect, 1);
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

void FlushArbiter::tick() {
    // Flushes are processed immediately in receive handlers
}

}  // namespace core
