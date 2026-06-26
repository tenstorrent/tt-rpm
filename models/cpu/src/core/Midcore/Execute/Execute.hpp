// SPDX-FileCopyrightText: © 2026 Tenstorrent AI ULC
//
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <array>
#include <deque>
#include <string>
#include <vector>

#include "sparta/ports/DataPort.hpp"
#include "sparta/simulation/ParameterSet.hpp"
#include "sparta/simulation/Unit.hpp"
#include "sparta/statistics/Counter.hpp"

#include "Common/PipelinePacket.hpp"
#include "Common/PipelineVisualizer.hpp"
#include "Common/WritePortArbiter.hpp"

namespace midcore {
class LSQ;
class BypassNetwork;
class WritebackBuffer;

class ExecuteParams : public sparta::ParameterSet {
   public:
    ExecuteParams(sparta::TreeNode* n)
        : sparta::ParameterSet(n) {}
    PARAMETER(std::string, granularity, "unified", "Execute granularity: unified, typed, or functional")

    // unified
    PARAMETER(uint32_t, unified_max_in_flight, 16, "Max in-flight (unified mode)")

    // typed
    PARAMETER(uint32_t, int_max_in_flight, 4, "Max in-flight for integer unit group")
    PARAMETER(uint32_t, fp_max_in_flight, 2, "Max in-flight for FP unit group")
    PARAMETER(uint32_t, vec_max_in_flight, 2, "Max in-flight for vector unit group")
    PARAMETER(uint32_t, branch_max_in_flight, 2, "Max in-flight for branch unit group")

    // functional
    PARAMETER(uint32_t, alu_count, 2, "Number of ALU units")
    PARAMETER(uint32_t, alu_per_unit_in_flight, 4, "Max in-flight per ALU unit")
    PARAMETER(uint32_t, mul_count, 1, "Number of Mul units")
    PARAMETER(uint32_t, mul_per_unit_in_flight, 2, "Max in-flight per Mul unit")
    PARAMETER(uint32_t, div_count, 1, "Number of Div units")
    PARAMETER(uint32_t, div_per_unit_in_flight, 1, "Max in-flight per Div unit")
    PARAMETER(uint32_t, branch_count, 1, "Number of Branch units")
    PARAMETER(uint32_t, branch_per_unit_in_flight, 2, "Max in-flight per Branch unit")
    PARAMETER(uint32_t, fp_count, 1, "Number of FP units")
    PARAMETER(uint32_t, fp_per_unit_in_flight, 4, "Max in-flight per FP unit")
    PARAMETER(uint32_t, vec_count, 1, "Number of Vec units")
    PARAMETER(uint32_t, vec_per_unit_in_flight, 4, "Max in-flight per Vec unit")
    PARAMETER(uint32_t, fence_count, 1, "Number of Fence units")
    PARAMETER(uint32_t, fence_per_unit_in_flight, 1, "Max in-flight per Fence unit")

    PARAMETER(std::string, write_port_mode, "unified", "Write port mode: unified | mapped")
    PARAMETER(uint32_t, write_port_count, 4, "Number of write ports")
    PARAMETER(bool, writeback_buffer_enabled, false, "Enable writeback buffer")
    PARAMETER(uint32_t, writeback_buffer_capacity, 16, "Writeback buffer capacity")
    PARAMETER(uint32_t, writeback_buffer_drain_width, 4, "Writeback buffer drain width per cycle")
    PARAMETER(uint32_t, writeback_buffer_latency, 1, "Writeback buffer latency in cycles")
    PARAMETER(std::vector<std::string>, write_port_mapping, std::vector<std::string>{}, "Write port mapping for mapped mode: comma-separated sources per port")
};

class Execute : public sparta::Unit {
   public:
    static constexpr char name[] = "execute";

    Execute(sparta::TreeNode* node, const ExecuteParams* params);

    sparta::DataInPort<std::vector<core::IssuePacket>> in_port{&unit_port_set_, "packets_in"};

