// SPDX-FileCopyrightText: © 2026 Tenstorrent AI ULC
//
// SPDX-License-Identifier: Apache-2.0

#include "ExecutionDriver.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <sstream>

#include "DecodedInst.hpp"
#include "Logging.hpp"
#include "models/cpu/common/SnapshotUtil.hpp"
#include "models/cpu/common/whisper_include_fix.hpp"

// Constants for store conditional memory operation handling
namespace {
constexpr unsigned SC_SIZE_CODE_MASK = 3;
constexpr unsigned SC_SIZE_CODE_SHIFT = 12;
constexpr unsigned SC_SIZE_1_BYTE = 0;
constexpr unsigned SC_SIZE_2_BYTE = 1;
constexpr unsigned SC_SIZE_4_BYTE = 2;
constexpr unsigned SC_SIZE_8_BYTE = 3;
}  // namespace

namespace {
constexpr uint64_t PAGE_SIZE_4KB_BYTES = 4096;
inline uint64_t get_page(uint64_t addr) { return addr / PAGE_SIZE_4KB_BYTES; }
inline uint64_t get_page_offset(uint64_t addr) { return addr & (PAGE_SIZE_4KB_BYTES - 1); }
enum CSRLabel : unsigned { vstart_c = 0, vl_c = 1, vtype_c = 2, vlen_c = 3 };
}  // namespace
#ifndef SPARTA_ASSERT_CLK
#define SPARTA_ASSERT_CLK(cond, msg) sparta_assert(cond, msg)
#endif

