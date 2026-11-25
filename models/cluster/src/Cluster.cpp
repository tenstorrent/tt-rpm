// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Tenstorrent USA, Inc.

#include "Cluster.hpp"

#include <iostream>
#include <string>

#include "sparta/simulation/Clock.hpp"
#include "sparta/simulation/TreeNode.hpp"

#include "ClusterBridge.hpp"
#include "ClusterCache.hpp"
#include "models/cpu/common/MemInterface.hpp"

namespace cluster {

Cluster::Cluster(sparta::TreeNode* root, CoreFactory factory, const Config& cfg, const sparta::Clock* root_clock)
    : mRoot(root),
      mFactory(std::move(factory)),
      mCfg(cfg),
      mRootClock(root_clock) {}

// Units go before the nodes they sit on.
Cluster::~Cluster() {
    mBridges.clear();
    mCache.reset();
    mAccels.clear();
}

void Cluster::build() {
    auto make_node = [this](const std::string& name, const std::string& desc) {
        return mNodes.emplace_back(std::make_unique<sparta::TreeNode>(mRoot, name, desc)).get();
    };

    // Build cores first so the shared cache can be sized to the total master count.
    mCores.reserve(mCfg.num_cores);
    mTotalMasters = 0;
    for (uint32_t i = 0; i < mCfg.num_cores; ++i) {
        auto core = mFactory(mRoot, i);  // the core creates and names its own subtree
        sparta_assert(core, "CoreFactory returned null for core " << i);
        core->init();  // high-level lifecycle: the core brings itself up
        mTotalMasters += core->numMemMasters();
        mCores.push_back(std::move(core));
    }

    // Accelerators: sibling nodes with their masters counted into the cache's requester
    // table. The attach spec picks the clock domain; a null clock inherits the fabric's.
    for (uint32_t i = 0; i < mAccelFactories.size(); ++i) {
        const auto& attach = mAccelAttach[i];
        auto* anode = make_node("accel" + std::to_string(i), "accelerator " + std::to_string(i));
        anode->setClock(const_cast<sparta::Clock*>(attach.clock ? attach.clock : mRootClock));
        mAccelNodes.push_back(anode);
        mAccels.push_back(mAccelFactories[i](anode, i));
        sparta_assert(mAccels.back(), "AcceleratorFactory returned null for accel " << i);
        mAccels.back()->init(anode);
        mTotalMasters += mAccels.back()->numMemMasters();
    }

    auto* cc_node = make_node("cluster_cache", "Shared cluster cache");
    const sparta::Clock* cache_clock = mRootClock ? mRootClock : cc_node->getClock();
    if (cache_clock) cc_node->setClock(const_cast<sparta::Clock*>(cache_clock));  // fabric clock domain
    mCache = std::make_unique<ClusterCache>(cc_node, mTotalMasters, mCfg.cache_hit_latency, mCfg.cache_miss_latency, mCfg.cache_size_kb,
                                            mCfg.cache_associativity, mCfg.cache_line_size, mCfg.coherence_enabled, mCfg.coherence_latency, cache_clock);

    // One clock-domain bridge per memory master: it lives on that core's clock and
    // crosses to the shared cache's clock, so cores may run at different frequencies.
    mBridges.reserve(mTotalMasters);
    for (auto& core : mCores) {
        for (uint32_t m = 0; m < core->numMemMasters(); ++m) {
            const uint32_t gid = static_cast<uint32_t>(mBridges.size());
            const sparta::Clock* core_clk = core->clock();
            auto* bnode = make_node("clk_bridge" + std::to_string(gid), "core<->cache clock bridge");
            bnode->setClock(core_clk);
            mBridges.push_back(std::make_unique<ClusterBridge>(bnode));
        }
    }
    for (uint32_t i = 0; i < mAccelNodes.size(); ++i) {
        for (uint32_t m = 0; m < mAccels[i]->numMemMasters(); ++m) {
            const uint32_t gid = static_cast<uint32_t>(mBridges.size());
            auto* bnode = make_node("clk_bridge" + std::to_string(gid), "accel<->cache clock bridge");
            bnode->setClock(mAccelNodes[i]->getClock());  // accelerator domain
            mBridges.push_back(std::make_unique<ClusterBridge>(bnode, mAccelAttach[i].link_latency_cycles));
        }
    }

    std::cout << "[cluster] Built " << mCfg.num_cores << " cores + " << mAccels.size() << " accelerator(s) exposing " << mTotalMasters
              << " memory master(s) -> 1 shared cluster cache\n";
}

void Cluster::bind() {
    // Assign each master a globally-unique requester id and connect it to the
    // matching shared-cache slave via the standardized interface.
    uint32_t global_id = 0;
    for (auto& core : mCores) {
        for (uint32_t m = 0; m < core->numMemMasters(); ++m) {
            core->setRequesterId(m, global_id);
            // core master -> bridge (same core clock) -> cache slave (cache clock, sync).
            mem::connect(core->memMaster(m), mBridges[global_id]->down());
            mem::connectSync(mBridges[global_id]->up(), mCache->slave(global_id));
            ++global_id;
        }
    }
    for (auto& accel : mAccels) {
        for (uint32_t m = 0; m < accel->numMemMasters(); ++m) {
            accel->setRequesterId(m, global_id);
            mem::connect(accel->memMaster(m), mBridges[global_id]->down());
            mem::connectSync(mBridges[global_id]->up(), mCache->slave(global_id));
            ++global_id;
        }
    }
    std::cout << "[cluster] Connected " << global_id << " master(s) via clock-crossing bridges\n";
}

void Cluster::start() {
    for (auto& core : mCores) core->start();
    for (auto& accel : mAccels) accel->start();
}

}  // namespace cluster
