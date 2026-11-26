// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Tenstorrent USA, Inc.

#pragma once

#include <deque>
#include <memory>
#include <string>

#include "sparta/ports/DataPort.hpp"
#include "sparta/simulation/ParameterSet.hpp"
#include "sparta/simulation/Unit.hpp"
#include "sparta/statistics/Counter.hpp"

#include "Common/PipelinePacket.hpp"
#include "Common/PrefetchHost.hpp"
#include "MemoryHierarchy/CacheInterface.hpp"
#include "MemoryHierarchy/Dram/MemoryBackend.hpp"
#include "models/cpu/common/MemInterface.hpp"

namespace memory {

enum class InclusivityPolicy : uint8_t {
    NINE,       // Non-Inclusive, Non-Exclusive (default)
    INCLUSIVE,  // L2 is strictly inclusive of L1
};

class L2CacheParams : public sparta::ParameterSet {
   public:
    L2CacheParams(sparta::TreeNode* n)
        : sparta::ParameterSet(n) {}

    PARAMETER(uint32_t, hit_latency, 4, "L2 array latency in cycles; load-to-use adds the L1 and transfer cycles")
    PARAMETER(uint32_t, miss_latency, 100, "Flat L2-miss latency in cycles, used only without a DRAM model")
    PARAMETER(uint32_t, mshr_capacity, 32, "L2 MSHR entries")
    PARAMETER(uint64_t, cache_size_kb, 1024, "L2 cache size in KB")
    PARAMETER(uint64_t, cache_line_size, 64, "Cache line size in bytes")
    PARAMETER(uint64_t, cache_associativity, 16, "L2 associativity")
    PARAMETER(std::string, replacement_policy, "lru", "lru | plru")
    PARAMETER(std::string, arb_policy, "dcache_first",
              "Arbitration between icache and dcache requests: "
              "dcache_first | icache_first | round_robin")
    PARAMETER(uint32_t, starvation_threshold, 8, "Cycles before a starved source overrides dcache_first/icache_first policy")
    PARAMETER(uint32_t, queue_capacity, 16, "Max pending requests per source; must cover that L1's MSHRs")
    PARAMETER(uint32_t, victim_buffer_capacity, 16, "Dirty victims awaiting memory; fills that evict a dirty line stall when full")
    // Inclusivity / writeback
    PARAMETER(std::string, inclusivity_policy, "nine", "nine | inclusive")
    PARAMETER(bool, enabled, true, "Enable L2 cache")
    PARAMETER(std::string, prefetcher_type, "stream", "L2 data prefetcher algorithm ('none' = disabled)")
    PARAMETER(uint32_t, prefetch_degree, 2, "Lines requested per prefetch trigger (without feedback)")
    PARAMETER(uint32_t, prefetch_distance, 16, "stream: lines the prefetch front runs ahead (without feedback)")
    PARAMETER(uint32_t, prefetch_table_entries, 64, "stream: tracked streams")
    PARAMETER(uint32_t, prefetch_queue_capacity, 32, "Prefetch queue entries; new prefetches are dropped when it is full")
    PARAMETER(uint32_t, prefetch_mshr_reserve, 4, "MSHRs prefetches may not use, kept for demand misses")
    PARAMETER(bool, prefetch_feedback, false, "Throttle degree and distance from accuracy, lateness and pollution")
};

// Shared unified L2 cache, wired as a composable mem:: level: a MemSlave below (one tagged
// stream from the L1RequestAggregator, routed to per-source queues by packet source) and a
// MemMaster above (toward the shared cluster cache). A single tag-array lookup is serviced
// per cycle; arbitration between the two sources is configurable.
class L2Cache : public sparta::Unit {
   public:
    static constexpr char name[] = "l2cache";

    L2Cache(sparta::TreeNode* node, const L2CacheParams* params);

    // Downstream face (toward the aggregator/L1s) and upstream face (toward the cluster).
    mem::MemSlave down{&unit_port_set_, "down"};
    mem::MemMaster up{&unit_port_set_, "up"};

    // Route L2 misses/writebacks out over the upstream master (instead of modelling DRAM as
    // a fixed-latency countdown). Call once, before run.
    void enableExternalFill();

    // Send misses and dirty victims to main memory (instead of the flat miss_latency countdown).
    // The L2 ticks the backend. Call once, before run.
    void setMemBackend(MemoryBackend* backend);

    void tick();

    // Attach a CacheTracer for arbitration + L2-hit/miss events.
    // Ownership stays with the caller (ChipSim).  Must be set before tick().
    void setTracer(cpu::CacheTracer* t) { mTracer = t; }

