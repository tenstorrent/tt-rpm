// SPDX-FileCopyrightText: © 2026 Tenstorrent AI ULC
//
// SPDX-License-Identifier: Apache-2.0

#include "Frontend/FetchStructures/FetchStructures.hpp"

#include <iostream>

#include "Common/SpeculationConfig.hpp"
#include "Common/SpeculationUtils.hpp"
#include "Frontend/FrontendMemoryStructures/FrontendMemoryStructures.hpp"
#include "Logging.hpp"
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

}  // anonymous namespace

FetchStructures::FetchStructures(sparta::TreeNode* node, const FetchStructuresParams* params)
    : sparta::Unit(node),
      mPc(params->initial_pc),
      mFetchWidth(params->fetch_width),
      mFetchBufferCapacity(params->fetch_buffer_capacity),
      mNumFetched(&unit_stat_set_, "num_fetched", "Total instructions fetched", sparta::Counter::COUNT_NORMAL),
      mNumBufferFullStallCycles(&unit_stat_set_, "num_buffer_full_stall_cycles", "Cycles stalled due to full fetch buffer", sparta::Counter::COUNT_NORMAL),
      mNumRedirects(&unit_stat_set_, "num_redirects", "Pipeline redirects received", sparta::Counter::COUNT_NORMAL),
      mNumMispredPenaltyStalls(&unit_stat_set_, "num_mispred_penalty_stalls", "Cycles stalled due to misprediction penalty", sparta::Counter::COUNT_NORMAL) {
    response_in.registerConsumerHandler(CREATE_SPARTA_HANDLER_WITH_DATA(FetchStructures, receiveResponse, core::FetchResponse));
    bp_redirect_in.registerConsumerHandler(CREATE_SPARTA_HANDLER_WITH_DATA(FetchStructures, receiveBpRedirect_, core::PredictedRedirect));
    mispred_redirect_in.registerConsumerHandler(CREATE_SPARTA_HANDLER_WITH_DATA(FetchStructures, receiveMispredRedirect_, core::BranchRedirect));
    branch_resolved_in.registerConsumerHandler(CREATE_SPARTA_HANDLER_WITH_DATA(FetchStructures, receiveBranchResolved_, core::BranchResolved));
}

