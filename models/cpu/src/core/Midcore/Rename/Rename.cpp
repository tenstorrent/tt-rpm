// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Tenstorrent USA, Inc.

#include "Midcore/Rename/Rename.hpp"

#include <iostream>

#include "Common/SpeculationUtils.hpp"
#include "Frontend/DecodeQueue/DecodeQueue.hpp"
#include "LoadStoreUnit/LSQ/LSQ.hpp"
#include "Logging.hpp"
#include "Midcore/Execute/Execute.hpp"
#include "Midcore/Issue/Issue.hpp"
#include "Midcore/Writeback/Writeback.hpp"

namespace midcore {

Rename::Rename(sparta::TreeNode* node, const RenameParams* params)
    : sparta::Unit(node),
      mPrf(params->num_phys_regs, params->freelist_headroom),
      mCheckpointMgr(params->max_branch_checkpoints),
      mDispatchWidth(params->dispatch_width),
      mNumDispatched(&unit_stat_set_, "num_dispatched", "Total instructions dispatched", sparta::Counter::COUNT_NORMAL),
      mNumZeroDispatchCycles(&unit_stat_set_, "num_zero_dispatch_cycles", "Cycles a uop was waiting to dispatch and none was dispatched",
                             sparta::Counter::COUNT_NORMAL),
      mNumCheckpointStallCycles(&unit_stat_set_, "num_checkpoint_stall_cycles", "Cycles stalled on checkpoint hazards", sparta::Counter::COUNT_NORMAL),
      mTopdownCycles(&unit_stat_set_, "topdown_cycles", "Cycles covered by top-down slot accounting", sparta::Counter::COUNT_NORMAL),
      mTopdownSlots(&unit_stat_set_, "topdown_slots", "Dispatch slots (cycles x dispatch_width)", sparta::Counter::COUNT_NORMAL),
      mTopdownFrontendBoundSlots(&unit_stat_set_, "topdown_frontend_bound_slots", "Slots not filled because the frontend delivered no uop (outside recovery)",
                                 sparta::Counter::COUNT_NORMAL),
      mTopdownFrontendLatencySlots(&unit_stat_set_, "topdown_frontend_latency_slots", "Frontend-bound slots in cycles in which no uop was delivered",
                                   sparta::Counter::COUNT_NORMAL),
      mTopdownRecoverySlots(&unit_stat_set_, "topdown_recovery_slots", "Slots not filled while recovering from a misprediction or pipeline flush",
                            sparta::Counter::COUNT_NORMAL),
      mTopdownBackendBoundSlots(&unit_stat_set_, "topdown_backend_bound_slots", "Slots not filled because a backend resource stalled dispatch",
                                sparta::Counter::COUNT_NORMAL),
      mTopdownBackendMemorySlots(&unit_stat_set_, "topdown_backend_memory_slots",
                                 "Backend-bound slots attributed to memory (load/store queue full or an incomplete load at the ROB head)",
                                 sparta::Counter::COUNT_NORMAL),
      mStallRobFull(&unit_stat_set_, "dispatch_stall_rob_full", "Cycles dispatch stopped early because the ROB was full", sparta::Counter::COUNT_NORMAL),
      mStallPrfFull(&unit_stat_set_, "dispatch_stall_prf_full", "Cycles dispatch stopped early for lack of free physical registers",
                    sparta::Counter::COUNT_NORMAL),
      mStallSchedulerFull(&unit_stat_set_, "dispatch_stall_scheduler_full",
                          "Cycles dispatch stopped early because the issue scheduler (OOO), or an execute group, issue port or divider (in-order), was full",
                          sparta::Counter::COUNT_NORMAL),
      mStallLoadQueueFull(&unit_stat_set_, "dispatch_stall_load_queue_full", "Cycles dispatch stopped early for lack of a load-queue entry",
                          sparta::Counter::COUNT_NORMAL),
      mStallStoreQueueFull(&unit_stat_set_, "dispatch_stall_store_queue_full", "Cycles dispatch stopped early for lack of a store-queue entry",
                           sparta::Counter::COUNT_NORMAL),
      mStallCheckpointFull(&unit_stat_set_, "dispatch_stall_checkpoint_full", "Cycles dispatch stopped early for lack of a branch checkpoint",
                           sparta::Counter::COUNT_NORMAL),
      mStallOperandsNotReady(&unit_stat_set_, "dispatch_stall_operands_not_ready",
                             "Cycles in-order dispatch stopped early because source operands were not ready", sparta::Counter::COUNT_NORMAL) {
    commit_in.registerConsumerHandler(CREATE_SPARTA_HANDLER_WITH_DATA(Rename, receiveCommits_, std::vector<core::PhysRegRef>));
    completion_exe_in.registerConsumerHandler(CREATE_SPARTA_HANDLER_WITH_DATA(Rename, receiveCompletions_, std::vector<core::PhysRegRef>));
    completion_lsq_in.registerConsumerHandler(CREATE_SPARTA_HANDLER_WITH_DATA(Rename, receiveCompletions_, std::vector<core::PhysRegRef>));
    flush_in.registerConsumerHandler(CREATE_SPARTA_HANDLER_WITH_DATA(Rename, receiveFlush_, core::FlushRequest));
    freed_regs_in.registerConsumerHandler(CREATE_SPARTA_HANDLER_WITH_DATA(Rename, receiveFreedRegs_, std::vector<core::PhysRegRef>));
    checkpoint_release_in.registerConsumerHandler(CREATE_SPARTA_HANDLER_WITH_DATA(Rename, receiveCheckpointRelease_, uint64_t));
    packets_in.registerConsumerHandler(CREATE_SPARTA_HANDLER_WITH_DATA(Rename, receivePackets_, std::vector<core::DecodePacket>));

    // Banking config is consumed by ChipSim during binding
    (void)static_cast<bool>(params->regfile_banking_enabled);
    (void)static_cast<uint32_t>(params->regfile_num_banks);
    (void)static_cast<uint32_t>(params->regfile_reads_per_bank);
}

void Rename::receiveCompletions_(const std::vector<core::PhysRegRef>& completions) {
    for (const auto& ref : completions) {
        mArchScoreboard.clearReg(ref.phys_reg, ref.type, ref.tag);
    }
}

void Rename::receiveFlush_(const core::FlushRequest& req) {
    uint64_t cycle = getClock()->currentCycle();
    ILOG("[rename] cycle " << cycle << " RECEIVED flush: branch_tag=" << req.branch_tag << " branch_depth=" << static_cast<int>(req.branch_depth)
                           << " source=" << static_cast<int>(req.source) << " pending_packets=" << mPendingPackets.size());

    // Squash pending packets that are on the wrong path
    size_t squashed_count = 0;
    auto it = mPendingPackets.begin();
    while (it != mPendingPackets.end()) {
        if (core::shouldSquash(it->pkt, req)) {
            ILOG("[rename] cycle " << cycle << " SQUASHING tag=" << it->pkt.tag << " depth=" << static_cast<int>(it->pkt.wrong_path_depth));
            if (mVis) mVis->onSquash(it->pkt.fetch_seq, cycle);
            it = mPendingPackets.erase(it);
            ++squashed_count;
        } else {
            ++it;
        }
    }
    ILOG("[rename] cycle " << cycle << " Squashed " << squashed_count << " pending packets, remaining=" << mPendingPackets.size());

    // Restore the RAT to the flush point: a branch checkpoint restores the whole map
    // at once; otherwise (non-branch trap/interrupt resync, or no checkpoint) fall back
    // to the history walk-back. Either way, drop squashed history younger than the flush.
    bool restored_via_ckpt = false;
    if (mCheckpointMgr.hasCheckpoint(req.branch_fetch_seq)) {
        restored_via_ckpt = mCheckpointMgr.restore(req.branch_fetch_seq, mPrf);
        if (restored_via_ckpt) ILOG("[rename] Restored RAT from checkpoint for fetch_seq=" << req.branch_fetch_seq);
    }
    rollbackRatTo_(req.branch_fetch_seq, /*apply_to_rat=*/!restored_via_ckpt);

    // In-order path has no RAT; clear scoreboard busy bits left by squashed writers.
    mArchScoreboard.restoreToTag(req.branch_tag);

    // The bubble before the flush is charged to the frontend, unless wrong path is off: Fetch then
    // held at the mispredicted branch. The bubble until the first post-flush uop is recovery.
    endBubble_(!mSpecCfg->enabled);
    mRecoveryPending = true;

    // Release all checkpoints for younger branches
    mCheckpointMgr.releaseYoungerThan(req.branch_fetch_seq);

    // Also release the mispredicted branch's checkpoint itself
    // (it's no longer needed after restoration)
    mCheckpointMgr.release(req.branch_fetch_seq);

    // Update speculation depth tracking
    // After misprediction recovery, depth should be one less than the branch's depth
    // (or 0 if branch was at depth 0)
    if (req.branch_depth > 0) {
        mCurrentWrongPathDepth = req.branch_depth - 1;
    } else {
        mCurrentWrongPathDepth = 0;
    }

    ILOG("[rename] Flush complete. Active checkpoints: " << mCheckpointMgr.numActive() << " new_depth=" << static_cast<int>(mCurrentWrongPathDepth));
}

void Rename::rollbackRatTo_(uint64_t branch_fetch_seq, bool apply_to_rat) {
    // Undo younger-than-flush renames youngest first; the last write per reg wins,
    // leaving the RAT at its state as of the branch. PRNs are freed by ROB-squash.
    size_t undone = 0;
    while (!mRenameHistory.empty() && mRenameHistory.back().fetch_seq > branch_fetch_seq) {
        const auto& rec = mRenameHistory.back();
        if (apply_to_rat) {
            mPrf.mapping(rec.type, rec.arch_reg).speculativeRegister = rec.old_phys;
        }
        mRenameHistory.pop_back();
        ++undone;
    }
    if (undone) {
        ILOG("[rename] Rolled back " << undone << " rename records to fetch_seq " << branch_fetch_seq
                                     << (apply_to_rat ? " (RAT restored via walk-back)" : " (history pruned; RAT via checkpoint)"));
    }
}

void Rename::pruneCommittedHistory_() {
    // Records older than the oldest in-flight ROB entry are committed; drop them.
    if (!mRob) return;
    if (mRob->empty()) {
        mRenameHistory.clear();
        return;
    }
    const uint64_t oldest_inflight = mRob->head().fetch_seq;
    while (!mRenameHistory.empty() && mRenameHistory.front().fetch_seq < oldest_inflight) {
        mRenameHistory.pop_front();
    }
}

void Rename::receiveFreedRegs_(const std::vector<core::PhysRegRef>& regs) {
    for (const auto& ref : regs) {
        mPrf.releaseRegisterSafe(ref.phys_reg, ref.type);
    }
    ILOG("[rename] Received " << regs.size() << " freed registers from ROB squash");
}

void Rename::receiveCheckpointRelease_(const uint64_t& branch_fetch_seq) {
    mCheckpointMgr.release(branch_fetch_seq);
    ILOG("[rename] Released checkpoint for fetch_seq=" << branch_fetch_seq << ", remaining=" << mCheckpointMgr.numActive());
}

void Rename::receivePackets_(const std::vector<core::DecodePacket>& pkts) {
    // Direct mode: receive packets from Decode
    for (const auto& pkt : pkts) {
        mPendingPackets.push_back(pkt);
    }
    ILOG("[rename] received " << pkts.size() << " packets directly, pending=" << mPendingPackets.size());
}

bool Rename::isReady() const {
    if (mDirectMode) {
        return !mPendingPackets.empty();
    }
    if (!mDecodeQueue || !mDecodeQueue->hasReady()) return false;
    return true;
}

void Rename::tick() {
    if (mDirectMode) {
        if (mPendingPackets.empty()) {
            accountSlots_(0, DispatchStall::Frontend);
            return;
        }
    } else {
        if (!mDecodeQueue || !mDecodeQueue->hasReady()) {
            accountSlots_(0, DispatchStall::Frontend);
            return;
        }
    }

    uint32_t dispatched = 0;
    if (mOooEnabled) {
        dispatched = tickOoo_();
    } else {
        dispatched = tickInorder_();
    }

    if (dispatched == 0) {
        ++mNumZeroDispatchCycles;
    } else {
        mNumDispatched += dispatched;
    }

    accountSlots_(dispatched, mDispatchStall);
}

bool Rename::isMemoryStall_(DispatchStall stall) const {
    if (stall == DispatchStall::LoadQueueFull || stall == DispatchStall::StoreQueueFull) return true;
    // The oldest instruction is a load still waiting for its data
    return mRob && !mRob->empty() && !mRob->headCompleted() && mRob->head().uop_type == core::UopType::Load;
}

void Rename::endBubble_(bool recovery) {
    if (recovery) {
        mTopdownRecoverySlots += mBubbleSlots;
    } else {
        mTopdownFrontendBoundSlots += mBubbleSlots;
        mTopdownFrontendLatencySlots += mBubbleLatencySlots;
    }
    mBubbleSlots = 0;
    mBubbleLatencySlots = 0;
    mRecoveryPending = false;
}

void Rename::simulationTerminating_() {
    // A bubble still open at the end of the run is charged to the frontend
    endBubble_(false);
}

void Rename::accountSlots_(uint32_t dispatched, DispatchStall stall) {
    ++mTopdownCycles;
    mTopdownSlots += mDispatchWidth;
    if (dispatched >= mDispatchWidth) return;

    const uint32_t lost = mDispatchWidth - dispatched;
    if (stall == DispatchStall::None || stall == DispatchStall::Frontend) {
        mBubbleSlots += lost;
        if (dispatched == 0) mBubbleLatencySlots += lost;
        return;
    }

    mTopdownBackendBoundSlots += lost;
    if (isMemoryStall_(stall)) mTopdownBackendMemorySlots += lost;

    switch (stall) {
        case DispatchStall::RobFull:
            ++mStallRobFull;
            break;
        case DispatchStall::PrfFull:
            ++mStallPrfFull;
            break;
        case DispatchStall::SchedulerFull:
            ++mStallSchedulerFull;
            break;
        case DispatchStall::LoadQueueFull:
            ++mStallLoadQueueFull;
            break;
        case DispatchStall::StoreQueueFull:
            ++mStallStoreQueueFull;
            break;
        case DispatchStall::CheckpointFull:
            ++mStallCheckpointFull;
            break;
        case DispatchStall::OperandsNotReady:
            ++mStallOperandsNotReady;
            break;
        case DispatchStall::None:
        case DispatchStall::Frontend:
            break;
    }
}

bool Rename::hasPendingPacket_() const { return mDirectMode ? !mPendingPackets.empty() : mDecodeQueue->hasReady(); }

const core::DecodePacket* Rename::peekPacket_() const {
    if (mDirectMode) {
        return mPendingPackets.empty() ? nullptr : &mPendingPackets.front();
    }
    return mDecodeQueue->peek();
}

core::DecodePacket Rename::pullPacket_() {
    if (mDirectMode) {
        auto dp = mPendingPackets.front();
        mPendingPackets.erase(mPendingPackets.begin());
        return dp;
    }
    return mDecodeQueue->pullOne();
}

void Rename::extractRegOperands_(const core::DecodePacket& dp, std::vector<core::RegOperand>& src_regs, std::vector<core::RegOperand>& dst_regs) {
    const auto& rec = dp.pkt.inst->getTraceRecord();
    for (const auto& src : rec.sourceOperands) {
        auto rt = static_cast<core::RegType>(src.type);
        if (!core::isRegFileType(rt)) continue;
        src_regs.push_back({rt, static_cast<uint8_t>(src.number)});
    }
    for (const auto& dst : rec.modifiedRegs) {
        auto rt = static_cast<core::RegType>(dst.type);
        if (!core::isRegFileType(rt)) continue;
        dst_regs.push_back({rt, static_cast<uint8_t>(dst.number)});
    }
}

uint32_t Rename::tickOoo_() {
    mRenamedBuf.clear();
    uint32_t dispatched = 0;
    mDispatchStall = DispatchStall::None;

    while (dispatched < mDispatchWidth && hasPendingPacket_()) {
        const auto* peek = peekPacket_();
        if (!peek) break;

        std::vector<core::RegOperand> src_regs;
        std::vector<core::RegOperand> dst_regs;
        extractRegOperands_(*peek, src_regs, dst_regs);

        if (!mPrf.canAllocate(dst_regs)) {
            mDispatchStall = DispatchStall::PrfFull;
            break;
        }
        if (mRob && !mRob->canAllocate()) {
            mDispatchStall = DispatchStall::RobFull;
            break;
        }
        if (!mDownstream->isReadyForType(peek->uop_type)) {
            mDispatchStall = DispatchStall::SchedulerFull;
            break;
        }

        // Stall dispatch when no LQ/SQ credit is available so memory-op occupancy stays bounded.
        const bool is_mem_load = (peek->uop_type == core::UopType::Load);
        const bool is_mem_store = (peek->uop_type == core::UopType::Store || peek->uop_type == core::UopType::Fence);
        if (mLsq && is_mem_load && !mLsq->canClaimLoad()) {
            mDispatchStall = DispatchStall::LoadQueueFull;
            break;
        }
        if (mLsq && is_mem_store && !mLsq->canClaimStore()) {
            mDispatchStall = DispatchStall::StoreQueueFull;
            break;
        }

        // As in BOOM (MicroOp.allocate_brtag), a direct jump takes no checkpoint: it cannot mispredict.
        bool is_branch = (peek->uop_type == core::UopType::Branch);
        bool need_checkpoint = is_branch && !peek->direct_jump && mSpecCfg->enabled;
        if (need_checkpoint && !mCheckpointMgr.canAllocate()) {
            ++mNumCheckpointStallCycles;
            mDispatchStall = DispatchStall::CheckpointFull;
            break;
        }

        // Pull the packet
        auto dp = pullPacket_();
        endBubble_(mRecoveryPending);

        // Claim the LSQ credit now that the uop is committed to dispatch.
        if (mLsq && (is_mem_load || is_mem_store)) mLsq->claim(dp);

        core::RenamedPacket rpkt;
        rpkt.pkt = dp.pkt;
        // Keep packet's original wrong_path_depth from fetch (don't overwrite)
        rpkt.uop_type = dp.uop_type;
        rpkt.latency = dp.latency;
        rpkt.predicted_taken = dp.predicted_taken;
        rpkt.was_mispredicted = dp.was_mispredicted;
        rpkt.target_mispredicted = dp.target_mispredicted;

        std::vector<core::PhysRegRef> old_phys_dsts;
        std::vector<core::PhysRegRef> new_phys_dsts;

        for (const auto& src : src_regs) {
            auto& m = mPrf.mapping(src.type, src.number);
            rpkt.phys_srcs.push_back({static_cast<uint16_t>(m.speculativeRegister), src.type});
        }

        // Set producer scheduler hint for dependency-aware routing
        for (const auto& phys_src : rpkt.phys_srcs) {
            uint8_t prod_sched = mDownstream->getProducerScheduler(phys_src);
            if (prod_sched != 255) {
                rpkt.producer_scheduler = prod_sched;
                break;
            }
        }
        rpkt.scheduler = mDownstream->reserveSlot(rpkt);

        for (const auto& dst : dst_regs) {
            if (dst.type == core::RegType::Int && dst.number == 0) continue;

            auto& m = mPrf.mapping(dst.type, dst.number);
            uint16_t old_phys = static_cast<uint16_t>(m.speculativeRegister);
            uint16_t new_phys = static_cast<uint16_t>(mPrf.getFreeRegister(dst.type));

            rpkt.phys_dsts.push_back({new_phys, dst.type});
            old_phys_dsts.push_back({old_phys, dst.type});
            new_phys_dsts.push_back({new_phys, dst.type});

            m.speculativeRegister = new_phys;

            // Record the overwritten mapping so a flush can roll the RAT back to any tag.
            mRenameHistory.push_back({dp.pkt.fetch_seq, dst.type, dst.number, old_phys});
        }

        // Allocate checkpoint for branches
        if (need_checkpoint) {
            auto slot = mCheckpointMgr.allocate(dp.pkt.fetch_seq);
            if (slot) {
                mCheckpointMgr.saveRAT(*slot, mPrf);
                rpkt.checkpoint_slot = slot;
                ILOG("[rename] Allocated checkpoint slot=" << *slot << " for fetch_seq=" << dp.pkt.fetch_seq);
            }
        }

        if (mRob) {
            core::ROBEntry entry;
            entry.tag = dp.pkt.tag;
            entry.fetch_seq = dp.pkt.fetch_seq;
            entry.pc = dp.pkt.pc;
            entry.uop_type = dp.uop_type;
            entry.wrong_path_depth = dp.pkt.wrong_path_depth;  // Use packet's depth from fetch
            entry.old_phys_dsts = std::move(old_phys_dsts);
            entry.new_phys_dsts = std::move(new_phys_dsts);
            entry.dispatch_cycle = getClock()->currentCycle();
            entry.was_mispredicted = dp.was_mispredicted;
            rpkt.rob_token = mRob->allocate(std::move(entry));
        }

        if (mVis) mVis->onRename(dp.pkt.fetch_seq, dp.uop_type, getClock()->currentCycle());
        mRenamedBuf.push_back(std::move(rpkt));
        ++dispatched;
    }

    if (dispatched > 0) {
        out_port.send(mRenamedBuf, 1);
    }

    return dispatched;
}

uint32_t Rename::tickInorder_() {
    mIssuedBuf.clear();
    uint32_t dispatched = 0;
    mDispatchStall = DispatchStall::None;

    while (dispatched < mDispatchWidth && hasPendingPacket_()) {
        const auto* peek = peekPacket_();
        if (!peek) break;

        std::vector<core::RegOperand> src_regs;
        std::vector<core::RegOperand> dst_regs;
        extractRegOperands_(*peek, src_regs, dst_regs);

        if (!mDownstreamExecute->isReadyForType(peek->uop_type)) {
            mDispatchStall = DispatchStall::SchedulerFull;
            break;
        }
        if (!mArchScoreboard.sourcesReady(src_regs, getClock()->currentCycle())) {
            mDispatchStall = DispatchStall::OperandsNotReady;
            break;
        }
        if (mRob && !mRob->canAllocate()) {
            mDispatchStall = DispatchStall::RobFull;
            break;
        }

        // Stall dispatch when no LQ/SQ credit is available so memory-op occupancy stays bounded.
        const bool is_mem_load = (peek->uop_type == core::UopType::Load);
        const bool is_mem_store = (peek->uop_type == core::UopType::Store || peek->uop_type == core::UopType::Fence);
        if (mLsq && is_mem_load && !mLsq->canClaimLoad()) {
            mDispatchStall = DispatchStall::LoadQueueFull;
            break;
        }
        if (mLsq && is_mem_store && !mLsq->canClaimStore()) {
            mDispatchStall = DispatchStall::StoreQueueFull;
            break;
        }
        // Last check: it claims the issue port and divider.
        if (!mDownstream->issueInorder(*peek)) {
            mDispatchStall = DispatchStall::SchedulerFull;
            break;
        }

        // Pull the packet
        auto dp = pullPacket_();
        endBubble_(mRecoveryPending);

        mDownstreamExecute->reserveSlot(dp.uop_type);

        if (mLsq && (is_mem_load || is_mem_store)) mLsq->claim(dp);

        core::IssuePacket ipkt;
        ipkt.pkt = dp.pkt;
        // Keep packet's original wrong_path_depth from fetch
        ipkt.uop_type = dp.uop_type;
        ipkt.latency = dp.latency;
        ipkt.predicted_taken = dp.predicted_taken;
        ipkt.was_mispredicted = dp.was_mispredicted;
        ipkt.target_mispredicted = dp.target_mispredicted;

        for (const auto& src : src_regs) {
            if (core::isRegFileType(src.type)) ipkt.phys_srcs.push_back({src.number, src.type});
        }

        // As in Issue: fixed-latency results reach a consumer dispatched `latency` cycles later on the
        // bypass; loads and atomics wake when their data is known.
        const bool waits_for_data = ipkt.uop_type == core::UopType::Load || ipkt.pkt.inst_class == cpu::InstClass::Atomic;
        const uint64_t ready_cycle = waits_for_data ? ArchitecturalScoreboard::kNever : getClock()->currentCycle() + ipkt.latency;
        for (const auto& dst : dst_regs) {
            if (!core::isRegFileType(dst.type)) continue;
            if (dst.type == core::RegType::Int && dst.number == 0) continue;
            ipkt.phys_dsts.push_back({dst.number, dst.type, ipkt.pkt.tag});
            mArchScoreboard.markBusy(dst.number, dst.type, ipkt.pkt.tag, ready_cycle);
        }

        if (mRob) {
            core::ROBEntry entry;
            entry.tag = dp.pkt.tag;
            entry.fetch_seq = dp.pkt.fetch_seq;
            entry.pc = dp.pkt.pc;
            entry.uop_type = dp.uop_type;
            entry.wrong_path_depth = dp.pkt.wrong_path_depth;  // Use packet's depth from fetch
            entry.dispatch_cycle = getClock()->currentCycle();
            entry.was_mispredicted = dp.was_mispredicted;
            ipkt.rob_token = mRob->allocate(std::move(entry));
        }

        if (mVis) mVis->onRename(dp.pkt.fetch_seq, dp.uop_type, getClock()->currentCycle());
        mIssuedBuf.push_back(std::move(ipkt));
        ++dispatched;
    }

    if (dispatched > 0) {
        inorder_out.send(mIssuedBuf, 1);
    }

    return dispatched;
}

void Rename::wakeOnLoadData(const std::vector<core::PhysRegRef>& dsts, uint64_t data_cycle) {
    const uint64_t dispatch_cycle = data_cycle > 0 ? data_cycle - 1 : 0;
    for (const auto& ref : dsts) mArchScoreboard.readyAt(static_cast<uint8_t>(ref.phys_reg), ref.type, ref.tag, dispatch_cycle);
}

void Rename::receiveCommits_(const std::vector<core::PhysRegRef>& old_dsts) {
    for (const auto& ref : old_dsts) {
        mPrf.releaseRegister(ref.phys_reg, ref.type);
    }
    pruneCommittedHistory_();
}

}  // namespace midcore
