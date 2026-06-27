// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Tenstorrent USA, Inc.

#include "LoadStoreUnit/BackendMemoryStructures/BackendMemoryStructures.hpp"

#include "Logging.hpp"
#include "MemoryHierarchy/L2Cache.hpp"

namespace midcore {

BackendMemoryStructures::BackendMemoryStructures(sparta::TreeNode* node, const BackendMemoryStructuresParams* params)
    : sparta::Unit(node),
      mHitRate(params->hit_rate),
      mHitLatency(params->hit_latency),
      mMissLatency(params->miss_latency),
      mMshrCapacity(params->mshr_capacity),
      mNumHits(&unit_stat_set_, "num_hits", "D-cache hits", sparta::Counter::COUNT_NORMAL),
      mNumMisses(&unit_stat_set_, "num_misses", "D-cache misses", sparta::Counter::COUNT_NORMAL),
      mNumCoalesced(&unit_stat_set_, "num_coalesced", "D-cache coalesced misses", sparta::Counter::COUNT_NORMAL) {
    mStructuralMode = (params->cache_mode == "structural");
    mRng.seed(params->rng_seed);

    // Read all cache parameters unconditionally
    mCacheInterfaceCfg.mshr_capacity = params->mshr_capacity;
    mCacheInterfaceCfg.hit_latency = params->hit_latency;
    mCacheInterfaceCfg.miss_latency = params->miss_latency;
    mCacheInterfaceCfg.cache.cacheSizeKb = params->cache_size_kb;
    mCacheInterfaceCfg.cache.lineSize = params->cache_line_size;
    mCacheInterfaceCfg.cache.associativity = params->cache_associativity;
    mCacheInterfaceCfg.cache.replacementPolicy = params->replacement_policy;
    mCacheInterfaceCfg.store_buffer_capacity = params->store_buffer_capacity;
    mCacheInterfaceCfg.write_policy = (params->write_policy == "write_through") ? cpu::WritePolicy::WRITE_THROUGH : cpu::WritePolicy::WRITE_BACK;
    mCacheInterfaceCfg.num_banks = params->num_banks;
    mCacheInterfaceCfg.reads_per_bank_per_cycle = params->reads_per_bank_per_cycle;
    mCacheInterfaceCfg.writes_per_bank_per_cycle = params->writes_per_bank_per_cycle;
    mCacheInterfaceCfg.fill_occupies_write_port = params->fill_occupies_write_port;

    if (mStructuralMode) {
        buildCacheInterface();
    }

    request_in.registerConsumerHandler(CREATE_SPARTA_HANDLER_WITH_DATA(BackendMemoryStructures, receiveRequest, core::MemRequest));
    fill_response_in.registerConsumerHandler(CREATE_SPARTA_HANDLER_WITH_DATA(BackendMemoryStructures, receiveFillResponse, core::FillResponse));
    invalidate_in.registerConsumerHandler(CREATE_SPARTA_HANDLER_WITH_DATA(BackendMemoryStructures, receiveInvalidate, core::InvalidateRequest));
}

void BackendMemoryStructures::buildCacheInterface() {
    mCacheInterface = std::make_unique<cpu::CacheInterface>(mCacheInterfaceCfg, [this](uint64_t token, bool miss) {
        ++(miss ? mNumMisses : mNumHits);
        core::MemResponse resp;
        resp.tag = token;
        resp.miss = miss;
        response_out.send(resp, 1);
    });

    if (mCacheInterfaceCfg.external_fill) {
        mCacheInterface->setFillRequestCallback([this](cpu::address_t line_addr) {
            core::FillRequest req;
            req.line_addr = line_addr;
            fill_request_out.send(req, 1);
        });
    }

    // Writeback callback: when dirty L1 line is evicted, send to L2
    // Only send if L2 is configured (mL2 != nullptr), otherwise port is unbound
    mCacheInterface->setWritebackCallback([this](cpu::address_t line_addr) {
        if (!mL2) return;  // No L2 configured, silently drop writeback
        core::WritebackRequest req;
        req.line_addr = line_addr;
        writeback_out.send(req, 1);
        ILOG("[dcache] writeback line=0x" << std::hex << line_addr);
    });

    // Re-attach tracer if one was set before the interface was rebuilt.
    if (mTracer) mCacheInterface->setTracer(mTracer);
}

void BackendMemoryStructures::setL2(memory::L2Cache* l2) {
    mL2 = l2;
    if (mStructuralMode && l2) {
        mCacheInterfaceCfg.external_fill = true;
        buildCacheInterface();
    }
}

bool BackendMemoryStructures::canAcceptRequest(cpu::address_t addr) const {
    if (mStructuralMode) {
        if (!mCacheInterface->canAcceptRequest(addr)) return false;
        if (mL2 && mCacheInterface->willNeedFill(addr) && !mL2->canAcceptDcacheRequest()) return false;
        return true;
    }
    return mOutstandingMisses < mMshrCapacity;
}

bool BackendMemoryStructures::canAcceptStore() const {
    if (mStructuralMode) {
        return !mCacheInterface->isStoreBufferFull();
    }
    return true;  // Probabilistic mode always accepts stores
}

void BackendMemoryStructures::receiveRequest(const core::MemRequest& req) {
    if (mStructuralMode) {
        uint64_t cycle = getClock()->currentCycle();
        if (req.is_store) {
            // Stores go to store buffer; they complete immediately from pipeline's view
            bool accepted = mCacheInterface->storeRequest(req.tag, req.address, req.access_size, cycle);
            if (accepted) {
                // Store buffer accepted; send immediate response (no miss from pipeline view)
                core::MemResponse resp;
                resp.tag = req.tag;
                resp.miss = false;
                response_out.send(resp, 1);
            }
            // If not accepted (store buffer full), LSQ should have checked canAcceptRequest
        } else {
            // Queue load - will be submitted to cache in tick() respecting bank limits
            mStructuralPendingLoads.push_back({req.tag, req.address});
        }
        return;
    }

    // Probabilistic mode
    if (req.is_store) {
        // Stores complete quickly (absorbed by store buffer)
        ++mNumHits;
        core::MemResponse resp;
        resp.tag = req.tag;
        resp.miss = false;
        mPendingMem.push_back({resp, 1, false});  // 1-cycle store buffer latency
        return;
    }

    // Loads use probabilistic hit/miss
    bool hit = mHitRate >= mDist(mRng);
    ++(hit ? mNumHits : mNumMisses);
    uint32_t latency = hit ? mHitLatency : mHitLatency + mMissLatency;
    if (!hit) ++mOutstandingMisses;
    core::MemResponse resp;
    resp.tag = req.tag;
    resp.miss = !hit;
    mPendingMem.push_back({resp, latency, !hit});
}

void BackendMemoryStructures::receiveFillResponse(const core::FillResponse& resp) {
    if (mCacheInterface) {
        mCacheInterface->fillComplete(resp.line_addr, getClock()->currentCycle());
    }
}

void BackendMemoryStructures::receiveInvalidate(const core::InvalidateRequest& req) {
    if (mCacheInterface) {
        mCacheInterface->invalidateLine(req.line_addr, getClock()->currentCycle());
        ILOG("[dcache] back-invalidate line=0x" << std::hex << req.line_addr);
    }
}

void BackendMemoryStructures::tick() {
    if (mStructuralMode) {
        uint64_t cycle = getClock()->currentCycle();
        mCacheInterface->tick(cycle);

        // Submit pending loads to cache (respecting bank limits)
        while (!mStructuralPendingLoads.empty()) {
            auto& pend = mStructuralPendingLoads.front();
            // The LSQ's canAcceptRequest() check at issue time is non-reserving, so a
            // burst of distinct-line loads can all pass against the same MSHR snapshot
            // and over-subscribe the MSHRs by the time they drain here. Re-check before
            // each allocation and back-pressure (retry next cycle) instead of letting
            // loadRequest() assert on MSHR exhaustion.
            if (!mCacheInterface->canAcceptRequest(pend.address)) {
                break;
            }
            uint64_t before = mCacheInterface->numCoalesced();
            bool accepted = mCacheInterface->loadRequest(pend.tag, pend.address, cycle);
            if (!accepted) {
                // Bank conflict - stop trying this cycle, will retry next cycle
                break;
            }
            if (mCacheInterface->numCoalesced() > before) ++mNumCoalesced;
            mStructuralPendingLoads.pop_front();
        }
        return;
    }

    // Probabilistic mode
    for (auto& entry : mPendingMem) {
        if (entry.cycles_remaining > 0) --entry.cycles_remaining;
    }
    while (!mPendingMem.empty() && mPendingMem.front().cycles_remaining == 0) {
        if (mPendingMem.front().is_miss) --mOutstandingMisses;
        response_out.send(mPendingMem.front().resp, 1);
        mPendingMem.pop_front();
    }
}

}  // namespace midcore
