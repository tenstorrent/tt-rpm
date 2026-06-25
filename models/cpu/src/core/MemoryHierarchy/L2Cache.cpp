#include "MemoryHierarchy/L2Cache.hpp"

#include "Logging.hpp"

namespace memory {

L2Cache::L2Cache(sparta::TreeNode* node, const L2CacheParams* params)
    : sparta::Unit(node),
      mArbPolicy(params->arb_policy),
      mStarvationThreshold(params->starvation_threshold),
      mQueueCapacity(params->queue_capacity),
      mNumHits(&unit_stat_set_, "num_hits", "L2 hits", sparta::Counter::COUNT_NORMAL),
      mNumMisses(&unit_stat_set_, "num_misses", "L2 misses", sparta::Counter::COUNT_NORMAL),
      mNumCoalesced(&unit_stat_set_, "num_coalesced", "L2 coalesced misses", sparta::Counter::COUNT_NORMAL),
      mNumWritebacks(&unit_stat_set_, "num_writebacks", "L2 writebacks from L1", sparta::Counter::COUNT_NORMAL) {
    // Parse inclusivity policy
    std::string inc_str = params->inclusivity_policy;
    if (inc_str == "inclusive")
        mInclusivityPolicy = InclusivityPolicy::INCLUSIVE;
    else if (inc_str == "exclusive")
        mInclusivityPolicy = InclusivityPolicy::EXCLUSIVE;
    else
        mInclusivityPolicy = InclusivityPolicy::NINE;

    cpu::CacheInterface::Config cfg;
    cfg.mshr_capacity = params->mshr_capacity;
    cfg.hit_latency = params->hit_latency;
    cfg.miss_latency = params->miss_latency;
    cfg.external_fill = false;  // L2 uses countdown to model DRAM
    cfg.cache.cacheSizeKb = params->cache_size_kb;
    cfg.cache.lineSize = params->cache_line_size;
    cfg.cache.associativity = params->cache_associativity;
    cfg.cache.replacementPolicy = params->replacement_policy;

    mCacheInterface = std::make_unique<cpu::CacheInterface>(cfg, [this](uint64_t token, bool miss) {
        Source src = tokenSource(token);
        cpu::address_t line_addr = tokenLineAddr(token);

        ++(miss ? mNumMisses : mNumHits);

        core::FillResponse resp;
        resp.line_addr = line_addr;
        if (src == Source::ICACHE)
            icache_response_out.send(resp, 1);
        else
            dcache_response_out.send(resp, 1);
    });

    // Set eviction callback for inclusive policy (back-invalidation)
    mCacheInterface->setEvictionCallback([this](cpu::address_t line_addr, bool dirty) { handleEviction(line_addr, dirty); });

    icache_request_in.registerConsumerHandler(CREATE_SPARTA_HANDLER_WITH_DATA(L2Cache, receiveIcacheRequest, core::FillRequest));
    dcache_request_in.registerConsumerHandler(CREATE_SPARTA_HANDLER_WITH_DATA(L2Cache, receiveDcacheRequest, core::FillRequest));
    dcache_writeback_in.registerConsumerHandler(CREATE_SPARTA_HANDLER_WITH_DATA(L2Cache, receiveWriteback, core::WritebackRequest));

    mEnabled = params->enabled;
}

void L2Cache::receiveIcacheRequest(const core::FillRequest& req) {
    sparta_assert(mIcachePending.size() < mQueueCapacity,
                  "L2Cache: icache request queue overflow — caller must check "
                  "canAcceptIcacheRequest() first");
    mIcachePending.push_back({req.line_addr, Source::ICACHE, mCurrentCycle});
    ILOG("[l2] icache fill request line=0x" << std::hex << req.line_addr);
}

void L2Cache::receiveDcacheRequest(const core::FillRequest& req) {
    sparta_assert(mDcachePending.size() < mQueueCapacity,
                  "L2Cache: dcache request queue overflow — caller must check "
                  "canAcceptDcacheRequest() first");
    mDcachePending.push_back({req.line_addr, Source::DCACHE, mCurrentCycle});
    ILOG("[l2] dcache fill request line=0x" << std::hex << req.line_addr);
}

void L2Cache::receiveWriteback(const core::WritebackRequest& req) {
    // A dirty L1 line was evicted — mark the L2 line as dirty
    ++mNumWritebacks;
    auto* tag = mCacheInterface->tagArray();
    if (tag) {
        auto* line = const_cast<cpu::SimpleCache*>(tag)->getLine(req.line_addr);
        if (line) {
            line->setModified(true);
            ILOG("[l2] writeback received, marking dirty line=0x" << std::hex << req.line_addr);
        }
    }
}

void L2Cache::handleEviction(cpu::address_t line_addr, bool dirty) {
    (void)dirty;  // could log or stat dirty evictions

    if (mInclusivityPolicy != InclusivityPolicy::INCLUSIVE) {
        return;  // NINE/Exclusive: no back-invalidation
    }

    // Inclusive policy: invalidate the line in both L1s
    core::InvalidateRequest inv;
    inv.line_addr = line_addr;
    icache_invalidate_out.send(inv, 1);
    dcache_invalidate_out.send(inv, 1);
    ILOG("[l2] back-invalidate line=0x" << std::hex << line_addr);
}

const L2Cache::PendingRequest* L2Cache::arbitrate() const {
    bool have_i = !mIcachePending.empty();
    bool have_d = !mDcachePending.empty();
    if (!have_i && !have_d) return nullptr;
    if (have_i && !have_d) return &mIcachePending.front();
    if (!have_i && have_d) return &mDcachePending.front();

    // Both sources have pending requests — apply policy.
    if (mArbPolicy == "round_robin") {
        return (mRoundRobinTurn == 0) ? &mIcachePending.front() : &mDcachePending.front();
    }

    // dcache_first / icache_first with starvation protection.
    const PendingRequest& prefer_i = mIcachePending.front();
    const PendingRequest& prefer_d = mDcachePending.front();

    if (mArbPolicy == "icache_first") {
        // I-cache wins unless D-cache is starving.
        bool d_starved = (mCurrentCycle - prefer_d.arrive_cycle) >= mStarvationThreshold;
        return d_starved ? &prefer_d : &prefer_i;
    }

    // Default: dcache_first — D-cache wins unless I-cache is starving.
    bool i_starved = (mCurrentCycle - prefer_i.arrive_cycle) >= mStarvationThreshold;
    return i_starved ? &prefer_i : &prefer_d;
}

void L2Cache::dequeue(Source src) {
    if (src == Source::ICACHE)
        mIcachePending.pop_front();
    else
        mDcachePending.pop_front();
}

void L2Cache::tick() {
    ++mCurrentCycle;

    // Advance existing MSHR fills first so any completions this cycle fire
    // before we potentially allocate a new MSHR below.
    mCacheInterface->tick(mCurrentCycle);

    // Service at most one new fill request per cycle (single-ported tag array).
    const PendingRequest* req = arbitrate();
    if (req) {
        uint64_t token = encodeToken(req->source, req->line_addr);
        if (mCacheInterface->canAcceptRequest(req->line_addr)) {
            uint64_t before = mCacheInterface->numCoalesced();
            mCacheInterface->request(token, req->line_addr, mCurrentCycle);
            if (mCacheInterface->numCoalesced() > before) ++mNumCoalesced;

            // Record arbitration win for the selected source.
            if (mTracer) {
                mTracer->recordL2ArbWin(mCurrentCycle, static_cast<uint8_t>(req->source), req->line_addr);
            }

            // Advance round-robin turn after actually servicing a request.
            if (mArbPolicy == "round_robin") mRoundRobinTurn ^= 1;

            dequeue(req->source);
        }
        // If CacheInterface can't accept (all MSHRs full), leave req in queue
        // and retry next cycle — record arb-lose for all waiting entries.
    }

    // Record arbitration losses only when there was actual contention:
    // - A request was selected (req != nullptr)
    // - The losing source had pending requests that could have been serviced
    if (mTracer && req) {
        // Only log losses for the source that lost arbitration (the other source)
        if (req->source == Source::ICACHE && !mDcachePending.empty()) {
            // D-cache lost this arbitration - only log the front entry (the one that competed)
            const auto& pr = mDcachePending.front();
            uint32_t waited = static_cast<uint32_t>(mCurrentCycle - pr.arrive_cycle);
            mTracer->recordL2ArbLose(mCurrentCycle, static_cast<uint8_t>(Source::DCACHE), pr.line_addr, waited);
        } else if (req->source == Source::DCACHE && !mIcachePending.empty()) {
            // I-cache lost this arbitration - only log the front entry (the one that competed)
            const auto& pr = mIcachePending.front();
            uint32_t waited = static_cast<uint32_t>(mCurrentCycle - pr.arrive_cycle);
            mTracer->recordL2ArbLose(mCurrentCycle, static_cast<uint8_t>(Source::ICACHE), pr.line_addr, waited);
        }
    }
}

}  // namespace memory
