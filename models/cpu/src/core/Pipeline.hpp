// SPDX-FileCopyrightText: © 2026 Tenstorrent AI ULC
//
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <chrono>
#include <cstdint>

#include "sparta/events/UniqueEvent.hpp"
#include "sparta/simulation/ParameterSet.hpp"
#include "sparta/simulation/Unit.hpp"

namespace frontend {
class FetchStructures;
class FrontendMemoryStructures;
class FetchQueue;
class DecodeStructures;
class DecodeQueue;
class BranchPredictor;
}  // namespace frontend

namespace midcore {
class Rename;
class Issue;
class Execute;
class LSQ;
class BackendMemoryStructures;
class Writeback;
}  // namespace midcore

namespace midcore {
class WritePortArbiter;
}

namespace core {
class FlushArbiter;
class PipelineVisualizer;
}  // namespace core

namespace memory {
class L2Cache;
}

namespace core {

class PipelineClock : public sparta::Unit {
   public:
    static constexpr char name[] = "pipeline_clock";

    class PipelineClockParams : public sparta::ParameterSet {
       public:
        PipelineClockParams(sparta::TreeNode* n)
            : sparta::ParameterSet(n) {}
    };

    PipelineClock(sparta::TreeNode* node, const PipelineClockParams* params);

    void setStages(frontend::FetchStructures* fetch, frontend::FrontendMemoryStructures* memory, frontend::FetchQueue* fetch_queue,
                   frontend::DecodeStructures* decode, frontend::DecodeQueue* decode_queue, frontend::BranchPredictor* bp, midcore::Rename* rename,
                   midcore::Issue* issue, midcore::Execute* execute, midcore::LSQ* lsq, midcore::BackendMemoryStructures* dcache,
                   midcore::Writeback* writeback);

    void setArbiter(midcore::WritePortArbiter* arb) { mArbiter = arb; }
    void setL2(memory::L2Cache* l2) { mL2 = l2; }
    void setFlushArbiter(core::FlushArbiter* fa) { mFlushArbiter = fa; }
    void setVisualizer(core::PipelineVisualizer* v) { mVisualizer = v; }

    void start();

   private:
    void simulationTerminating_() override;
    void tick();
    void reportStats();
    void deadlockDiag();

    sparta::UniqueEvent<> mTickEvent;

    uint64_t mCycle{0};
    uint64_t mLastRetired{0};
    uint64_t mLastReportCycle{0};
    uint64_t mLastRetiredDeadlock{0};
    uint64_t mNoProgressCycles{0};

    // Wall-clock anchors for KIPS/KHz throughput reporting.
    std::chrono::steady_clock::time_point mStartTime{};
    std::chrono::steady_clock::time_point mLastReportTime{};

    static constexpr uint64_t kReportInterval = 1000000;
    static constexpr uint64_t kDeadlockThreshold = 1000;

    frontend::FetchStructures* mFetch = nullptr;
    frontend::FrontendMemoryStructures* mMemory = nullptr;
    frontend::FetchQueue* mFetchQueue = nullptr;
    frontend::DecodeStructures* mDecode = nullptr;
    frontend::DecodeQueue* mDecodeQueue = nullptr;
    frontend::BranchPredictor* mBp = nullptr;
    midcore::Rename* mRename = nullptr;
    midcore::Issue* mIssue = nullptr;
    midcore::Execute* mExecute = nullptr;
    midcore::LSQ* mLsq = nullptr;
    midcore::BackendMemoryStructures* mDcache = nullptr;
    midcore::Writeback* mWriteback = nullptr;
    midcore::WritePortArbiter* mArbiter = nullptr;
    memory::L2Cache* mL2 = nullptr;
    core::FlushArbiter* mFlushArbiter = nullptr;
    core::PipelineVisualizer* mVisualizer = nullptr;
};

}  // namespace core
