// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Tenstorrent USA, Inc.

#pragma once

#include <deque>
#include <vector>

#include "sparta/ports/DataPort.hpp"
#include "sparta/simulation/ParameterSet.hpp"
#include "sparta/simulation/Unit.hpp"
#include "sparta/statistics/Counter.hpp"

#include "Common/PipelinePacket.hpp"
#include "Common/PipelineVisualizer.hpp"

namespace frontend {

class DecodeQueueParams : public sparta::ParameterSet {
   public:
    DecodeQueueParams(sparta::TreeNode* n)
        : sparta::ParameterSet(n) {}

    PARAMETER(uint32_t, capacity, 32, "DecodeQueue capacity (entries)")
    PARAMETER(uint32_t, latency, 1, "Extra rename stages between Decode and Rename (0 = Rename takes a uop the cycle after Decode)")
};

class DecodeQueue : public sparta::Unit {
   public:
    static constexpr char name[] = "decode_queue";

    DecodeQueue(sparta::TreeNode* node, const DecodeQueueParams* params);

    sparta::DataInPort<std::vector<core::DecodePacket>> decode_in{&unit_port_set_, "decode_in"};

    // Flush support
    sparta::DataInPort<core::FlushRequest> flush_in{&unit_port_set_, "flush_in"};

    void tick();

    // Pull model: Rename calls these methods
    const core::DecodePacket* peek() const { return hasReady() ? &mQueue.front().dp : nullptr; }
    core::DecodePacket pullOne();  // Pull single packet from head

    // The head has cleared the decode-to-rename latency.
    bool hasReady() const { return !mQueue.empty() && mQueue.front().ready_cycle <= getClock()->currentCycle(); }
    bool isReady() const { return mQueue.size() < mCapacity; }
    size_t available() const { return mCapacity - mQueue.size(); }
    size_t size() const { return mQueue.size(); }
    void setVisualizer(core::PipelineVisualizer* v) { mVis = v; }
    void flush();

    uint64_t numEnqueued() const { return mNumEnqueued.get(); }
    uint64_t numDequeued() const { return mNumDequeued.get(); }
    uint64_t numFullStallCycles() const { return mNumFullStallCycles.get(); }
    uint64_t numLatencyWaitCycles() const { return mNumLatencyWaitCycles.get(); }

   private:
    void receiveDecodePackets_(const std::vector<core::DecodePacket>& pkts);
    void receiveFlush_(const core::FlushRequest& req);

    struct Entry {
        core::DecodePacket dp;
        uint64_t ready_cycle{0};
    };
    std::deque<Entry> mQueue;

    uint32_t mCapacity;
    uint32_t mLatency;
    core::PipelineVisualizer* mVis{nullptr};

    sparta::Counter mNumEnqueued;
    sparta::Counter mNumDequeued;
    sparta::Counter mNumFullStallCycles;
    sparta::Counter mNumLatencyWaitCycles;
};

}  // namespace frontend
