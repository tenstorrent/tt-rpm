// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Tenstorrent USA, Inc.

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

    PARAMETER(uint32_t, decode_width, 4, "Instructions decoded per cycle")

    PARAMETER(uint32_t, lat_alu, 1, "ALU latency")
    PARAMETER(uint32_t, lat_mul, 3, "Multiply latency")
    // Divide and FP defaults follow public x86 and Rocket latencies; CONFIG_README names the sources.
    PARAMETER(uint32_t, lat_div, 14, "Divide latency")
    PARAMETER(uint32_t, lat_branch, 1, "Branch latency")
    PARAMETER(uint32_t, lat_load, 1, "Load execution latency (cache latency is separate)")
    PARAMETER(uint32_t, lat_store, 1, "Store execution latency")
    PARAMETER(uint32_t, lat_fp, 4, "Latency of FP ops outside the classes below")
    PARAMETER(uint32_t, lat_fp_add, 3, "FP add and subtract latency")
    PARAMETER(uint32_t, lat_fp_mul, 4, "FP multiply latency")
    PARAMETER(uint32_t, lat_fp_fma, 4, "FP fused multiply-add latency")
    PARAMETER(uint32_t, lat_fp_cvt, 4, "FP convert latency: to and from integer, and between formats")
    PARAMETER(uint32_t, lat_fp_cmp, 2, "FP compare, min and max latency")
    PARAMETER(uint32_t, lat_fp_move, 2, "FP sign-injection, move and classify latency")
    PARAMETER(uint32_t, lat_fp_div_s, 10, "Half- and single-precision FP divide and square-root latency (vector: SEW <= 32)")
    PARAMETER(uint32_t, lat_fp_div_d, 13, "Double-precision FP divide and square-root latency (vector: SEW 64)")
    PARAMETER(uint32_t, lat_vec, 4, "Vector operation latency")
    PARAMETER(uint32_t, lat_fence, 1, "Fence latency")
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

    // Misfetch redirect to FlushArbiter
    sparta::DataOutPort<core::FlushRequest> misfetch_flush_out{&unit_port_set_, "misfetch_flush_out"};

    void tick();

    // Set upstream FetchQueue (pull model, used when queues are not bypassed)
    void setFetchQueue(frontend::FetchQueue* fq) { mFetchQueue = fq; }
    // Set downstream DecodeQueue for backpressure (used when queues are not bypassed)
    void setDecodeQueue(frontend::DecodeQueue* dq) { mDecodeQueue = dq; }
    // Set downstream Rename for backpressure (direct mode)
    void setRename(midcore::Rename* r) { mRename = r; }
    void setVisualizer(core::PipelineVisualizer* v) { mVis = v; }

    // Enable direct mode (bypass queues)
    void setDirectMode(bool enabled) { mDirectMode = enabled; }

    uint64_t numDecoded() const { return mNumDecoded.get(); }
    size_t numPendingPackets() const { return mPendingPackets.size(); }
    uint64_t numBranchWaitCycles() const { return mNumBranchWaitCycles.get(); }

   private:
    void receiveFlush_(const core::FlushRequest& req);
    void receivePackets_(const std::vector<core::PipelinePacket>& pkts);
    void receiveBranchPrediction_(const core::BranchPrediction& pred);

    static core::UopType classifyUop(const core::PipelinePacket& pkt);
    uint8_t latencyFor_(const core::PipelinePacket& pkt, core::UopType t) const;
    bool redirectMisfetch_(const core::BranchPrediction& pred, core::DecodePacket& dp);

    std::array<uint8_t, core::kNumUopTypes> mLatencyTable{};
    std::array<uint8_t, 8> mFpLatency{};  // by core::FpOpClass
    uint8_t mLatFpDivS;
    uint8_t mLatFpDivD;

    frontend::FetchQueue* mFetchQueue{nullptr};
    frontend::DecodeQueue* mDecodeQueue{nullptr};
    midcore::Rename* mRename{nullptr};

    std::vector<core::DecodePacket> mDecodePacketsBuf;
    std::vector<core::PipelinePacket> mPendingPackets;               // Buffer for direct mode
    std::map<uint64_t, core::BranchPrediction> mPendingPredictions;  // tag -> prediction

    core::PipelineVisualizer* mVis{nullptr};

    uint32_t mDecodeWidth;
    bool mDirectMode{false};

    sparta::Counter mNumDecoded;
    sparta::Counter mNumBranchWaitCycles;
    sparta::Counter mNumMisfetchRedirects;
    sparta::Counter mNumWidthLimitedCycles;
};

}  // namespace frontend
