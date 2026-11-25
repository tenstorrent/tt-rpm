// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Tenstorrent USA, Inc.

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "sparta/app/Simulation.hpp"
#include "sparta/kernel/Scheduler.hpp"
#include "sparta/log/Tap.hpp"

#include "Cluster.hpp"

namespace accel {
class RingFrontEnd;
class FunctionSink;
}  // namespace accel

// Concrete top-level simulation: drives the reusable cluster template with a
// chosen core factory (here, the out-of-order core from models/cpu/src/core).
// Swapping in a different core is purely a change of the factory passed to the
// Cluster.
class ClusterSim : public sparta::app::Simulation {
   public:
    explicit ClusterSim(sparta::Scheduler* sched);
    ~ClusterSim() override;

    void run(uint64_t run_time) override;

    // CLI overrides (set from main before populateSimulation; take precedence over
    // the values read from the cluster config file). Mirrors ChipSim's setters.
    void setClusterConfigPath(const std::string& path) { mClusterConfigPath = path; }
    void setTargetCommandOverride(const std::string& path) { mProgramOverride = path; }
    void setSnapshotOverride(const std::string& path) { mSnapshotOverride = path; }
    void setCoreConfigOverride(const std::string& path) { mCoreConfigOverride = path; }
    void setInstructionLimit(uint64_t limit) { mInstructionLimitOverride = limit; }
    void setCpuFreqGHz(double freq) { mCpuFreqOverride = freq; }

   protected:
    void buildTree_() override;
    void configureTree_() override;
    void bindTree_() override;

   private:
    bool mVerbose = false;
    std::unique_ptr<cluster::Cluster> mCluster;
    std::vector<std::unique_ptr<sparta::log::Tap>> mTaps;
    std::vector<std::unique_ptr<accel::RingFrontEnd>> mAccelRings;  // doorbell/ring front-ends
    std::vector<std::unique_ptr<accel::FunctionSink>> mAccelSinks;  // completion logging sinks
    std::vector<uint32_t> mCoreHartids;
    std::vector<std::pair<uint64_t, uint64_t>> mAccelBars;  // (base, size) per accelerator

    // Config file path + optional CLI overrides.
    std::string mClusterConfigPath = "config.yaml";
    std::string mProgramOverride;
    std::string mSnapshotOverride;
    std::string mCoreConfigOverride;
    uint64_t mInstructionLimitOverride = 0;
    double mCpuFreqOverride = 0.0;
};
