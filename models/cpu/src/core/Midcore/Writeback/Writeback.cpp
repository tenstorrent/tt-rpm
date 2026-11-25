// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Tenstorrent USA, Inc.

#include "Midcore/Writeback/Writeback.hpp"

#include <iostream>

#include "LoadStoreUnit/LSQ/LSQ.hpp"
#include "Logging.hpp"
#include "models/cpu/common/ExecutionDriver.hpp"
#include "models/cpu/common/whisper_include_fix.hpp"

namespace midcore {

Writeback::Writeback(sparta::TreeNode* node, const WritebackParams* params)
    : sparta::Unit(node),
      mRetireWidth(params->retire_width),
      mRob(params->rob_capacity),
      mRetireStallL1MissAge(params->retire_stall_l1_miss_age),
      mRetireStallL2MissAge(params->retire_stall_l2_miss_age),
      mRetireTimeoutCycles(params->retire_timeout_cycles),
      mNumRetired(&unit_stat_set_, "num_retired", "Total instructions retired", sparta::Counter::COUNT_NORMAL),
      mNumSquashed(&unit_stat_set_, "num_squashed", "Total instructions squashed", sparta::Counter::COUNT_NORMAL),
      mNumRetiredBranches(&unit_stat_set_, "num_retired_branches", "Branches and jumps retired", sparta::Counter::COUNT_NORMAL),
      mNumRetiredMispredicts(&unit_stat_set_, "num_retired_mispredicts", "Retired branches and jumps that were mispredicted", sparta::Counter::COUNT_NORMAL),
      mRetireStallLoadAny(&unit_stat_set_, "retire_stall_load_any", "Cycles with no retirement while the oldest uop is an unfinished load",
                          sparta::Counter::COUNT_NORMAL),
      mRetireStallLoadL1Miss(&unit_stat_set_, "retire_stall_load_l1_miss",
                             "retire_stall_load_any cycles where that load is older than retire_stall_l1_miss_age", sparta::Counter::COUNT_NORMAL),
      mRetireStallLoadL2Miss(&unit_stat_set_, "retire_stall_load_l2_miss",
                             "retire_stall_load_any cycles where that load is older than retire_stall_l2_miss_age", sparta::Counter::COUNT_NORMAL),
      mNumMemOrderFlushes(&unit_stat_set_, "num_mem_order_flushes", "Replays from a load that violated memory ordering", sparta::Counter::COUNT_NORMAL) {
    rob_complete_exe_in.registerConsumerHandler(CREATE_SPARTA_HANDLER_WITH_DATA(Writeback, receiveCompletions_, std::vector<core::ROBToken>));
    rob_complete_lsq_in.registerConsumerHandler(CREATE_SPARTA_HANDLER_WITH_DATA(Writeback, receiveCompletions_, std::vector<core::ROBToken>));
    flush_in.registerConsumerHandler(CREATE_SPARTA_HANDLER_WITH_DATA(Writeback, receiveFlush_, core::FlushRequest));
    branch_resolved_in.registerConsumerHandler(CREATE_SPARTA_HANDLER_WITH_DATA(Writeback, receiveBranchResolved_, core::BranchResolved));
    mWatchdogEvent.setContinuing(false);
    sparta_assert(params->rob_capacity > 0 && params->retire_width > 0, "writeback.params.rob_capacity and retire_width must be at least 1");
}

void Writeback::receiveCompletions_(const std::vector<core::ROBToken>& tokens) {
    for (const auto& tok : tokens) {
        mRob.markComplete(tok);
    }
}

void Writeback::receiveFlush_(const core::FlushRequest& req) {
    uint64_t cycle = getClock()->currentCycle();
    ILOG("[writeback] cycle " << cycle << " RECEIVED flush: branch_tag=" << req.branch_tag << " branch_depth=" << static_cast<int>(req.branch_depth)
                              << " source=" << static_cast<int>(req.source) << " ROB_size_before=" << mRob.size());

    // Collect fetch_seqs to squash
    std::vector<uint64_t> fetch_seqs_to_squash;
    for (uint32_t i = 0; i < mRob.size(); ++i) {
        uint32_t idx = (mRob.headIndex() + i) % mRob.capacity();
        const auto& entry = mRob.entryAt(idx);
        if (core::shouldSquash(entry.tag, entry.wrong_path_depth, req)) {
            fetch_seqs_to_squash.push_back(entry.fetch_seq);
            ILOG("[writeback] TRACE squash entry tag=" << entry.tag << " depth=" << static_cast<int>(entry.wrong_path_depth)
                                                       << " (branch_tag=" << req.branch_tag << " branch_depth=" << static_cast<int>(req.branch_depth) << ")");
        } else {
            ILOG("[writeback] TRACE keep   entry tag=" << entry.tag << " depth=" << static_cast<int>(entry.wrong_path_depth));
        }
    }

    // Squash ROB entries based on shouldSquash logic
    auto freed_regs = mRob.squashFrom(req);
    uint64_t num_squashed = fetch_seqs_to_squash.size();

    // Notify visualizer of squashed instructions
    for (uint64_t fetch_seq : fetch_seqs_to_squash) {
        if (mVis) mVis->onSquash(fetch_seq, cycle);
    }

    ILOG("[writeback] cycle " << cycle << " Squashed from ROB, freed " << freed_regs.size() << " regs, ROB_size_after=" << mRob.size());

    mNumSquashed += num_squashed;

    // Send freed registers to Rename for release back to free list
    if (!freed_regs.empty()) {
        freed_regs_out.send(freed_regs, 0);
    }

    ILOG("[writeback] Flush complete. ROB size: " << mRob.size());
}

void Writeback::receiveBranchResolved_(const core::BranchResolved& resolved) {
    // When a branch at depth D resolves correctly, all younger ROB entries
    // at depth > D should have their depth decremented by 1.
    // This represents the resolved speculation: those instructions are no
    // longer speculative relative to this branch.

    uint64_t cycle = getClock()->currentCycle();
    uint32_t num_decremented = 0;

    // A correctly resolved branch is never a flush target again (traps name the last retired
    // instruction and restore by walk-back), so its checkpoint frees now, as BOOM frees a branch tag.
    checkpoint_release_out.send(resolved.branch_fetch_seq, 0);

    for (uint32_t i = 0; i < mRob.size(); ++i) {
        uint32_t idx = (mRob.headIndex() + i) % mRob.capacity();
        auto& entry = mRob.entryAt(idx);

        // Only decrement entries younger than the resolved branch and at depth greater than
        // the resolved branch's depth. Use fetch_seq for "younger" (monotonic, never reused);
        // entry.tag is a reused model tag, so a tag comparison mis-selects entries after a
        // flush and corrupts depth tracking.
        bool younger = (resolved.branch_fetch_seq != 0) ? (entry.fetch_seq > resolved.branch_fetch_seq) : (entry.tag > resolved.branch_tag);
        if (younger && entry.wrong_path_depth > resolved.resolved_depth) {
            ILOG("[writeback] TRACE decrement tag=" << entry.tag << " depth " << static_cast<int>(entry.wrong_path_depth) << "->"
                                                    << static_cast<int>(entry.wrong_path_depth - 1) << " (resolved branch_tag=" << resolved.branch_tag
                                                    << " resolved_depth=" << static_cast<int>(resolved.resolved_depth) << ")");
            --entry.wrong_path_depth;
            ++num_decremented;
        }
    }

    if (num_decremented > 0) {
        ILOG("[writeback] cycle " << cycle << " BRANCH RESOLVED: tag=" << resolved.branch_tag << " depth=" << static_cast<int>(resolved.resolved_depth)
                                  << " decremented depth of " << num_decremented << " ROB entries");
    }
}

void Writeback::tick() {
    if (!mWatchdogStarted) {
        scheduleWatchdog();
        mWatchdogStarted = true;
    }
    if (mRob.empty()) return;

    uint32_t retired_this_cycle = 0;
    mOldDstsBuf.clear();

    while (retired_this_cycle < mRetireWidth && mRob.headCompleted()) {
        auto& entry = mRob.head();

        // Trap/interrupt redirect detection. Whisper's architectural PC is the PC
        // of the next instruction it will commit. For a correct-path ROB head this
        // must equal entry.pc. If it doesn't, Whisper took an unmodeled trap or
        // interrupt (e.g. a timer interrupt) at a prior retire and diverged to a
        // handler the model never fetched -- the speculatively-fetched stream from
        // here on is wrong. Squash everything fetched after the last committed
        // instruction and redirect Fetch to the handler. Reuses the Execute flush
        // path (FlushArbiter::receiveExeFlush_) which flushes all stages and
        // redirects Fetch to target_pc.
        if (mDriver && mLastRetiredTagValid && entry.wrong_path_depth == 0 && entry.pc != mDriver->expectedRetirePc()) {
            if (mTrapFlushSentTag != entry.tag) {
                core::FlushRequest req;
                req.source = core::FlushSource::Execute;
                req.branch_tag = mLastRetiredTag;
                req.branch_fetch_seq = mLastRetiredFetchSeq;
                req.branch_pc = mLastRetiredPc;
                req.target_pc = mDriver->expectedRetirePc();
                req.branch_depth = 0;
                req.authoritative = true;  // from the committed ROB head; never stale
                trap_flush_out.send(req, 0);
                mTrapFlushSentTag = entry.tag;
                ILOG("[writeback] TRAP/INTERRUPT redirect: head tag=" << entry.tag << " pc=0x" << std::hex << entry.pc << " != whisper pc=0x" << req.target_pc
                                                                      << std::dec << " -- squashing fetch_seq>" << mLastRetiredFetchSeq
                                                                      << " and redirecting fetch to handler");
            }
            break;
        }

        // Memory-ordering violation: replay from the load, like a trap at the ROB head.
        if (entry.uop_type == core::UopType::Load && mLsq && mLastRetiredTagValid && mLsq->hasOrderingViolation(entry.tag)) {
            if (mOrderFlushSentSeq != entry.fetch_seq) {
                core::FlushRequest req;
                req.source = core::FlushSource::Execute;
                req.branch_tag = mLastRetiredTag;
                req.branch_fetch_seq = mLastRetiredFetchSeq;
                req.branch_pc = mLastRetiredPc;
                req.target_pc = entry.pc;
                req.branch_depth = 0;
                req.authoritative = true;  // from the ROB head; never stale
                trap_flush_out.send(req, 0);
                mOrderFlushSentSeq = entry.fetch_seq;
                ++mNumMemOrderFlushes;
                ILOG("[writeback] memory-order replay from load tag=" << entry.tag << " pc=0x" << std::hex << entry.pc << std::dec);
            }
            break;
        }

        // TRACE: detect a retire-order jump (ROB head tag not contiguous with last retired)
        if (mLastRetiredTagValid && entry.tag != mLastRetiredTag + 1) {
            ILOG("[writeback] TRACE retire-jump: head tag=" << entry.tag << " depth=" << static_cast<int>(entry.wrong_path_depth)
                                                            << " but last retired tag=" << mLastRetiredTag << " (gap!) -- dumping ROB:");
            for (uint32_t i = 0; i < mRob.size(); ++i) {
                uint32_t idx = (mRob.headIndex() + i) % mRob.capacity();
                const auto& e = mRob.entryAt(idx);
                ILOG("[writeback] TRACE   rob[" << i << "] tag=" << e.tag << " depth=" << static_cast<int>(e.wrong_path_depth) << " completed=" << e.completed
                                                << " pc=0x" << std::hex << e.pc << std::dec);
            }
        }
        // Verify wrong-path instructions never reach ROB head
        // They should have been squashed before getting here
        if (entry.wrong_path_depth > 0 && mSpecCfg->enabled) {
            sparta_assert(false, "Wrong-path instruction at ROB head! tag=" << entry.tag << " pc=0x" << std::hex << entry.pc << std::dec
                                                                            << " depth=" << static_cast<int>(entry.wrong_path_depth)
                                                                            << " - indicates missing flush or incorrect depth tracking");
        }

        // Retire via the driver first -- it decides whether this instruction actually
        // commits. On an async interrupt the driver takes the interrupt, leaves the
        // instruction uncommitted, and returns false: keep it at the ROB head so the
        // trap/interrupt redirect (top of loop) fires next cycle with branch_tag =
        // mLastRetiredTag (the last committed instruction), flushing this instruction
        // inclusive and redirecting fetch to the handler. Do not advance the last-retired
        // markers, commit its store, or count it.
        if (mDriver && !mDriver->retireInstruction(entry.tag)) {
            ILOG("[writeback] cycle " << getClock()->currentCycle() << " tag=" << entry.tag
                                      << " preempted by async interrupt; leaving at ROB head for redirect");
            break;
        }

        mLastRetiredTag = entry.tag;
        mLastRetiredFetchSeq = entry.fetch_seq;
        mLastRetiredPc = entry.pc;
        mLastRetiredTagValid = true;

        // For stores, tell LSQ to mark them as committed so they can drain
        if ((entry.uop_type == core::UopType::Store || entry.uop_type == core::UopType::Fence) && mLsq) {
            mLsq->commitStore(entry.tag);
        } else if (entry.uop_type == core::UopType::Load && mLsq) {
            mLsq->commitLoad(entry.tag);
        }

        for (const auto& ref : entry.old_phys_dsts) {
            mOldDstsBuf.push_back(ref);
        }

        // Commit-time training: tell the branch predictor this branch retired.
        if (entry.uop_type == core::UopType::Branch) {
            branch_retire_out.send(entry.fetch_seq, 0);
            ++mNumRetiredBranches;
            if (entry.was_mispredicted) ++mNumRetiredMispredicts;
        }

        ILOG("[writeback] cycle " << getClock()->currentCycle() << " retired tag=" << entry.tag << " pc=0x" << std::hex << entry.pc << std::dec);

        if (mVis) mVis->onWriteback(entry.fetch_seq, getClock()->currentCycle());
        ++mNumRetired;
        ++retired_this_cycle;
        mRob.commitHead();

        // The scheduler is shared, so this stops every core in the cluster at this tick.
        const bool program_ended = mDriver && mDriver->programEnded();
        if (program_ended || (mInstructionLimit > 0 && mNumRetired.get() >= mInstructionLimit)) {
            ILOG("[writeback] stopping simulation: " << (program_ended ? "end of program" : "instruction limit reached"));
            // Flush old_dsts before stopping so PRF free list stays consistent
            if (!mOldDstsBuf.empty()) {
                commit_out.send(mOldDstsBuf, 1);
            }
            getScheduler()->stopRunning();
            return;
        }
    }

    if (retired_this_cycle == 0 && !mRob.empty()) {
        const auto& head = mRob.head();
        if (!head.completed && head.uop_type == core::UopType::Load) {
            const uint64_t age = getClock()->currentCycle() - head.dispatch_cycle;
            ++mRetireStallLoadAny;
            if (age > mRetireStallL1MissAge) ++mRetireStallLoadL1Miss;
            if (age > mRetireStallL2MissAge) ++mRetireStallLoadL2Miss;
        }
    }

    if (!mOldDstsBuf.empty()) {
        commit_out.send(mOldDstsBuf, 1);
    }

    // Program finished naturally: whisper has no more instructions and the ROB
    // has drained. Stop the scheduler so the watchdog doesn't fire on the idle
    // tail of a short program.
    if (retired_this_cycle > 0 && mRob.empty() && mDriver && mDriver->isFinished()) {
        ILOG("[writeback] program finished, stopping simulation");
        getScheduler()->stopRunning();
    }
}

void Writeback::scheduleWatchdog() { mWatchdogEvent.preparePayload(mNumRetired.get())->schedule(mRetireTimeoutCycles); }

void Writeback::checkForwardProgress(const uint64_t& last_retired_count) {
    if (last_retired_count == mNumRetired.get()) {
        const uint64_t cycle = getClock()->currentCycle();
        std::cerr << "[watchdog] ROB stall: no instruction retired in " << mRetireTimeoutCycles << " cycles (cycle " << cycle << ")\n"
                  << "  total retired=" << mNumRetired.get() << "  ROB occupancy=" << mRob.size() << "/" << mRob.capacity() << "\n";
        // Driver tag-space snapshot: head.pc != expectedRetirePc, or a gap between the
        // fetch and retire counters, is the fingerprint of a fetch/retire desync.
        if (mDriver) {
            std::cerr << "  driver: expectedRetirePc=0x" << std::hex << mDriver->expectedRetirePc() << std::dec
                      << "  fetchSeq(mSequence)=" << mDriver->getFetchSequence() << "  retireSeq=" << mDriver->getInstructionCount()
                      << "  invalidState=" << mDriver->isDriverInInvalidState() << "\n";
        }
        if (mRob.empty()) {
            sparta_assert(false, "ROB timeout: ROB is empty — frontend failed to deliver instructions (see [watchdog] diagnostics above)");
        }

        const auto& oldest = mRob.head();
        const bool pc_desync = mDriver && !oldest.completed && oldest.pc != mDriver->expectedRetirePc();
        std::cerr << "  STUCK head: tag=" << oldest.tag << " fetch_seq=" << oldest.fetch_seq << " pc=0x" << std::hex << oldest.pc << std::dec
                  << " uop_type=" << static_cast<int>(oldest.uop_type) << " completed=" << oldest.completed
                  << " wrong_path_depth=" << static_cast<int>(oldest.wrong_path_depth) << " epoch=" << oldest.epoch << "\n";
        if (pc_desync) {
            std::cerr << "  *** TAG DESYNC: head pc 0x" << std::hex << oldest.pc << " != driver expectedRetirePc 0x" << mDriver->expectedRetirePc() << std::dec
                      << " -- the ROB and driver disagree on what this tag is.\n"
                      << "      Likely an unhandled retire-time redirect (trap/interrupt) or a squash that did not rewind the driver.\n";
        }
        if (!oldest.completed) {
            const bool is_mem = oldest.uop_type == core::UopType::Load || oldest.uop_type == core::UopType::Store;
            std::cerr << "      head never completed -> "
                      << (is_mem ? "LSU/cache never returned, or a lost wakeup on a reused tag (mem op)."
                                 : "execution never produced a result, or a lost scheduler wakeup (reused-tag hazard).")
                      << "\n";
        } else {
            std::cerr << "      head is COMPLETED but not retiring -> retire/commit logic or driver retire() is refusing it.\n";
        }
        // Dump the ROB (oldest first) so the wrong-path / uncompleted tail is visible.
        const uint32_t dump = std::min<uint32_t>(mRob.size(), 32);
        std::cerr << "  ROB (oldest first, up to " << dump << " of " << mRob.size() << "):\n";
        for (uint32_t i = 0; i < dump; ++i) {
            const auto& e = mRob.entryAt((mRob.headIndex() + i) % mRob.capacity());
            std::cerr << "    [" << i << "] tag=" << e.tag << " fetch_seq=" << e.fetch_seq << " pc=0x" << std::hex << e.pc << std::dec
                      << " uop=" << static_cast<int>(e.uop_type) << " completed=" << e.completed << " depth=" << static_cast<int>(e.wrong_path_depth) << "\n";
        }
        sparta_assert(false, "ROB timeout: oldest instruction stuck (tag=" << oldest.tag << " pc=0x" << std::hex << oldest.pc << std::dec
                                                                           << " completed=" << oldest.completed << ") — see [watchdog] diagnostics above");
    }
    scheduleWatchdog();
}

}  // namespace midcore
