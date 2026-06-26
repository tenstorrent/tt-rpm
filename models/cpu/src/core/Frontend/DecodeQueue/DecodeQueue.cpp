// SPDX-FileCopyrightText: © 2026 Tenstorrent AI ULC
//
// SPDX-License-Identifier: Apache-2.0

#include "Frontend/DecodeQueue/DecodeQueue.hpp"

#include "sparta/utils/SpartaAssert.hpp"

#include "Common/PipelineVisualizer.hpp"
#include "Common/SpeculationUtils.hpp"
#include "Logging.hpp"
#include "Midcore/Rename/Rename.hpp"

namespace frontend {

DecodeQueue::DecodeQueue(sparta::TreeNode* node, const DecodeQueueParams* params)
    : sparta::Unit(node),
      mCapacity(params->capacity),
      mLogEnabled(params->log_enabled),
      mNumEnqueued(&unit_stat_set_, "num_enqueued", "Uops enqueued", sparta::Counter::COUNT_NORMAL),
      mNumDequeued(&unit_stat_set_, "num_dequeued", "Uops pulled by Rename", sparta::Counter::COUNT_NORMAL),
      mNumFullStallCycles(&unit_stat_set_, "full_stall_cycles", "Cycles stalled due to full queue", sparta::Counter::COUNT_NORMAL) {
    decode_in.registerConsumerHandler(CREATE_SPARTA_HANDLER_WITH_DATA(DecodeQueue, receiveDecodePackets_, std::vector<core::DecodePacket>));
    flush_in.registerConsumerHandler(CREATE_SPARTA_HANDLER_WITH_DATA(DecodeQueue, receiveFlush_, core::FlushRequest));
}

void DecodeQueue::flush() {
    mQueue.clear();
    ILOG("[decode_queue] flushed");
}

void DecodeQueue::receiveFlush_(const core::FlushRequest& req) {
    uint64_t cycle = getClock()->currentCycle();
    ILOG("[decode_queue] cycle=" << cycle << " RECEIVED flush: branch_tag=" << req.branch_tag << " branch_depth=" << static_cast<int>(req.branch_depth)
                                 << " source=" << static_cast<int>(req.source));

    // No time-based flush barrier: in-flight wrong-path uop vectors are cancelled at the
    // source (Decode cancels its out_port on flush), so only resident entries are squashed
    // here. This avoids the reused-tag hazard the old barrier had.

    // Squash entries younger than the mispredicted branch
    auto it = mQueue.begin();
    uint32_t squashed = 0;
    while (it != mQueue.end()) {
        if (core::shouldSquash(it->pkt, req)) {
            if (mVis) mVis->onSquash(it->pkt.fetch_seq, cycle);
            it = mQueue.erase(it);
            ++squashed;
        } else {
            ++it;
        }
    }
    ILOG("[decode_queue] Squashed " << squashed << " entries, remaining=" << mQueue.size());
}

void DecodeQueue::receiveDecodePackets_(const std::vector<core::DecodePacket>& pkts) {
    uint64_t current_cycle = getClock()->currentCycle();

    // Stale wrong-path uop vectors are cancelled at the source (Decode) on flush, so
    // anything arriving here is on the current path -- just enqueue it.
    ILOG("[decode_queue] received " << pkts.size() << " uop(s), queue_size=" << mQueue.size());

    for (const auto& dp : pkts) {
        sparta_assert(mQueue.size() < mCapacity,
                      "[DecodeQueue] Queue overflow - backpressure should prevent this! queue_size=" << mQueue.size() << " capacity=" << mCapacity);
        mQueue.push_back(dp);
        ++mNumEnqueued;
        if (mVis) mVis->onDecodeQueueEnter(dp.pkt.fetch_seq, current_cycle);
    }
    ILOG("[decode_queue] enqueued " << pkts.size() << " uop(s), queue_size=" << mQueue.size());
}

void DecodeQueue::tick() {
    // Nothing to do - Rename will pull packets when ready
}

core::DecodePacket DecodeQueue::pullOne() {
    sparta_assert(!mQueue.empty(), "[DecodeQueue] pullOne called on empty queue");

    auto dp = std::move(mQueue.front());
    mQueue.pop_front();

    ++mNumDequeued;
    if (mVis) mVis->onDecodeQueueExit(dp.pkt.fetch_seq, getClock()->currentCycle());
    ILOG("[decode_queue] pulled tag=" << dp.pkt.tag << ", remaining=" << mQueue.size());

    return dp;
}

}  // namespace frontend