    void setRequesterId(uint32_t id) { mRequesterId = id; }

    bool canAccept(mem::CacheSource src) const { return (src == mem::CacheSource::ICACHE ? mIcachePending.size() : mDcachePending.size()) < mQueueCapacity; }

    // Holds a victim-buffer entry for an L1 dirty eviction (the L1 may need to write past the L2).
    bool reserveWriteback();

    bool isEnabled() const { return mEnabled; }
    uint64_t numHits() const { return mNumHits.get(); }
    uint64_t numMisses() const { return mNumMisses.get(); }
    uint64_t numCoalesced() const { return mNumCoalesced.get(); }
    uint32_t queueCapacity() const { return mQueueCapacity; }
    const core::PrefetchHost* prefetchHost() const { return mPfHost.get(); }

   private:
    using Source = mem::CacheSource;

    struct PendingRequest {
        cpu::address_t line_addr;
        Source source;
        uint64_t arrive_cycle{0};
        bool from_prefetch{false};
        bool is_write{false};  // the L1 sent ReadUnique
    };

    void receiveRequest(const mem::Request& req);  // routes to per-source queue by req.source
    void receiveWriteback(const mem::Writeback& req);

    // External-fill handlers (registered by enableExternalFill()).
    void receiveFillResponse(const mem::Response& resp);
    void receiveInvalidate(const mem::Invalidate& inv);
    void receiveSnoopResponse(const mem::SnoopResponse& rsp);  // relay L1 ack up to the home node

    // Pick the next request to service using the configured arbitration policy.
    // Returns nullptr if no requests are pending.
    const PendingRequest* arbitrate() const;

    // Dequeue the front entry for the given source.
    void dequeue(Source src);

    // Queue a dirty line for main memory.
    void writeToMemory(cpu::address_t line_addr);

    // Handle L2 eviction: for inclusive policy, send invalidate to L1s.
    void handleEviction(cpu::address_t line_addr);

    // Token carries only the requesting source and whether it is an L1 prefetch; the line address is
    // delivered to the completion callback directly, so it is never packed into (and corrupted by) the token.
    static constexpr uint64_t kPrefetchToken = 0x100;
    uint64_t encodeToken(Source src, bool prefetch) const { return static_cast<uint64_t>(src) | (prefetch ? kPrefetchToken : 0); }
    Source tokenSource(uint64_t token) const { return static_cast<Source>(token & ~kPrefetchToken); }

    std::deque<PendingRequest> mIcachePending;
    std::deque<PendingRequest> mDcachePending;

    std::unique_ptr<cpu::CacheInterface> mCacheInterface;
    cpu::CacheTracer* mTracer{nullptr};
    MemoryBackend* mBackend{nullptr};
    std::deque<cpu::address_t> mVictims;  // dirty lines awaiting the backend's write queue
    uint32_t mVictimCapacity;
    uint32_t mWbReserved{0};  // entries held for L1 writebacks in flight
    // Also queues L1 prefetches that fill this level only, so it exists without an L2 prefetcher.
    std::unique_ptr<core::PrefetchHost> mPfHost;
    uint32_t mPfMshrReserve{0};

    std::string mArbPolicy;
    InclusivityPolicy mInclusivityPolicy{InclusivityPolicy::NINE};
    uint32_t mStarvationThreshold;
    uint32_t mQueueCapacity;
    uint64_t mCurrentCycle{0};
    bool mEnabled{false};
    bool mExternalFill{false};
    // Set while a store's request is serviced: its miss asks upstream for ReadUnique. Not passed as
    // loadRequest's exclusive, which would need an L2-originated upgrade path for lines held shared.
    bool mServicingWrite{false};
    uint32_t mRoundRobinTurn{0};  // 0 = icache, 1 = dcache
    uint32_t mRequesterId{0};     // stamped on upstream traffic toward the cluster

    sparta::Counter mNumHits;
    sparta::Counter mNumMisses;
    sparta::Counter mNumL1PrefetchHits;
    sparta::Counter mNumL1PrefetchMisses;
    sparta::Counter mNumCoalesced;
    sparta::Counter mNumWritebacks;
    sparta::Counter mNumMemWrites;
    sparta::Counter mNumMshrFullCycles;
    sparta::Counter mNumMemQueueFullCycles;
    sparta::Counter mNumVictimFullStalls;
    sparta::Counter mNumL1WbStalls;
    sparta::Counter mNumVictimHits;
    sparta::Counter mNumVictimOverflows;
};

}  // namespace memory
