// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Tenstorrent USA, Inc.

#pragma once

#include <functional>
#include <map>
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

// Per-model XLEN: core/simple/cluster drive RV64; a model built with RPM_XLEN=32 drives RV32.
// Each model compiles its own ExecutionDriver.cpp, so this is a per-target define.
#ifndef RPM_XLEN
#define RPM_XLEN 64
#endif
#if RPM_XLEN == 32
using XlenType = uint32_t;
#elif RPM_XLEN == 64
using XlenType = uint64_t;
#else
#error "RPM_XLEN must be 32 or 64"
#endif

using PerfApiType = TT_PERF::PerfApi<XlenType>;
using HartPtrType = std::shared_ptr<WdRiscv::Hart<XlenType>>;
using SessionType = WdRiscv::Session<XlenType>;

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
        PARAMETER(uint32_t, shared_hart_count, 1, "Harts in the Whisper system shared by all cores (set by the cluster)")
        PARAMETER(bool, private_system, false, "Build a single-hart Whisper system owned by this core alone (set by the cluster)")
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
    bool doSetup(coreid_t id, const std::string &snapshotFolderName = "");
    bool isSetupDone() const { return mSetupDone; }

    //==================== State Accessors ====================

    bool isFinished();
    // Set once the program exits or the ROI-exit HINT commits; flushes leave it set.
    bool programEnded() const { return mProgramEnded; }
    address_t getInitPc();
    address_t getNextPc();
    bool isNextPcAvailable() const { return mNextPcSet; }
    // I-cache address of a fetch from `va` in the hart's current mode; `va` itself if the translation
    // faults. Overwrites the hart's fetch page-table-walk record.
    address_t fetchPa(address_t va) const;
    uint64_t getInstructionCount() const { return mRetireSequence; }

    //==================== Address translation (TLB timing) ====================

    // Whisper's page-table walk for one translation. PTE addresses are model PAs, root level first.
    struct PageWalk {
        std::vector<address_t> pte_addrs;
        address_t pa{0};
        uint64_t page_size{4096};
        uint32_t levels{0};  // levels of the translation mode (3 for Sv39); 0 for a two-stage walk
        bool global{false};
        bool fault{false};
    };
    // True when the hart's current privilege mode translates (S/U with satp, or vsatp/hgatp, not Bare).
    bool translationOn() const;
    uint32_t currentAsid() const;
    // Redo Whisper's walk of `va`; a data walk runs in the CSR context of the instruction with model tag `tag`.
    PageWalk walkPageTable(address_t va, bool fetch, bool store, uint64_t tag) const;

    struct SfenceVma {
        bool all_va{true};
        address_t va{0};
        bool all_asid{true};
        uint32_t asid{0};
    };
    // Called at retire of every sfence.vma / sinval.vma.
    void setSfenceHook(std::function<void(const SfenceVma &)> hook) { mSfenceHook = std::move(hook); }
    // Rewindable fetch tag counter; used by the ROB watchdog to detect fetch/retire desync.
    uint64_t getFetchSequence() const { return mSequence; }

    // Whisper's current architectural (committed) PC -- the PC of the next
    // instruction to retire. Writeback compares this against the correct-path
    // ROB head to detect a trap/interrupt (e.g. a timer interrupt) taken at a
    // prior retire that diverged execution to a handler the model never fetched.
    address_t expectedRetirePc() { return mHartPtr ? mHartPtr->peekPc() : mNextPc; }

    // Data-carrying caches (shadow): read `len` bytes of physical memory at `addr` from Whisper
    // (the oracle) into `buf`, side-effect-free. Used to seed a shadow line on fill and for the
    // shadow-vs-Whisper self-check. Zero-fills if the hart isn't up yet. Pass translate=false
    // when the address is already physical (loose-coupling accelerator descriptors).
    void peekPhysBytes(uint64_t addr, uint8_t *buf, uint32_t len, bool translate = true) const;

    // Write side of the oracle path for accelerator devices: apply `len` bytes at
    // `addr` to Whisper memory so an accelerator's writes become visible to the cores.
    // Translates VA->PA like peekPhysBytes when `translate` (tight coupling); pass
    // translate=false for PA-mode descriptors (loose coupling). Failure policy: count-and-log
    // in all builds + sparta_assert in debug — a silently dropped accelerator write is an
    // oracle divergence. Returns false on failure (translation fault or PMA not-writable).
    bool pokePhysBytes(uint64_t addr, const uint8_t *buf, uint32_t len, bool translate = true);
    uint64_t pokeFailures() const { return mPokeFailures; }

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
    bool retireInstruction(uint64_t tag);

    //==================== Flush Interface ====================

    // Flush all instructions with tag >= the given tag. Returns number of instructions flushed.
    // Also sets mNextPc to the correct PC for resuming fetch.
    uint64_t flushInstruction(uint64_t tag);

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
    const uint32_t mSharedHartCount;
    const bool mPrivateSystem;
    // Set in every PA handed to the timing model, so private systems' identical images stay apart in shared caches.
    uint64_t mPaTag{0};
    address_t tagPa(address_t pa) const { return pa == 0 ? 0 : pa | mPaTag; }

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
    bool mProgramEnded{false};
    bool mOwnsSystem{false};

    uint64_t mNextPc{0};
    bool mNextPcSet{true};
    instid_t mNextPcSetInstId{static_cast<instid_t>(-1)};

    uint64_t mSequence{1};
    uint64_t mRetireSequence{0};
    uint64_t mSnapshotOffset{0};

    // Oracle-write divergence counter (see pokePhysBytes).
    uint64_t mPokeFailures{0};

    std::function<void(const SfenceVma &)> mSfenceHook;

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
    // PerfApi sizes vector operands by the register width, which is 0 on a hart without V.
    bool isVectorWithoutV(const WdRiscv::DecodedInst &di) const { return di.isVector() && mHartPtr->vecRegs().bytesPerRegister() == 0; }

    //==================== Helper Functions: Setup ====================

    bool doSetupHelper(coreid_t id, const std::string &snapshotFolderName);
    std::vector<std::string> buildWhisperArguments();
    void initializeWhisperSystem(std::vector<std::string> &wargv);
};

}  // namespace cpu