void FetchStructures::tick() {
    uint64_t cycle = getClock()->currentCycle();

    if (!mDriver || mDriver->isFinished()) {
        ILOG("[fetch] cycle " << cycle << " driver not ready or finished");
        return;
    }

    // Stall fetch for misprediction penalty (used when wrong_path_enabled=false)
    if (mMispredStallCycles > 0) {
        --mMispredStallCycles;
        ++mNumMispredPenaltyStalls;
        ILOG("[fetch] cycle " << cycle << " STALLED: misprediction penalty (" << mMispredStallCycles << " cycles remaining)");
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

    // Stall fetch if I-Cache MSHRs are saturated
    if (mCache && !mCache->canAcceptRequest(base_pc)) {
        ILOG("[fetch] cycle " << getClock()->currentCycle() << " STALLED: I-Cache MSHRs full");
        return;
    }

    std::vector<core::PipelinePacket> packets;

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
        if (pkt.inst_class == cpu::InstClass::Branch || pkt.inst_class == cpu::InstClass::Jump) {
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
        if (mOnSpeculativePath) {
            mSpeculativePc = fetch_pc + pkt.size;
        }

        packets.push_back(std::move(pkt));

        if (is_serializing || (!mOnSpeculativePath && !mDriver->isNextPcAvailable())) {
            break;
        }

        // Check if we should continue fetching
        uint64_t next_pc = mOnSpeculativePath ? mSpeculativePc : mDriver->getNextPc();
        if (next_pc <= fetch_pc || next_pc >= base_pc + mFetchWidth) break;
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
    req.base_pc = base_pc;
    req.size_bytes = mFetchWidth;
    req.packets = std::move(packets);
    request_out.send(req, 1);
}

void FetchStructures::receiveBpRedirect_(const core::PredictedRedirect& redirect) {
    uint64_t cycle = getClock()->currentCycle();

    // If speculation is disabled, ignore all BP redirects - they would corrupt Whisper state
    if (!core::getSpeculationConfig().enabled) {
        ILOG("[fetch] cycle " << cycle << " IGNORING BP redirect: speculation disabled");
        return;
    }

    // Ignore stale BP redirects from squashed branches. After a mispred recovery,
    // squash_threshold_fetch_seq holds the fetch_seq counter at that recovery — anything
    // older is from wrong-path instructions.
    if (mStaleFilter.squash_threshold_fetch_seq > 0 && redirect.branch_fetch_seq < mStaleFilter.squash_threshold_fetch_seq) {
        ILOG("[fetch] cycle " << cycle << " IGNORING stale BP redirect: branch_fetch_seq=" << redirect.branch_fetch_seq
                              << " < squash_threshold=" << mStaleFilter.squash_threshold_fetch_seq);
        return;
    }

    // Also filter by depth as a secondary check
    if (redirect.wrong_path_depth > mCurrentWrongPathDepth) {
        ILOG("[fetch] cycle " << cycle << " IGNORING stale BP redirect: branch_tag=" << redirect.branch_tag
                              << " redirect_depth=" << (int)redirect.wrong_path_depth << " > current_depth=" << (int)mCurrentWrongPathDepth);
        return;
    }

    // Enforce max_depth - don't speculate beyond max_depth
    const auto& spec_cfg = core::getSpeculationConfig();
    if (mCurrentWrongPathDepth >= spec_cfg.max_depth) {
        ILOG("[fetch] cycle " << cycle << " BLOCKING BP redirect: max_depth=" << (int)spec_cfg.max_depth
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
    uint32_t cancelled = request_out.cancelIf([&](const core::FetchRequest& req) -> bool {
        bool should_cancel = false;
        for (const auto& pkt : req.packets) {
            if (core::shouldSquash(pkt.tag, pkt.wrong_path_depth, redirect.branch_tag, branch_depth, core::FlushSource::BranchPredictor)) {
                ILOG("[fetch] cycle " << cycle << " CANCELLING in-flight tag=" << pkt.tag << " depth=" << (int)pkt.wrong_path_depth);
                if (mVis) mVis->onSquash(pkt.fetch_seq, cycle);
                should_cancel = true;  // Mark for cancellation but continue to call onSquash for all packets
            }
        }
        return should_cancel;
    });

    if (cancelled > 0) {
        ILOG("[fetch] cycle " << cycle << " cancelled " << cancelled << " in-flight requests");
    }

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

    // If speculation is disabled, apply static misprediction penalty instead of full wrong-path modeling
    if (!core::getSpeculationConfig().enabled) {
        if (mMispredictionPenalty > 0) {
            mMispredStallCycles = mMispredictionPenalty;
            ++mNumRedirects;
            ILOG("[fetch] cycle " << cycle << " MISPREDICTION PENALTY: stalling for " << mMispredictionPenalty << " cycles (branch_tag=" << redirect.branch_tag
                                  << ")");
        } else {
            ILOG("[fetch] cycle " << cycle << " IGNORING mispred redirect: speculation disabled, no penalty configured");
        }
        return;
    }

    // Filter stale mispred redirects. Speculative (depth>0) and depth=0 mispreds
    // use different thresholds — see StaleRedirectFilter docs in the header.
    if (redirect.wrong_path_depth > 0) {
        if (mStaleFilter.squash_threshold_fetch_seq > 0 && redirect.branch_fetch_seq < mStaleFilter.squash_threshold_fetch_seq) {
            ILOG("[fetch] cycle " << cycle << " IGNORING stale SPECULATIVE mispred: depth=" << (int)redirect.wrong_path_depth
                                  << " fetch_seq=" << redirect.branch_fetch_seq << " < threshold=" << mStaleFilter.squash_threshold_fetch_seq);
            return;
        }
    } else if (mStaleFilter.depth0_fetch_seq > 0 && redirect.branch_fetch_seq > mStaleFilter.depth0_branch_fetch_seq &&
               redirect.branch_fetch_seq < mStaleFilter.depth0_fetch_seq) {
        // Depth=0 mispred whose branch was fetched strictly inside the last depth=0
        // recovery's squashed window (after its branch, before its frontier) — it was
        // flushed by that recovery. fetch_seq is monotonic/never-reused, so this window
        // test is exact even across model-tag reuse.
        ILOG("[fetch] cycle " << cycle << " IGNORING stale DEPTH0 mispred: branch_fetch_seq=" << redirect.branch_fetch_seq << " in ("
                              << mStaleFilter.depth0_branch_fetch_seq << ", " << mStaleFilter.depth0_fetch_seq << ")");
        return;
    }

    // Secondary filter: depth check as a safety net
    // If a mispred comes from a depth deeper than our current depth, it's from a
    // speculative path that was already recovered from
    if (redirect.wrong_path_depth > mCurrentWrongPathDepth) {
        ILOG("[fetch] cycle " << cycle << " IGNORING stale MISPRED redirect: depth=" << (int)redirect.wrong_path_depth
                              << " > current_depth=" << (int)mCurrentWrongPathDepth);
        return;
    }

    ++mNumRedirects;

    // Flush Whisper/ExecutionDriver from branch_tag+1 onwards
    // This must happen at the same time as Fetch receives the redirect to keep them synchronized
    if (mDriver) {
        uint64_t num_flushed = mDriver->flushInstruction(redirect.branch_tag + 1);
        ILOG("[fetch] Flushed " << num_flushed << " instructions from ExecutionDriver (tags >= " << (redirect.branch_tag + 1) << ")");
    }

    // Cancel any in-flight packets to ICache that should be squashed
    // (packets on the wrong path that need to be flushed due to misprediction recovery)
    uint32_t cancelled = request_out.cancelIf([&](const core::FetchRequest& req) -> bool {
        bool should_cancel = false;
        for (const auto& pkt : req.packets) {
            if (core::shouldSquash(pkt.tag, pkt.wrong_path_depth, redirect.branch_tag, redirect.wrong_path_depth, core::FlushSource::Execute)) {
                ILOG("[fetch] cycle " << cycle << " CANCELLING in-flight tag=" << pkt.tag << " depth=" << (int)pkt.wrong_path_depth << " (mispred recovery)");
                if (mVis) mVis->onSquash(pkt.fetch_seq, cycle);
                should_cancel = true;  // Mark for cancellation but continue to call onSquash for all packets
            }
        }
        return should_cancel;
    });

    if (cancelled > 0) {
        ILOG("[fetch] cycle " << cycle << " cancelled " << cancelled << " in-flight requests (mispred)");
    }

    // When a branch at depth D mispredicts, we return to depth D (the branch itself was at that depth)
    mCurrentWrongPathDepth = redirect.wrong_path_depth;

    // Refresh stale-redirect filter: any branch with fetch_seq < mFetchSeq is now invalid.
    mStaleFilter.squash_threshold_fetch_seq = mFetchSeq;
    if (redirect.wrong_path_depth == 0) {
        mStaleFilter.depth0_fetch_seq = mFetchSeq;                         // upper bound: recovery frontier
        mStaleFilter.depth0_branch_fetch_seq = redirect.branch_fetch_seq;  // lower bound: recovering branch
    }

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
