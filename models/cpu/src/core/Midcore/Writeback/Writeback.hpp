// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Tenstorrent USA, Inc.

#pragma once

#include <cassert>
#include <vector>

#include "sparta/events/PayloadEvent.hpp"
#include "sparta/ports/DataPort.hpp"
#include "sparta/simulation/ParameterSet.hpp"
#include "sparta/simulation/Unit.hpp"
#include "sparta/statistics/Counter.hpp"

#include "Common/PipelinePacket.hpp"
#include "Common/PipelineVisualizer.hpp"
#include "Common/SpeculationConfig.hpp"
#include "Common/SpeculationUtils.hpp"

namespace cpu {
class ExecutionDriver;
}

namespace midcore {

class LSQ;

class ReorderBuffer {
    std::vector<core::ROBEntry> mEntries;
    std::vector<uint32_t> mSlotEpoch;
    uint32_t mHead{0};
    uint32_t mTail{0};
    uint32_t mSize{0};
    uint32_t mCapacity;

   public:
    explicit ReorderBuffer(uint32_t capacity)
        : mEntries(capacity),
          mSlotEpoch(capacity, 0),
          mCapacity(capacity) {}

    bool canAllocate(uint32_t count = 1) const { return (mSize + count) <= mCapacity; }

    core::ROBToken allocate(core::ROBEntry&& entry) {
        uint32_t idx = mTail;
        uint32_t epoch = mSlotEpoch[idx];
        mEntries[idx] = std::move(entry);
        mEntries[idx].completed = false;
        mEntries[idx].epoch = epoch;
        ++mSlotEpoch[idx];
        mTail = (mTail + 1) % mCapacity;
        ++mSize;
        return {idx, epoch};
    }

    void markComplete(const core::ROBToken& token) {
        if (mEntries[token.idx].epoch == token.epoch) {
            mEntries[token.idx].completed = true;
        }
    }

    bool empty() const { return mSize == 0; }
    uint32_t size() const { return mSize; }
    uint32_t capacity() const { return mCapacity; }

    bool headCompleted() const { return mSize > 0 && mEntries[mHead].completed; }

    core::ROBEntry& head() { return mEntries[mHead]; }
    const core::ROBEntry& head() const { return mEntries[mHead]; }
    uint32_t headIndex() const { return mHead; }
    const core::ROBEntry& entryAt(uint32_t idx) const { return mEntries[idx]; }
    core::ROBEntry& entryAt(uint32_t idx) { return mEntries[idx]; }

    void commitHead() {
        mHead = (mHead + 1) % mCapacity;
        --mSize;
    }

    // Squash ROB entries based on flush request (using shouldSquash)
    // Returns the new_phys_dsts of squashed entries for release back to free list
    std::vector<core::PhysRegRef> squashFrom(const core::FlushRequest& req) {
        std::vector<core::PhysRegRef> freed_regs;

        if (mSize == 0) return freed_regs;

        // Walk from tail backwards to head, peeling the wrong-path suffix. The ROB is in
        // fetch order, and fetch_seq is monotonic and NEVER reused, so everything fetched
        // after the resolving branch (fetch_seq > branch_fetch_seq) is a contiguous suffix
        // and everything head-ward of it is correct-path. The old test used entry.tag, but
        // model tags are reused after a flush, so a recycled small tag mid-ROB tripped the
        // `tag <= branch_tag` early-break before reaching an older wrong-path entry, leaving
        // it un-squashed -- it then reached the ROB head and tripped the wrong-path assert.
        uint32_t count = mSize;
        uint32_t current = (mTail == 0) ? mCapacity - 1 : mTail - 1;

        while (count > 0) {
            core::ROBEntry& entry = mEntries[current];

            bool squash = (req.branch_fetch_seq != 0) ? (entry.fetch_seq > req.branch_fetch_seq) : core::shouldSquash(entry.tag, entry.wrong_path_depth, req);
            if (squash) {
                // Release new_phys_dsts back to free list (not old_phys_dsts!)
                for (const auto& ref : entry.new_phys_dsts) {
                    freed_regs.push_back(ref);
                }

                // Move tail back
                mTail = current;
                --mSize;
            } else {
                // Reached the branch or an older (in fetch order) instruction; the remaining
                // head-side entries are all correct-path. Stop.
                break;
            }

            current = (current == 0) ? mCapacity - 1 : current - 1;
            --count;
        }

        return freed_regs;
    }
};

class WritebackParams : public sparta::ParameterSet {
   public:
    WritebackParams(sparta::TreeNode* n)
        : sparta::ParameterSet(n) {}

    PARAMETER(uint32_t, retire_width, 4, "Max instructions retired per cycle")
    PARAMETER(uint32_t, rob_capacity, 128, "Reorder buffer capacity")
    PARAMETER(uint32_t, retire_timeout_cycles, 10000, "Watchdog: max cycles with no retirement before asserting")
    // Age is cycles since dispatch; a stalled oldest load past the threshold counts as missing that cache level.
    PARAMETER(uint32_t, retire_stall_l1_miss_age, 4, "Oldest-load age (cycles) beyond which a retire stall counts as an L1 miss")
    PARAMETER(uint32_t, retire_stall_l2_miss_age, 16, "Oldest-load age (cycles) beyond which a retire stall counts as an L2 miss")
};

class Writeback : public sparta::Unit {
   public:
    static constexpr char name[] = "writeback";

