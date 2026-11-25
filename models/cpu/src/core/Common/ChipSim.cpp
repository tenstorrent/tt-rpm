// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Tenstorrent USA, Inc.

#include "ChipSim.hpp"

#include <iostream>

#include "sparta/simulation/ResourceTreeNode.hpp"
#include "sparta/simulation/TreeNode.hpp"

#include "Common/CoreImpl.hpp"

// ============================================================================
// ChipSim — thin single-core Simulation shell around core::CoreImpl.
// ============================================================================

ChipSim::ChipSim(sparta::Scheduler* scheduler)
    : sparta::app::Simulation("core_cpu", scheduler) {
    core::registerCoreResources(getResourceSet());
}

// mCore frees its tree nodes, which Sparta allows only in teardown.
ChipSim::~ChipSim() { getRoot()->enterTeardown(); }

void ChipSim::buildTree_() {
    core::CoreImpl::Config cfg;
    cfg.sim = this;
    cfg.index = 0;
    cfg.external_cache = false;  // standalone: private hierarchy terminates at DRAM
    cfg.program = mTargetCommandOverride;
    cfg.snapshot_folder = mSnapshotOverride;
    cfg.instruction_limit = mInstructionLimit;
    cfg.cpu_freq_ghz = mCpuFreqGHz;

    mCore = std::make_unique<core::CoreImpl>(getRoot(), cfg);
    mCore->buildUnits_();
    std::cout << "[chipsim] Tree built\n";
}

void ChipSim::configureTree_() {
    mCore->configure_();
    std::cout << "[chipsim] Tree configured\n";
}

void ChipSim::bindTree_() {
    mCore->wireInternal_();
    std::cout << "[chipsim] All ports bound\n";
}

// ============================================================================
// run()
// ============================================================================

void ChipSim::run(uint64_t run_time) {
    mCore->start();
    sparta::app::Simulation::run(run_time);
    mCore->finish_();
}
