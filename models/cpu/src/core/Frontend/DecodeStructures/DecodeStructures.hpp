// SPDX-FileCopyrightText: © 2026 Tenstorrent AI ULC
//
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <array>
#include <map>
#include <vector>

#include "sparta/ports/DataPort.hpp"
#include "sparta/simulation/ParameterSet.hpp"
#include "sparta/simulation/Unit.hpp"
#include "sparta/statistics/Counter.hpp"

#include "Common/PipelinePacket.hpp"
#include "Common/PipelineVisualizer.hpp"

namespace midcore {
class Rename;
}

namespace frontend {

class FetchQueue;
class DecodeQueue;

class DecodeStructuresParams : public sparta::ParameterSet {
   public:
    DecodeStructuresParams(sparta::TreeNode* n)
        : sparta::ParameterSet(n) {}

    PARAMETER(uint32_t, lat_alu, 1, "ALU latency")
    PARAMETER(uint32_t, lat_mul, 3, "Multiply latency")
    PARAMETER(uint32_t, lat_div, 12, "Divide latency")
    PARAMETER(uint32_t, lat_branch, 1, "Branch latency")
    PARAMETER(uint32_t, lat_load, 1, "Load execution latency (cache latency is separate)")
    PARAMETER(uint32_t, lat_store, 1, "Store execution latency")
    PARAMETER(uint32_t, lat_fp, 4, "Floating-point latency")
    PARAMETER(uint32_t, lat_vec, 4, "Vector operation latency")
    PARAMETER(uint32_t, lat_fence, 1, "Fence latency")
    PARAMETER(uint32_t, bp_mispred_penalty, 5, "Decode stall cycles on branch misprediction")
};

class DecodeStructures : public sparta::Unit {
   public:
    static constexpr char name[] = "decode";

    DecodeStructures(sparta::TreeNode* node, const DecodeStructuresParams* params);

    // Direct input port for packets from ICache (bypass FetchQueue)
    sparta::DataInPort<std::vector<core::PipelinePacket>> packets_in{&unit_port_set_, "packets_in"};

    // Branch prediction input port (direct mode - bypass FetchQueue)
    sparta::DataInPort<core::BranchPrediction> branch_predict_in{&unit_port_set_, "branch_predict_in"};

    // Output port to Rename (bypass DecodeQueue)
    sparta::DataOutPort<std::vector<core::DecodePacket>> out_port{&unit_port_set_, "packets_out"};

    // Flush input port
    sparta::DataInPort<core::FlushRequest> flush_in{&unit_port_set_, "flush_in"};

    void tick();

    // Set upstream FetchQueue (pull model) - for backwards compatibility
    void setFetchQueue(frontend::FetchQueue* fq) { mFetchQueue = fq; }
    // Set downstream DecodeQueue for backpressure - for backwards compatibility
    void setDecodeQueue(frontend::DecodeQueue* dq) { mDecodeQueue = dq; }
    // Set downstream Rename for backpressure (direct mode)
    void setRename(midcore::Rename* r) { mRename = r; }
    void setVisualizer(core::PipelineVisualizer* v) { mVis = v; }

    // Enable direct mode (bypass queues)
    void setDirectMode(bool enabled) { mDirectMode = enabled; }

    uint64_t numDecoded() const { return mNumDecoded.get(); }
    uint64_t numMispredStallCycles() const { return mNumMispredStallCycles.get(); }
    uint64_t numBranchWaitCycles() const { return mNumBranchWaitCycles.get(); }

   private:
    void receiveFlush_(const core::FlushRequest& req);
    void receivePackets_(const std::vector<core::PipelinePacket>& pkts);
    void receiveBranchPrediction_(const core::BranchPrediction& pred);

    static constexpr size_t kNumUopTypes = 9;

    static core::UopType classifyUop(cpu::InstClass ic);

    std::array<uint8_t, kNumUopTypes> mLatencyTable{};

    frontend::FetchQueue* mFetchQueue{nullptr};
    frontend::DecodeQueue* mDecodeQueue{nullptr};
    midcore::Rename* mRename{nullptr};

    std::vector<core::DecodePacket> mDecodePacketsBuf;
    std::vector<core::PipelinePacket> mPendingPackets;               // Buffer for direct mode
    std::map<uint64_t, core::BranchPrediction> mPendingPredictions;  // tag -> prediction

    core::PipelineVisualizer* mVis{nullptr};

    uint32_t mMispredPenalty{5};
    uint32_t mMispredStall{0};
    bool mDirectMode{false};

    sparta::Counter mNumDecoded;
    sparta::Counter mNumMispredStallCycles;
    sparta::Counter mNumBranchWaitCycles;
};

}  // namespace frontend
