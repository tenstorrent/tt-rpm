#include "Midcore/Rename/Rename.hpp"

#include <iostream>

#include "Common/SpeculationConfig.hpp"
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
      mOooEnabled(params->ooo_enabled),
      mDispatchWidth(params->dispatch_width),
      mLogEnabled(params->log_enabled),
      mNumDispatched(&unit_stat_set_, "num_dispatched", "Total instructions dispatched", sparta::Counter::COUNT_NORMAL),
      mNumPrfStallCycles(&unit_stat_set_, "num_prf_stall_cycles", "Cycles stalled on register hazards", sparta::Counter::COUNT_NORMAL),
      mNumCheckpointStallCycles(&unit_stat_set_, "num_checkpoint_stall_cycles", "Cycles stalled on checkpoint hazards", sparta::Counter::COUNT_NORMAL) {
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
    (void)static_cast<uint32_t>(params->regfile_writes_per_bank);
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

    // Restore RAT state from checkpoint
    if (mCheckpointMgr.hasCheckpoint(req.branch_tag)) {
        bool restored = mCheckpointMgr.restore(req.branch_tag, mPrf);
        if (restored) {
            ILOG("[rename] Restored RAT from checkpoint for tag=" << req.branch_tag);
        }
    } else {
        ILOG("[rename] No checkpoint found for branch_tag=" << req.branch_tag);
    }

    // Release all checkpoints for younger branches
    mCheckpointMgr.releaseYoungerThan(req.branch_tag);

    // Also release the mispredicted branch's checkpoint itself
    // (it's no longer needed after restoration)
    mCheckpointMgr.release(req.branch_tag);

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

void Rename::receiveFreedRegs_(const std::vector<core::PhysRegRef>& regs) {
    for (const auto& ref : regs) {
        mPrf.releaseRegisterSafe(ref.phys_reg, ref.type);
    }
    ILOG("[rename] Received " << regs.size() << " freed registers from ROB squash");
}

void Rename::receiveCheckpointRelease_(const uint64_t& branch_tag) {
    mCheckpointMgr.release(branch_tag);
    ILOG("[rename] Released checkpoint for branch_tag=" << branch_tag << ", remaining=" << mCheckpointMgr.numActive());
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
    if (!mDecodeQueue || mDecodeQueue->isEmpty()) return false;
    return true;
}

void Rename::tick() {
    if (mDirectMode) {
        if (mPendingPackets.empty()) return;
    } else {
        if (!mDecodeQueue || mDecodeQueue->isEmpty()) return;
    }

    uint32_t dispatched = 0;
    if (mOooEnabled) {
        dispatched = tickOoo_();
    } else {
        dispatched = tickInorder_();
    }

    if (dispatched == 0) {
        ++mNumPrfStallCycles;
    } else {
        mNumDispatched += dispatched;
    }
}

uint32_t Rename::tickOoo_() {
    mRenamedBuf.clear();
    uint32_t dispatched = 0;

    auto hasPackets = [this]() { return mDirectMode ? !mPendingPackets.empty() : !mDecodeQueue->isEmpty(); };

    auto peekPacket = [this]() -> const core::DecodePacket* {
        if (mDirectMode) {
            return mPendingPackets.empty() ? nullptr : &mPendingPackets.front();
        }
        return mDecodeQueue->peek();
    };

    auto pullPacket = [this]() -> core::DecodePacket {
        if (mDirectMode) {
            auto dp = mPendingPackets.front();
            mPendingPackets.erase(mPendingPackets.begin());
            return dp;
        }
        return mDecodeQueue->pullOne();
    };

    while (dispatched < mDispatchWidth && hasPackets()) {
        const auto* peek = peekPacket();
        if (!peek) break;

        // Extract operand info from the instruction
        std::vector<core::RegOperand> src_regs;
        std::vector<core::RegOperand> dst_regs;
        const auto& rec = peek->pkt.inst->getTraceRecord();
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

        if (!mPrf.canAllocate(dst_regs)) break;
        if (mRob && !mRob->canAllocate()) break;
        if (!mDownstream->isReadyForType(peek->uop_type)) break;

        // Stall dispatch when no LQ/SQ credit is available so memory-op occupancy stays bounded.
        const bool is_mem_load = (peek->uop_type == core::UopType::Load);
        const bool is_mem_store = (peek->uop_type == core::UopType::Store);
        if (mLsq && is_mem_load && !mLsq->canClaimLoad()) break;
        if (mLsq && is_mem_store && !mLsq->canClaimStore()) break;

        // Check if branch needs checkpoint and if we have capacity
        bool is_branch = (peek->uop_type == core::UopType::Branch);
        bool need_checkpoint = is_branch && core::getSpeculationConfig().enabled;
        if (need_checkpoint && !mCheckpointMgr.canAllocate()) {
            ++mNumCheckpointStallCycles;
            break;
        }

        // Pull the packet
        auto dp = pullPacket();

        // Reserve slot to prevent over-dispatching in same cycle
        mDownstream->reserveSlot(dp.uop_type);

        // Claim the LSQ credit now that the uop is committed to dispatch.
        if (mLsq && is_mem_load)
            mLsq->claimLoad(dp.pkt.fetch_seq, dp.pkt.tag, dp.pkt.wrong_path_depth);
        else if (mLsq && is_mem_store)
            mLsq->claimStore(dp.pkt.fetch_seq, dp.pkt.tag, dp.pkt.wrong_path_depth);

        core::RenamedPacket rpkt;
        rpkt.pkt = dp.pkt;
        // Keep packet's original wrong_path_depth from fetch (don't overwrite)
        rpkt.uop_type = dp.uop_type;
        rpkt.latency = dp.latency;
        rpkt.predicted_taken = dp.predicted_taken;
        rpkt.was_mispredicted = dp.was_mispredicted;

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

        for (const auto& dst : dst_regs) {
            if (dst.type == core::RegType::Int && dst.number == 0) continue;

            auto& m = mPrf.mapping(dst.type, dst.number);
            uint16_t old_phys = static_cast<uint16_t>(m.speculativeRegister);
            uint16_t new_phys = static_cast<uint16_t>(mPrf.getFreeRegister(dst.type));

            rpkt.phys_dsts.push_back({new_phys, dst.type});
            old_phys_dsts.push_back({old_phys, dst.type});
            new_phys_dsts.push_back({new_phys, dst.type});

            m.speculativeRegister = new_phys;
        }

        // Allocate checkpoint for branches
        if (need_checkpoint) {
            auto slot = mCheckpointMgr.allocate(dp.pkt.tag);
            if (slot) {
                mCheckpointMgr.saveRAT(*slot, mPrf);
                rpkt.checkpoint_slot = slot;
                ILOG("[rename] Allocated checkpoint slot=" << *slot << " for branch_tag=" << dp.pkt.tag);
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
            entry.checkpoint_slot = rpkt.checkpoint_slot;
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

    auto hasPackets = [this]() { return mDirectMode ? !mPendingPackets.empty() : !mDecodeQueue->isEmpty(); };

    auto peekPacket = [this]() -> const core::DecodePacket* {
        if (mDirectMode) {
            return mPendingPackets.empty() ? nullptr : &mPendingPackets.front();
        }
        return mDecodeQueue->peek();
    };

    auto pullPacket = [this]() -> core::DecodePacket {
        if (mDirectMode) {
            auto dp = mPendingPackets.front();
            mPendingPackets.erase(mPendingPackets.begin());
            return dp;
        }
        return mDecodeQueue->pullOne();
    };

    while (dispatched < mDispatchWidth && hasPackets()) {
        const auto* peek = peekPacket();
        if (!peek) break;

        // Extract operand info from the instruction
        std::vector<core::RegOperand> src_regs;
        std::vector<core::RegOperand> dst_regs;
        const auto& rec = peek->pkt.inst->getTraceRecord();
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

        if (!mDownstreamExecute->isReadyForType(peek->uop_type)) break;
        if (!mArchScoreboard.sourcesReady(src_regs)) break;
        if (mRob && !mRob->canAllocate()) break;

        // Stall dispatch when no LQ/SQ credit is available so memory-op occupancy stays bounded.
        const bool is_mem_load = (peek->uop_type == core::UopType::Load);
        const bool is_mem_store = (peek->uop_type == core::UopType::Store);
        if (mLsq && is_mem_load && !mLsq->canClaimLoad()) break;
        if (mLsq && is_mem_store && !mLsq->canClaimStore()) break;

        // Pull the packet
        auto dp = pullPacket();

        mDownstreamExecute->reserveSlot(dp.uop_type);

        if (mLsq && is_mem_load)
            mLsq->claimLoad(dp.pkt.fetch_seq, dp.pkt.tag, dp.pkt.wrong_path_depth);
        else if (mLsq && is_mem_store)
            mLsq->claimStore(dp.pkt.fetch_seq, dp.pkt.tag, dp.pkt.wrong_path_depth);

        core::IssuePacket ipkt;
        ipkt.pkt = dp.pkt;
        // Keep packet's original wrong_path_depth from fetch
        ipkt.uop_type = dp.uop_type;
        ipkt.latency = dp.latency;
        ipkt.predicted_taken = dp.predicted_taken;
        ipkt.was_mispredicted = dp.was_mispredicted;

        for (const auto& src : src_regs) {
            if (core::isRegFileType(src.type)) ipkt.phys_srcs.push_back({src.number, src.type});
        }

        for (const auto& dst : dst_regs) {
            if (!core::isRegFileType(dst.type)) continue;
            if (dst.type == core::RegType::Int && dst.number == 0) continue;
            ipkt.phys_dsts.push_back({dst.number, dst.type, ipkt.pkt.tag});
            mArchScoreboard.markBusy(dst.number, dst.type, ipkt.pkt.tag);
        }

        if (mRob) {
            core::ROBEntry entry;
            entry.tag = dp.pkt.tag;
            entry.fetch_seq = dp.pkt.fetch_seq;
            entry.pc = dp.pkt.pc;
            entry.uop_type = dp.uop_type;
            entry.wrong_path_depth = dp.pkt.wrong_path_depth;  // Use packet's depth from fetch
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

void Rename::receiveCommits_(const std::vector<core::PhysRegRef>& old_dsts) {
    for (const auto& ref : old_dsts) {
        mPrf.releaseRegister(ref.phys_reg, ref.type);
    }
}

}  // namespace midcore
