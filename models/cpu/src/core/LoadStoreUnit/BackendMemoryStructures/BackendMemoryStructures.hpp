#pragma once

#include <deque>
#include <memory>
#include <random>
#include <string>

#include "sparta/ports/DataPort.hpp"
#include "sparta/simulation/ParameterSet.hpp"
#include "sparta/simulation/Unit.hpp"
#include "sparta/statistics/Counter.hpp"

#include "Common/PipelinePacket.hpp"
#include "MemoryHierarchy/CacheInterface.hpp"

namespace memory {
class L2Cache;
}

namespace midcore {

class BackendMemoryStructuresParams : public sparta::ParameterSet {
   public:
    BackendMemoryStructuresParams(sparta::TreeNode* n)
        : sparta::ParameterSet(n) {}

    PARAMETER(double, hit_rate, 1.0, "D-cache hit rate (0.0 - 1.0)")
    PARAMETER(uint32_t, hit_latency, 1, "D-cache hit latency in cycles")
    PARAMETER(uint32_t, miss_latency, 10, "D-cache miss latency (no-L2 fallback)")
    PARAMETER(uint32_t, mshr_capacity, 8, "Max outstanding D-cache misses (MSHRs)")
    PARAMETER(std::string, cache_mode, "probabilistic", "Cache mode: probabilistic | structural")
    PARAMETER(uint64_t, cache_size_kb, 64, "Structural cache size in KB")
    PARAMETER(uint64_t, cache_line_size, 64, "Structural cache line size in bytes")
    PARAMETER(uint64_t, cache_associativity, 8, "Structural cache associativity")
    PARAMETER(std::string, replacement_policy, "lru", "Replacement policy: lru | plru")
    // Store buffer / write policy
    PARAMETER(uint32_t, store_buffer_capacity, 8, "Store buffer entries")
    PARAMETER(std::string, write_policy, "write_back", "write_back | write_through")
    // Banking
    PARAMETER(uint32_t, num_banks, 1, "Number of banks (1 = no banking)")
    PARAMETER(uint32_t, reads_per_bank_per_cycle, 1, "Read ports per bank")
    PARAMETER(uint32_t, writes_per_bank_per_cycle, 1, "Write ports per bank")
    PARAMETER(bool, fill_occupies_write_port, false, "Fills use the write port")
    PARAMETER(uint64_t, rng_seed, 0xBEEFULL, "Seed for the probabilistic-mode hit/miss RNG")
};

class BackendMemoryStructures : public sparta::Unit {
   public:
    static constexpr char name[] = "dcache";

    BackendMemoryStructures(sparta::TreeNode* node, const BackendMemoryStructuresParams* params);

    sparta::DataInPort<core::MemRequest> request_in{&unit_port_set_, "request_in"};
    sparta::DataOutPort<core::MemResponse> response_out{&unit_port_set_, "response_out"};

    // Fill ports — connected to L2Cache when L2 is enabled.
    sparta::DataOutPort<core::FillRequest> fill_request_out{&unit_port_set_, "fill_request_out"};
    sparta::DataInPort<core::FillResponse> fill_response_in{&unit_port_set_, "fill_response_in"};

    // Writeback port — sends dirty evictions to L2.
    sparta::DataOutPort<core::WritebackRequest> writeback_out{&unit_port_set_, "writeback_out"};

    // Invalidate port — receives back-invalidation from inclusive L2.
    sparta::DataInPort<core::InvalidateRequest> invalidate_in{&unit_port_set_, "invalidate_in"};

    void tick();

    // Attach the shared L2.  Must be called before the simulation starts.
    void setL2(memory::L2Cache* l2);

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

    // Check if the store buffer can accept a store.
    bool canAcceptStore() const;

    uint64_t numHits() const { return mNumHits.get(); }
    uint64_t numMisses() const { return mNumMisses.get(); }
    uint64_t numCoalesced() const { return mNumCoalesced.get(); }

   private:
    // Probabilistic-mode pending queue.
    struct PendingMem {
        core::MemResponse resp;
        uint32_t cycles_remaining;
        bool is_miss;
    };

    void buildCacheInterface();
    void receiveRequest(const core::MemRequest& req);
    void receiveFillResponse(const core::FillResponse& resp);
    void receiveInvalidate(const core::InvalidateRequest& req);

    double mHitRate;
    uint32_t mHitLatency;
    uint32_t mMissLatency;
    uint32_t mMshrCapacity;
    uint32_t mOutstandingMisses{0};  // probabilistic mode only

    bool mStructuralMode{false};
    cpu::CacheInterface::Config mCacheInterfaceCfg;        // saved for re-init
    std::unique_ptr<cpu::CacheInterface> mCacheInterface;  // structural mode
    cpu::CacheTracer* mTracer{nullptr};

    memory::L2Cache* mL2{nullptr};

    std::deque<PendingMem> mPendingMem;  // probabilistic mode only

    // Structural mode: loads waiting for bank port availability
    struct PendingLoad {
        uint64_t tag;
        cpu::address_t address;
    };
    std::deque<PendingLoad> mStructuralPendingLoads;

    // Per-instance RNG for probabilistic mode (seeded from params->rng_seed in ctor)
    std::mt19937_64 mRng;
    std::uniform_real_distribution<double> mDist{0.0, 1.0};

    sparta::Counter mNumHits;
    sparta::Counter mNumMisses;
    sparta::Counter mNumCoalesced;
};

}  // namespace midcore
