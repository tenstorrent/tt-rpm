// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Tenstorrent USA, Inc.

#pragma once

#include <memory>
#include <string>

#include "sparta/app/Simulation.hpp"
#include "sparta/simulation/TreeNode.hpp"

#include "Common/BypassNetwork.hpp"
#include "Common/PipelineVisualizer.hpp"
#include "Common/WritePortArbiter.hpp"
#include "Common/WritebackBuffer.hpp"
#include "MemoryHierarchy/CacheTracer.hpp"

namespace cpu {
class ExecutionDriver;
}

class ChipSim : public sparta::app::Simulation {
   public:
    ChipSim(sparta::Scheduler* scheduler);
    ~ChipSim() override;

    void buildTree_() override;
    void configureTree_() override;
    void bindTree_() override;

    void run(uint64_t run_time) override;

    void setTargetCommandOverride(const std::string& path) { mTargetCommandOverride = path; }
    void setTraceFilenameOverride(const std::string& path) { mTraceFilenameOverride = path; }
    void setInstructionLimit(uint64_t limit) { mInstructionLimit = limit; }
    void setCpuFreqGHz(double freq) { mCpuFreqGHz = freq; }

   private:
    void initExecutionDriver(const std::string& target_command, const std::string& trace_filename);
    void bindCore(sparta::TreeNode* core_tn);

    cpu::ExecutionDriver* mExecutionDriver = nullptr;

    // Non-Unit objects owned by the simulation
    std::unique_ptr<core::PipelineVisualizer> mVisualizer;
    std::unique_ptr<midcore::WritePortArbiter> mWritePortArbiter;
    std::unique_ptr<midcore::BypassNetwork> mBypassNetwork;
    std::unique_ptr<midcore::WritebackBuffer> mWritebackBuffer;

    std::unique_ptr<cpu::CacheTracer> mIcacheTracer;
    std::unique_ptr<cpu::CacheTracer> mDcacheTracer;
    std::unique_ptr<cpu::CacheTracer> mL2Tracer;

    std::string mTargetCommandOverride;
    std::string mTraceFilenameOverride;
    uint64_t mInstructionLimit{0};
    double mCpuFreqGHz{0};

    std::string mCacheViewerFormat{"log"};
    std::string mCacheViewerOutfile{"stderr"};
};
