#pragma once

#include <deque>
#include <memory>
#include <string>

#include "sparta/ports/DataPort.hpp"
#include "sparta/simulation/ParameterSet.hpp"
#include "sparta/simulation/Unit.hpp"
#include "sparta/statistics/Counter.hpp"

#include "Common/PipelinePacket.hpp"
#include "MemoryHierarchy/CacheInterface.hpp"

namespace memory {

enum class InclusivityPolicy : uint8_t {
    NINE,       // Non-Inclusive, Non-Exclusive (default)
    INCLUSIVE,  // L2 is strictly inclusive of L1
    EXCLUSIVE   // L2 is exclusive (L1 and L2 are disjoint)
};

class L2CacheParams : public sparta::ParameterSet {
   public:
    L2CacheParams(sparta::TreeNode* n)
        : sparta::ParameterSet(n) {}

    PARAMETER(uint32_t, hit_latency, 10, "L2 hit latency in cycles")
    PARAMETER(uint32_t, miss_latency, 100, "L2 miss latency (DRAM) in cycles")
    PARAMETER(uint32_t, mshr_capacity, 16, "L2 MSHR entries")
    PARAMETER(uint64_t, cache_size_kb, 1024, "L2 cache size in KB")
    PARAMETER(uint64_t, cache_line_size, 64, "Cache line size in bytes")
    PARAMETER(uint64_t, cache_associativity, 8, "L2 associativity")
    PARAMETER(std::string, replacement_policy, "lru", "lru | plru")
    PARAMETER(std::string, arb_policy, "dcache_first",
              "Arbitration between icache and dcache requests: "
              "dcache_first | icache_first | round_robin")
    PARAMETER(uint32_t, starvation_threshold, 8, "Cycles before a starved source overrides dcache_first/icache_first policy")
    PARAMETER(uint32_t, queue_capacity, 4, "Max pending requests per source")
    // Inclusivity / writeback
    PARAMETER(std::string, inclusivity_policy, "nine", "nine | inclusive | exclusive")
    PARAMETER(bool, enabled, false, "Enable L2 cache")
};

// Shared unified L2 cache.
//
// Accepts fill requests from the I-cache (icache_request_in) and D-cache
// (dcache_request_in) and routes fill responses back on the matching
// per-source response ports.  A single tag-array lookup is serviced per
// cycle; arbitration between the two sources is configurable.
//
// Token encoding: high bit 63 = source (0=icache, 1=dcache); lower 63 bits
// hold the line address.  This scheme is safe for RISC-V SV39/SV48 where
// physical addresses fit in 56 bits.
class L2Cache : public sparta::Unit {
   public:
    static constexpr char name[] = "l2cache";

    L2Cache(sparta::TreeNode* node, const L2CacheParams* params);

    // Fill request/response ports
    sparta::DataInPort<core::FillRequest> icache_request_in{&unit_port_set_, "icache_request_in"};
    sparta::DataInPort<core::FillRequest> dcache_request_in{&unit_port_set_, "dcache_request_in"};
    sparta::DataOutPort<core::FillResponse> icache_response_out{&unit_port_set_, "icache_response_out"};
    sparta::DataOutPort<core::FillResponse> dcache_response_out{&unit_port_set_, "dcache_response_out"};

    // Writeback port (D-cache dirty evictions)
    sparta::DataInPort<core::WritebackRequest> dcache_writeback_in{&unit_port_set_, "dcache_writeback_in"};

    // Invalidate ports (inclusive L2 back-invalidation)
    sparta::DataOutPort<core::InvalidateRequest> icache_invalidate_out{&unit_port_set_, "icache_invalidate_out"};
    sparta::DataOutPort<core::InvalidateRequest> dcache_invalidate_out{&unit_port_set_, "dcache_invalidate_out"};

    void tick();

    // Attach a CacheTracer for arbitration + L2-hit/miss events.
    // Ownership stays with the caller (CoreTop).  Must be set before tick().
    void setTracer(cpu::CacheTracer* t) { mTracer = t; }

    bool canAcceptIcacheRequest() const { return mIcachePending.size() < mQueueCapacity; }
    bool canAcceptDcacheRequest() const { return mDcachePending.size() < mQueueCapacity; }

    bool isEnabled() const { return mEnabled; }
    uint64_t numHits() const { return mNumHits.get(); }
    uint64_t numMisses() const { return mNumMisses.get(); }
    uint64_t numCoalesced() const { return mNumCoalesced.get(); }

   private:
    enum class Source : uint8_t { ICACHE = 0, DCACHE = 1 };

    static constexpr uint64_t kDcacheBit = 1ULL << 63;

    struct PendingRequest {
        cpu::address_t line_addr;
        Source source;
        uint64_t arrive_cycle{0};
    };

    void receiveIcacheRequest(const core::FillRequest& req);
    void receiveDcacheRequest(const core::FillRequest& req);
    void receiveWriteback(const core::WritebackRequest& req);

    // Pick the next request to service using the configured arbitration policy.
    // Returns nullptr if no requests are pending.
    const PendingRequest* arbitrate() const;

    // Dequeue the front entry for the given source.
    void dequeue(Source src);

    // Handle L2 eviction: for inclusive policy, send invalidate to L1s.
    void handleEviction(cpu::address_t line_addr, bool dirty);

    uint64_t encodeToken(Source src, cpu::address_t line_addr) const { return (src == Source::DCACHE ? kDcacheBit : 0ULL) | line_addr; }
    Source tokenSource(uint64_t token) const { return (token & kDcacheBit) ? Source::DCACHE : Source::ICACHE; }
    cpu::address_t tokenLineAddr(uint64_t token) const { return static_cast<cpu::address_t>(token & ~kDcacheBit); }

    std::deque<PendingRequest> mIcachePending;
    std::deque<PendingRequest> mDcachePending;

    std::unique_ptr<cpu::CacheInterface> mCacheInterface;
    cpu::CacheTracer* mTracer{nullptr};

    std::string mArbPolicy;
    InclusivityPolicy mInclusivityPolicy{InclusivityPolicy::NINE};
    uint32_t mStarvationThreshold;
    uint32_t mQueueCapacity;
    uint64_t mCurrentCycle{0};
    bool mEnabled{false};
    uint32_t mRoundRobinTurn{0};  // 0 = icache, 1 = dcache

    sparta::Counter mNumHits;
    sparta::Counter mNumMisses;
    sparta::Counter mNumCoalesced;
    sparta::Counter mNumWritebacks;
};

}  // namespace memory
