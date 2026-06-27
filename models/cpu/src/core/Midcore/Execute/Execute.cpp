// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Tenstorrent USA, Inc.

#include "Midcore/Execute/Execute.hpp"

#include <iostream>

#include "Common/BypassNetwork.hpp"
#include "Common/SpeculationConfig.hpp"
#include "Common/SpeculationUtils.hpp"
#include "Common/WritebackBuffer.hpp"
#include "LoadStoreUnit/LSQ/LSQ.hpp"
#include "Logging.hpp"

namespace midcore {

Execute::Execute(sparta::TreeNode* node, const ExecuteParams* params)
    : sparta::Unit(node),
      mNumExecuted(&unit_stat_set_, "num_executed", "Total instructions executed", sparta::Counter::COUNT_NORMAL),
      mNumSquashed(&unit_stat_set_, "num_squashed", "Instructions squashed due to flush", sparta::Counter::COUNT_NORMAL),
      mNumMispredictions(&unit_stat_set_, "num_mispredictions", "Branch mispredictions detected", sparta::Counter::COUNT_NORMAL) {
    in_port.registerConsumerHandler(CREATE_SPARTA_HANDLER_WITH_DATA(Execute, receivePackets_, std::vector<core::IssuePacket>));
    flush_in.registerConsumerHandler(CREATE_SPARTA_HANDLER_WITH_DATA(Execute, receiveFlush_, core::FlushRequest));

    // Read all parameters upfront (Sparta requires all params to be consumed)
    const std::string gran = params->granularity;
    const uint32_t unified_mif = params->unified_max_in_flight;
    const uint32_t int_mif = params->int_max_in_flight;
    const uint32_t fp_mif = params->fp_max_in_flight;
    const uint32_t vec_mif = params->vec_max_in_flight;
    const uint32_t branch_mif = params->branch_max_in_flight;
    const uint32_t alu_cnt = params->alu_count, alu_pui = params->alu_per_unit_in_flight;
    const uint32_t mul_cnt = params->mul_count, mul_pui = params->mul_per_unit_in_flight;
    const uint32_t div_cnt = params->div_count, div_pui = params->div_per_unit_in_flight;
    const uint32_t br_cnt = params->branch_count, br_pui = params->branch_per_unit_in_flight;
    const uint32_t fp_cnt = params->fp_count, fp_pui = params->fp_per_unit_in_flight;
    const uint32_t vec_cnt = params->vec_count, vec_pui = params->vec_per_unit_in_flight;
    const uint32_t fence_cnt = params->fence_count, fence_pui = params->fence_per_unit_in_flight;
    (void)unified_mif;
    (void)int_mif;
    (void)fp_mif;
    (void)vec_mif;
    (void)branch_mif;
    (void)alu_cnt;
    (void)alu_pui;
    (void)mul_cnt;
    (void)mul_pui;
    (void)div_cnt;
    (void)div_pui;
    (void)br_cnt;
    (void)br_pui;
    (void)fp_cnt;
    (void)fp_pui;
    (void)vec_cnt;
    (void)vec_pui;
    (void)fence_cnt;
    (void)fence_pui;

    // Read write port, writeback buffer, and port mapping params (consumed by ChipSim during binding)
    (void)static_cast<std::string>(params->write_port_mode);
    (void)static_cast<uint32_t>(params->write_port_count);
    (void)static_cast<bool>(params->writeback_buffer_enabled);
    (void)static_cast<uint32_t>(params->writeback_buffer_capacity);
    (void)static_cast<uint32_t>(params->writeback_buffer_drain_width);
    (void)static_cast<uint32_t>(params->writeback_buffer_latency);
    (void)params->write_port_mapping.getValue();

    if (gran == "unified") {
        buildUnified(params);
    } else if (gran == "typed") {
        buildTyped(params);
    } else if (gran == "functional") {
        buildFunctional(params);
    } else {
        sparta_assert(false, "Unknown execute granularity: " << gran);
    }

    ILOG("[execute] granularity=" << gran << " groups=" << mGroups.size());
}

void Execute::buildUnified(const ExecuteParams* p) {
    mGroups.resize(1);
    mGroups[0].max_in_flight = p->unified_max_in_flight;
    mGroupForType.fill(0);
}

void Execute::buildTyped(const ExecuteParams* p) {
    mGroups.resize(4);
    mGroups[0].max_in_flight = p->int_max_in_flight;
    mGroups[1].max_in_flight = p->fp_max_in_flight;
    mGroups[2].max_in_flight = p->vec_max_in_flight;
    mGroups[3].max_in_flight = p->branch_max_in_flight;

    mGroupForType[static_cast<uint8_t>(core::UopType::ALU)] = 0;
    mGroupForType[static_cast<uint8_t>(core::UopType::Mul)] = 0;
    mGroupForType[static_cast<uint8_t>(core::UopType::Div)] = 0;
    mGroupForType[static_cast<uint8_t>(core::UopType::Load)] = 0;
    mGroupForType[static_cast<uint8_t>(core::UopType::Store)] = 0;
    mGroupForType[static_cast<uint8_t>(core::UopType::Fence)] = 0;
    mGroupForType[static_cast<uint8_t>(core::UopType::Branch)] = 3;
    mGroupForType[static_cast<uint8_t>(core::UopType::FpOp)] = 1;
    mGroupForType[static_cast<uint8_t>(core::UopType::VecOp)] = 2;
}

void Execute::buildFunctional(const ExecuteParams* p) {
    mGroups.resize(8);
    mGroups[0].max_in_flight = p->alu_count * p->alu_per_unit_in_flight;
    mGroups[1].max_in_flight = p->mul_count * p->mul_per_unit_in_flight;
    mGroups[2].max_in_flight = p->div_count * p->div_per_unit_in_flight;
    mGroups[3].max_in_flight = p->branch_count * p->branch_per_unit_in_flight;
    mGroups[4].max_in_flight = p->alu_count * p->alu_per_unit_in_flight;
    mGroups[5].max_in_flight = p->fp_count * p->fp_per_unit_in_flight;
    mGroups[6].max_in_flight = p->vec_count * p->vec_per_unit_in_flight;
    mGroups[7].max_in_flight = p->fence_count * p->fence_per_unit_in_flight;

    mGroupForType[static_cast<uint8_t>(core::UopType::ALU)] = 0;
    mGroupForType[static_cast<uint8_t>(core::UopType::Mul)] = 1;
    mGroupForType[static_cast<uint8_t>(core::UopType::Div)] = 2;
    mGroupForType[static_cast<uint8_t>(core::UopType::Branch)] = 3;
    mGroupForType[static_cast<uint8_t>(core::UopType::Load)] = 4;
    mGroupForType[static_cast<uint8_t>(core::UopType::Store)] = 4;
    mGroupForType[static_cast<uint8_t>(core::UopType::FpOp)] = 5;
    mGroupForType[static_cast<uint8_t>(core::UopType::VecOp)] = 6;
    mGroupForType[static_cast<uint8_t>(core::UopType::Fence)] = 7;
}

void Execute::receivePackets_(const std::vector<core::IssuePacket>& pkts) {
    uint64_t cycle = getClock()->currentCycle();
    for (const auto& ipkt : pkts) {
        uint8_t lat = (ipkt.latency > 0) ? ipkt.latency - 1 : 0;
        uint8_t gi = mGroupForType[static_cast<uint8_t>(ipkt.uop_type)];
        mGroups[gi].queue.push_back({ipkt, lat, false});
        if (mVis) mVis->onExeEnter(ipkt.pkt.fetch_seq, cycle);
    }
}

void Execute::receiveFlush_(const core::FlushRequest& req) {
    uint64_t cycle = getClock()->currentCycle();
    ILOG("[execute] cycle=" << cycle << " RECEIVED flush: branch_tag=" << req.branch_tag << " branch_depth=" << static_cast<int>(req.branch_depth));

    uint32_t total_squashed = 0;
    for (auto& group : mGroups) {
        total_squashed += group.squash([&req](const core::IssuePacket& pkt) { return core::shouldSquash(pkt.pkt, req); });
    }

    mNumSquashed += total_squashed;
    ILOG("[execute] Squashed " << total_squashed << " in-flight instructions");
}

void Execute::handleBranchMisprediction_(const InFlightUop& uop) {
    const auto& pkt = uop.pkt;
    if (!pkt.was_mispredicted) return;

    ILOG("[execute] cycle=" << getClock()->currentCycle() << " DETECTED misprediction: tag=" << pkt.pkt.tag << " pc=0x" << std::hex << pkt.pkt.pc << std::dec
                            << " predicted_taken=" << pkt.predicted_taken);

    core::FlushRequest flush;
    flush.source = core::FlushSource::Execute;
    flush.branch_tag = pkt.pkt.tag;
    flush.branch_fetch_seq = pkt.pkt.fetch_seq;  // Use fetch_seq for prediction clearing
    flush.branch_pc = pkt.pkt.pc;
    flush.branch_depth = pkt.pkt.wrong_path_depth;
    flush.predicted_taken = pkt.predicted_taken;
    flush.was_taken = !pkt.predicted_taken;  // Actual outcome is opposite of prediction

    // Target PC: depends on actual vs predicted direction
    // For predicted-taken-but-not-taken: fall-through (pc + size)
    // For predicted-not-taken-but-taken: use takenBranchTarget from trace
    const auto& rec = pkt.pkt.inst->getTraceRecord();
    if (pkt.predicted_taken) {
        // Predicted taken but should have been not-taken: target is fall-through
        flush.target_pc = pkt.pkt.pc + pkt.pkt.size;
    } else {
        // Predicted not-taken but should have been taken: use the branch target
        // The takenBranchTarget field contains the actual taken target from Whisper
        if (rec.takenBranchTarget != 0) {
            flush.target_pc = rec.takenBranchTarget;
        } else {
            // This shouldn't happen for a taken branch, but fallback to sequential
            ILOG("[execute] WARNING: branch was taken but takenBranchTarget=0 at pc=0x" << std::hex << pkt.pkt.pc << std::dec);
            flush.target_pc = pkt.pkt.pc + pkt.pkt.size;
        }
    }

    ++mNumMispredictions;
    mispred_flush_out.send(flush, 0);
}

void Execute::tick() {
    for (auto& g : mGroups) g.clearPending();

    bool any_work = false;
    for (const auto& g : mGroups) {
        if (!g.queue.empty()) {
            any_work = true;
            break;
        }
    }
    if (!any_work) return;

    // LSQ admission is reserved in program order at rename (credit-based), so a
    // memory op released here is guaranteed a slot — no readiness gate needed.
    mMemOpsBuf.clear();

    // If no arbiter, use the old direct-completion path.
    if (!mArbiter || !mArbiter->enabled()) {
        mCompletionsBuf.clear();
        mRobCompletionsBuf.clear();

        for (auto& group : mGroups) {
            for (auto it = group.queue.begin(); it != group.queue.end();) {
                if (it->cycles_remaining > 0) {
                    --it->cycles_remaining;
                    ++it;
                    continue;
                }

                if (!it->done_signaled && mVis) {
                    mVis->onExeDone(it->pkt.pkt.fetch_seq, getClock()->currentCycle());
                    it->done_signaled = true;
                }

                auto& ipkt = it->pkt;
                bool is_memory = (ipkt.uop_type == core::UopType::Load || ipkt.uop_type == core::UopType::Store);
                bool is_branch = (ipkt.uop_type == core::UopType::Branch);

                // Check for branch misprediction
                if (is_branch && core::getSpeculationConfig().enabled) {
                    // Detect misprediction from Whisper trace: compare predicted vs actual
                    const auto& rec = ipkt.pkt.inst->getTraceRecord();
                    bool actual_taken = (rec.takenBranchTarget != 0);
                    bool predicted_taken = ipkt.predicted_taken;
                    bool mispredicted = (actual_taken != predicted_taken);

                    ILOG("[execute] cycle " << getClock()->currentCycle() << " branch tag=" << ipkt.pkt.tag
                                            << " depth=" << static_cast<int>(ipkt.pkt.wrong_path_depth) << " predicted=" << predicted_taken
                                            << " actual=" << actual_taken << " mispredicted=" << mispredicted);

                    if (mispredicted) {
                        // Update the packet's misprediction flag and handle it
                        it->pkt.was_mispredicted = true;
                        handleBranchMisprediction_(*it);
                    } else {
                        // Branch predicted correctly - signal depth decrement
                        core::BranchResolved resolved;
                        resolved.branch_tag = ipkt.pkt.tag;
                        resolved.branch_fetch_seq = ipkt.pkt.fetch_seq;
                        resolved.resolved_depth = ipkt.pkt.wrong_path_depth;
                        branch_resolved_out.send(resolved, 0);
                        ILOG("[execute] cycle " << getClock()->currentCycle() << " CORRECT prediction: tag=" << resolved.branch_tag
                                                << " depth=" << static_cast<int>(resolved.resolved_depth));
                    }
                }

                // Save fetch_seq before potential move
                uint64_t saved_fetch_seq = ipkt.pkt.fetch_seq;

                if (!is_memory) {
                    for (const auto& dst : ipkt.phys_dsts) mCompletionsBuf.push_back(dst);
                    mRobCompletionsBuf.push_back(ipkt.rob_token);
                    if (mBypassNetwork) mBypassNetwork->completeProducer(ipkt.pkt.tag);
                } else {
                    if (ipkt.uop_type == core::UopType::Store && !ipkt.phys_dsts.empty()) {
                        for (const auto& dst : ipkt.phys_dsts) mCompletionsBuf.push_back(dst);
                    }
                    mMemOpsBuf.push_back(std::move(ipkt));
                }

                if (mVis) mVis->onExeExit(saved_fetch_seq, getClock()->currentCycle());
                ++mNumExecuted;
                it = group.queue.erase(it);
            }
        }

        if (!mMemOpsBuf.empty()) mem_out.send(mMemOpsBuf, 1);
        if (!mCompletionsBuf.empty()) completion_out.send(mCompletionsBuf, 1);
        if (!mRobCompletionsBuf.empty()) rob_complete_out.send(mRobCompletionsBuf, 1);
        return;
    }

    // ── Arbiter-enabled path ──
    // Tick writeback buffer first if present
    if (mWritebackBuffer) {
        mWritebackBuffer->tick();
    }

    // Clear store destinations buffer (for stores with register outputs)
    mStoreDestsBuf.clear();

    // Phase 1: count down, forward memory ops, submit non-memory to writeback buffer or arbiter.
    for (uint8_t gi = 0; gi < mGroups.size(); ++gi) {
        auto& group = mGroups[gi];
        for (auto it = group.queue.begin(); it != group.queue.end();) {
            if (it->cycles_remaining > 0) {
                --it->cycles_remaining;
                ++it;
                continue;
            }

            if (!it->done_signaled && mVis) {
                mVis->onExeDone(it->pkt.pkt.fetch_seq, getClock()->currentCycle());
                it->done_signaled = true;
            }

            auto& ipkt = it->pkt;
            uint64_t saved_fetch_seq = ipkt.pkt.fetch_seq;  // Save before potential move
            bool is_memory = (ipkt.uop_type == core::UopType::Load || ipkt.uop_type == core::UopType::Store);
            bool is_branch = (ipkt.uop_type == core::UopType::Branch);

            // Check for branch misprediction (arbiter-enabled path)
            if (is_branch && core::getSpeculationConfig().enabled) {
                // Detect misprediction from Whisper trace: compare predicted vs actual
                const auto& rec = ipkt.pkt.inst->getTraceRecord();
                bool actual_taken = (rec.takenBranchTarget != 0);
                bool predicted_taken = ipkt.predicted_taken;
                bool mispredicted = (actual_taken != predicted_taken);

                ILOG("[execute-arb] cycle " << getClock()->currentCycle() << " branch tag=" << ipkt.pkt.tag
                                            << " depth=" << static_cast<int>(ipkt.pkt.wrong_path_depth) << " predicted=" << predicted_taken
                                            << " actual=" << actual_taken << " mispredicted=" << mispredicted);

                if (mispredicted) {
                    it->pkt.was_mispredicted = true;
                    handleBranchMisprediction_(*it);
                } else {
                    // Branch predicted correctly - signal depth decrement
                    core::BranchResolved resolved;
                    resolved.branch_tag = ipkt.pkt.tag;
                    resolved.branch_fetch_seq = ipkt.pkt.fetch_seq;
                    resolved.resolved_depth = ipkt.pkt.wrong_path_depth;
                    branch_resolved_out.send(resolved, 0);
                    ILOG("[execute-arb] cycle " << getClock()->currentCycle() << " CORRECT prediction: tag=" << resolved.branch_tag
                                                << " depth=" << static_cast<int>(resolved.resolved_depth));
                }
            }

            if (is_memory) {
                // Stores with destinations (like auto-increment) need immediate register
                // completion - they bypass the arbiter since this is just address computation.
                if (ipkt.uop_type == core::UopType::Store && !ipkt.phys_dsts.empty()) {
                    for (const auto& dst : ipkt.phys_dsts) mStoreDestsBuf.push_back(dst);
                }
                if (mVis) mVis->onExeExit(saved_fetch_seq, getClock()->currentCycle());
                ++mNumExecuted;
                mMemOpsBuf.push_back(std::move(ipkt));
                it = group.queue.erase(it);
            } else {
                if (!it->awaiting_write_port) {
                    // First time this instruction is ready for writeback
                    if (mWritebackBuffer) {
                        // Submit to writeback buffer
                        if (mWritebackBuffer->accept(ipkt.pkt.tag, ipkt.pkt.fetch_seq, ipkt.phys_dsts, ipkt.rob_token, gi)) {
                            it->awaiting_write_port = true;
                            if (mVis) mVis->onExeExit(saved_fetch_seq, getClock()->currentCycle());
                            ++mNumExecuted;
                            it = group.queue.erase(it);
                            continue;
                        } else {
                            // Writeback buffer full, retry next cycle
                            ++it;
                            continue;
                        }
                    } else {
                        it->awaiting_write_port = true;
                    }
                }
                // Submit directly to arbiter (no writeback buffer, or for old entries)
                WriteCandidate cand;
                cand.source_id = gi;
                cand.tag = ipkt.pkt.tag;
                cand.fetch_seq = ipkt.pkt.fetch_seq;
                cand.phys_dsts = ipkt.phys_dsts;
                cand.rob_token = ipkt.rob_token;
                mArbiter->submit(cand);
                ++it;
            }
        }
    }

    // Phase 2: Submit ready entries from writeback buffer to arbiter
    if (mWritebackBuffer) {
        auto ready = mWritebackBuffer->getReadyEntries();
        for (auto* entry : ready) {
            WriteCandidate cand;
            cand.source_id = entry->source_id;
            cand.tag = entry->tag;
            cand.fetch_seq = entry->fetch_seq;
            cand.phys_dsts = entry->phys_dsts;
            cand.rob_token = entry->rob_token;
            mArbiter->submit(cand);
        }
    }

    if (!mMemOpsBuf.empty()) mem_out.send(mMemOpsBuf, 1);
    // Store destinations bypass the arbiter - send them immediately
    if (!mStoreDestsBuf.empty()) completion_out.send(mStoreDestsBuf, 1);
    // Completions for non-memory ops are sent in processWritePortGrants().
}

void Execute::processWritePortGrants() {
    if (!mArbiter || !mArbiter->enabled()) return;

    mCompletionsBuf.clear();
    mRobCompletionsBuf.clear();

    // Process grants for entries in FU queues (no writeback buffer path)
    for (auto& group : mGroups) {
        for (auto it = group.queue.begin(); it != group.queue.end();) {
            if (!it->awaiting_write_port) {
                ++it;
                continue;
            }

            if (mArbiter->isGranted(it->pkt.pkt.tag)) {
                for (const auto& dst : it->pkt.phys_dsts) mCompletionsBuf.push_back(dst);
                mRobCompletionsBuf.push_back(it->pkt.rob_token);
                // Remove from bypass network (result now in PRF)
                if (mBypassNetwork) mBypassNetwork->completeProducer(it->pkt.pkt.tag);
                if (mVis) mVis->onExeExit(it->pkt.pkt.fetch_seq, getClock()->currentCycle());
                ++mNumExecuted;
                it = group.queue.erase(it);
            } else {
                // Denied — FU stays occupied, retry next cycle
                ++it;
            }
        }
    }

    // Process grants for entries in writeback buffer
    if (mWritebackBuffer) {
        auto ready = mWritebackBuffer->getReadyEntries();
        for (auto* entry : ready) {
            if (mArbiter->isGranted(entry->tag)) {
                for (const auto& dst : entry->phys_dsts) mCompletionsBuf.push_back(dst);
                mRobCompletionsBuf.push_back(entry->rob_token);
                // Remove from bypass network (result now in PRF)
                if (mBypassNetwork) mBypassNetwork->completeProducer(entry->tag);
                mWritebackBuffer->remove(entry->tag);
            }
        }
    }

    if (!mCompletionsBuf.empty()) completion_out.send(mCompletionsBuf, 1);
    if (!mRobCompletionsBuf.empty()) rob_complete_out.send(mRobCompletionsBuf, 1);
}

}  // namespace midcore
