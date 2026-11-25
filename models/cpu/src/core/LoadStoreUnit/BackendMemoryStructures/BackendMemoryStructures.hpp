// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Tenstorrent USA, Inc.

#pragma once

#include <deque>
#include <functional>
#include <memory>
#include <random>
#include <string>
#include <unordered_map>

#include "sparta/ports/DataPort.hpp"
#include "sparta/simulation/ParameterSet.hpp"
#include "sparta/simulation/Unit.hpp"
#include "sparta/statistics/Counter.hpp"

#include "Common/PipelinePacket.hpp"
#include "Common/PrefetchHost.hpp"
#include "MemoryHierarchy/CacheInterface.hpp"

namespace core {
class L1RequestAggregator;
}

namespace midcore {

class BackendMemoryStructuresParams : public sparta::ParameterSet {
   public:
    BackendMemoryStructuresParams(sparta::TreeNode* n)
        : sparta::ParameterSet(n) {}

    PARAMETER(double, hit_rate, 1.0, "D-cache hit rate (0.0 - 1.0)")
    PARAMETER(uint32_t, hit_latency, 3, "D-cache hit latency in cycles")
    PARAMETER(uint32_t, miss_latency, 10, "D-cache miss latency (no-L2 fallback)")
    PARAMETER(uint32_t, mshr_capacity, 8, "Max outstanding D-cache misses (MSHRs)")
    PARAMETER(std::string, cache_mode, "probabilistic", "Cache mode: probabilistic | structural")
    PARAMETER(uint64_t, cache_size_kb, 32, "Structural cache size in KB")
    PARAMETER(uint64_t, cache_line_size, 64, "Structural cache line size in bytes")
    PARAMETER(uint64_t, cache_associativity, 8, "Structural cache associativity")
    PARAMETER(std::string, replacement_policy, "lru", "Replacement policy: lru | plru")
    // Store buffer / write policy
    PARAMETER(uint32_t, store_buffer_capacity, 8, "Store buffer entries")
    PARAMETER(std::string, write_policy, "write_back", "write_back (the only policy modeled)")
    // Banking
    PARAMETER(uint32_t, num_banks, 1, "Number of banks (1 = no banking)")
    PARAMETER(uint32_t, reads_per_bank_per_cycle, 1, "Read ports per bank")
    PARAMETER(uint32_t, writes_per_bank_per_cycle, 1, "Write ports per bank")
    PARAMETER(bool, fill_occupies_write_port, false, "Fills use the write port")
    PARAMETER(uint64_t, rng_seed, 0xBEEFULL, "Seed for the probabilistic-mode hit/miss RNG")
    PARAMETER(std::string, prefetcher_type, "ip_stride", "Data prefetcher algorithm ('none' = disabled)")
    PARAMETER(uint32_t, prefetch_degree, 2, "Lines requested per prefetch trigger")
    PARAMETER(uint32_t, prefetch_distance, 4, "ip_stride: strides (lines, for sub-line strides) ahead of the trigger, kept inside the page")
    PARAMETER(uint32_t, prefetch_table_entries, 64, "ip_stride: PC-indexed table entries")
    PARAMETER(uint32_t, prefetch_queue_capacity, 16, "Prefetch queue entries; new prefetches are dropped when it is full")
    PARAMETER(uint32_t, prefetch_mshr_reserve, 2, "MSHRs prefetches may not use, kept for demand misses")
    PARAMETER(std::string, prefetch_fill_level, "l1", "Prefetch fill level: l1 | l2 (l2 needs the L2 enabled)")
    // Uncacheable MMIO range (e.g. an accelerator doorbell BAR): stores bypass the L1 and
    // are forwarded downstream every time. Size 0 = disabled.
    PARAMETER(uint64_t, uncacheable_base, 0, "Uncacheable MMIO range base address (0 = disabled)")
    PARAMETER(uint64_t, uncacheable_size, 0, "Uncacheable MMIO range size in bytes (0 = disabled)")
};

class BackendMemoryStructures : public sparta::Unit {
   public:
    static constexpr char name[] = "dcache";

    BackendMemoryStructures(sparta::TreeNode* node, const BackendMemoryStructuresParams* params);

    sparta::DataInPort<core::MemRequest> request_in{&unit_port_set_, "request_in"};
    sparta::DataOutPort<core::MemResponse> response_out{&unit_port_set_, "response_out"};

    // Fill ports — connected to L2Cache when L2 is enabled.
    sparta::DataOutPort<mem::Request> fill_request_out{&unit_port_set_, "fill_request_out"};
    sparta::DataInPort<mem::Response> fill_response_in{&unit_port_set_, "fill_response_in"};

    // Writeback port — sends dirty evictions to L2.
    sparta::DataOutPort<mem::Writeback> writeback_out{&unit_port_set_, "writeback_out"};

    // Invalidate port — receives back-invalidation from inclusive L2.
    sparta::DataInPort<mem::Invalidate> invalidate_in{&unit_port_set_, "invalidate_in"};

    void tick();

