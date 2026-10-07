// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Tenstorrent USA, Inc.

#include "Frontend/DecodeStructures/DecodeStructures.hpp"

#include "Common/SpeculationUtils.hpp"
#include "Frontend/DecodeQueue/DecodeQueue.hpp"
#include "Frontend/FetchQueue/FetchQueue.hpp"
#include "Logging.hpp"
#include "Midcore/Rename/Rename.hpp"

namespace frontend {

DecodeStructures::DecodeStructures(sparta::TreeNode* node, const DecodeStructuresParams* params)
    : sparta::Unit(node),
      mMispredPenalty(params->bp_mispred_penalty),
      mNumDecoded(&unit_stat_set_, "num_decoded", "Total instructions decoded", sparta::Counter::COUNT_NORMAL),
      mNumMispredStallCycles(&unit_stat_set_, "num_mispred_stall_cycles", "Cycles stalled due to branch misprediction", sparta::Counter::COUNT_NORMAL),
      mNumBranchWaitCycles(&unit_stat_set_, "num_branch_wait_cycles", "Cycles waiting for branch prediction", sparta::Counter::COUNT_NORMAL) {
    flush_in.registerConsumerHandler(CREATE_SPARTA_HANDLER_WITH_DATA(DecodeStructures, receiveFlush_, core::FlushRequest));
    packets_in.registerConsumerHandler(CREATE_SPARTA_HANDLER_WITH_DATA(DecodeStructures, receivePackets_, std::vector<core::PipelinePacket>));
    branch_predict_in.registerConsumerHandler(CREATE_SPARTA_HANDLER_WITH_DATA(DecodeStructures, receiveBranchPrediction_, core::BranchPrediction));

    mLatencyTable[static_cast<size_t>(core::UopType::ALU)] = static_cast<uint8_t>(params->lat_alu);
    mLatencyTable[static_cast<size_t>(core::UopType::Mul)] = static_cast<uint8_t>(params->lat_mul);
    mLatencyTable[static_cast<size_t>(core::UopType::Div)] = static_cast<uint8_t>(params->lat_div);
    mLatencyTable[static_cast<size_t>(core::UopType::Branch)] = static_cast<uint8_t>(params->lat_branch);
    mLatencyTable[static_cast<size_t>(core::UopType::Load)] = static_cast<uint8_t>(params->lat_load);
    mLatencyTable[static_cast<size_t>(core::UopType::Store)] = static_cast<uint8_t>(params->lat_store);
    mLatencyTable[static_cast<size_t>(core::UopType::FpOp)] = static_cast<uint8_t>(params->lat_fp);
    mLatencyTable[static_cast<size_t>(core::UopType::VecOp)] = static_cast<uint8_t>(params->lat_vec);
    mLatencyTable[static_cast<size_t>(core::UopType::Fence)] = static_cast<uint8_t>(params->lat_fence);
}

void DecodeStructures::receiveFlush_(const core::FlushRequest& req) {
    uint64_t cycle = getClock()->currentCycle();
    size_t pending_before = mPendingPackets.size();
    size_t buf_before = mDecodePacketsBuf.size();
    size_t pred_before = mPendingPredictions.size();

    ILOG("[decode] received flush: branch_tag=" << req.branch_tag << " source=" << static_cast<int>(req.source));

    // Cancel in-flight uop vectors heading to the DecodeQueue that belong to the squashed
    // (wrong) path. Synchronous at flush time, so reused tags from the upcoming re-fetch
    // cannot be confused with these. Replaces the DecodeQueue's time-based flush barrier.
    // tick() splits sends at branch/jump boundaries so each vector is all-or-nothing.
    uint32_t cancelled_to_dq = out_port.cancelIf([&req](const std::vector<core::DecodePacket>& dps) {
        if (dps.empty()) return false;
        for (const auto& dp : dps)
            if (!core::shouldSquash(dp.pkt, req)) return false;
        return true;
    });
    if (cancelled_to_dq > 0) ILOG("[decode] cycle " << cycle << " cancelled " << cancelled_to_dq << " in-flight uop vector(s) to DecodeQueue");

    // Immediately clear all buffered decode packets that should be squashed
    auto it = mDecodePacketsBuf.begin();
    while (it != mDecodePacketsBuf.end()) {
        if (core::shouldSquash(it->pkt, req)) {
            if (mVis) mVis->onSquash(it->pkt.fetch_seq, cycle);
            it = mDecodePacketsBuf.erase(it);
        } else {
            ++it;
        }
    }

    // Immediately clear all pending packets that should be squashed (direct mode)
    auto pit = mPendingPackets.begin();
    while (pit != mPendingPackets.end()) {
        if (core::shouldSquash(*pit, req)) {
            if (mVis) mVis->onSquash(pit->fetch_seq, cycle);
            pit = mPendingPackets.erase(pit);
        } else {
            ++pit;
        }
    }

    // Clear pending predictions for squashed branches (use fetch_seq for accurate matching)
    for (auto pred_it = mPendingPredictions.begin(); pred_it != mPendingPredictions.end();) {
        if (pred_it->first > req.branch_fetch_seq) {
            pred_it = mPendingPredictions.erase(pred_it);
        } else {
            ++pred_it;
        }
    }

    size_t cleared = (pending_before - mPendingPackets.size()) + (buf_before - mDecodePacketsBuf.size());
    size_t pred_cleared = pred_before - mPendingPredictions.size();
    ILOG("[decode] cycle " << getClock()->currentCycle() << " FLUSH: tag=" << req.branch_tag << " fetch_seq=" << req.branch_fetch_seq << " cleared=" << cleared
                           << " pred_cleared=" << pred_cleared);
}

void DecodeStructures::receiveBranchPrediction_(const core::BranchPrediction& pred) {
    // Store prediction keyed by fetch_seq (never reused, even after flush)
    mPendingPredictions[pred.fetch_seq] = pred;
    ILOG("[decode] cycle " << getClock()->currentCycle() << " received BP for fetch_seq=" << pred.fetch_seq << " (tag=" << pred.tag << ")"
                           << " predicted_taken=" << pred.predicted_taken << " mispredicted=" << pred.mispredicted);
}

void DecodeStructures::receivePackets_(const std::vector<core::PipelinePacket>& pkts) {
    // Direct mode: receive packets from ICache - just buffer them
    for (const auto& pkt : pkts) {
        mPendingPackets.push_back(pkt);
    }
    ILOG("[decode] received " << pkts.size() << " packets directly, pending=" << mPendingPackets.size());
}

core::UopType DecodeStructures::classifyUop(cpu::InstClass ic) {
    switch (ic) {
        case cpu::InstClass::ALU:
            return core::UopType::ALU;
        case cpu::InstClass::Multiply:
            return core::UopType::Mul;
        case cpu::InstClass::Divide:
            return core::UopType::Div;
        case cpu::InstClass::Branch:
        case cpu::InstClass::Jump:
            return core::UopType::Branch;
        case cpu::InstClass::Load:
            return core::UopType::Load;
        case cpu::InstClass::Store:
        case cpu::InstClass::Atomic:
            return core::UopType::Store;
        case cpu::InstClass::Float:
            return core::UopType::FpOp;
        case cpu::InstClass::Vector:
            return core::UopType::VecOp;
        case cpu::InstClass::Custom:
            return core::UopType::ALU;
    }
    return core::UopType::ALU;
}

void DecodeStructures::tick() {
    uint64_t cycle = getClock()->currentCycle();

    ILOG("[decode] cycle " << cycle << " pending_pkts=" << mPendingPackets.size() << " pending_preds=" << mPendingPredictions.size());

    // Handle misprediction stall
    if (mMispredStall > 0) {
        ILOG("[decode] mispred stall, " << mMispredStall << " cycle(s) remaining");
        --mMispredStall;
        ++mNumMispredStallCycles;
        mRestartAfterMispredStall = true;
        return;
    }

    // Check downstream capacity
    size_t downstream_space = 32;
    if (mDirectMode && mRename) {
        downstream_space = mRename->available();
    } else if (mDecodeQueue) {
        downstream_space = mDecodeQueue->available();
    }

    if (downstream_space == 0) {
        ILOG("[decode] stalled: downstream full");
        return;
    }

    mDecodePacketsBuf.clear();

    if (mDirectMode) {
        // Direct mode: process packets from mPendingPackets buffer
        while (!mPendingPackets.empty() && mDecodePacketsBuf.size() < downstream_space) {
            const auto& pkt = mPendingPackets.front();  // Peek first, don't remove yet

            // Check if this is a branch that needs prediction
            bool is_branch = (pkt.inst_class == cpu::InstClass::Branch || pkt.inst_class == cpu::InstClass::Jump);

            if (is_branch) {
                // Use fetch_seq for prediction lookup (never reused, even after flush)
                auto pred_it = mPendingPredictions.find(pkt.fetch_seq);
                if (pred_it == mPendingPredictions.end()) {
                    // Prediction hasn't arrived yet - STALL until it does
                    ++mNumBranchWaitCycles;
                    ILOG("[decode] direct mode: waiting for branch prediction for fetch_seq=" << pkt.fetch_seq);
                    break;  // Stop processing, maintain in-order semantics
                }
            }

            // Safe to remove and process - prediction is available if needed
            core::PipelinePacket pkt_copy = pkt;  // Copy before erasing
            mPendingPackets.erase(mPendingPackets.begin());

            core::DecodePacket dp;
            dp.pkt = pkt_copy;
            dp.uop_type = classifyUop(pkt_copy.inst_class);
            dp.latency = mLatencyTable[static_cast<size_t>(dp.uop_type)];

            // Look up prediction for branches (guaranteed to exist now, keyed by fetch_seq)
            if (is_branch) {
                auto pred_it = mPendingPredictions.find(pkt_copy.fetch_seq);
                dp.predicted_taken = pred_it->second.predicted_taken;
                dp.was_mispredicted = pred_it->second.mispredicted;
                ILOG("[decode] cycle " << getClock()->currentCycle() << " fetch_seq=" << pkt_copy.fetch_seq << " (tag=" << pkt_copy.tag
                                       << ") merged prediction: predicted_taken=" << dp.predicted_taken << " was_mispredicted=" << dp.was_mispredicted);

                // Apply misprediction penalty (stall decode for N cycles)
                if (pred_it->second.mispredicted && mMispredPenalty > 0) {
                    mMispredStall = mMispredPenalty;
                    if (mVis) mVis->onBranchMispredict(pkt_copy.fetch_seq, cycle);
                    ILOG("[decode] direct mode: misprediction at pc=0x" << std::hex << pkt_copy.pc << std::dec << " stalling for " << mMispredPenalty
                                                                        << " cycles");
                }

                mPendingPredictions.erase(pred_it);  // Consumed
            } else {
                dp.predicted_taken = false;  // Non-branch: not applicable
                dp.was_mispredicted = false;
            }

            dp.after_mispredict_stall = mRestartAfterMispredStall;
            mRestartAfterMispredStall = false;
            mDecodePacketsBuf.push_back(dp);
        }
    } else {
        // Pull model from FetchQueue
        if (!mFetchQueue) return;

        while (!mFetchQueue->isEmpty() && mDecodePacketsBuf.size() < downstream_space) {
            const auto* peek_entry = mFetchQueue->peek();
            if (!peek_entry) break;

            // Branch must have prediction before we can decode
            if (peek_entry->needs_branch_prediction && !peek_entry->has_branch_prediction) {
                ++mNumBranchWaitCycles;
                ILOG("[decode] waiting for branch prediction for tag=" << peek_entry->pkt.tag);
                break;
            }

            // Pull the entry from FetchQueue
            auto entry = mFetchQueue->pullOne();

            core::DecodePacket dp;
            dp.pkt = entry.pkt;
            dp.uop_type = classifyUop(entry.pkt.inst_class);
            dp.latency = mLatencyTable[static_cast<size_t>(dp.uop_type)];

            // Copy branch prediction info
            if (entry.has_branch_prediction) {
                dp.predicted_taken = entry.branch_pred.predicted_taken;
                dp.was_mispredicted = entry.branch_pred.mispredicted;

                // Handle misprediction penalty
                if (entry.branch_pred.mispredicted) {
                    mMispredStall = mMispredPenalty;
                    if (mVis) mVis->onBranchMispredict(entry.pkt.fetch_seq, getClock()->currentCycle());
                    ILOG("[decode] misprediction at pc=0x" << std::hex << entry.pkt.pc << std::dec << " stalling for " << mMispredPenalty << " cycles");
                }
            }

            dp.after_mispredict_stall = mRestartAfterMispredStall;
            mRestartAfterMispredStall = false;
            mDecodePacketsBuf.push_back(dp);
        }
    }

    if (!mDecodePacketsBuf.empty()) {
        if (mVis) {
            for (const auto& dp : mDecodePacketsBuf) mVis->onDecode(dp.pkt.fetch_seq, getClock()->currentCycle());
        }
        mNumDecoded += mDecodePacketsBuf.size();
        // Split at control-flow boundaries so no payload straddles a branch (depth changes
        // only at branches, so each sub-vector is also depth-uniform). Makes the
        // all-or-nothing flush cancelIf on out_port exact -- see FrontendMemoryStructures.
        std::vector<core::DecodePacket> sub;
        sub.reserve(mDecodePacketsBuf.size());
        for (const auto& dp : mDecodePacketsBuf) {
            sub.push_back(dp);
            if (dp.pkt.inst_class == cpu::InstClass::Branch || dp.pkt.inst_class == cpu::InstClass::Jump) {
                out_port.send(sub, 0);
                sub.clear();
            }
        }
        if (!sub.empty()) out_port.send(sub, 0);
        ILOG("[decode] sent " << mDecodePacketsBuf.size() << " packet(s) downstream");
    }
}

}  // namespace frontend
