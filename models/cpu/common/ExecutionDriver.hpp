// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Tenstorrent USA, Inc.

#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "sparta/simulation/ParameterSet.hpp"
#include "sparta/simulation/TreeNode.hpp"
#include "sparta/simulation/Unit.hpp"

#include "Args.hpp"
#include "HartConfig.hpp"
#include "PerfApi.hpp"
#include "Session.hpp"
#include "models/cpu/common/Instruction.hpp"
#include "models/cpu/common/whisper_include_fix.hpp"

// Core uses 64-bit RISC-V with templated PerfApi
using PerfApiType = TT_PERF::PerfApi<uint64_t>;
using HartPtrType = std::shared_ptr<WdRiscv::Hart<uint64_t>>;
using SessionType = WdRiscv::Session<uint64_t>;

namespace cpu {

class ExecutionDriver : public sparta::Unit {
   public:
    class ExecutionDriverParamSet : public sparta::ParameterSet {
       public:
        ExecutionDriverParamSet(sparta::TreeNode *n)
            : sparta::ParameterSet(n) {}

        PARAMETER(std::string, target_command, "", "Executable filename")
        PARAMETER(std::vector<std::string>, whisper_flags, {}, "Additional whisper flags for driving execution")
        PARAMETER(std::string, snapshot_foldername, "", "Snapshot folder name")
        PARAMETER(bool, enable_snapshot_usage, false, "Look for a snapshot folder for this trace")
        PARAMETER(bool, allow_early_termination, false, "If set, don't treat unread records in the trace as an error")
        PARAMETER(bool, log_enabled, false, "Enable debug logging for this unit")
    };

    static constexpr char name[] = "edriver";
    static const address_t endPc = 0xfffffffffffffffe;

    ExecutionDriver(sparta::TreeNode *node, const ExecutionDriverParamSet *p);
    ~ExecutionDriver() override;

    ExecutionDriver(const ExecutionDriver &) = delete;
    ExecutionDriver &operator=(const ExecutionDriver &) = delete;
    ExecutionDriver(ExecutionDriver &&) = delete;
    ExecutionDriver &operator=(ExecutionDriver &&) = delete;

    //==================== Setup & Initialization ====================

    void setId(coreid_t id) { mId = id; }
    void setTraceFileName(std::string traceFileName) { mTraceFileName = std::move(traceFileName); }
    bool doSetup(coreid_t id, const std::string &snapshotFolderName = "");
    bool isSetupDone() const { return mSetupDone; }

    //==================== State Accessors ====================

    bool isFinished();
    address_t getInitPc();
    address_t getNextPc();
    bool isNextPcAvailable() const { return mNextPcSet; }
    uint64_t getInstructionCount() const { return mRetireSequence; }

    // Whisper's current architectural (committed) PC -- the PC of the next
    // instruction to retire. Writeback compares this against the correct-path
    // ROB head to detect a trap/interrupt (e.g. a timer interrupt) taken at a
    // prior retire that diverged execution to a handler the model never fetched.
    address_t expectedRetirePc() { return mHartPtr ? mHartPtr->peekPc() : mNextPc; }

    // Set when a speculative fetch decodes an illegal instruction.
    bool isDriverInInvalidState() const { return mDriverInvalidState; }

    // Override the next PC for speculative execution (e.g., after branch prediction redirect)
    void setNextPc(address_t pc) {
        mNextPc = pc;
        mNextPcSet = true;
    }

    //==================== Execution Interface ====================

    InstPtr peekInstruction(address_t fetchPc, bool onSpeculativePath = false);
    bool executeInstruction(InstPtr &inst, bool onSpeculativePath = false);
    bool retireInstruction(uint64_t tag, uint32_t rob_occupancy = 0, uint32_t rob_size = 0);

    //==================== Flush Interface ====================

    // Flush all instructions with tag >= the given tag. Returns number of instructions flushed.
    // Also sets mNextPc to the correct PC for resuming fetch.
    uint64_t flushInstruction(uint64_t tag);