    Writeback(sparta::TreeNode* node, const WritebackParams* params);

    sparta::DataInPort<std::vector<core::ROBToken>> rob_complete_exe_in{&unit_port_set_, "rob_complete_exe_in"};

    sparta::DataInPort<std::vector<core::ROBToken>> rob_complete_lsq_in{&unit_port_set_, "rob_complete_lsq_in"};

    sparta::DataOutPort<std::vector<core::PhysRegRef>> commit_out{&unit_port_set_, "commit_out"};

    // Flush support for misprediction recovery
    sparta::DataInPort<core::FlushRequest> flush_in{&unit_port_set_, "flush_in"};
    sparta::DataOutPort<std::vector<core::PhysRegRef>> freed_regs_out{&unit_port_set_, "freed_regs_out"};
    sparta::DataOutPort<uint64_t> checkpoint_release_out{&unit_port_set_, "checkpoint_release_out"};

    // Trap/interrupt redirect: squash the diverged stream and redirect Fetch to
    // the trap handler when Whisper takes an unmodeled trap/interrupt at retire.
    sparta::DataOutPort<core::FlushRequest> trap_flush_out{&unit_port_set_, "trap_flush_out"};

    // Commit-time branch-predictor training: fetch_seq of a retiring branch.
    sparta::DataOutPort<uint64_t> branch_retire_out{&unit_port_set_, "branch_retire_out"};

    // Correct branch resolution - decrements depth of younger instructions
    sparta::DataInPort<core::BranchResolved> branch_resolved_in{&unit_port_set_, "branch_resolved_in"};

    void tick();
    bool isReady() const { return true; }
    void setExecutionDriver(cpu::ExecutionDriver* d) { mDriver = d; }
    void setVisualizer(core::PipelineVisualizer* v) { mVis = v; }
    void setLsq(LSQ* lsq) { mLsq = lsq; }
    void setSpeculationConfig(const core::SpeculationConfig* cfg) { mSpecCfg.set(cfg); }
    void setInstructionLimit(uint64_t limit) { mInstructionLimit = limit; }

    ReorderBuffer& rob() { return mRob; }

    uint64_t numRetired() const { return mNumRetired.get(); }
    uint64_t numSquashed() const { return mNumSquashed.get(); }
    uint64_t numRetiredBranches() const { return mNumRetiredBranches.get(); }
    uint64_t numRetiredMispredicts() const { return mNumRetiredMispredicts.get(); }

   private:
    void receiveCompletions_(const std::vector<core::ROBToken>& tokens);
    void receiveFlush_(const core::FlushRequest& req);
    void receiveBranchResolved_(const core::BranchResolved& resolved);

    cpu::ExecutionDriver* mDriver{nullptr};
    core::PipelineVisualizer* mVis{nullptr};
    core::SpeculationConfigRef mSpecCfg;
    LSQ* mLsq{nullptr};
    uint32_t mRetireWidth;
    ReorderBuffer mRob;
    uint32_t mRetireStallL1MissAge;
    uint32_t mRetireStallL2MissAge;

    std::vector<core::PhysRegRef> mOldDstsBuf;
    uint64_t mInstructionLimit{0};
    uint32_t mRetireTimeoutCycles;
    bool mWatchdogStarted{false};

    void checkForwardProgress(const uint64_t& last_retired_count);
    void scheduleWatchdog();

    sparta::PayloadEvent<uint64_t, sparta::SchedulingPhase::Tick> mWatchdogEvent{&unit_event_set_, "forward_progress_check",
                                                                                 CREATE_SPARTA_HANDLER_WITH_DATA(Writeback, checkForwardProgress, uint64_t)};

    sparta::Counter mNumRetired;
    sparta::Counter mNumSquashed;
    sparta::Counter mNumRetiredBranches;
    sparta::Counter mNumRetiredMispredicts;

    // Cycles nothing retired behind an unfinished oldest load; each is a subset of the previous.
    sparta::Counter mRetireStallLoadAny;
    sparta::Counter mRetireStallLoadL1Miss;
    sparta::Counter mRetireStallLoadL2Miss;
    sparta::Counter mNumMemOrderFlushes;

    // TRACE-only: detect non-contiguous retire order (debugging wrong-path retire bug)
    uint64_t mLastRetiredTag{0};
    bool mLastRetiredTagValid{false};
    // Anchor for the trap/interrupt redirect: the last successfully committed
    // instruction. A redirect squashes everything fetched after it.
    uint64_t mLastRetiredFetchSeq{0};
    uint64_t mLastRetiredPc{0};
    // Tag of the diverged ROB head we already sent a trap flush for, to avoid
    // re-sending every cycle while the squash propagates.
    uint64_t mTrapFlushSentTag{static_cast<uint64_t>(-1)};
    // fetch_seq of the load a memory-order replay was sent for (tags repeat after the replay).
    uint64_t mOrderFlushSentSeq{static_cast<uint64_t>(-1)};
};

}  // namespace midcore
