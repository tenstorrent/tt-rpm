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
    PARAMETER(bool, log_enabled, false, "Enable debug logging for this unit")
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
    const core::DecodePacket* peek() const { return mQueue.empty() ? nullptr : &mQueue.front(); }
    core::DecodePacket pullOne();  // Pull single packet from head

    bool isEmpty() const { return mQueue.empty(); }
    bool isReady() const { return mQueue.size() < mCapacity; }
    size_t available() const { return mCapacity - mQueue.size(); }
    size_t size() const { return mQueue.size(); }
    void setVisualizer(core::PipelineVisualizer* v) { mVis = v; }
    void flush();

    uint64_t numEnqueued() const { return mNumEnqueued.get(); }
    uint64_t numDequeued() const { return mNumDequeued.get(); }
    uint64_t numFullStallCycles() const { return mNumFullStallCycles.get(); }

   private:
    void receiveDecodePackets_(const std::vector<core::DecodePacket>& pkts);
    void receiveFlush_(const core::FlushRequest& req);

    std::deque<core::DecodePacket> mQueue;

    uint32_t mCapacity;
    core::PipelineVisualizer* mVis{nullptr};
    bool mLogEnabled{false};

    sparta::Counter mNumEnqueued;
    sparta::Counter mNumDequeued;
    sparta::Counter mNumFullStallCycles;
};

}  // namespace frontend
