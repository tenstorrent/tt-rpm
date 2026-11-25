// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Tenstorrent USA, Inc.

#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "models/cluster/common/ClusterCoreIf.hpp"
#include "models/cpu/accel/common/AcceleratorIf.hpp"
#include "models/cpu/accel/common/AttachSpec.hpp"

namespace sparta {
class TreeNode;
class Clock;
}  // namespace sparta

class ClusterCache;

namespace cluster {

class ClusterBridge;

// ============================================================================
// Reusable cluster template.
//
// A configuration skeleton that wires an arbitrary set of droppable cores to a
// single shared cluster cache through the standardized memory interface.  It owns
// the shared infrastructure (the shared cache and the boundary wiring) and nothing
// about the cores themselves -- those come from the user-supplied CoreFactory.
//
// Lifecycle mirrors sparta::app::Simulation phases:
//   build()  -> create cores (via factory) + shared cache under the root node
//   bind()   -> connect every core master to a shared-cache slave
//   start()  -> kick off all cores (after the tree is finalized)
// ============================================================================
class Cluster {
   public:
    struct Config {
        uint32_t num_cores = 4;
        uint32_t cache_hit_latency = 12;
        uint32_t cache_miss_latency = 100;
        uint64_t cache_size_kb = 2048;     // shared cluster cache capacity
        uint32_t cache_associativity = 8;  // shared cluster cache ways
        uint64_t cache_line_size = 64;
        bool coherence_enabled = false;  // false => tag-only shared cache, no directory or snoops
        uint32_t coherence_latency = 0;  // extra cycles for a coherence transaction (snoop/transfer)
        bool data_carrying = false;      // L1 shadow carries data + self-check vs oracle
    };

    Cluster(sparta::TreeNode* root, CoreFactory factory, const Config& cfg, const sparta::Clock* root_clock = nullptr);
    // Frees the units and the nodes it created; the owner must have entered teardown.
    ~Cluster();

    // Register an accelerator and how it attaches — call between the constructor and build().
    // The default spec is a fabric peer on the fabric clock with no link delay.
    void addAccelerator(accel::AcceleratorFactory factory, const accel::AttachSpec& attach = {}) {
        mAccelFactories.push_back(std::move(factory));
        mAccelAttach.push_back(attach);
    }

    void build();
    void bind();
    void start();

    ClusterCache* cache() const { return mCache.get(); }
    CoreIf* core(uint32_t i) const { return mCores.at(i).get(); }
    uint32_t numCores() const { return mCfg.num_cores; }
    uint32_t numAccelerators() const { return mAccels.size(); }
    accel::AcceleratorIf* accelerator(uint32_t i) const { return mAccels.at(i).get(); }
    uint32_t numMasters() const { return mTotalMasters; }

   private:
    sparta::TreeNode* mRoot;
    CoreFactory mFactory;
    Config mCfg;
    const sparta::Clock* mRootClock = nullptr;  // shared-cache (root) clock domain

    std::vector<std::unique_ptr<CoreIf>> mCores;
    std::vector<accel::AcceleratorFactory> mAccelFactories;
    std::vector<accel::AttachSpec> mAccelAttach;
    std::vector<std::unique_ptr<accel::AcceleratorIf>> mAccels;
    std::vector<sparta::TreeNode*> mAccelNodes;             // in mNodes; kept for clock/bridge wiring
    std::vector<std::unique_ptr<sparta::TreeNode>> mNodes;  // a TreeNode does not own its children
    std::unique_ptr<ClusterCache> mCache;
    std::vector<std::unique_ptr<ClusterBridge>> mBridges;  // per-master clock-domain bridges
    uint32_t mTotalMasters = 0;
};

}  // namespace cluster
