#pragma once

#include <deque>
#include <unordered_map>
#include <vector>

#include "sparta/ports/DataPort.hpp"
#include "sparta/simulation/ParameterSet.hpp"
#include "sparta/simulation/Unit.hpp"
#include "sparta/statistics/Counter.hpp"

#include "Common/PipelinePacket.hpp"
#include "Common/PipelineVisualizer.hpp"

namespace frontend {

class DecodeStructures;

class FetchQueueParams : public sparta::ParameterSet {
   public:
    FetchQueueParams(sparta::TreeNode* n)
        : sparta::ParameterSet(n) {}

    PARAMETER(uint32_t, capacity, 16, "FetchQueue capacity (entries)")
    PARAMETER(bool, log_enabled, false, "Enable debug logging for this unit")
};

class FetchQueue : public sparta::Unit {
   public:
    static constexpr char name[] = "fetch_queue";

    FetchQueue(sparta::TreeNode* node, const FetchQueueParams* params);

    sparta::DataInPort<std::vector<core::PipelinePacket>> packets_in{&unit_port_set_, "packets_in"};
    sparta::DataInPort<core::BranchPrediction> branch_predict_in{&unit_port_set_, "branch_predict_in"};

    // Flush support
    sparta::DataInPort<core::FlushRequest> flush_in{&unit_port_set_, "flush_in"};

    // Expose Entry type for Decode to inspect
    struct Entry {
        core::PipelinePacket pkt;
        bool needs_branch_prediction{false};
        bool has_branch_prediction{false};
        core::BranchPrediction branch_pred;
    };

    void tick();

    // Pull model: Decode calls these methods
    const Entry* peek() const { return mQueue.empty() ? nullptr : &mQueue.front(); }
    Entry pullOne();  // Pull single packet from head

    bool isEmpty() const { return mQueue.empty(); }
    bool isReady() const { return mQueue.size() < mCapacity; }
    size_t available() const { return mCapacity - mQueue.size(); }
    size_t size() const { return mQueue.size(); }
    void setVisualizer(core::PipelineVisualizer* v) { mVis = v; }
    void flush();

    uint64_t numEnqueued() const { return mNumEnqueued.get(); }
    uint64_t numDequeued() const { return mNumDequeued.get(); }
    uint64_t numFullStallCycles() const { return mNumFullStallCycles.get(); }
    uint64_t numBranchWaitCycles() const { return mNumBranchWaitCycles.get(); }

   private:
    void receivePackets_(const std::vector<core::PipelinePacket>& pkts);
    void receivePrediction_(const core::BranchPrediction& pred);
    void receiveFlush_(const core::FlushRequest& req);

    std::deque<Entry> mQueue;
    // tag -> index of pending branch entries waiting for prediction
    std::unordered_map<uint64_t, std::deque<size_t>> mPendingBranchTags;

    // Early-arriving predictions (arrived before the packet)
    std::unordered_map<uint64_t, core::BranchPrediction> mEarlyPredictions;

    // Squash state for filtering incoming packets (BP flushes)
    bool mSquashActive{false};
    uint64_t mSquashTag{0};
    uint8_t mSquashDepth{0};
    core::FlushSource mSquashSource{core::FlushSource::Execute};
    uint64_t mSquashCycle{0};

    // Separate Execute squash state that persists independently of BP flushes
    bool mExeSquashActive{false};
    uint64_t mExeSquashCycle{0};

    uint32_t mCapacity;
    core::PipelineVisualizer* mVis{nullptr};
    bool mLogEnabled{false};

    sparta::Counter mNumEnqueued;
    sparta::Counter mNumDequeued;
    sparta::Counter mNumFullStallCycles;
    sparta::Counter mNumBranchWaitCycles;
};

}  // namespace frontend
