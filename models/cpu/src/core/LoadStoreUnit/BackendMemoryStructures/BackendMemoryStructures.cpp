// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Tenstorrent USA, Inc.

#include "LoadStoreUnit/BackendMemoryStructures/BackendMemoryStructures.hpp"

#include <utility>

#include "Common/L1RequestAggregator.hpp"
#include "Common/ParamChoice.hpp"
#include "Logging.hpp"
#include "MemoryHierarchy/L2Cache.hpp"
#include "prefetch/common/PrefetcherRegistry.hpp"
#include "prefetch/prefetcher/src/DataPrefetcherInterface.hpp"

namespace midcore {

BackendMemoryStructures::BackendMemoryStructures(sparta::TreeNode* node, const BackendMemoryStructuresParams* params)
    : sparta::Unit(node),
      mHitRate(params->hit_rate),
      mHitLatency(params->hit_latency),
      mMissLatency(params->miss_latency),
      mMshrCapacity(params->mshr_capacity),
      mNumHits(&unit_stat_set_, "num_hits", "D-cache hits", sparta::Counter::COUNT_NORMAL),
      mNumMisses(&unit_stat_set_, "num_misses", "D-cache misses", sparta::Counter::COUNT_NORMAL),
      mNumCoalesced(&unit_stat_set_, "num_coalesced", "D-cache coalesced misses", sparta::Counter::COUNT_NORMAL),
      mNumBankConflicts(&unit_stat_set_, "num_bank_conflicts", "Load-cycles a load lost its bank's read port", sparta::Counter::COUNT_NORMAL),
      mNumUpgrades(&unit_stat_set_, "num_upgrades", "Stores and atomics that waited for ownership of a line held shared", sparta::Counter::COUNT_NORMAL) {
    mStructuralMode = core::checkChoice(params->cache_mode, {"probabilistic", "structural"}) == "structural";
    mRng.seed(params->rng_seed);

    mPfFillL2 = core::checkChoice(params->prefetch_fill_level, {"l1", "l2"}) == "l2";
    mPfMshrReserve = params->prefetch_mshr_reserve;
    prefetch::PrefetcherConfig pf_cfg;
    pf_cfg.line_size = static_cast<uint32_t>(params->cache_line_size);
    pf_cfg.degree = params->prefetch_degree;
    pf_cfg.distance = params->prefetch_distance;
    pf_cfg.table_entries = params->prefetch_table_entries;
    core::PrefetchHost::Config host_cfg;
    host_cfg.line_size = pf_cfg.line_size;
    host_cfg.queue_capacity = params->prefetch_queue_capacity;
    if (params->prefetcher_type.getValue() != "none") {
        sparta_assert(mPfMshrReserve < params->mshr_capacity, params->prefetch_mshr_reserve.getLocation()
                                                                  << " (" << mPfMshrReserve << ") must be below " << params->mshr_capacity.getLocation() << " ("
                                                                  << params->mshr_capacity.getValue() << ") with a prefetcher");
        sparta_assert(host_cfg.queue_capacity > 0, params->prefetch_queue_capacity.getLocation() << " must be nonzero with a prefetcher");
        sparta_assert(pf_cfg.degree > 0, params->prefetch_degree.getLocation() << " must be nonzero with a prefetcher");
        auto pf =
            prefetch::Registry<prefetch::DataPrefetcherInterface, prefetch::PrefetcherConfig>::instance().create(params->prefetcher_type.getValue(), pf_cfg);
        mPfHost = std::make_unique<core::PrefetchHost>(&unit_stat_set_, std::move(pf), host_cfg);
    }

    // Read all cache parameters unconditionally
    mCacheInterfaceCfg.mshr_capacity = params->mshr_capacity;
    mCacheInterfaceCfg.hit_latency = params->hit_latency;
    mCacheInterfaceCfg.miss_latency = params->miss_latency;
    mCacheInterfaceCfg.cache.cacheSizeKb = params->cache_size_kb;
    mCacheInterfaceCfg.cache.lineSize = params->cache_line_size;
    mCacheInterfaceCfg.cache.associativity = params->cache_associativity;
    mCacheInterfaceCfg.cache.sizeParam = params->cache_size_kb.getLocation();
    mCacheInterfaceCfg.cache.lineSizeParam = params->cache_line_size.getLocation();
    mCacheInterfaceCfg.cache.associativityParam = params->cache_associativity.getLocation();
    mCacheInterfaceCfg.cache.replacementPolicy = params->replacement_policy;
    mCacheInterfaceCfg.store_buffer_capacity = params->store_buffer_capacity;
    core::checkChoice(params->write_policy, {"write_back"});
    mCacheInterfaceCfg.num_banks = params->num_banks;
    mCacheInterfaceCfg.reads_per_bank_per_cycle = params->reads_per_bank_per_cycle;
    mCacheInterfaceCfg.writes_per_bank_per_cycle = params->writes_per_bank_per_cycle;
    mCacheInterfaceCfg.fill_occupies_write_port = params->fill_occupies_write_port;
    mCacheInterfaceCfg.uncacheable_base = params->uncacheable_base;
    mCacheInterfaceCfg.uncacheable_size = params->uncacheable_size;

    if (mStructuralMode) {
        buildCacheInterface();
    }

    request_in.registerConsumerHandler(CREATE_SPARTA_HANDLER_WITH_DATA(BackendMemoryStructures, receiveRequest, core::MemRequest));
    fill_response_in.registerConsumerHandler(CREATE_SPARTA_HANDLER_WITH_DATA(BackendMemoryStructures, receiveFillResponse, mem::Response));
    invalidate_in.registerConsumerHandler(CREATE_SPARTA_HANDLER_WITH_DATA(BackendMemoryStructures, receiveInvalidate, mem::Invalidate));
}

void BackendMemoryStructures::buildCacheInterface() {
    mCacheInterface = std::make_unique<cpu::CacheInterface>(mCacheInterfaceCfg, [this](uint64_t token, cpu::address_t line_addr, bool miss) {
        (void)line_addr;  // dcache response is keyed by the pipeline tag, not the address
        ++(miss ? mNumMisses : mNumHits);
        const auto it = mLoadTokens.find(token);
        core::MemResponse resp;
        std::tie(resp.tag, resp.req_id) = it->second;
        mLoadTokens.erase(it);
        resp.miss = miss;
        response_out.send(resp, 1);
    });

    if (mPfHost) {
        mPfHost->bind(mCacheInterface.get());
        mCacheInterface->setEvictionCallback([this](cpu::address_t line_addr, bool) { mPfHost->onEviction(line_addr, getClock()->currentCycle()); });
        mCacheInterface->setFillDoneCallback(
            [this](cpu::address_t line_addr, bool prefetched) { mPfHost->onFill(line_addr, prefetched, getClock()->currentCycle()); });
        // Stores train at drain, with their true hit/miss.
        mCacheInterface->setStoreDrainCallback([this](cpu::address_t addr, cpu::address_t pc, bool miss) {
            cpu::CacheInterface::Probe p;
            p.hit = !miss;
            mPfHost->train(p, addr, pc, /*is_store=*/true, prefetch::CacheLevel::L1D, getClock()->currentCycle());
        });
    }

    if (mCacheInterfaceCfg.external_fill) {
        mCacheInterface->setFillRequestCallback([this](cpu::address_t line_addr, bool write_allocate, bool prefetch) {
            mem::Request req;
            req.line_addr = line_addr;
            req.source = mem::CacheSource::DCACHE;
            req.opcode = write_allocate ? mem::ReqOpcode::ReadUnique : mem::ReqOpcode::ReadShared;
            req.from_prefetch = prefetch;
            fill_request_out.send(req, 1);
        });
        // Write to a line held shared: ask for ownership; the grant comes back as a fill response.
        mCacheInterface->setUpgradeRequestCallback([this](cpu::address_t line_addr) {
            ++mNumUpgrades;
            mem::Request req;
            req.line_addr = line_addr;
            req.source = mem::CacheSource::DCACHE;
            req.opcode = mem::ReqOpcode::CleanUnique;
            fill_request_out.send(req, 1);
        });
    }

    // Writeback callback: when dirty L1 line is evicted, send to L2
    // Only send if L2 is configured (mL2 != nullptr), otherwise port is unbound
    mCacheInterface->setWritebackCallback([this](cpu::address_t line_addr) {
        if (!mAgg) return;  // No next level configured, silently drop writeback
        mem::Writeback req;
        req.line_addr = line_addr;
        req.source = mem::CacheSource::DCACHE;
        req.reserved = std::exchange(mWbReserved, false);  // a snoop's writeback passes no gate
        req.keep_sharer = mSnooped;                        // whoever invalidated the line already updated the directory
        writeback_out.send(req, 1);
        ILOG("[dcache] writeback line=0x" << std::hex << line_addr);
    });

    // A dirty victim may go past the L2 to memory, so it needs a victim-buffer entry.
    if (mAgg) mCacheInterface->setWritebackGate([this] { return mWbReserved = mAgg->reserveWriteback(); });

    // Re-attach tracer if one was set before the interface was rebuilt.
    if (mTracer) mCacheInterface->setTracer(mTracer);
    // Re-apply data-carrying shadow config (survives rebuilds, e.g. setUpstream).
    if (mDataCarrying) mCacheInterface->setDataCarrying(mDataCarrying, mDataCarryingPeek);
}

void BackendMemoryStructures::setUpstream(core::L1RequestAggregator* agg) {
    mAgg = agg;
    if (mStructuralMode && agg) {
        mCacheInterfaceCfg.external_fill = true;
        buildCacheInterface();
    }
}

bool BackendMemoryStructures::canAcceptRequest(cpu::address_t addr) const {
    if (mStructuralMode) {
        if (!mCacheInterface->canAcceptRequest(addr)) return false;
        if (mAgg && mCacheInterface->willNeedFill(addr) && !mAgg->canAccept(mem::CacheSource::DCACHE)) return false;
        return true;
    }
    return mOutstandingMisses < mMshrCapacity;
}

void BackendMemoryStructures::receiveRequest(const core::MemRequest& req) {
    if (mStructuralMode) {
        uint64_t cycle = getClock()->currentCycle();
        if (req.is_store) {
            // Stores go to store buffer; they complete immediately from pipeline's view.
            // The prefetcher is notified at drain time (setStoreDrainCallback) with the
            // store's true cache hit/miss.
            bool accepted = mCacheInterface->storeRequest(req.tag, req.address, req.pc, req.access_size, cycle);
            if (accepted) {
                // Store buffer accepted; send immediate response (no miss from pipeline view)
                core::MemResponse resp;
                resp.tag = req.tag;
                resp.req_id = req.req_id;
                resp.miss = false;
                response_out.send(resp, 1);
            }
            // If not accepted (store buffer full), LSQ should have checked canAcceptRequest
        } else {
            // Queue load - will be submitted to cache in tick() respecting bank limits
            mStructuralPendingLoads.push_back({req.tag, req.req_id, req.address, req.pc, req.exclusive, req.page_walk});
        }
        return;
    }

    // Probabilistic mode
    if (req.is_store) {
        // Stores complete quickly (absorbed by store buffer)
        ++mNumHits;
        core::MemResponse resp;
        resp.tag = req.tag;
        resp.req_id = req.req_id;
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
    resp.req_id = req.req_id;
    resp.miss = !hit;
    mPendingMem.push_back({resp, latency, !hit});
}

void BackendMemoryStructures::receiveFillResponse(const mem::Response& resp) {
    if (mCacheInterface) {
        // MMIO doorbell ack: the uncacheable bypass allocated no MSHR for this line, so
        // there is nothing to complete — the ack only tells the fabric the store landed.
        if (mCacheInterface->isUncacheable(resp.line_addr)) {
            ILOG("[dcache] mmio ack line=0x" << std::hex << resp.line_addr);
            return;
        }
        const bool unique = resp.granted_state == mem::LineState::E || resp.granted_state == mem::LineState::M;
        if (resp.granted_state == mem::LineState::I)
            mCacheInterface->invalidateLine(resp.line_addr, getClock()->currentCycle());  // serve the waiters, keep nothing
        mCacheInterface->fillComplete(resp.line_addr, getClock()->currentCycle(), unique);
    }
}

void BackendMemoryStructures::receiveInvalidate(const mem::Invalidate& req) {
    if (mCacheInterface) {
        mSnooped = true;
        mCacheInterface->invalidateLine(req.line_addr, getClock()->currentCycle());
        mSnooped = false;
        ILOG("[dcache] back-invalidate line=0x" << std::hex << req.line_addr);
    }
}

cpu::CacheInterface::PrefetchOutcome BackendMemoryStructures::issuePrefetch(cpu::address_t addr, uint64_t cycle) {
    using Outcome = cpu::CacheInterface::PrefetchOutcome;
    if (mPfFillL2) {
        sparta_assert(mAgg && mAgg->hasL2(), "dcache.prefetch_fill_level = l2 needs l2cache.enabled");
        const auto p = mCacheInterface->probe(addr);
        if (p.hit || p.in_flight || mCacheInterface->isUncacheable(addr)) return Outcome::Filtered;
        mem::Request req;
        req.line_addr = addr;
        req.source = mem::CacheSource::DCACHE;
        req.prefetch = true;
        fill_request_out.send(req, 1);
        return Outcome::Issued;
    }
    if (mAgg && mCacheInterface->willNeedFill(addr) && !mAgg->canAccept(mem::CacheSource::DCACHE)) return Outcome::NoMshr;
    return mCacheInterface->prefetchRequest(addr, cycle, mPfMshrReserve);
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
            const bool train = mPfHost && !pend.page_walk;
            const auto probe = train ? mCacheInterface->probe(pend.address) : cpu::CacheInterface::Probe{};
            uint64_t hits_before = mCacheInterface->numHits();
            const uint64_t token = ++mNextToken;
            mLoadTokens[token] = {pend.tag, pend.req_id};
            bool accepted = mCacheInterface->loadRequest(token, pend.address, cycle, pend.exclusive, /*demand=*/!pend.page_walk);
            if (!accepted) {
                mLoadTokens.erase(token);
                // Bank conflict - stop trying this cycle, will retry next cycle
                ++mNumBankConflicts;
                break;
            }
            if (train) mPfHost->train(probe, pend.address, pend.pc, /*is_store=*/false, prefetch::CacheLevel::L1D, cycle);
            if (mCacheInterface->numCoalesced() > before) ++mNumCoalesced;
            if (mLoadHitCb && mCacheInterface->numHits() > hits_before) mLoadHitCb(pend.tag, pend.req_id, cycle + mCacheInterfaceCfg.hit_latency);
            mStructuralPendingLoads.pop_front();
        }

        // Issue prefetches after demand loads so they never starve demand traffic.
        if (mPfHost) mPfHost->tick([this, cycle](cpu::address_t addr) { return issuePrefetch(addr, cycle); });
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
