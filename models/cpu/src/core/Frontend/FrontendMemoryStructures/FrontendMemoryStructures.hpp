#pragma once

#include <deque>
#include <memory>
#include <random>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "sparta/ports/DataPort.hpp"
#include "sparta/simulation/ParameterSet.hpp"
#include "sparta/simulation/Unit.hpp"
#include "sparta/statistics/Counter.hpp"

#include "Common/PipelinePacket.hpp"
#include "MemoryHierarchy/CacheInterface.hpp"

namespace memory {
class L2Cache;
}

namespace core {
class PipelineVisualizer;
}

namespace frontend {

class FetchQueue;

class FrontendMemoryStructuresParams : public sparta::ParameterSet {
   public:
    FrontendMemoryStructuresParams(sparta::TreeNode* n)
        : sparta::ParameterSet(n) {}

    PARAMETER(uint32_t, hit_latency, 1, "Icache hit latency in cycles")
    PARAMETER(uint32_t, miss_latency, 20, "Icache miss latency (no-L2 fallback)")
    PARAMETER(double, hit_rate, 1.0, "Icache hit rate (0.0-1.0)")
    PARAMETER(uint32_t, mshr_capacity, 4, "Max outstanding I-cache misses (MSHRs)")
    PARAMETER(std::string, cache_mode, "probabilistic", "Cache mode: probabilistic | structural")
    PARAMETER(uint64_t, cache_size_kb, 64, "Structural cache size in KB")
    PARAMETER(uint64_t, cache_line_size, 64, "Structural cache line size in bytes")
    PARAMETER(uint64_t, cache_associativity, 8, "Structural cache associativity")
    PARAMETER(std::string, replacement_policy, "lru", "Replacement policy: lru | plru")
    PARAMETER(uint64_t, rng_seed, 0xCAFEULL, "Seed for the probabilistic-mode hit/miss RNG")
};

class FrontendMemoryStructures : public sparta::Unit {
   public:
    static constexpr char name[] = "icache";

    FrontendMemoryStructures(sparta::TreeNode* node, const FrontendMemoryStructuresParams* params);

    sparta::DataInPort<core::FetchRequest> request_in{&unit_port_set_, "request_in"};
    sparta::DataOutPort<core::FetchResponse> response_out{&unit_port_set_, "response_out"};

    // Packets forwarded to FetchQueue after cache latency
    sparta::DataOutPort<std::vector<core::PipelinePacket>> packets_to_queue_out{&unit_port_set_, "packets_to_queue_out"};

    // Fill ports — connected to L2Cache when L2 is enabled.
    sparta::DataOutPort<core::FillRequest> fill_request_out{&unit_port_set_, "fill_request_out"};
    sparta::DataInPort<core::FillResponse> fill_response_in{&unit_port_set_, "fill_response_in"};

    // Invalidate port — receives back-invalidation from inclusive L2.
    sparta::DataInPort<core::InvalidateRequest> invalidate_in{&unit_port_set_, "invalidate_in"};

    // Flush port — clears pending packets on misprediction recovery
    sparta::DataInPort<core::FlushRequest> flush_in{&unit_port_set_, "flush_in"};

    void tick();

    // Attach the shared L2.  Must be called before the simulation starts.
    // Re-initialises the structural CacheInterface with external_fill=true and
    // wires the fill-request callback so misses are forwarded to the L2.
    void setL2(memory::L2Cache* l2);

    // Attach the FetchQueue for backpressure checking
    void setFetchQueue(frontend::FetchQueue* fq) { mFetchQueue = fq; }

    // Attach a PipelineVisualizer for debug output
    void setVisualizer(core::PipelineVisualizer* v) { mVis = v; }

    // Attach a CacheTracer for cache-viewer output.  Ownership stays with
    // caller; must be set before the first tick().
    void setTracer(cpu::CacheTracer* t) {
        mTracer = t;
        if (mCacheInterface) mCacheInterface->setTracer(t);
    }