    // Connect this L1 to the per-core L1RequestAggregator (the next level below it).
    // Enables external fill/writeback emission and routes backpressure through it.
    // Requires structural cache_mode.
    void setUpstream(core::L1RequestAggregator* agg);

    // Attach a CacheTracer for cache-viewer output.  Ownership stays with
    // caller; must be set before the first tick().
    void setTracer(cpu::CacheTracer* t) {
        mTracer = t;
        if (mCacheInterface) mCacheInterface->setTracer(t);
    }

    // addr: the effective address of the request — used for coalescing in
    // structural mode; ignored (capacity-only check) in probabilistic mode.
    // Use for loads.
    bool canAcceptRequest(cpu::address_t addr) const;

    // Stores the store buffer can still accept.
    uint32_t storeBufferRoom() const { return mStructuralMode ? mCacheInterface->storeBufferRoom() : UINT32_MAX; }

    bool storeBufferEmpty() const { return !mStructuralMode || mCacheInterface->storeBufferEmpty(); }
    // Buffered stores vs [addr, addr + size): 0 none, 1 one store covers it, 2 partial overlap.
    int storeBufferOverlap(cpu::address_t addr, uint32_t size) const { return mStructuralMode ? mCacheInterface->storeBufferOverlap(addr, size) : 0; }

    uint64_t numHits() const { return mNumHits.get(); }
    uint64_t numMisses() const { return mNumMisses.get(); }
    uint64_t numCoalesced() const { return mNumCoalesced.get(); }
    const core::PrefetchHost* prefetchHost() const { return mPfHost.get(); }

    // Data-carrying shadow. Stored so it survives CacheInterface rebuilds
    // (setUpstream rebuilds with external_fill=true), then forwarded to the live interface.
    void setDataCarrying(bool on, cpu::CacheInterface::PeekFn peek) {
        mDataCarrying = on;
        mDataCarryingPeek = std::move(peek);
        if (mCacheInterface) mCacheInterface->setDataCarrying(mDataCarrying, mDataCarryingPeek);
    }
    uint64_t numShadowMismatches() const { return mCacheInterface ? mCacheInterface->numShadowMismatches() : 0; }
    uint64_t numShadowChecks() const { return mCacheInterface ? mCacheInterface->numShadowChecks() : 0; }

    // Structural mode: told at tag-check time that a load hit, and the cycle its data returns.
    void setLoadHitCallback(std::function<void(uint64_t tag, uint64_t req_id, uint64_t data_cycle)> cb) { mLoadHitCb = std::move(cb); }

   private:
    // Probabilistic-mode pending queue.
    struct PendingMem {
        core::MemResponse resp;
        uint32_t cycles_remaining;
        bool is_miss;
    };

    void buildCacheInterface();
    void receiveRequest(const core::MemRequest& req);
    void receiveFillResponse(const mem::Response& resp);
    void receiveInvalidate(const mem::Invalidate& req);

    double mHitRate;
    uint32_t mHitLatency;
    uint32_t mMissLatency;
    uint32_t mMshrCapacity;
    uint32_t mOutstandingMisses{0};  // probabilistic mode only

    bool mStructuralMode{false};
    cpu::CacheInterface::Config mCacheInterfaceCfg;        // saved for re-init
    std::unique_ptr<cpu::CacheInterface> mCacheInterface;  // structural mode
    bool mDataCarrying{false};                             // survives CacheInterface rebuilds
    cpu::CacheInterface::PeekFn mDataCarryingPeek;

    cpu::CacheInterface::PrefetchOutcome issuePrefetch(cpu::address_t addr, uint64_t cycle);

    // Data prefetcher (null when disabled).
    std::unique_ptr<core::PrefetchHost> mPfHost;
    uint32_t mPfMshrReserve{0};
    bool mPfFillL2{false};
    cpu::CacheTracer* mTracer{nullptr};

    core::L1RequestAggregator* mAgg{nullptr};
    bool mWbReserved{false};  // the writeback gate just reserved an entry for the next writeback
    bool mSnooped{false};     // a writeback now comes from invalidateLine, not an eviction
    std::function<void(uint64_t, uint64_t, uint64_t)> mLoadHitCb;

    std::deque<PendingMem> mPendingMem;  // probabilistic mode only

    // Structural mode: loads waiting for bank port availability
    struct PendingLoad {
        uint64_t tag;
        uint64_t req_id;
        cpu::address_t address;
        cpu::address_t pc;
        bool exclusive;
        bool page_walk;
    };
    std::deque<PendingLoad> mStructuralPendingLoads;
    // CacheInterface token -> (tag, req_id) of the load it completes.
    std::unordered_map<uint64_t, std::pair<uint64_t, uint64_t>> mLoadTokens;
    uint64_t mNextToken{0};

    // Per-instance RNG for probabilistic mode (seeded from params->rng_seed in ctor)
    std::mt19937_64 mRng;
    std::uniform_real_distribution<double> mDist{0.0, 1.0};

    sparta::Counter mNumHits;
    sparta::Counter mNumMisses;
    sparta::Counter mNumCoalesced;
    sparta::Counter mNumBankConflicts;
    sparta::Counter mNumUpgrades;
};

}  // namespace midcore
