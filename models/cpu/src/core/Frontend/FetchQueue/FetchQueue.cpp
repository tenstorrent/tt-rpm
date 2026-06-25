#include "Frontend/FetchQueue/FetchQueue.hpp"

#include <iostream>

#include "sparta/utils/SpartaAssert.hpp"

#include "Common/SpeculationUtils.hpp"
#include "Frontend/DecodeStructures/DecodeStructures.hpp"
#include "Logging.hpp"

namespace frontend {

namespace {
// Squash decision for a front-end item identified by (model tag, speculation depth)
// against a flush request. Shared by the FetchQueue resident-entry scan and the
// early-prediction buffer so the two never disagree: a buffered prediction is dropped
// exactly when the matching packet would be squashed. This is what keeps the early-
// prediction buffer bounded -- otherwise predictions whose packets were squashed
// upstream (and so never enqueue to consume them) accumulate forever, and the
// per-flush full scan of the buffer becomes O(n^2) over the run.
inline bool frontendShouldSquash(uint64_t tag, uint8_t depth, const core::FlushRequest& req) {
    if (req.source == core::FlushSource::Execute) {
        // Execute flush: protect older correct-path instructions; squash deeper
        // speculation, younger same-depth, and any wrong-path instruction.
        if (depth == 0 && tag <= req.branch_tag) return false;
        if (depth > req.branch_depth) return true;
        if (depth == req.branch_depth && tag > req.branch_tag) return true;
        return depth > 0;
    }
    // BP flush: ONLY squash same-depth younger fallthrough. Deeper speculation is on
    // the predicted path and must survive.
    return depth == req.branch_depth && tag > req.branch_tag;
}
}  // namespace

FetchQueue::FetchQueue(sparta::TreeNode* node, const FetchQueueParams* params)
    : sparta::Unit(node),
      mCapacity(params->capacity),
      mLogEnabled(params->log_enabled),
      mNumEnqueued(&unit_stat_set_, "num_enqueued", "Packets enqueued", sparta::Counter::COUNT_NORMAL),
      mNumDequeued(&unit_stat_set_, "num_dequeued", "Packets pulled by Decode", sparta::Counter::COUNT_NORMAL),
      mNumFullStallCycles(&unit_stat_set_, "full_stall_cycles", "Cycles stalled due to full queue", sparta::Counter::COUNT_NORMAL),
      mNumBranchWaitCycles(&unit_stat_set_, "branch_wait_cycles", "Cycles head-of-queue blocked on prediction", sparta::Counter::COUNT_NORMAL) {
    packets_in.registerConsumerHandler(CREATE_SPARTA_HANDLER_WITH_DATA(FetchQueue, receivePackets_, std::vector<core::PipelinePacket>));
    branch_predict_in.registerConsumerHandler(CREATE_SPARTA_HANDLER_WITH_DATA(FetchQueue, receivePrediction_, core::BranchPrediction));
    flush_in.registerConsumerHandler(CREATE_SPARTA_HANDLER_WITH_DATA(FetchQueue, receiveFlush_, core::FlushRequest));
}

void FetchQueue::flush() {
    mQueue.clear();
    mPendingBranchTags.clear();
    mEarlyPredictions.clear();
    ILOG("[fetch_queue] flushed");
}

void FetchQueue::receiveFlush_(const core::FlushRequest& req) {
    uint64_t current_cycle = getClock()->currentCycle();
    ILOG("[fetch_queue] cycle " << current_cycle << " FLUSH: branch_tag=" << req.branch_tag << " branch_depth=" << static_cast<int>(req.branch_depth)
                                << " source=" << static_cast<int>(req.source) << " queue_size=" << mQueue.size());

    // Set squash state for filtering in-flight packets (1 cycle window)
    mSquashActive = true;
    mSquashTag = req.branch_tag;
    mSquashDepth = req.branch_depth;
    mSquashSource = req.source;
    mSquashCycle = current_cycle;

    // For Execute flushes, also set the separate Execute squash state
    if (req.source == core::FlushSource::Execute) {
        mExeSquashActive = true;
        mExeSquashCycle = current_cycle;
    }

    // Squash entries already in the queue
    size_t before_size = mQueue.size();
    auto it = mQueue.begin();
    while (it != mQueue.end()) {
        bool should_flush = frontendShouldSquash(it->pkt.tag, it->pkt.wrong_path_depth, req);

        if (should_flush) {
            auto pending_it = mPendingBranchTags.find(it->pkt.tag);
            if (pending_it != mPendingBranchTags.end()) {
                mPendingBranchTags.erase(pending_it);
            }
            if (mVis) mVis->onSquash(it->pkt.fetch_seq, current_cycle);
            it = mQueue.erase(it);
        } else {
            ++it;
        }
    }

    // Rebuild pending branch tracking
    mPendingBranchTags.clear();
    for (size_t i = 0; i < mQueue.size(); ++i) {
        auto& e = mQueue[i];
        if (e.needs_branch_prediction && !e.has_branch_prediction) {
            mPendingBranchTags[e.pkt.tag].push_back(i);
        }
    }

    // Drop early predictions for squashed branches, using the SAME predicate as the
    // resident-queue scan above. A prediction is buffered here only until its packet
    // arrives to consume it; if that packet is squashed upstream it never arrives, so
    // without this the entry would leak. The old `tag > branch_tag` test missed these
    // under model-tag reuse, letting the buffer grow unbounded and turning this
    // per-flush scan into an O(n^2) hot spot.
    for (auto pred_it = mEarlyPredictions.begin(); pred_it != mEarlyPredictions.end();) {
        if (frontendShouldSquash(pred_it->second.tag, pred_it->second.wrong_path_depth, req)) {
            pred_it = mEarlyPredictions.erase(pred_it);
        } else {
            ++pred_it;
        }
    }

    ILOG("[fetch_queue] cycle " << getClock()->currentCycle() << " flushed " << (before_size - mQueue.size()) << " entries, remaining=" << mQueue.size());
}

void FetchQueue::receivePackets_(const std::vector<core::PipelinePacket>& pkts) {
    ILOG("[fetch_queue] received " << pkts.size() << " packet(s), queue_size=" << mQueue.size() << " squash_active=" << mSquashActive
                                   << " exe_squash=" << mExeSquashActive);

    uint64_t current_cycle = getClock()->currentCycle();

    // Filter in-flight packets for 1 cycle after Execute flush
    // ICache marks pending packets as flushed, but packets already sent need filtering here
    if (mExeSquashActive && current_cycle > mExeSquashCycle + 1) {
        mExeSquashActive = false;
    }

    // For BP flushes, clear state after 1 cycle
    if (mSquashActive && mSquashSource == core::FlushSource::BranchPredictor && current_cycle > mSquashCycle + 1) {
        mSquashActive = false;
    }

    size_t squashed_count = 0;
    for (const auto& pkt : pkts) {
        bool should_squash = false;

        // Execute squash: use time-based filtering
        // Within 1 cycle of flush: use full shouldSquash (in-flight packets)
        // Beyond 1 cycle: only filter deeper speculation (correct-path from redirect)
        if (mExeSquashActive) {
            bool within_window = (current_cycle <= mExeSquashCycle + 1);
            if (within_window) {
                core::FlushRequest exe_req;
                exe_req.branch_tag = mSquashTag;
                exe_req.branch_depth = mSquashDepth;
                exe_req.source = core::FlushSource::Execute;
                should_squash = core::shouldSquash(pkt, exe_req);
            } else {
                // Beyond window: only filter deeper speculation
                should_squash = (pkt.wrong_path_depth > mSquashDepth);
            }
        }

        // BP squash: filter fallthrough packets (same depth, younger tag) for 1 cycle
        if (!should_squash && mSquashActive && mSquashSource == core::FlushSource::BranchPredictor) {
            if (pkt.wrong_path_depth == mSquashDepth && pkt.tag > mSquashTag) {
                should_squash = true;
            }
        }

        if (should_squash) {
            ++squashed_count;
            ILOG("[fetch_queue] SQUASHING in-flight pkt tag=" << pkt.tag << " depth=" << static_cast<int>(pkt.wrong_path_depth));
            continue;
        }

        sparta_assert(mQueue.size() < mCapacity,
                      "[FetchQueue] Queue overflow - backpressure should prevent this! queue_size=" << mQueue.size() << " capacity=" << mCapacity);
        Entry entry;
        entry.pkt = pkt;
        bool is_branch = (pkt.inst_class == cpu::InstClass::Branch || pkt.inst_class == cpu::InstClass::Jump);
        entry.needs_branch_prediction = is_branch;

        if (is_branch) {
            auto it = mEarlyPredictions.find(pkt.tag);
            if (it != mEarlyPredictions.end()) {
                entry.has_branch_prediction = true;
                entry.branch_pred = it->second;
                mEarlyPredictions.erase(it);
            } else {
                entry.has_branch_prediction = false;
                mPendingBranchTags[pkt.tag].push_back(mQueue.size());
            }
        } else {
            entry.has_branch_prediction = false;
        }

        mQueue.push_back(std::move(entry));
        ++mNumEnqueued;

        // Track FetchQueue entry for visualization
        if (mVis) {
            mVis->onFetchQueueEnter(pkt.fetch_seq, getClock()->currentCycle());
        }
    }

    ILOG("[fetch_queue] enqueued " << (pkts.size() - squashed_count) << " packet(s), squashed " << squashed_count << ", queue_size=" << mQueue.size());
}

void FetchQueue::receivePrediction_(const core::BranchPrediction& pred) {
    ILOG("[fetch_queue] receivePrediction_ called for tag=" << pred.tag);
    auto it = mPendingBranchTags.find(pred.tag);
    if (it != mPendingBranchTags.end() && !it->second.empty()) {
        size_t idx = it->second.front();
        it->second.pop_front();
        if (it->second.empty()) mPendingBranchTags.erase(it);

        if (idx < mQueue.size()) {
            mQueue[idx].has_branch_prediction = true;
            mQueue[idx].branch_pred = pred;
            ILOG("[fetch_queue] attached prediction for tag=" << pred.tag << " mispred=" << pred.mispredicted);
        }
    } else {
        // Prediction arrived before the packet — buffer it
        mEarlyPredictions[pred.tag] = pred;
        ILOG("[fetch_queue] early prediction buffered for tag=" << pred.tag);
    }
}

void FetchQueue::tick() {
    // Nothing to do - Decode will pull packets when ready
    // Track branch wait cycles if head is blocked
    if (!mQueue.empty()) {
        auto& entry = mQueue.front();
        if (entry.needs_branch_prediction && !entry.has_branch_prediction) {
            ++mNumBranchWaitCycles;
            ILOG("[fetch_queue] head blocked: waiting for prediction tag=" << entry.pkt.tag);
        }
    }
}

FetchQueue::Entry FetchQueue::pullOne() {
    sparta_assert(!mQueue.empty(), "[FetchQueue] pullOne called on empty queue");

    auto& entry = mQueue.front();

    // Branches must have prediction before being pulled
    sparta_assert(!entry.needs_branch_prediction || entry.has_branch_prediction,
                  "[FetchQueue] Attempting to pull branch without prediction, tag=" << entry.pkt.tag);

    Entry result = std::move(entry);
    mQueue.pop_front();

    // Track FetchQueue exit for visualization
    if (mVis) {
        mVis->onFetchQueueExit(result.pkt.fetch_seq, getClock()->currentCycle());
    }

    // Update branch tracking indices after pop
    if (!mQueue.empty() && !mPendingBranchTags.empty()) {
        // Rebuild the index map after popping
        mPendingBranchTags.clear();
        for (size_t i = 0; i < mQueue.size(); ++i) {
            auto& e = mQueue[i];
            if (e.needs_branch_prediction && !e.has_branch_prediction) {
                mPendingBranchTags[e.pkt.tag].push_back(i);
            }
        }
    }

    ++mNumDequeued;
    ILOG("[fetch_queue] pulled tag=" << result.pkt.tag << ", remaining=" << mQueue.size());

    return result;
}

}  // namespace frontend