namespace cpu {

//==================== Static Members ====================

std::unordered_map<coreid_t, std::pair<std::shared_ptr<PerfApiType>, HartPtrType>> ExecutionDriver::mCoreIdToPerfApiMap = {};

//==================== Lifecycle ====================

ExecutionDriver::ExecutionDriver(sparta::TreeNode *node, const ExecutionDriverParamSet *p)
    : sparta::Unit(node),
      mTargetCommand(p->target_command),
      mWhisperFlags(p->whisper_flags),
      mAllowEarlyTermination(p->allow_early_termination),
      mEnableSnapshot(p->enable_snapshot_usage),
      mLogEnabled(p->log_enabled),
      mHartIx(0),
      mSnapshotFolderName(p->snapshot_foldername),
      mInstsFetched(&unit_stat_set_, "instructions_fetched", "total instructions fetched", sparta::Counter::COUNT_NORMAL),
      mInstsCommitted(&unit_stat_set_, "instructions_committed", "total instructions committed", sparta::Counter::COUNT_NORMAL) {
    ILOG("[ExecutionDriver] Exec Driver " << static_cast<uint32_t>(mId) << " finished constructing");
}

ExecutionDriver::~ExecutionDriver() {
    if (mId == 0 && mSetupDone) {
        mSession.cleanup(mArgs);
    }

    mCoreIdToPerfApiMap.erase(mId);
}

//==================== Setup & Initialization ====================

bool ExecutionDriver::doSetupHelper(coreid_t id, const std::string &snapshotFolderName) {
    const auto start_time = std::chrono::system_clock::now();
    mId = id;

    if (mId > 0) {
        ILOG("[doSetupHelper] Setting up core " << mId << " by mapping to core 0");
        auto it = mCoreIdToPerfApiMap.find(0);
        if (it != mCoreIdToPerfApiMap.end()) {
            mPerfApiHandle = it->second.first;
            mHartPtr = it->second.second;
            sparta_assert(mPerfApiHandle && mHartPtr, "[Execution Driver] doSetup() mPerfApiHandle or mHartPtr is nullptr for core " << mId);
            mSetupDone = true;
        } else {
            sparta_assert(false, "[doSetupHelper] core 0 not found in map -- please check setup order");
        }
    } else {
        ILOG("[doSetupHelper] Starting setup for core 0");

        if (!snapshotFolderName.empty()) {
            mSnapshotFolderName = snapshotFolderName;
            mEnableSnapshot = true;
        }

        auto wargv = buildWhisperArguments();
        initializeWhisperSystem(wargv);
    }

    mCoreIdToPerfApiMap[id] = std::make_pair(mPerfApiHandle, mHartPtr);

    const auto end_time = std::chrono::system_clock::now();
    const auto duration = std::chrono::duration<double>(end_time - start_time).count();
    std::cout << "[doSetupHelper] Spent: " << duration << " seconds on setup" << '\n';

    return true;
}

std::vector<std::string> ExecutionDriver::buildWhisperArguments() {
    std::vector<std::string> wargv = {"whisper", "--perfapi", "-v"};

    const std::unordered_set<std::string> whisperFlagsSet(mWhisperFlags.begin(), mWhisperFlags.end());
    bool config_present = whisperFlagsSet.contains("--configfile") || whisperFlagsSet.contains("--config");
    std::string whisperConfigPath;
    if (!config_present) {
        std::string whisperPath;
        if (!mSnapshotFolderName.empty()) {
            whisperPath = generateWhisperPath(mSnapshotFolderName);
        }
        if (whisperPath.empty() && !mTargetCommand.empty()) {
            std::filesystem::path elfPath(mTargetCommand);
            whisperPath = elfPath.parent_path().string();
            if (whisperPath.empty()) whisperPath = ".";
        }
        whisperConfigPath = whisperPath + "/" + mDefaultWhisperConfigName;
        sparta_assert(std::filesystem::exists(whisperConfigPath), "[Execution Driver] doSetup(): could not find whisper configuration");
    }
    if (!config_present) {
        wargv.emplace_back("--configfile");
        wargv.emplace_back(whisperConfigPath);
    }

    for (size_t i = 0; i < mWhisperFlags.size(); ++i) {
        if (mWhisperFlags[i] == "--xlen" && i + 1 < mWhisperFlags.size()) {
            wargv.emplace_back("--xlen");
            wargv.emplace_back(mWhisperFlags[i + 1]);
            break;
        }
    }

    wargv.emplace_back("--target");
    if (!mTargetCommand.empty()) {
        wargv.emplace_back(mTargetCommand);
    } else {
        auto whisperPath = generateWhisperPath(mSnapshotFolderName);
        auto whisperElfPath = whisperPath + "/" + mDefaultWhisperElfName;
        sparta_assert(std::filesystem::exists(whisperElfPath), "[ExecutionDriver] doSetup() Could not find executable at " << whisperElfPath);
        wargv.emplace_back(whisperElfPath);
    }

    for (size_t i = 0; i < mWhisperFlags.size(); ++i) {
        if (mWhisperFlags[i] == "--xlen" && i + 1 < mWhisperFlags.size()) {
            ++i;
            continue;
        }
        wargv.emplace_back(mWhisperFlags[i]);
    }

    for (const auto &warg : mWarmupWhisperFlags) {
        if (!whisperFlagsSet.contains(warg.first)) {
            wargv.emplace_back(warg.first);
            wargv.emplace_back(warg.second);
        }
    }

    if (mEnableSnapshot && !mSnapshotFolderName.empty()) {
        std::cout << "Execution Driver: Loading snapshot from: " << mSnapshotFolderName << '\n';
        wargv.emplace_back("--loadfrom");
        wargv.emplace_back(mSnapshotFolderName);
    }

    std::cout << "Whisper command line args: ";
    for (const auto &arg : wargv) {
        std::cout << arg << " ";
    }
    std::cout << '\n';

    return wargv;
}

void ExecutionDriver::initializeWhisperSystem(std::vector<std::string> &wargv) {
    auto parseArgsOk = mArgs.parseCmdLineArgs(wargv);
    sparta_assert(parseArgsOk, "[Execution Driver] Could not parse whisper arguments");

    if (!mArgs.configFile.empty()) {
        if (!mConfig.loadConfigFile(mArgs.configFile)) {
            sparta_assert(0, "Unable to load whisper configuration file");
        }
    }

    ILOG("[initializeWhisperSystem] Creating Whisper system");
    auto system = mSession.defineSystem(mArgs, mConfig);
    sparta_assert(system, "returned system is nullptr");

    ILOG("[initializeWhisperSystem] Configuring Whisper system");
    bool whisperOk = mSession.configureSystem(mArgs, mConfig);
    sparta_assert(whisperOk, "unable to configure whisper system");

    ILOG("[initializeWhisperSystem] Getting PerfApi handle from system");
    mPerfApiHandle = system->getPerfApi();
    sparta_assert(mPerfApiHandle, "got invalid perfApi handler");

    ILOG("[initializeWhisperSystem] Getting Hart " << mId << " from PerfApi");
    mHartPtr = mPerfApiHandle->getHart(mId);
    sparta_assert(mHartPtr, "whisper mHartPtr should not be nullptr");

    mHartIx = mHartPtr->sysHartIndex();
    mNextPc = mHartPtr->peekPc();
    mSetupDone = true;

    std::cout << "HART Instruction Limit: " << std::dec << mHartPtr->getInstructionCountLimit() << '\n';

    if (mEnableSnapshot) {
        ILOG("[initializeWhisperSystem] Loading snapshot with instruction count: " << mHartPtr->getInstructionCount());
        mSnapshotOffset = mHartPtr->getInstructionCount();
        mSequence += mSnapshotOffset;
        mInstructionCount = mRetireSequence = mSnapshotOffset;
        std::cout << "Loaded snapshot with instruction count: " << mHartPtr->getInstructionCount() << '\n';
    }

    ILOG("[initializeWhisperSystem] Setup complete for executable: " << mTargetCommand << " Max Inst Count: " << std::dec
                                                                     << mHartPtr->getInstructionCountLimit());
}

bool ExecutionDriver::doSetup(coreid_t id, const std::string &snapshotFolderName) { return doSetupHelper(id, snapshotFolderName); }

//==================== State Accessors ====================

bool ExecutionDriver::isFinished() {
    bool isProgramFinished = mHartPtr && (mHartPtr->hasTargetProgramFinished() || (mInstructionCount >= mHartPtr->getInstructionCountLimit()));
    if (isProgramFinished) {
        ILOG("[isFinished] Program has finished execution on whisper hart");
    }
    return mFinishedOverride || isProgramFinished;
}

//==================== Serialization Checks ====================

bool ExecutionDriver::isSerializing(const std::shared_ptr<TT_PERF::InstrPac> &pacPtr) {
    using CN = WdRiscv::CsrNumber;
    // Writes to these CSRs change pipeline-affecting architectural state (FP/vector
    // rounding mode & vector config). Younger instructions implicitly consume them at
    // execute time — e.g. an FP op reads FRM/FCSR for its rounding mode — so the write
    // must resync the pipeline. Otherwise a younger FP op executes speculatively with
    // the stale rounding mode and trips whisper's exec-vs-retire check at retire.
    static const std::set<unsigned> non_spec_resync_csrs = {
        unsigned(CN::FRM),   unsigned(CN::FCSR),  unsigned(CN::FFLAGS), unsigned(CN::VSTART), unsigned(CN::VL),
        unsigned(CN::VTYPE), unsigned(CN::VLENB), unsigned(CN::VCSR),   unsigned(CN::VXRM),   unsigned(CN::VXSAT),
    };
    const WdRiscv::DecodedInst &di = pacPtr->decodedInst();
    if (di.instEntry() != nullptr) {
        switch (di.instEntry()->instId()) {
            case WdRiscv::InstId::csrrs:
            case WdRiscv::InstId::csrrc:
            case WdRiscv::InstId::csrrci:
            case WdRiscv::InstId::csrrsi:
                // rs1 == x0 means no CSR side-effect, so no serialization needed.
                if (di.op1() == 0) return false;
                if (mHartPtr->privilegeMode() != WdRiscv::PrivilegeMode::User) return true;
                return non_spec_resync_csrs.contains(di.op2());

            case WdRiscv::InstId::csrrw:
            case WdRiscv::InstId::csrrwi:
                if (mHartPtr->privilegeMode() != WdRiscv::PrivilegeMode::User) return true;
                return non_spec_resync_csrs.contains(di.op2());

            case WdRiscv::InstId::sc_w:
            case WdRiscv::InstId::sc_d:
            case WdRiscv::InstId::vsetvl:
            case WdRiscv::InstId::ecall:
            case WdRiscv::InstId::ebreak:
            case WdRiscv::InstId::mret:
            case WdRiscv::InstId::sret:
            case WdRiscv::InstId::fence_i:
            case WdRiscv::InstId::sfence_vma:
            case WdRiscv::InstId::wfi:
                return true;

            default:
                return false;
        }
    }

    return false;
}

address_t ExecutionDriver::getInitPc() {
    sparta_assert(mSetupDone, "Cannot call getInitPc before setup is done");
    return mNextPc;
}

address_t ExecutionDriver::getNextPc() {
    sparta_assert(mNextPcSet, "[ExecutionDriver] getNextPc called but mNextPcSet is not true.");
    return mNextPc;
}

//==================== Instruction Lifecycle ====================

InstPtr ExecutionDriver::peekInstruction(address_t fetchPc, bool onSpeculativePath) {
    if (mFinishedOverride) {
        ILOG("[peekInstruction] mFinishedOverride flag raised, pausing fetch");
        return nullptr;
    }

    const auto instId = mSequence - mSnapshotOffset - 1;
    ILOG("[peekInstruction] Peeking instruction at Pc: " << std::hex << fetchPc << std::dec << " InstId: " << instId);

    WhisperUtil::TraceRecord record;

    auto pacPtr = processNextRecordHelper(record, fetchPc, mSequence, false, false);

    if (!pacPtr) {
        ILOG("[peekInstruction] Could not fetch from perfapi Pc: " << std::hex << fetchPc << std::dec << " InstId: " << instId);
        return nullptr;
    }

    // Wrong-path invalid-state detection. Fetch resumes only after a flush clears the state.
    if (onSpeculativePath && pacPtr->decodedInst().instId() == WdRiscv::InstId::illegal) {
        ILOG("[peekInstruction] Speculative illegal instruction at Pc: " << std::hex << fetchPc << std::dec << " InstId: " << instId
                                                                         << " -- entering driver invalid state, stopping wrong-path fetch");
        // Erase the PerfApi packet that the fetch above just inserted at this tag and abandon
        // this instruction without advancing mSequence, so the next fetch will re-issue the same tag.
        mPerfApiHandle->flush(mHartIx, 0, mSequence);
        mDriverInvalidState = true;
        mInvalidStateTag = instId;
        return nullptr;
    }

    ILOG("[peekInstruction] Peeked instruction InstId: " << instId << " inst: " << record.assembly);

    InstPtr inst = InstPtr{new Instruction(record, instId, mId, pacPtr)};

    if (isSerializing(pacPtr) || pacPtr->isDeviceLdSt()) {
        inst->setPerfApiSerializing();
        // On the wrong path a serializing instruction is not executed at fetch (its
        // result/side effects are deferred to retire). We must not fetch past it
        // speculatively, or a younger speculative instruction would execute against an
        // unexecuted producer (Whisper PerfApi: "depends on tag which is not yet
        // executed"). Arm the invalid state; it clears when this instruction is flushed
        // (wrong path) or retires (correct path). Unlike the illegal case, the serializing
        // instruction itself is valid, so let it into the pipeline -- we only stop
        // fetching PAST it.
        if (onSpeculativePath) {
            mDriverInvalidState = true;
            mInvalidStateTag = instId;
            ILOG("[peekInstruction] Speculative serializing inst InstId=" << instId << " -- arming driver invalid state, stopping wrong-path fetch past it");
        }
    }

    return inst;
}

bool ExecutionDriver::executeInstruction(InstPtr &inst, bool onSpeculativePath) {
    sparta_assert(inst, "[ExecutionDriver] executeInstruction() called with nullptr instruction");

    const auto modelTag = inst->getId().getInstNum();
    const auto perfApiTag = modelTag + mSnapshotOffset + 1;
    const auto pacPtr = inst->getPerfPtr();

    ILOG("[executeInstruction] Executing instruction Id: " << modelTag);

    auto &perfApi = *mPerfApiHandle;

    if (!pacPtr->executed() && !isSerializing(pacPtr)) {
        auto ok = perfApi.execute(mHartIx, 0, perfApiTag);
        sparta_assert(ok, "[ExecutionDriver] perfApi execution failure for perfApi tag: " << perfApiTag);

        if (pacPtr->trapped()) {
            ILOG("[executeInstruction] Instruction " << modelTag << " encountered trap");
        }

        bool traceRecordSuccess = populateTraceRecord(inst->getMutableTraceRecord(), pacPtr);
        if (!traceRecordSuccess) {
            ILOG("[executeInstruction] Failed to populate trace record for instruction " << modelTag);
            return false;
        }
        inst->refreshInstClass();
    }

    mNextPcSet = false;
    if (!isSerializing(pacPtr) && !pacPtr->trapped()) {
        mNextPc = pacPtr->nextPc();
        mNextPcSet = true;
        ILOG("[executeInstruction] NextPc set to " << std::hex << mNextPc << std::dec);
    } else {
        ILOG("[executeInstruction] Instruction " << modelTag << " is serializing/trapped, deferring nextPc setting");
        mNextPcSetInstId = mSequence;
    }
    mSequence++;

    // Only treat reaching the end-of-program sentinel as genuine completion on the correct
    // path. On the wrong path a garbage indirect jump can land on/after endPc; honoring it
    // would set mFinishedOverride and let Writeback declare the run finished the moment the
    // ROB transiently drains (observed: premature "Run Successful" mid-run). Wrong-path
    // fetch already stops at fetch_pc >= endPc on its own.
    if (mNextPcSet && mNextPc >= endPc && !onSpeculativePath) {
        inst->setLast();
        ILOG("[executeInstruction] Setting mFinishedOverride due to program exit (correct path)");
        mFinishedOverride = true;
    }

    mInstsFetched++;

    return true;
}

bool ExecutionDriver::retireInstruction(uint64_t tag, uint32_t rob_occupancy, uint32_t rob_size) {
    (void)rob_occupancy;
    (void)rob_size;

    if (!mSetupDone) {
        return true;
    }

    // If the instruction that armed the invalid state (a speculative serializing or illegal
    // instruction) is now retiring, the speculation was correct: clear the state so fetch
    // resumes. Its result is available post-retire, so dependents can execute. (Wrong-path
    // arming is instead cleared by flushInstruction.)
    if (mDriverInvalidState && tag >= mInvalidStateTag) {
        ILOG("[retireInstruction] Clearing driver invalid state on retire of tag " << tag << " (armed tag " << mInvalidStateTag << ")");
        mDriverInvalidState = false;
    }

    const auto perfApiTag = tag + mSnapshotOffset + 1;

    sparta_assert(perfApiTag <= (mRetireSequence + 1), "[Execution Driver] Error in retire sequence expected: " << mRetireSequence << " Actual: " << tag);

    sparta_assert(perfApiTag <= mSequence, "[Execution Driver] can't retire a tag that has not been fetched yet");
    auto &perfApi = *mPerfApiHandle;
    const auto pacPtr = perfApi.getInstructionPacket(mHartIx, perfApiTag);
    sparta_assert(pacPtr, "[ExecutionDriver] Instruction packet not found for tag: " << tag);

    if (!pacPtr->executed()) {
        ILOG("[retireInstruction] Serializing instruction " << tag << " executing at retire time");
        auto ok = perfApi.execute(mHartIx, 0, perfApiTag);
        sparta_assert(ok, "[ExecutionDriver] perfApi execute-at-retire failure for tag: " << tag);
    }

    bool success;
    try {
        success = perfApi.retire(mHartIx, 0, perfApiTag);

        sparta_assert(success, "[ExecutionDriver] Error in retiring instruction " << std::dec << tag << " Pc: " << std::hex << pacPtr->instrVa() << std::dec);

        if (SPARTA_EXPECT_FALSE(info_logger_)) {
            std::string instAssembly;
            mHartPtr->disassembleInst(pacPtr->decodedInst(), instAssembly);
            ILOG("[retireInstruction] Retiring instruction Id " << tag << " Inst: " << instAssembly << " nextPc: " << std::hex << pacPtr->nextPc() << std::dec);
        }

        if (!mNextPcSet && mNextPcSetInstId == perfApiTag) {
            // FIX: For both trapped and non-trapped instructions, use nextPc().
            // For trapped instructions, nextPc() returns the trap handler address.
            // The previous code incorrectly used instrVa() + instrSize() for trapped
            // instructions, which gave the sequential next instruction instead of
            // the trap handler, causing PC divergence with Whisper.
            mNextPc = pacPtr->nextPc();
            mNextPcSet = true;
        }

        if (pacPtr->isStore() || pacPtr->isVectorStore() || pacPtr->isCbo_zero() || pacPtr->isSc() || pacPtr->isAmo()) {
            // The store commit happens here; a store to the HTIF "tohost" address
            // is committed via drainStore and can throw CoreException (see below).
            success = success && perfApi.drainStore(mHartIx, 0, perfApiTag);
            sparta_assert(success, "[ExecutionDriver] Error in draining store instruction " << std::dec << tag);
        }
    } catch (const WdRiscv::CoreException &e) {
        // A store to the HTIF "tohost" address is how a bare-metal program signals
        // normal exit (Whisper throws the same CoreException for the exit syscall).
        // The store commits inside perfApi.retire()/drainStore() above. This is
        // graceful termination, not an error: end the run through the normal
        // mFinishedOverride path instead of letting the exception unwind the Sparta
        // scheduler (which dumps error-dump.dbg and returns a nonzero status).
        const uint64_t exitCode = e.value() >> 1;  // HTIF: payload bit0=done, rest=code
        ILOG("[retireInstruction] tohost stop on tag " << tag << " (exit code " << exitCode << "); finishing run");
        mFinishedOverride = true;
    }

    mRetireSequence++;
    mInstructionCount++;
    mInstsCommitted++;

    return true;
}

//==================== TraceRecord Helpers: Core ====================

const std::shared_ptr<TT_PERF::InstrPac> ExecutionDriver::processNextRecordHelper(WhisperUtil::TraceRecord &record, address_t fetchPc, uint64_t tag, bool flush,
                                                                                  bool execute) {
    auto &perfApi = *mPerfApiHandle;
    auto &hart = *mHartPtr;
    if (hart.hasTargetProgramFinished()) {
        ILOG("[processNextRecordHelper] Target program has finished, returning nullptr");
        return nullptr;
    }

    ILOG("[processNextRecordHelper] Fetching instruction at Pc: " << std::hex << fetchPc << std::dec << " tag: " << tag);

    bool trap = false;
    WdRiscv::ExceptionCause cause = WdRiscv::ExceptionCause::NONE;
    uint64_t trapPc = 0;
    bool ok = perfApi.fetch(mHartIx, 0, tag, fetchPc, trap, cause, trapPc);

    if (!ok) {
        ILOG("[processNextRecordHelper] Failed to fetch instruction at Pc: " << std::hex << fetchPc << std::dec << " tag: " << tag);
        return nullptr;
    }

    auto pacPtr = perfApi.getInstructionPacket(mHartIx, tag);
    if (trap) {
        ILOG("[processNextRecordHelper] Whisper context encountered a Trap at Pc " << std::hex << fetchPc << std::dec);
        if (cause == WdRiscv::ExceptionCause::INST_PAGE_FAULT || cause == WdRiscv::ExceptionCause::INST_ACC_FAULT) {
            ILOG("[processNextRecordHelper] Instruction page fault encountered at Pc: " << std::hex << fetchPc << std::dec);
            if (pacPtr->instrPa() != pacPtr->instrPa2() && pacPtr->instrSize() == 4) {
                sparta_assert(get_page_offset(fetchPc) == 0xFFE && get_page_offset(pacPtr->instrPa()) == 0xFFE,
                              "[processNextRecordHelper] Expected page crossing instruction to be at the page crossing offset");
                pacPtr->setInstrVa(fetchPc);
            }

        } else {
            sparta_assert(false, "[processNextRecordHelper] Unexpected trap cause: " << static_cast<int>(cause));
        }
    }

    sparta_assert(pacPtr->instrVa() == fetchPc, "[processNextRecordHelper] Instruction virtual address mismatch: expected: "
                                                    << std::hex << fetchPc << " got: " << pacPtr->instrVa() << " for tag: " << tag << std::dec);

    ok = ok and perfApi.decode(mHartIx, 0, tag);
    sparta_assert(ok, "Decode failure in execution driver");

    const WdRiscv::DecodedInst &decodedInst = pacPtr->decodedInst();
    ILOG("[processNextRecordHelper] Fetched instruction tag: " << tag << " at Pc: " << std::hex << fetchPc << std::dec << " Assembly: " << decodedInst.name());

    // Use DecodedInst methods for instruction type checks - they're reliable after decode
    // pacPtr methods may not return correct values until after execution for some instruction types
    const bool needsExecute = decodedInst.isBranchToRegister() || decodedInst.isLoad() || decodedInst.isStore() || decodedInst.isVectorLoad() ||
                              decodedInst.isVectorStore() || decodedInst.isAmo();
    if ((!flush && execute) || (needsExecute && !isSerializing(pacPtr))) {
        ok = perfApi.execute(mHartIx, 0, tag);
        sparta_assert(ok, "Execute failure in execution driver");

        trap = trap or pacPtr->trapped();
    }

    bool traceRecordSuccess = populateTraceRecord(record, pacPtr);
    if (!traceRecordSuccess) {
        ILOG("[processNextRecordHelper] Failed to populate trace record for instruction " << tag);
        return nullptr;
    }

    if (flush) {
        ok = perfApi.flush(mHartIx, 0, tag);
        sparta_assert(ok, "Could not flush instruction from next record helper");
    }

    return pacPtr;
}

//==================== TraceRecord Helpers: Operands ====================

void ExecutionDriver::populateOperand(const TT_PERF::Operand &perfOperand, WhisperUtil::Operand &recordOperand, bool isDest) {
    (void)isDest;
    recordOperand.type = static_cast<WhisperUtil::OperandType>(perfOperand.type);
    recordOperand.number = recordOperand.type != WhisperUtil::OperandType::Imm ? perfOperand.number : 0;
    if (recordOperand.type == WhisperUtil::OperandType::Vec) {
        recordOperand.vecValue.assign(perfOperand.value.vec.rbegin(), perfOperand.value.vec.rend());
        recordOperand.vecPrevValue.assign(perfOperand.prevValue.vec.rbegin(), perfOperand.prevValue.vec.rend());
        recordOperand.emul = perfOperand.lmul;
    } else {
        recordOperand.value = perfOperand.value.scalar;
        recordOperand.prevValue = perfOperand.prevValue.scalar;
    }
}

template <size_t ArrayLen>
bool readCsrFromOperandArray(std::array<TT_PERF::Operand, ArrayLen> &operands, unsigned count, WdRiscv::CsrNumber csr, uint64_t &val) {
    val = 0;
    for (unsigned i = 0; i < count; i++) {
        if (operands.at(i).number == static_cast<unsigned>(csr)) {
            val = operands.at(i).value.scalar;
            return true;
        }
    }
    return false;
}

bool ExecutionDriver::populateRecordOperands(WhisperUtil::TraceRecord &record, const std::shared_ptr<TT_PERF::InstrPac> &pacPtr) {
    std::array<TT_PERF::Operand, 3> sourceOperands;
    std::array<TT_PERF::Operand, 2> destOperands;

    const auto srcCount = pacPtr->getSourceOperands(sourceOperands);
    const auto destCount = pacPtr->getDestOperands(destOperands);

    sparta_assert(srcCount <= 3, "[Execution Driver] source operand count: " << srcCount << " higher than expected");
    sparta_assert(destCount <= 2, "[Execution Driver] source operand count: " << destCount << " higher than expected");

    for (unsigned i = 0; i < srcCount; i++) {
        WhisperUtil::Operand operand;
        populateOperand(sourceOperands.at(i), operand, false);
        record.sourceOperands.emplace_back(operand);
    }

    for (unsigned i = 0; i < destCount; i++) {
        if (destOperands.at(i).type == WdRiscv::OperandType::Imm) continue;

        if (destOperands.at(i).type == WdRiscv::OperandType::IntReg && destOperands.at(i).number == 0) continue;

        if (destOperands.at(i).type == WdRiscv::OperandType::VecReg) {
            std::vector<TT_PERF::Operand> flattenedOperands;
            mPerfApiHandle->flattenOperand(destOperands.at(i), flattenedOperands);

            for (auto &it : flattenedOperands) {
                WhisperUtil::Operand operand;
                populateOperand(it, operand, true);
                record.modifiedRegs.emplace_back(operand);
            }
        } else {
            WhisperUtil::Operand operand;
            populateOperand(destOperands.at(i), operand, true);
            record.modifiedRegs.emplace_back(operand);
        }
    }

    if (pacPtr->decodedInst().isVector() && pacPtr->executed()) {
        std::array<TT_PERF::Operand, 4> implicitDestOperands;
        const auto numImplicitDestinations = pacPtr->getImplicitDestOperands(implicitDestOperands);

        for (unsigned i = 0; i < numImplicitDestinations; i++) {
            WhisperUtil::Operand operand;
            populateOperand(implicitDestOperands.at(i), operand, true);
            if (pacPtr->isVset()) {
                if ((operand.number == CSRLabel::vl_c || operand.number == CSRLabel::vtype_c)) record.modifiedRegs.emplace_back(operand);
            } else {
                if (operand.number == CSRLabel::vstart_c && operand.value != 0) record.modifiedRegs.emplace_back(operand);
            }
        }
    }

    return true;
}

//==================== TraceRecord Helpers: Memory ====================

uint32_t ExecutionDriver::getStoreConditionalDataSize(uint32_t instruction) {
    const unsigned sizeCode = (instruction >> SC_SIZE_CODE_SHIFT) & SC_SIZE_CODE_MASK;
    switch (sizeCode) {
        case SC_SIZE_1_BYTE:
            return 1;
        case SC_SIZE_2_BYTE:
            return 2;
        case SC_SIZE_4_BYTE:
            return 4;
        case SC_SIZE_8_BYTE:
            return 8;
        default:
            return 0;
    }
}

bool ExecutionDriver::populateRegularMemoryOps(WhisperUtil::TraceRecord &record, const std::shared_ptr<TT_PERF::InstrPac> &pacPtr) {
    if (pacPtr->executed() && !isSerializing(pacPtr) && !pacPtr->trapped() && !pacPtr->isSc() && !pacPtr->isCbo_zero()) {
        if (pacPtr->dataSize() == 0) {
            ILOG("[populateRegularMemoryOps] Non-serializing memory instruction does not have data size: " << record.inst << " assembly: " << record.assembly);
            return false;
        }
    }

    if (pacPtr->isSc()) {
        record.dataSize = getStoreConditionalDataSize(record.inst);
    } else {
        record.dataSize = pacPtr->dataSize();
    }

    record.virtAddrs.emplace_back(pacPtr->dataVa());
    record.physAddrs.emplace_back(pacPtr->dataPa());
    record.maskedAddrs.emplace_back(false);

    if (pacPtr->dataPa2() != 0 && pacPtr->dataPa2() != pacPtr->dataPa() && pacPtr->dataPa2() != pacPtr->dataVa()) {
        const auto page1 = get_page(pacPtr->dataVa());
        const auto page2 = get_page(pacPtr->dataVa() + pacPtr->dataSize() - 1);

        sparta_assert(page1 != page2, "[ExecutionDriver] Page crossing detected but page numbers are same: " << page1 << " == " << page2);
    }

    if (pacPtr->isStore()) {
        record.memVals.emplace_back(pacPtr->stData());
    }

    return true;
}

bool ExecutionDriver::populateVectorMemoryOps(WhisperUtil::TraceRecord &record, const std::shared_ptr<TT_PERF::InstrPac> &pacPtr) {
    if (pacPtr->executed() && !pacPtr->trapped() && !isSerializing(pacPtr)) {
        if (pacPtr->dataSize() == 0) {
            ILOG("[populateVectorMemoryOps] Vector load/store does not have data size: " << record.inst << " assembly: " << record.assembly
                                                                                         << " data size: " << pacPtr->dataSize());
            return false;
        }
    }

    const auto &vecDataAddrs = pacPtr->vecDataAddrs();
    const size_t vecSize = vecDataAddrs.size();

    record.virtAddrs.reserve(vecSize);
    record.physAddrs.reserve(vecSize);
    record.maskedAddrs.reserve(vecSize);

    record.dataSize = pacPtr->dataSize();

    for (const auto &vecDataAddr : vecDataAddrs) {
        record.virtAddrs.emplace_back(std::get<0>(vecDataAddr));
        record.physAddrs.emplace_back(std::get<1>(vecDataAddr));
        record.maskedAddrs.emplace_back(std::get<2>(vecDataAddr));
    }

    return true;
}

bool ExecutionDriver::populateRecordMemoryOps(WhisperUtil::TraceRecord &record, const std::shared_ptr<TT_PERF::InstrPac> &pacPtr) {
    if (!pacPtr->executed() || pacPtr->trapped()) {
        return false;
    }

    const bool isRegularMemoryOp = pacPtr->isLoad() || pacPtr->isStore() || pacPtr->isAmo() || pacPtr->isCbo_zero() || pacPtr->isSc();
    const bool isVectorMemoryOp = pacPtr->isVectorLoad() || pacPtr->isVectorStore();

    if (isRegularMemoryOp) {
        return populateRegularMemoryOps(record, pacPtr);
    }
    if (isVectorMemoryOp) {
        return populateVectorMemoryOps(record, pacPtr);
    }
    return true;
}

//==================== TraceRecord Helpers: Misc ====================

bool ExecutionDriver::populateTraceRecord(WhisperUtil::TraceRecord &record, const std::shared_ptr<TT_PERF::InstrPac> &pacPtr) {
    record.clear();

    const auto &dI = pacPtr->decodedInst();
    mHartPtr->disassembleInst(dI, record.assembly);
    std::ranges::replace(record.assembly, ';', ',');

    if (dI.hasRoundingMode()) {
        record.roundingMode = dI.roundingMode();
    }

    record.virtPc = pacPtr->instrVa();
    record.physPc = pacPtr->interupted() ? record.virtPc : pacPtr->instrPa();
    record.inst = dI.inst();
    record.instSize = pacPtr->instrSize();

    if (dI.isCall())
        record.instType = 'c';
    else if (dI.isReturn())
        record.instType = 'r';
    else if (dI.isConditionalBranch()) {
        if (pacPtr->executed()) {
            record.instType = pacPtr->isTakenBranch() ? 't' : 'n';
        } else {
            record.instType = 't';
        }
    } else if (dI.isUnconditionalBranch())
        record.instType = 'j';
    else if (dI.isVector()) {
        // Use DecodedInst for vector detection, pacPtr for load/store distinction
        if (pacPtr->isVectorLoad() || pacPtr->isVectorStore()) record.instType = 'v';
    } else if (dI.isLoad())
        record.instType = 'l';
    else if (dI.isStore())
        record.instType = 's';
    else if (dI.isAmo())
        record.instType = 'a';

    populateRecordOperands(record, pacPtr);

    if (static_cast<bool>(pacPtr->isBranch())) {
        if (static_cast<bool>(pacPtr->isBranchToRegister())) {
            sparta_assert(pacPtr->executed(), "Branch target population requires instruction to be executed (indirect branches)");
            if (pacPtr->isTakenBranch()) record.takenBranchTarget = pacPtr->nextPc();
        } else {
            if (pacPtr->isTakenBranch()) record.takenBranchTarget = pacPtr->branchTargetFromDecode();
        }
    }

    if (!pacPtr->trapped()) {
        bool memoryOpSuccess = populateRecordMemoryOps(record, pacPtr);
        if (pacPtr->executed() && !memoryOpSuccess) {
            ILOG("[populateTraceRecord] Failed to populate memory operations for instruction: " << record.inst << " assembly: " << record.assembly);
            record.inst = 0;
            return false;
        }
    }

    return true;
}

//==================== Termination ====================

void ExecutionDriver::simulationTerminating_() {
    ILOG("[simulationTerminating_] Got notified simulation is terminating");
    SPARTA_ASSERT_CLK((mSetupDone && mHartPtr->getInstructionCountLimit()) || mAllowEarlyTermination || !mHartPtr || mHartPtr->hasTargetProgramFinished(),
                      "Execution Driver has additional unprocessed records");
}

uint64_t ExecutionDriver::flushInstruction(uint64_t tag) {
    if (!mSetupDone) {
        return 0;
    }

    // Convert model tag to perfApi tag (same conversion as retireInstruction)
    const auto perfApiTag = tag + mSnapshotOffset + 1;

    ILOG("[flushInstruction] Driver " << static_cast<uint32_t>(mId) << " flushing model tag " << tag << " (perfApiTag=" << perfApiTag << ") at time "
                                      << getClock()->currentCycle());

    auto &perfApi = *mPerfApiHandle;
    const auto pacPtr = perfApi.getInstructionPacket(mHartIx, perfApiTag);

    // Flush invalidates speculative state, including any program-exit detection
    // from nextPc >= endPc that may have set mFinishedOverride during speculation
    mFinishedOverride = false;

    // Clear the wrong-path invalid state if the flush removes the illegal instruction
    // that armed it (it is always the youngest fetched, so any recovery flush covers it).
    if (mDriverInvalidState && mInvalidStateTag >= tag) {
        ILOG("[flushInstruction] Clearing driver invalid state (illegal tag " << mInvalidStateTag << " flushed by tag " << tag << ")");
        mDriverInvalidState = false;
    }

    uint64_t numInstsFlushed = 0;
    if (pacPtr) {
        bool ok = perfApi.flush(mHartIx, 0, perfApiTag);
        sparta_assert(ok, "[Execution Driver] Flush for inst id " << perfApiTag << " Failed in whisper");

        numInstsFlushed = mSequence - perfApiTag;  // flushing all instructions newer than tag
        mSequence = perfApiTag;                    // reset sequence number
    } else {
        ILOG("[flushInstruction] No packet found for perfApiTag " << perfApiTag << ", nothing to flush");
    }
    ILOG("[flushInstruction] Flushed perfApiTag " << perfApiTag << " num instructions flushed " << numInstsFlushed);

    // Get packet with the largest tag in the range [0, perfApiTag - 1]
    // This is the last packet before the flush, which will be used to set mNextPc
    // If no packet is found, use the hart's peekPc as mNextPc
    if (perfApi.getInstructionPacketCount(mHartIx) > 0) {
        bool found = false;
        auto lastValidTag = perfApiTag - 1;
        // Add bounds check: don't search below mRetireSequence (already retired instructions)
        const auto lowerBound = mRetireSequence > 0 ? mRetireSequence : 1;
        while (!found && lastValidTag >= lowerBound) {
            auto lastValidPacPtr = perfApi.getInstructionPacket(mHartIx, lastValidTag);
            if (lastValidPacPtr) {
                found = true;
                mNextPc = lastValidPacPtr->nextPc();
                ILOG("[flushInstruction] Found last packet before flush: " << std::hex << lastValidPacPtr->instrVa() << " nextPc: " << lastValidPacPtr->nextPc()
                                                                           << std::dec);
            }
            lastValidTag--;
        }
        if (!found) {
            ILOG("[flushInstruction] No valid packet found in range, using hart's peekPc: " << std::hex << mHartPtr->peekPc() << std::dec);
            mNextPc = mHartPtr->peekPc();
        }
    } else {
        ILOG("[flushInstruction] Last packet before flush not found, using hart's peekPc: " << std::hex << mHartPtr->peekPc() << std::dec);
        mNextPc = mHartPtr->peekPc();
    }

    mNextPcSetInstId = mSequence;  // set the next instruction's id to the current sequence number
    mNextPcSet = true;             // nextPc is set after flush

    ILOG("[flushInstruction] Flush complete, mNextPc: " << std::hex << mNextPc << " next fetch instruction id: " << std::dec << mSequence);

    return numInstsFlushed;
}

bool ExecutionDriver::flushInstruction(InstPtr &inst) {
    const auto modelTag = inst->getId().getInstNum();

    auto &perfApi = *mPerfApiHandle;
    const auto pacPtr = perfApi.getInstructionPacket(mHartIx, modelTag);
    sparta_assert(pacPtr, "[Execution Driver] instruction being flushed, tag " << modelTag << " must be valid in perfApi");

    bool ok = perfApi.flush(mHartIx, 0, modelTag);
    sparta_assert(ok, "[Execution Driver] Flush for inst id " << modelTag << " Failed in whisper");

    return true;
}

}  // namespace cpu
