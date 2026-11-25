// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Tenstorrent USA, Inc.

#include "Frontend/FetchStructures/FetchStructures.hpp"

#include <algorithm>
#include <bit>
#include <iostream>

#include "Common/SpeculationUtils.hpp"
#include "Frontend/FrontendMemoryStructures/FrontendMemoryStructures.hpp"
#include "Logging.hpp"
#include "Mmu/Mmu.hpp"
#include "models/cpu/common/ExecutionDriver.hpp"
#include "models/cpu/common/whisper_include_fix.hpp"

namespace frontend {

namespace {

// Decode RISC-V branch/jump instruction to compute target address from encoding.
// This allows us to get the branch target even for not-taken branches (where
// takenBranchTarget from Whisper's trace would be 0).
// Returns the computed target, or 0 if unable to decode (e.g., JALR needs register value)
uint64_t decodeBranchTarget(uint64_t pc, uint32_t inst) {
    uint32_t opcode = inst & 0x7F;

    // B-type (conditional branches): BEQ, BNE, BLT, BGE, BLTU, BGEU
    // Opcode: 0x63
    if (opcode == 0x63) {
        // Immediate bits: [31]=imm[12], [30:25]=imm[10:5], [11:8]=imm[4:1], [7]=imm[11]
        int32_t imm12 = (inst >> 31) & 0x1;
        int32_t imm10_5 = (inst >> 25) & 0x3F;
        int32_t imm4_1 = (inst >> 8) & 0xF;
        int32_t imm11 = (inst >> 7) & 0x1;

        int32_t imm = (imm12 << 12) | (imm11 << 11) | (imm10_5 << 5) | (imm4_1 << 1);
        // Sign extend from bit 12
        if (imm12) imm |= 0xFFFFE000;

        return pc + imm;
    }

    // JAL (J-type): opcode 0x6F
    if (opcode == 0x6F) {
        // Immediate bits: [31]=imm[20], [30:21]=imm[10:1], [20]=imm[11], [19:12]=imm[19:12]
        int32_t imm20 = (inst >> 31) & 0x1;
        int32_t imm10_1 = (inst >> 21) & 0x3FF;
        int32_t imm11 = (inst >> 20) & 0x1;
        int32_t imm19_12 = (inst >> 12) & 0xFF;

        int32_t imm = (imm20 << 20) | (imm19_12 << 12) | (imm11 << 11) | (imm10_1 << 1);
        // Sign extend from bit 20
        if (imm20) imm |= 0xFFE00000;

        return pc + imm;
    }

    // JALR (I-type): opcode 0x67
    // Cannot decode statically - target depends on register value
    // Return 0 to indicate we need to use takenBranchTarget from trace
    if (opcode == 0x67) {
        return 0;
    }

    // C.J (RVC): opcode pattern 101 01 (bits [15:13]=101, [1:0]=01)
    if ((inst & 0xE003) == 0xA001) {
        // 16-bit compressed jump
        int32_t imm11 = (inst >> 12) & 0x1;
        int32_t imm4 = (inst >> 11) & 0x1;
        int32_t imm9_8 = (inst >> 9) & 0x3;
        int32_t imm10 = (inst >> 8) & 0x1;
        int32_t imm6 = (inst >> 7) & 0x1;
        int32_t imm7 = (inst >> 6) & 0x1;
        int32_t imm3_1 = (inst >> 3) & 0x7;
        int32_t imm5 = (inst >> 2) & 0x1;

        int32_t imm = (imm11 << 11) | (imm10 << 10) | (imm9_8 << 8) | (imm7 << 7) | (imm6 << 6) | (imm5 << 5) | (imm4 << 4) | (imm3_1 << 1);
        if (imm11) imm |= 0xFFFFF000;

        return pc + imm;
    }

    // C.BEQZ/C.BNEZ (RVC): opcode patterns 110 01 and 111 01
    if ((inst & 0xC003) == 0xC001) {
        // 16-bit compressed branch
        int32_t imm8 = (inst >> 12) & 0x1;
        int32_t imm4_3 = (inst >> 10) & 0x3;
        int32_t imm7_6 = (inst >> 5) & 0x3;
        int32_t imm2_1 = (inst >> 3) & 0x3;
        int32_t imm5 = (inst >> 2) & 0x1;

        int32_t imm = (imm8 << 8) | (imm7_6 << 6) | (imm5 << 5) | (imm4_3 << 3) | (imm2_1 << 1);
        if (imm8) imm |= 0xFFFFFE00;

        return pc + imm;
    }

    // C.JAL (RV32 only, RVC): opcode pattern 001 01
    if ((inst & 0xE003) == 0x2001) {
        // Same encoding as C.J but different opcode
        int32_t imm11 = (inst >> 12) & 0x1;
        int32_t imm4 = (inst >> 11) & 0x1;
        int32_t imm9_8 = (inst >> 9) & 0x3;
        int32_t imm10 = (inst >> 8) & 0x1;
        int32_t imm6 = (inst >> 7) & 0x1;
        int32_t imm7 = (inst >> 6) & 0x1;
        int32_t imm3_1 = (inst >> 3) & 0x7;
        int32_t imm5 = (inst >> 2) & 0x1;

        int32_t imm = (imm11 << 11) | (imm10 << 10) | (imm9_8 << 8) | (imm7 << 7) | (imm6 << 6) | (imm5 << 5) | (imm4 << 4) | (imm3_1 << 1);
        if (imm11) imm |= 0xFFFFF000;

        return pc + imm;
    }

    return 0;
}

// Classifies a branch from its encoding. RAS actions follow the RISC-V unprivileged spec's JAL/JALR
// hint table: rd a link register (x1 or x5) pushes, rs1 one pops, and both, if different, pop then push.
predictors::BranchType branchType(uint32_t inst) {
    using predictors::BranchType;
    auto link = [](uint32_t r) { return r == 1 || r == 5; };
    uint32_t rd = 0;
    uint32_t rs1 = 0;
    if ((inst & 3) != 3) {
        if ((inst & 0xE003) == 0xA001) return BranchType::DirectJump;   // C.J
        if ((inst & 0xE003) == 0x2001) return BranchType::Call;         // C.JAL (RV32)
        if ((inst & 0xE07F) != 0x8002) return BranchType::Conditional;  // C.BEQZ, C.BNEZ
        rd = (inst >> 12) & 1;                                          // C.JR writes x0, C.JALR x1
        rs1 = (inst >> 7) & 0x1F;
    } else {
        if ((inst & 0x7F) == 0x63) return BranchType::Conditional;
        rd = (inst >> 7) & 0x1F;
        if ((inst & 0x7F) == 0x6F) return link(rd) ? BranchType::Call : BranchType::DirectJump;
        rs1 = (inst >> 15) & 0x1F;
    }
    if (link(rd) && link(rs1) && rd != rs1) return BranchType::CoroutineSwap;
    if (link(rd)) return BranchType::IndirectCall;
    return link(rs1) ? BranchType::Return : BranchType::IndirectJump;
}

}  // anonymous namespace

FetchStructures::FetchStructures(sparta::TreeNode* node, const FetchStructuresParams* params)
    : sparta::Unit(node),
      mPc(params->initial_pc),
      mFetchWidth(params->fetch_width),
      mFetchBufferCapacity(params->fetch_buffer_capacity),
      mMaxBranchesPerFetch(params->max_branches_per_fetch),
      mNumFetched(&unit_stat_set_, "num_fetched", "Total instructions fetched", sparta::Counter::COUNT_NORMAL),
      mNumBufferFullStallCycles(&unit_stat_set_, "num_buffer_full_stall_cycles",
                                "Cycles stalled because fetched packets filled the FetchQueue (with bypass_queues, Decode's buffer)",
                                sparta::Counter::COUNT_NORMAL),
      mNumRedirects(&unit_stat_set_, "num_redirects", "Pipeline redirects received", sparta::Counter::COUNT_NORMAL),
      mNumMispredPenaltyStalls(&unit_stat_set_, "num_mispred_penalty_stalls",
                               "Cycles fetch waited for a mispredicted branch to resolve, plus misprediction_penalty_cycles (wrong_path_enabled false)",
                               sparta::Counter::COUNT_NORMAL),
      mNumBranchLimitEnds(&unit_stat_set_, "num_branch_limit_ends", "Fetch groups ended by max_branches_per_fetch alone", sparta::Counter::COUNT_NORMAL),
      mNumTakenBubbleCycles(&unit_stat_set_, "num_taken_bubble_cycles", "Cycles stalled before a predicted-taken branch steered fetch",
                            sparta::Counter::COUNT_NORMAL) {
    sparta_assert(mMaxBranchesPerFetch > 0, params->max_branches_per_fetch.getLocation() << " must be at least 1");
    sparta_assert(std::has_single_bit(mFetchWidth), params->fetch_width.getLocation() << " must be a power of two");
    response_in.registerConsumerHandler(CREATE_SPARTA_HANDLER_WITH_DATA(FetchStructures, receiveResponse, core::FetchResponse));
    bp_redirect_in.registerConsumerHandler(CREATE_SPARTA_HANDLER_WITH_DATA(FetchStructures, receiveBpRedirect_, core::PredictedRedirect));
    mispred_redirect_in.registerConsumerHandler(CREATE_SPARTA_HANDLER_WITH_DATA(FetchStructures, receiveMispredRedirect_, core::BranchRedirect));
    branch_resolved_in.registerConsumerHandler(CREATE_SPARTA_HANDLER_WITH_DATA(FetchStructures, receiveBranchResolved_, core::BranchResolved));
    taken_bubble_in.registerConsumerHandler(CREATE_SPARTA_HANDLER_WITH_DATA(FetchStructures, receiveTakenBubble_, uint32_t));
}

void FetchStructures::tick() {
    uint64_t cycle = getClock()->currentCycle();

    if (!mDriver || mDriver->isFinished()) {
        ILOG("[fetch] cycle " << cycle << " driver not ready or finished");
        return;
    }

    // A misprediction redirect arrives in the cycle the branch resolves and takes effect the next.
    if (mMispredRedirectCycle == cycle) return;

    // Wrong path off: fetch waits for a mispredicted branch to resolve, then the extra penalty.
    if (mAwaitingResolve) {
        ++mNumMispredPenaltyStalls;
        return;
    }
    if (mMispredStallCycles > 0) {
        --mMispredStallCycles;
        ++mNumMispredPenaltyStalls;
        ILOG("[fetch] cycle " << cycle << " STALLED: misprediction penalty (" << mMispredStallCycles << " cycles remaining)");
        return;
    }

    if (mTakenBubbleCycles > 0) {
        --mTakenBubbleCycles;
        ++mNumTakenBubbleCycles;
        return;
    }

    // Stall wrong-path fetch when the driver is in the invalid state: a previous
    // speculative fetch decoded an illegal instruction. Resume only after a flush clears it.
    if (mOnSpeculativePath && mDriver->isDriverInInvalidState()) {
        ILOG("[fetch] cycle " << cycle << " STALLED: driver in invalid state (wrong-path illegal), waiting for flush");
        return;
    }

    // Stall fetch while a serializing instruction is draining the pipeline (only on correct path)
    if (!mOnSpeculativePath && !mDriver->isNextPcAvailable()) {
        ILOG("[fetch] cycle " << cycle << " STALLED: waiting for serializing instruction to retire");
        return;
    }

    // Determine base PC - use speculative PC when on predicted path
    uint64_t base_pc;
    if (mOnSpeculativePath) {
        base_pc = mSpeculativePc;
        ILOG("[fetch] cycle " << cycle << " SPECULATIVE fetch from pc=0x" << std::hex << base_pc << std::dec);
    } else {
        base_pc = mFirstFetch ? mDriver->getInitPc() : mDriver->getNextPc();
    }
    mFirstFetch = false;

    if (mCache && mCache->downstreamFull()) {
        ++mNumBufferFullStallCycles;
        return;
    }

    // Stall fetch if I-Cache MSHRs are saturated. The cache is physically addressed; fetchPa uses the
    // hart's current mode, which may not be this fetch's, so it serves only this check, not the request.
    if (mCache) {
        if (base_pc != mPrecheckVa) {
            mPrecheckVa = base_pc;
            mPrecheckPa = mDriver->fetchPa(base_pc);
        }
        if (!mCache->canAcceptRequest(mPrecheckPa)) {
            ILOG("[fetch] cycle " << getClock()->currentCycle() << " STALLED: I-Cache MSHRs full");
            return;
        }
    }

    if (mMmu && !mMmu->fetchReady(base_pc)) {
        ILOG("[fetch] cycle " << cycle << " STALLED: ITLB miss");
        return;
    }

    std::vector<core::PipelinePacket> packets;
    uint32_t num_branches = 0;
    // A group ends at the next fetch_width-aligned boundary, so it never crosses an I-cache line (BOOM nextFetch).
    const uint64_t fetch_end = (base_pc & ~static_cast<uint64_t>(mFetchWidth - 1)) + mFetchWidth;

    while (!mDriver->isFinished()) {
        // Determine fetch PC - use speculative PC when on predicted path
        uint64_t fetch_pc = mOnSpeculativePath ? mSpeculativePc : mDriver->getNextPc();

        if (fetch_pc == 0 || fetch_pc >= cpu::ExecutionDriver::endPc) break;

        cpu::InstPtr inst = mDriver->peekInstruction(fetch_pc, mOnSpeculativePath);
        if (!inst) break;

        if (!mDriver->executeInstruction(inst, mOnSpeculativePath)) break;

        // If we're on speculative path, restore our speculative PC
        // (executeInstruction overwrites mNextPc with actual next PC, but we want to continue on predicted path)
        // We'll update mSpeculativePc after processing the instruction

        core::PipelinePacket pkt;
        pkt.pc = inst->getFetchVA();
        pkt.inst = inst;
        pkt.tag = inst->getId().getInstNum();
        pkt.fetch_seq = mFetchSeq++;  // Unique visualization tag (never reset)
        pkt.inst_class = inst->getInstClass();
        pkt.size = static_cast<uint8_t>(pkt.inst->getTraceRecord().instSize);
        pkt.wrong_path_depth = mCurrentWrongPathDepth;  // Track speculation depth

        // Send branch prediction request IN PARALLEL with ICache request
        const bool is_branch = (pkt.inst_class == cpu::InstClass::Branch || pkt.inst_class == cpu::InstClass::Jump);
        if (is_branch) {
            ++num_branches;
            const auto& rec = pkt.inst->getTraceRecord();
            core::BranchPrediction pred_req;
            pred_req.tag = pkt.tag;
            pred_req.fetch_seq = pkt.fetch_seq;  // Use fetch_seq for prediction keying (never reused)
            pred_req.pc = pkt.pc;
            pred_req.inst_size = pkt.size;
            pred_req.wrong_path_depth = mCurrentWrongPathDepth;

            // Decode branch target from instruction encoding - this gives us the target
            // even for not-taken branches (where takenBranchTarget would be 0)
            uint64_t decoded_target = decodeBranchTarget(pkt.pc, rec.inst);

            // Use decoded target if available, otherwise fall back to Whisper's takenBranchTarget
            // (JALR can't be decoded statically - needs register values from execution)
            if (decoded_target != 0) {
                pred_req.target_pc = decoded_target;
            } else if (rec.takenBranchTarget != 0) {
                pred_req.target_pc = rec.takenBranchTarget;  // Fallback for JALR
            } else {
                pred_req.target_pc = pkt.pc + pkt.size;  // Last resort: sequential
            }

            // Initial prediction based on actual outcome from Whisper's trace
            pred_req.predicted_taken = (rec.takenBranchTarget != 0);
            pred_req.type = branchType(rec.inst);

            predict_request_out.send(pred_req, 0);

            ILOG("[fetch] branch tag=" << pkt.tag << " pc=0x" << std::hex << pkt.pc << " decoded_target=0x" << decoded_target << " trace_target=0x"
                                       << rec.takenBranchTarget);
        }

        ILOG("[fetch] tag=" << pkt.tag << " pc=0x" << std::hex << pkt.pc << std::dec << " depth=" << static_cast<int>(pkt.wrong_path_depth)
                            << " speculative=" << mOnSpeculativePath);
        if (mVis)
            mVis->onFetch(pkt.fetch_seq, pkt.pc, getClock()->currentCycle(), pkt.inst_class, inst->getTraceRecord().assembly, pkt.wrong_path_depth, pkt.tag);
        ++mNumFetched;

        bool is_serializing = pkt.inst->isPerfApiSerializing();

        // Update speculative PC for next fetch (sequential advance)
        // BP will send a redirect if a branch is predicted taken
        const uint64_t taken_target = pkt.inst->getTraceRecord().takenBranchTarget;
        if (mOnSpeculativePath) {
            mSpeculativePc = (mFollowWrongPathBranches && taken_target != 0) ? taken_target : fetch_pc + pkt.size;
        }

        const bool taken = (taken_target != 0) && (!mOnSpeculativePath || mFollowWrongPathBranches);
        packets.push_back(std::move(pkt));

        if (is_serializing || (!mOnSpeculativePath && !mDriver->isNextPcAvailable())) {
            break;
        }

        // A fetch block ends at its first taken branch.
        uint64_t next_pc = mOnSpeculativePath ? mSpeculativePc : mDriver->getNextPc();
        const bool block_ends = taken || next_pc <= fetch_pc || next_pc >= fetch_end;
        if (is_branch && num_branches == mMaxBranchesPerFetch) {
            if (!block_ends) ++mNumBranchLimitEnds;
            break;
        }
        if (block_ends) break;
    }

    if (!packets.empty()) {
        sendRequest(base_pc, std::move(packets));
    }
}

void FetchStructures::receiveResponse(const core::FetchResponse& response) {
    if (response.miss && mVis) {
        ILOG("[fetch] I-Cache miss for pc=0x" << std::hex << response.base_pc << std::dec);
    }
}

void FetchStructures::sendRequest(uint64_t base_pc, std::vector<core::PipelinePacket>&& packets) {
    uint64_t cycle = getClock()->currentCycle();
    ILOG("[fetch] cycle " << cycle << " SENDING " << packets.size() << " packets to ICache");

    core::FetchRequest req;
    // The I-cache is physically addressed; base_pc is the first packet's PC.
    const uint64_t pa = packets.empty() ? 0 : packets.front().inst->getTraceRecord().physPc;
    req.base_pc = pa ? pa : base_pc;
    req.size_bytes = mFetchWidth;
    req.packets = std::move(packets);
    request_out.send(req, 1);
}

// A request can hold the redirecting branch and older instructions as well as squashed ones; the
// survivors are re-sent. Redirects land before Fetch ticks, so a request still on the port is due
// this cycle and the re-send must arrive before the I-cache ticks and Fetch checks its MSHRs.
void FetchStructures::cancelInFlight_(uint64_t branch_tag, uint8_t branch_depth, core::FlushSource source) {
    const uint64_t cycle = getClock()->currentCycle();
    std::vector<core::FetchRequest> survivors;
    const uint32_t cancelled = request_out.cancelIf([&](const core::FetchRequest& req) -> bool {
        core::FetchRequest kept = req;
        kept.packets.clear();
        for (const auto& pkt : req.packets) {
            if (core::shouldSquash(pkt.tag, pkt.wrong_path_depth, branch_tag, branch_depth, source)) {
                ILOG("[fetch] cycle " << cycle << " CANCELLING in-flight tag=" << pkt.tag << " depth=" << (int)pkt.wrong_path_depth);
                if (mVis) mVis->onSquash(pkt.fetch_seq, cycle);
            } else {
                kept.packets.push_back(pkt);
            }
        }
        if (kept.packets.size() == req.packets.size()) return false;
        if (!kept.packets.empty()) survivors.push_back(std::move(kept));
        return true;
    });
    for (auto& req : survivors) request_out.send(req, 0);
    if (cancelled > 0) ILOG("[fetch] cycle " << cycle << " cancelled " << cancelled << " in-flight requests, re-sent " << survivors.size());
}

void FetchStructures::receiveBpRedirect_(const core::PredictedRedirect& redirect) {
    uint64_t cycle = getClock()->currentCycle();

    // Wrong path off: drop what was fetched past the branch and wait for it to resolve.
    if (!mSpecCfg->enabled) {
        if (mDriver) mDriver->flushInstruction(redirect.branch_tag + 1);
        cancelInFlight_(redirect.branch_tag, mCurrentWrongPathDepth, core::FlushSource::BranchPredictor);
        mAwaitingResolve = true;
        ILOG("[fetch] cycle " << cycle << " HOLD until branch_tag=" << redirect.branch_tag << " resolves");
        return;
    }

    // Enforce max_depth - don't speculate beyond max_depth
    if (mCurrentWrongPathDepth >= mSpecCfg->max_depth) {
        ILOG("[fetch] cycle " << cycle << " BLOCKING BP redirect: max_depth=" << (int)mSpecCfg->max_depth
                              << " reached, current_depth=" << (int)mCurrentWrongPathDepth);
        return;  // Don't speculate further, stay on current path
    }

    ++mNumRedirects;

    // The branch was at the current depth before we increment
    uint8_t branch_depth = mCurrentWrongPathDepth;

    // Flush Whisper/ExecutionDriver from branch_tag+1 onwards
    // This must happen at the same time as Fetch receives the redirect to keep them synchronized
    if (mDriver) {
        uint64_t num_flushed = mDriver->flushInstruction(redirect.branch_tag + 1);
        ILOG("[fetch] Flushed " << num_flushed << " instructions from ExecutionDriver (tags >= " << (redirect.branch_tag + 1) << ")");
    }

    // Cancel any in-flight packets to ICache that should be squashed
    // (packets younger than the branch at the same or deeper depth)
    cancelInFlight_(redirect.branch_tag, branch_depth, core::FlushSource::BranchPredictor);

    // Increment speculation depth - we're now fetching on a predicted path
    ++mCurrentWrongPathDepth;

    // Enable speculative fetching mode and set target PC
    mOnSpeculativePath = true;
    mSpeculativePc = redirect.target_pc;

    // Track which branch caused this speculation level (for BranchResolved filtering)
    mCurrentSpecBranchTag = redirect.branch_tag;

    ILOG("[fetch] cycle " << cycle << " BP redirect: branch_tag=" << redirect.branch_tag << " target_pc=0x" << std::hex << redirect.target_pc << std::dec
                          << " NEW depth=" << static_cast<int>(mCurrentWrongPathDepth) << " speculative_pc=0x" << std::hex << mSpeculativePc);
}

void FetchStructures::receiveMispredRedirect_(const core::BranchRedirect& redirect) {
    uint64_t cycle = getClock()->currentCycle();

    // A retire-time trap/interrupt redirect resolves to the handler path: no misprediction penalty.
    if (redirect.authoritative) {
        ILOG("[fetch] cycle " << cycle << " AUTHORITATIVE redirect (retire trap/interrupt): branch_tag=" << redirect.branch_tag
                              << " fetch_seq=" << redirect.branch_fetch_seq << " target=0x" << std::hex << redirect.correct_target_pc << std::dec);
        mMispredStallCycles = 0;
    }
    if (!mSpecCfg->enabled && !redirect.authoritative) mMispredStallCycles = mMispredictionPenalty;
    mAwaitingResolve = false;

    ++mNumRedirects;
    mTakenBubbleCycles = 0;  // the redirect restarts the front end
    mMispredRedirectCycle = cycle;

    // Flush Whisper/ExecutionDriver from branch_tag+1 onwards
    // This must happen at the same time as Fetch receives the redirect to keep them synchronized
    if (mDriver) {
        uint64_t num_flushed = mDriver->flushInstruction(redirect.branch_tag + 1);
        ILOG("[fetch] Flushed " << num_flushed << " instructions from ExecutionDriver (tags >= " << (redirect.branch_tag + 1) << ")");
    }

    // Cancel any in-flight packets to ICache that should be squashed
    // (packets on the wrong path that need to be flushed due to misprediction recovery)
    cancelInFlight_(redirect.branch_tag, redirect.wrong_path_depth, core::FlushSource::Execute);

    // When a branch at depth D mispredicts, we return to depth D (the branch itself was at that depth)
    mCurrentWrongPathDepth = redirect.wrong_path_depth;

    // If we're returning to depth 0, we're back on the correct path
    if (mCurrentWrongPathDepth == 0) {
        mOnSpeculativePath = false;
        mCurrentSpecBranchTag = 0;  // Clear spec tracking since we're on correct path
    }
    // Update speculative PC to the correct target
    mSpeculativePc = redirect.correct_target_pc;

    ILOG("[fetch] cycle " << cycle << " misprediction redirect: branch_tag=" << redirect.branch_tag << " correct_target=0x" << std::hex
                          << redirect.correct_target_pc << std::dec << " branch_depth=" << static_cast<int>(redirect.wrong_path_depth)
                          << " NEW depth=" << static_cast<int>(mCurrentWrongPathDepth) << " on_speculative=" << mOnSpeculativePath);

    // Redirect fetch to the correct target after misprediction recovery
    if (mDriver) {
        mDriver->setNextPc(redirect.correct_target_pc);
    }
}

void FetchStructures::receiveTakenBubble_(const uint32_t& cycles) { mTakenBubbleCycles = std::max(mTakenBubbleCycles, cycles); }

void FetchStructures::receiveBranchResolved_(const core::BranchResolved& resolved) {
    uint64_t cycle = getClock()->currentCycle();

    // BranchResolved should only decrement depth if:
    // 1. The resolved branch matches the one that CAUSED the current speculation
    // 2. The resolved depth is at the immediately preceding speculation level
    //
    // Multiple branches can be fetched at depth 0 before any bp_redirect.
    // When a bp_redirect for branch X creates depth=1, a BranchResolved for a
    // DIFFERENT branch Y (also at depth 0) should NOT decrement depth.
    // Only a BranchResolved for branch X should decrement the depth it created.
    // mCurrentSpecBranchTag tracks which branch caused the current speculation.
    if (mCurrentWrongPathDepth > 0 && resolved.resolved_depth == mCurrentWrongPathDepth - 1 && resolved.branch_tag == mCurrentSpecBranchTag) {
        uint8_t old_depth = mCurrentWrongPathDepth;
        mCurrentWrongPathDepth = resolved.resolved_depth;

        // If we return to depth 0, we're no longer speculative
        if (mCurrentWrongPathDepth == 0) {
            mOnSpeculativePath = false;
            mCurrentSpecBranchTag = 0;  // Clear tracking since we're no longer speculating
        }

        ILOG("[fetch] cycle " << cycle << " BRANCH RESOLVED CORRECTLY: tag=" << resolved.branch_tag << " fetch_seq=" << resolved.branch_fetch_seq << " depth "
                              << static_cast<int>(old_depth) << " -> " << static_cast<int>(mCurrentWrongPathDepth));
    } else if (mCurrentWrongPathDepth > 0 && resolved.branch_tag != mCurrentSpecBranchTag) {
        // Branch resolved is NOT the one that caused the current speculation - ignore it
        ILOG("[fetch] cycle " << cycle << " IGNORING BranchResolved for non-speculative branch: tag=" << resolved.branch_tag
                              << " (current spec branch=" << mCurrentSpecBranchTag << ")"
                              << " resolved_depth=" << static_cast<int>(resolved.resolved_depth)
                              << " current_depth=" << static_cast<int>(mCurrentWrongPathDepth));
    } else if (mCurrentWrongPathDepth > resolved.resolved_depth) {
        // Branch resolved at a shallower depth than (current - 1)
        // This is a stale resolution from an earlier speculation level - ignore it
        ILOG("[fetch] cycle " << cycle << " IGNORING stale BranchResolved: tag=" << resolved.branch_tag << " resolved_depth="
                              << static_cast<int>(resolved.resolved_depth) << " current_depth=" << static_cast<int>(mCurrentWrongPathDepth)
                              << " (expected resolved_depth=" << static_cast<int>(mCurrentWrongPathDepth - 1) << ")");
    }
}

}  // namespace frontend