    // addr: the base PC of the next fetch — used for coalescing check in
    // structural mode; ignored (capacity-only check) in probabilistic mode.
    bool canAcceptRequest(cpu::address_t addr) const;

    uint64_t numHits() const { return mNumHits.get(); }
    uint64_t numMisses() const { return mNumMisses.get(); }
    uint64_t numCoalesced() const { return mNumCoalesced.get(); }

   private:
    // Probabilistic-mode pending queue (structural mode uses CacheInterface).
    struct PendingFetch {
        core::FetchRequest request;
        uint32_t cycles_to_complete{0};
        bool hit{false};
    };

    void buildCacheInterface();
    void receiveRequest(const core::FetchRequest& request);
    void receiveFillResponse(const core::FillResponse& resp);
    void receiveInvalidate(const core::InvalidateRequest& req);
    void receiveFlush_(const core::FlushRequest& req);
    void sendResponseProb();
    bool canForwardPackets(const core::FetchRequest& req) const;
    void forwardPackets(const core::FetchRequest& req, bool miss);

    // Packets accepted from Fetch but not yet forwarded to the FetchQueue (in flight
    // inside the icache). Summed on demand from the pending containers, which are both
    // erased on flush, so it can never leak. Used with mPacketsSentThisTick for
    // FetchQueue backpressure accounting.
    size_t pendingPacketCount() const;

    std::deque<PendingFetch> mPendingRequests;  // probabilistic mode only

    // Active flush state - filter packets arriving after flush due to port delay
    bool mFlushActive{false};
    uint64_t mFlushCycle{0};
    core::FlushRequest mActiveFlush;

    double mHitRate;
    uint32_t mHitLatency;
    uint32_t mMissLatency;
    uint32_t mMshrCapacity;
    uint32_t mOutstandingMisses{0};          // probabilistic mode only
    mutable size_t mPacketsSentThisTick{0};  // Track packets sent this tick for backpressure

    bool mStructuralMode{false};
    cpu::CacheInterface::Config mCacheInterfaceCfg;        // saved for re-init
    std::unique_ptr<cpu::CacheInterface> mCacheInterface;  // structural mode
    cpu::CacheTracer* mTracer{nullptr};

    memory::L2Cache* mL2{nullptr};
    frontend::FetchQueue* mFetchQueue{nullptr};
    core::PipelineVisualizer* mVis{nullptr};

    // Structural mode: packets indexed by fetch_seq (unique, never overwrites)
    std::unordered_map<uint64_t, std::vector<core::PipelinePacket>> mStructuralPacketsBySeq;

    // Structural mode: track which fetch_seqs are waiting on each cache line address
    std::unordered_map<uint64_t, std::vector<uint64_t>> mSeqsWaitingOnAddr;

    // Structural mode: requests waiting for bank port availability
    struct PendingRequest {
        uint64_t token;  // Cache line address
        uint64_t fetch_seq;
    };
    std::deque<PendingRequest> mStructuralPendingRequests;  // Queue of requests waiting on bank

    // Structural mode: preserve fetch order despite out-of-order cache completions
    struct PendingCompletion {
        uint64_t fetch_seq;
        bool miss;
    };
    std::vector<PendingCompletion> mPendingCompletions;  // Completed but not yet forwarded
    std::unordered_set<uint64_t> mFlushedSequences;      // Sequences that were flushed
    uint64_t mNextFetchSeq{0};                           // Sequence number for next fetch request
    uint64_t mNextForwardSeq{0};                         // Next sequence to forward

    // Per-instance RNG for probabilistic mode (seeded from params->rng_seed in ctor)
    std::mt19937_64 mRng;
    std::uniform_real_distribution<double> mDist{0.0, 1.0};

    sparta::Counter mNumHits;
    sparta::Counter mNumMisses;
    sparta::Counter mNumCoalesced;
};

}  // namespace frontend