    // Flush a single instruction by InstPtr
    bool flushInstruction(InstPtr &inst);

   protected:
    void simulationTerminating_() override;

   private:
    //==================== Static Members ====================

    // NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
    static std::unordered_map<coreid_t, std::pair<std::shared_ptr<PerfApiType>, HartPtrType>> mCoreIdToPerfApiMap;

    //==================== Constants ====================

    const std::map<std::string, std::string> mWarmupWhisperFlags = {{"--memorysize", "0x580000000"}, {"--tlbsize", "0"}};
    const std::string mDefaultWhisperConfigName = "whisper.json";
    const std::string mDefaultWhisperElfName = "fw_jump.elf";

    //==================== Configuration (from params) ====================

    const std::string mTargetCommand;
    const std::vector<std::string> mWhisperFlags;
    const bool mAllowEarlyTermination;
    bool mEnableSnapshot;
    bool mLogEnabled{false};
    std::string mTraceFileName;

    //==================== Whisper Components ====================

    WdRiscv::Args mArgs;
    WdRiscv::HartConfig mConfig;
    SessionType mSession{};
    std::shared_ptr<PerfApiType> mPerfApiHandle;
    HartPtrType mHartPtr;
    uint64_t mHartIx;

    //==================== Runtime State ====================

    coreid_t mId{0};
    std::string mSnapshotFolderName;
    uint64_t mInstructionCount{0};
    bool mSetupDone{false};
    bool mFinishedOverride{false};

    uint64_t mNextPc{0};
    bool mNextPcSet{true};
    instid_t mNextPcSetInstId{static_cast<instid_t>(-1)};

    uint64_t mSequence{1};
    uint64_t mRetireSequence{0};
    uint64_t mSnapshotOffset{0};

    // Wrong-path invalid-state tracker. mInvalidStateTag is the model tag (instId) of
    // the illegal speculative instruction that armed the state.
    bool mDriverInvalidState{false};
    uint64_t mInvalidStateTag{0};

    //==================== Statistics ====================

    sparta::Counter mInstsFetched;
    sparta::Counter mInstsCommitted;

    //==================== Helper Functions: TraceRecord Population ====================

    bool populateTraceRecord(WhisperUtil::TraceRecord &record, const std::shared_ptr<TT_PERF::InstrPac> &pacPtr);
    bool populateRecordOperands(WhisperUtil::TraceRecord &record, const std::shared_ptr<TT_PERF::InstrPac> &pacPtr);
    static void populateOperand(const TT_PERF::Operand &perfOperand, WhisperUtil::Operand &recordOperand, bool isDest);
    bool populateRecordMemoryOps(WhisperUtil::TraceRecord &record, const std::shared_ptr<TT_PERF::InstrPac> &pacPtr);

    //==================== Helper Functions: Memory Operations ====================

    static uint32_t getStoreConditionalDataSize(uint32_t instruction);
    bool populateRegularMemoryOps(WhisperUtil::TraceRecord &record, const std::shared_ptr<TT_PERF::InstrPac> &pacPtr);
    bool populateVectorMemoryOps(WhisperUtil::TraceRecord &record, const std::shared_ptr<TT_PERF::InstrPac> &pacPtr);

    //==================== Helper Functions: Execution ====================

    const std::shared_ptr<TT_PERF::InstrPac> processNextRecordHelper(WhisperUtil::TraceRecord &record, address_t fetchPc, uint64_t tag, bool flush = false,
                                                                     bool execute = true);
    bool isSerializing(const std::shared_ptr<TT_PERF::InstrPac> &pacPtr);

    //==================== Helper Functions: Setup ====================

    bool doSetupHelper(coreid_t id, const std::string &snapshotFolderName);
    std::vector<std::string> buildWhisperArguments();
    void initializeWhisperSystem(std::vector<std::string> &wargv);
};

}  // namespace cpu
