// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Tenstorrent USA, Inc.

#pragma once

#include <memory>
#include <string>

#include "sparta/app/Simulation.hpp"

#include "Common/CoreImpl.hpp"

// Single-core OOO simulation.  All of the build/wire logic lives in the reusable
// core::CoreImpl (shared with the cluster); ChipSim is just the Simulation
// shell that drives one CoreImpl (with a private L2 backing) across the
// standard Sparta phases.
class ChipSim : public sparta::app::Simulation {
   public:
    ChipSim(sparta::Scheduler* scheduler);
    ~ChipSim() override;

    void buildTree_() override;
    void configureTree_() override;
    void bindTree_() override;

    void run(uint64_t run_time) override;

    void setTargetCommandOverride(const std::string& path) { mTargetCommandOverride = path; }
    void setSnapshotOverride(const std::string& path) { mSnapshotOverride = path; }
    void setInstructionLimit(uint64_t limit) { mInstructionLimit = limit; }
    void setCpuFreqGHz(double freq) { mCpuFreqGHz = freq; }

   private:
    std::unique_ptr<core::CoreImpl> mCore;

    std::string mTargetCommandOverride;
    std::string mSnapshotOverride;
    uint64_t mInstructionLimit{0};
    double mCpuFreqGHz{0};
};
