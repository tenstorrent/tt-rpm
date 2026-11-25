// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Tenstorrent USA, Inc.

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "models/cluster/common/ClusterCoreIf.hpp"

namespace sparta {
class TreeNode;
class ResourceSet;
namespace app {
class Simulation;
}
}  // namespace sparta

namespace cpu {
class ExecutionDriver;
}

namespace core {

// Register every OOO-core unit resource factory on a simulation's resource set.
// Both the single-core sim (ChipSim) and the cluster sim call this once in their
// constructors so the units can be built as ResourceTreeNodes (and thus receive
// `-c` config parameters).
void registerCoreResources(sparta::ResourceSet* rs);

// ============================================================================
// CoreImpl — the out-of-order core as a self-contained, reusable assembly.
//
// This is the single source of truth for how an OOO core is built and wired.
// It implements cluster::CoreIf, so the cluster can drop it in directly via
// init().  The single-core simulation (ChipSim) reuses the very same steps by
// calling the granular phase hooks across its Sparta phases:
//
//     buildUnits_()   create the core::Core node + every pipeline stage
//                     (+ private L2 / clock / flush arbiter) as ResourceTreeNodes
//     configure_()    clock domain, ISS driver, initial PC, cache-mode overrides
//     wireInternal_() wire the whole pipeline, the last-level cache, helpers
//     finish_()       dump visualizer / cache-tracer output
//
// init() simply bundles build + configure and defers wireInternal_() to Sparta's
// late-bind phase, so the cluster only ever calls core->init().
//
// Memory topology is governed by two independent things:
//   • external_cache  - is there an external shared cache behind this core?
//                       (false: standalone; true: dropped into a cluster)
//   • the private L2   - the existing l2cache.params.enabled knob
//
// The core attaches to the external cache at whichever level is its last private
// one: below the private L2 if enabled (shared cache acts as an L3), otherwise
// straight off the L1s (shared cache acts as the L2/LLC). With no external cache,
// the private hierarchy terminates internally (DRAM countdown).
// ============================================================================
class CoreImpl : public cluster::CoreIf {
   public:
    struct Config {
        sparta::app::Simulation* sim = nullptr;  // owns the resource set / clock manager
        uint32_t index = 0;                      // core id (and tree node suffix)
        bool external_cache = false;             // is there an external shared cache?
        // Parameter overrides applied at build time (path -> value). Lets a host
        // without a Sparta `-c` file (e.g. the cluster) drive core knobs such as
        // l2cache.params.enabled. Standalone ChipSim uses `-c` instead.
        std::vector<std::pair<std::string, std::string>> param_overrides;
        std::string program;                     // ELF to execute (target_command)
        std::string snapshot_folder;             // Whisper snapshot directory; execution-driven
        std::vector<std::string> whisper_flags;  // extra whisper flags (e.g. --configfile <snapshot>/linux.json)
        uint64_t instruction_limit = 0;          // 0 = no limit
        double cpu_freq_ghz = 0.0;               // 0 = inherit root clock
        uint64_t line_size = 64;                 // structural cache line (external cache)
        bool data_carrying = false;              // L1 shadow carries data + self-check vs oracle
        uint32_t shared_harts = 1;               // harts in the Whisper system all cores share
        bool private_system = false;             // this core builds its own single-hart Whisper system
    };

    CoreImpl(sparta::TreeNode* parent, const Config& cfg);
    // Frees the core's tree nodes, so the host's tree must already be in teardown.
    ~CoreImpl() override;

    // High-level, single-call lifecycle used by the cluster.
    void init() override;

    // cluster::CoreIf
    uint32_t numMemMasters() const override;
    mem::MemMaster& memMaster(uint32_t i) override;
    const sparta::Clock* clock() const override;
    void setRequesterId(uint32_t master_idx, uint32_t requester_id) override;
    void start() override;
    cpu::ExecutionDriver* executionDriver() override;

    // Granular phase hooks used by ChipSim across its Sparta phases.
    void buildUnits_();
    void configure_();
    void wireInternal_();
    void finish_();

   private:
    struct Impl;
    std::unique_ptr<Impl> mImpl;
};

}  // namespace core