    sparta::DataOutPort<std::vector<core::IssuePacket>> mem_out{&unit_port_set_, "mem_out"};

    sparta::DataOutPort<std::vector<core::PhysRegRef>> completion_out{&unit_port_set_, "completion_out"};

    sparta::DataOutPort<std::vector<core::ROBToken>> rob_complete_out{&unit_port_set_, "rob_complete_out"};

    // Flush support
    sparta::DataInPort<core::FlushRequest> flush_in{&unit_port_set_, "flush_in"};
    sparta::DataOutPort<core::FlushRequest> mispred_flush_out{&unit_port_set_, "mispred_flush_out"};

    // Correct branch resolution - signals depth decrement (no flush needed)
    sparta::DataOutPort<core::BranchResolved> branch_resolved_out{&unit_port_set_, "branch_resolved_out"};

    void tick();

    // Called by PipelineClock after both LSQ and Execute have submitted candidates.
    void processWritePortGrants();

    bool isReadyForType(core::UopType t) const { return mGroups[mGroupForType[static_cast<uint8_t>(t)]].isReady(); }

    void reserveSlot(core::UopType t) { mGroups[mGroupForType[static_cast<uint8_t>(t)]].reserveSlot(); }

    uint32_t numGroups() const { return static_cast<uint32_t>(mGroups.size()); }

    void setDownstream(midcore::LSQ* lsq) { mDownstream = lsq; }
    void setArbiter(WritePortArbiter* arb) { mArbiter = arb; }
    void setVisualizer(core::PipelineVisualizer* v) { mVis = v; }
    void setBypassNetwork(BypassNetwork* bn) { mBypassNetwork = bn; }
    void setWritebackBuffer(WritebackBuffer* wb) { mWritebackBuffer = wb; }
    uint64_t numExecuted() const { return mNumExecuted.get(); }

   private:
    struct InFlightUop {
        core::IssuePacket pkt;
        uint8_t cycles_remaining;
        bool awaiting_write_port{false};
        bool done_signaled{false};
    };

    struct FunctionalUnitGroup {
        std::deque<InFlightUop> queue;
        uint32_t max_in_flight{4};
        uint32_t pending{0};

        bool isReady() const { return queue.size() + pending < max_in_flight; }
        void reserveSlot() { ++pending; }
        void clearPending() { pending = 0; }

        // Squash entries matching the predicate
        template <typename Pred>
        uint32_t squash(Pred should_squash) {
            uint32_t count = 0;
            auto it = queue.begin();
            while (it != queue.end()) {
                if (should_squash(it->pkt)) {
                    it = queue.erase(it);
                    ++count;
                } else {
                    ++it;
                }
            }
            return count;
        }
    };

    void receivePackets_(const std::vector<core::IssuePacket>& pkts);
    void receiveFlush_(const core::FlushRequest& req);
    void handleBranchMisprediction_(const InFlightUop& uop);

    void buildUnified(const ExecuteParams* p);
    void buildTyped(const ExecuteParams* p);
    void buildFunctional(const ExecuteParams* p);

    std::vector<FunctionalUnitGroup> mGroups;
    std::array<uint8_t, core::kNumUopTypes> mGroupForType{};

    midcore::LSQ* mDownstream = nullptr;
    WritePortArbiter* mArbiter = nullptr;
    core::PipelineVisualizer* mVis{nullptr};
    BypassNetwork* mBypassNetwork{nullptr};
    WritebackBuffer* mWritebackBuffer{nullptr};

    std::vector<core::PhysRegRef> mCompletionsBuf;
    std::vector<core::ROBToken> mRobCompletionsBuf;
    std::vector<core::IssuePacket> mMemOpsBuf;
    std::vector<core::PhysRegRef> mStoreDestsBuf;

    sparta::Counter mNumExecuted;
    sparta::Counter mNumSquashed;
    sparta::Counter mNumMispredictions;
};

}  // namespace midcore
