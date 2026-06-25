#include "Midcore/Writeback/Writeback.hpp"

#include <iostream>

#include "Common/SpeculationConfig.hpp"
#include "LoadStoreUnit/LSQ/LSQ.hpp"
#include "Logging.hpp"
#include "Midcore/Rename/Rename.hpp"
#include "models/cpu/common/ExecutionDriver.hpp"
#include "models/cpu/common/whisper_include_fix.hpp"

namespace midcore {

Writeback::Writeback(sparta::TreeNode* node, const WritebackParams* params)
    : sparta::Unit(node),
      mRetireWidth(params->retire_width),
      mRob(params->rob_capacity),
      mLogEnabled(params->log_enabled),
      mRetireTimeoutCycles(params->retire_timeout_cycles),
      mNumRetired(&unit_stat_set_, "num_retired", "Total instructions retired", sparta::Counter::COUNT_NORMAL),
      mNumSquashed(&unit_stat_set_, "num_squashed", "Total instructions squashed", sparta::Counter::COUNT_NORMAL) {
    rob_complete_exe_in.registerConsumerHandler(CREATE_SPARTA_HANDLER_WITH_DATA(Writeback, receiveCompletions_, std::vector<core::ROBToken>));
    rob_complete_lsq_in.registerConsumerHandler(CREATE_SPARTA_HANDLER_WITH_DATA(Writeback, receiveCompletions_, std::vector<core::ROBToken>));
    flush_in.registerConsumerHandler(CREATE_SPARTA_HANDLER_WITH_DATA(Writeback, receiveFlush_, core::FlushRequest));
    branch_resolved_in.registerConsumerHandler(CREATE_SPARTA_HANDLER_WITH_DATA(Writeback, receiveBranchResolved_, core::BranchResolved));
    mWatchdogEvent.setContinuing(false);
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

    // Send freed registers to Rename for release back to free list
    if (!freed_regs.empty()) {
        freed_regs_out.send(freed_regs, 0);
        mNumSquashed += num_squashed;
    }

    // If the branch has a checkpoint, release it after restoration
    // (Rename handles the restore, we just need to signal the release)
    auto* branch_entry = mRob.findByTag(req.branch_tag);
    if (branch_entry && branch_entry->checkpoint_slot.has_value()) {
        checkpoint_release_out.send(req.branch_tag, 0);
        ILOG("[writeback] Signaled checkpoint release for branch_tag=" << req.branch_tag);
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
                trap_flush_out.send(req, 0);
                mTrapFlushSentTag = entry.tag;
                ILOG("[writeback] TRAP/INTERRUPT redirect: head tag=" << entry.tag << " pc=0x" << std::hex << entry.pc << " != whisper pc=0x" << req.target_pc
                                                                      << std::dec << " -- squashing fetch_seq>" << mLastRetiredFetchSeq
                                                                      << " and redirecting fetch to handler");
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
        mLastRetiredTag = entry.tag;
        mLastRetiredFetchSeq = entry.fetch_seq;
        mLastRetiredPc = entry.pc;
        mLastRetiredTagValid = true;

        // Verify wrong-path instructions never reach ROB head
        // They should have been squashed before getting here
        if (entry.wrong_path_depth > 0 && core::getSpeculationConfig().enabled) {
            sparta_assert(false, "Wrong-path instruction at ROB head! tag=" << entry.tag << " pc=0x" << std::hex << entry.pc << std::dec
                                                                            << " depth=" << static_cast<int>(entry.wrong_path_depth)
                                                                            << " - indicates missing flush or incorrect depth tracking");
        }

        // For stores, tell LSQ to mark them as committed so they can drain
        if (entry.uop_type == core::UopType::Store && mLsq) {
            mLsq->commitStore(entry.tag);
        }

        if (mDriver) {
            mDriver->retireInstruction(entry.tag);
        }

        for (const auto& ref : entry.old_phys_dsts) {
            mOldDstsBuf.push_back(ref);
        }

        // Release checkpoint if this was a branch that retired successfully
        if (entry.checkpoint_slot.has_value()) {
            checkpoint_release_out.send(entry.tag, 0);
            ILOG("[writeback] Branch retired successfully, releasing checkpoint for tag=" << entry.tag);
        }

        ILOG("[writeback] cycle " << getClock()->currentCycle() << " retired tag=" << entry.tag << " pc=0x" << std::hex << entry.pc << std::dec);

        if (mVis) mVis->onWriteback(entry.fetch_seq, getClock()->currentCycle());
        ++mNumRetired;
        ++retired_this_cycle;
        mRob.commitHead();

        if (mInstructionLimit > 0 && mNumRetired.get() >= mInstructionLimit) {
            ILOG("[writeback] instruction limit reached (" << mInstructionLimit << "), stopping simulation");
            // Flush old_dsts before stopping so PRF free list stays consistent
            if (!mOldDstsBuf.empty()) {
                commit_out.send(mOldDstsBuf, 1);
            }
            getScheduler()->stopRunning();
            return;
        }
    }

    if (retired_this_cycle > 0) {
        mDbgStallCycles = 0;
    } else if (!mRob.empty()) {
        ++mDbgStallCycles;
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
        ILOG("[watchdog] ROB stall detected at cycle " << getClock()->currentCycle());
        ILOG("  No instructions retired in the last " << mRetireTimeoutCycles << " cycles");
        ILOG("  Total retired: " << mNumRetired.get() << "  ROB occupancy: " << mRob.size() << "/" << mRob.capacity());
        if (mRob.empty()) {
            sparta_assert(false, "ROB timeout: ROB is empty — frontend failed to deliver instructions");
        } else {
            auto& oldest = mRob.head();
            sparta_assert(false, "ROB timeout: oldest instruction stuck (tag=" << oldest.tag << " pc=0x" << std::hex << oldest.pc << std::dec
                                                                               << " completed=" << oldest.completed << ")");
        }
    }
    scheduleWatchdog();
}

}  // namespace midcore
