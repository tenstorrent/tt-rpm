// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Tenstorrent USA, Inc.

#include "MemoryHierarchy/L2Cache.hpp"

#include <algorithm>
#include <optional>

#include "Common/ParamChoice.hpp"
#include "Logging.hpp"
#include "prefetch/common/PrefetcherRegistry.hpp"
#include "prefetch/prefetcher/src/DataPrefetcherInterface.hpp"

namespace memory {

L2Cache::L2Cache(sparta::TreeNode* node, const L2CacheParams* params)
    : sparta::Unit(node),
      mVictimCapacity(params->victim_buffer_capacity),
      mArbPolicy(core::checkChoice(params->arb_policy, {"dcache_first", "icache_first", "round_robin"})),
      mStarvationThreshold(params->starvation_threshold),
      mQueueCapacity(params->queue_capacity),
      mNumHits(&unit_stat_set_, "num_hits", "L2 hits", sparta::Counter::COUNT_NORMAL),
      mNumMisses(&unit_stat_set_, "num_misses", "L2 misses", sparta::Counter::COUNT_NORMAL),
      mNumL1PrefetchHits(&unit_stat_set_, "num_l1_prefetch_hits", "L1 prefetch fills that hit (not in num_hits)", sparta::Counter::COUNT_NORMAL),
      mNumL1PrefetchMisses(&unit_stat_set_, "num_l1_prefetch_misses", "L1 prefetch fills that missed (not in num_misses)", sparta::Counter::COUNT_NORMAL),
      mNumCoalesced(&unit_stat_set_, "num_coalesced", "L2 coalesced misses", sparta::Counter::COUNT_NORMAL),
      mNumWritebacks(&unit_stat_set_, "num_writebacks", "L2 writebacks from L1", sparta::Counter::COUNT_NORMAL),
      mNumMemWrites(&unit_stat_set_, "num_mem_writes", "Dirty lines written to main memory", sparta::Counter::COUNT_NORMAL),
      mNumMshrFullCycles(&unit_stat_set_, "num_mshr_full_cycles", "Cycles the next request waited for a free L2 MSHR", sparta::Counter::COUNT_NORMAL),
      mNumMemQueueFullCycles(&unit_stat_set_, "num_mem_queue_full_cycles", "Cycles the next miss waited for the memory read queue",
                             sparta::Counter::COUNT_NORMAL),
      mNumVictimFullStalls(&unit_stat_set_, "num_victim_full_stalls", "Fill-cycles deferred because the victim buffer was full", sparta::Counter::COUNT_NORMAL),
      mNumL1WbStalls(&unit_stat_set_, "num_l1_writeback_stalls", "L1 fill-cycles deferred because the victim buffer had no room for the L1's dirty victim",
                     sparta::Counter::COUNT_NORMAL),
      mNumVictimHits(&unit_stat_set_, "num_victim_hits", "L2 misses served from the victim buffer instead of memory", sparta::Counter::COUNT_NORMAL),
      mNumVictimOverflows(&unit_stat_set_, "num_victim_overflows", "Snoop or back-invalidate writebacks queued past victim_buffer_capacity (they cannot wait)",
                          sparta::Counter::COUNT_NORMAL) {
    // Parse inclusivity policy
    mInclusivityPolicy =
        core::checkChoice(params->inclusivity_policy, {"nine", "inclusive"}) == "inclusive" ? InclusivityPolicy::INCLUSIVE : InclusivityPolicy::NINE;

    cpu::CacheInterface::Config cfg;
    cfg.mshr_capacity = params->mshr_capacity;
    cfg.hit_latency = params->hit_latency;
    cfg.miss_latency = params->miss_latency;
    cfg.external_fill = false;  // flat miss_latency countdown until a next level is attached
    cfg.cache.cacheSizeKb = params->cache_size_kb;
    cfg.cache.lineSize = params->cache_line_size;
    cfg.cache.associativity = params->cache_associativity;
    cfg.cache.sizeParam = params->cache_size_kb.getLocation();
    cfg.cache.lineSizeParam = params->cache_line_size.getLocation();
    cfg.cache.associativityParam = params->cache_associativity.getLocation();
    cfg.cache.replacementPolicy = params->replacement_policy;

    mCacheInterface = std::make_unique<cpu::CacheInterface>(cfg, [this](uint64_t token, cpu::address_t line_addr, bool miss) {
        Source src = tokenSource(token);

        if (token & kPrefetchToken)
            ++(miss ? mNumL1PrefetchMisses : mNumL1PrefetchHits);
        else
            ++(miss ? mNumMisses : mNumHits);

        mem::Response resp;
        resp.line_addr = line_addr;
        resp.source = src;  // aggregator demuxes back to the right L1
        const auto* held = mCacheInterface->tagArray()->peekLine(line_addr);
        // Not held: a snoop hit the fill in flight, so the home no longer lists this core and the L1 must not keep the line.
        resp.granted_state = !held ? mem::LineState::I : held->isUnique() ? mem::LineState::E : mem::LineState::S;
        down.resp_out.send(resp, 1);
    });

    prefetch::PrefetcherConfig pf_cfg;
    pf_cfg.line_size = static_cast<uint32_t>(params->cache_line_size);
    pf_cfg.degree = params->prefetch_degree;
    pf_cfg.distance = params->prefetch_distance;
    pf_cfg.table_entries = params->prefetch_table_entries;
    pf_cfg.feedback = params->prefetch_feedback;
    std::unique_ptr<prefetch::PrefetcherInterface> pf;
    if (params->prefetcher_type.getValue() != "none") {
        sparta_assert(params->prefetch_mshr_reserve < params->mshr_capacity, params->prefetch_mshr_reserve.getLocation()
                                                                                 << " (" << params->prefetch_mshr_reserve.getValue() << ") must be below "
                                                                                 << params->mshr_capacity.getLocation() << " ("
                                                                                 << params->mshr_capacity.getValue() << ") with a prefetcher");
        sparta_assert(params->prefetch_queue_capacity > 0, params->prefetch_queue_capacity.getLocation() << " must be nonzero with a prefetcher");
        sparta_assert(params->prefetch_degree > 0, params->prefetch_degree.getLocation() << " must be nonzero with a prefetcher");
        pf = prefetch::Registry<prefetch::DataPrefetcherInterface, prefetch::PrefetcherConfig>::instance().create(params->prefetcher_type.getValue(), pf_cfg);
    }
    core::PrefetchHost::Config hc;
    hc.line_size = static_cast<uint32_t>(params->cache_line_size);
    hc.queue_capacity = params->prefetch_queue_capacity;
    // Srinath et al. (HPCA 2007): an interval ends after evictions of half the cache's blocks.
    if (pf && params->prefetch_feedback) hc.feedback_interval = params->cache_size_kb * 1024 / params->cache_line_size / 2;
    mPfHost = std::make_unique<core::PrefetchHost>(&unit_stat_set_, std::move(pf), hc);
    mPfHost->bind(mCacheInterface.get());
    mPfMshrReserve = params->prefetch_mshr_reserve;
    mCacheInterface->setFillDoneCallback([this](cpu::address_t line_addr, bool prefetched) { mPfHost->onFill(line_addr, prefetched, mCurrentCycle); });

    // Set eviction callback for inclusive policy (back-invalidation)
    mCacheInterface->setEvictionCallback([this](cpu::address_t line_addr, bool) {
        mPfHost->onEviction(line_addr, mCurrentCycle);
        handleEviction(line_addr);
    });

    down.req_in.registerConsumerHandler(CREATE_SPARTA_HANDLER_WITH_DATA(L2Cache, receiveRequest, mem::Request));
    down.wb_in.registerConsumerHandler(CREATE_SPARTA_HANDLER_WITH_DATA(L2Cache, receiveWriteback, mem::Writeback));

    mEnabled = params->enabled;
}

void L2Cache::enableExternalFill() {
    mExternalFill = true;
    mCacheInterface->setExternalFill(true);

    // On an L2 miss, request the line from the higher level instead of counting
    // down a DRAM latency. The MSHR completes when receiveFillResponse() arrives.
    mCacheInterface->setFillRequestCallback([this](cpu::address_t line_addr, bool write_allocate, bool) {
        mem::Request req;
        req.line_addr = line_addr;
        req.requester_id = mRequesterId;
        req.opcode = write_allocate || mServicingWrite ? mem::ReqOpcode::ReadUnique : mem::ReqOpcode::ReadShared;
        up.req_out.send(req, 1);
        ILOG("[l2] external fill request line=0x" << std::hex << line_addr << std::dec);
    });

    // A dirty L2 line evicted to the higher level (write-back).
    mCacheInterface->setWritebackCallback([this](cpu::address_t line_addr) {
        mem::Writeback wb;
        wb.line_addr = line_addr;
        wb.requester_id = mRequesterId;
        wb.keep_sharer = true;  // the L1s are not tracked and may still hold the line
        ++mNumWritebacks;
        up.wb_out.send(wb, 1);
        ILOG("[l2] external writeback line=0x" << std::hex << line_addr << std::dec);
    });

    up.resp_in.registerConsumerHandler(CREATE_SPARTA_HANDLER_WITH_DATA(L2Cache, receiveFillResponse, mem::Response));
    up.inval_in.registerConsumerHandler(CREATE_SPARTA_HANDLER_WITH_DATA(L2Cache, receiveInvalidate, mem::Invalidate));
    // Relay the L1s' snoop-ack up to the home node (else its held grant never releases).
    down.snprsp_in.registerConsumerHandler(CREATE_SPARTA_HANDLER_WITH_DATA(L2Cache, receiveSnoopResponse, mem::SnoopResponse));
}

void L2Cache::setMemBackend(MemoryBackend* backend) {
    sparta_assert(mVictimCapacity > 0, "l2cache.params.victim_buffer_capacity must be non-zero");
    mBackend = backend;
    mCacheInterface->setExternalFill(true);
    mCacheInterface->setFillRequestCallback([this](cpu::address_t line_addr, bool, bool) {
        if (std::find(mVictims.begin(), mVictims.end(), line_addr) == mVictims.end()) {
            mBackend->access(line_addr, /*is_write=*/false);
            return;
        }
        ++mNumVictimHits;  // the entry still drains to memory; the refill is clean
        mCacheInterface->fillComplete(line_addr, mCurrentCycle, /*unique=*/true);
    });
    mCacheInterface->setWritebackCallback([this](cpu::address_t line_addr) { writeToMemory(line_addr); });
    mCacheInterface->setWritebackGate([this] {
        if (mVictims.size() + mWbReserved < mVictimCapacity) return true;
        ++mNumVictimFullStalls;
        return false;
    });
    // The only requester of main memory: every line it returns is exclusive.
    mBackend->setReadDoneCallback([this](uint64_t line_addr) { mCacheInterface->fillComplete(line_addr, mCurrentCycle, /*unique=*/true); });
}

void L2Cache::writeToMemory(cpu::address_t line_addr) {
    if (std::find(mVictims.begin(), mVictims.end(), line_addr) != mVictims.end()) return;  // merges into the queued entry
    if (mVictims.size() >= mVictimCapacity) ++mNumVictimOverflows;
    mVictims.push_back(line_addr);
}

bool L2Cache::reserveWriteback() {
    if (!mBackend) return true;
    if (mVictims.size() + mWbReserved >= mVictimCapacity) {
        ++mNumL1WbStalls;
        return false;
    }
    ++mWbReserved;
    return true;
}

void L2Cache::receiveFillResponse(const mem::Response& resp) {
    if (resp.resp_opcode == mem::RespOpcode::CompUpgrade) {  // an L1's CleanUnique: the core now owns the line
        if (auto* line = const_cast<cpu::SimpleCache*>(mCacheInterface->tagArray())->getLine(resp.line_addr)) line->setUnique(true);
        down.resp_out.send(resp, 1);
        return;
    }
    ILOG("[l2] external fill done line=0x" << std::hex << resp.line_addr << std::dec);
    const bool unique = resp.granted_state == mem::LineState::E || resp.granted_state == mem::LineState::M;
    mCacheInterface->fillComplete(resp.line_addr, mCurrentCycle, unique);
}

void L2Cache::receiveSnoopResponse(const mem::SnoopResponse& rsp) {
    // Forward the aggregator's ack up so the home node can release its held grant.
    up.snprsp_out.send(rsp, 1);
}

void L2Cache::receiveInvalidate(const mem::Invalidate& inv) {
    // Drop the line locally and propagate the snoop down (the L2 is inclusive). Forward
    // the whole snoop so its txn_id survives -- the aggregator's ack echoes it, and the
    // home node needs it to match the held grant (else txn=0 and it never releases).
    mCacheInterface->invalidateLine(inv.line_addr, mCurrentCycle);
    down.inval_out.send(inv, 1);  // aggregator fans to both L1s and acks with inv.txn_id
    ILOG("[l2] external back-invalidate line=0x" << std::hex << inv.line_addr << std::dec << " txn=" << inv.txn_id);
}

void L2Cache::receiveRequest(const mem::Request& req) {
    // CleanUnique is an ownership upgrade, not a fill: no MSHR. Mark the inclusive line dirty and
    // forward it upstream so the directory can invalidate other sharers; its grant is relayed down.
    if (req.opcode == mem::ReqOpcode::CleanUnique) {
        auto* tag = mCacheInterface->tagArray();
        if (tag) {
            if (auto* line = const_cast<cpu::SimpleCache*>(tag)->getLine(req.line_addr)) line->setModified(true);
        }
        if (mExternalFill) {
            mem::Request up_req;
            up_req.line_addr = req.line_addr;
            up_req.requester_id = mRequesterId;
            up_req.source = req.source;
            up_req.opcode = mem::ReqOpcode::CleanUnique;
            up.req_out.send(up_req, 1);
        } else {
            mem::Response resp;  // no other requester to invalidate
            resp.line_addr = req.line_addr;
            resp.source = req.source;
            resp.resp_opcode = mem::RespOpcode::CompUpgrade;
            resp.granted_state = mem::LineState::M;
            down.resp_out.send(resp, 1);
        }
        ILOG("[l2] upgrade (CleanUnique) forwarded line=0x" << std::hex << req.line_addr << std::dec);
        return;
    }

    if (req.prefetch) {
        mPfHost->enqueue(req.line_addr);
        return;
    }

    auto& queue = (req.source == Source::ICACHE) ? mIcachePending : mDcachePending;
    sparta_assert(queue.size() < mQueueCapacity, "L2Cache: request queue overflow — caller must check canAccept() first");
    queue.push_back({req.line_addr, req.source, mCurrentCycle, req.from_prefetch, req.opcode == mem::ReqOpcode::ReadUnique});
    ILOG("[l2] fill request src=" << static_cast<int>(req.source) << " line=0x" << std::hex << req.line_addr);
}

void L2Cache::receiveWriteback(const mem::Writeback& req) {
    // A dirty L1 line was evicted — mark the L2 line as dirty
    ++mNumWritebacks;
    if (req.reserved && mWbReserved > 0) --mWbReserved;
    auto* tag = mCacheInterface->tagArray();
    if (tag) {
        auto* line = const_cast<cpu::SimpleCache*>(tag)->getLine(req.line_addr);
        if (line) {
            line->setModified(true);
            ILOG("[l2] writeback received, marking dirty line=0x" << std::hex << req.line_addr);
        } else if (mBackend) {
            // Non-inclusive: the L2 already evicted this line, so the dirty data goes on to memory.
            writeToMemory(req.line_addr);
        } else if (mExternalFill) {
            mem::Writeback wb = req;
            wb.requester_id = mRequesterId;
            up.wb_out.send(wb, 1);
        }
    }
}

void L2Cache::handleEviction(cpu::address_t line_addr) {
    if (mInclusivityPolicy != InclusivityPolicy::INCLUSIVE) {
        return;  // NINE: no back-invalidation
    }

    // Inclusive policy: invalidate the line in both L1s (aggregator fans the single send).
    mem::Invalidate inv;
    inv.line_addr = line_addr;
    down.inval_out.send(inv, 1);
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
    if (mBackend) mBackend->tick();
    mCacheInterface->tick(mCurrentCycle);

    if (!mVictims.empty() && mBackend->canAccept(mVictims.front(), /*is_write=*/true)) {
        mBackend->access(mVictims.front(), /*is_write=*/true);
        mVictims.pop_front();
        ++mNumMemWrites;
    }

    // Service at most one new fill request per cycle (single-ported tag array).
    const PendingRequest* req = arbitrate();
    // dequeue() below pops *req; the tracer still needs the winner's source.
    const std::optional<Source> winner = req ? std::optional<Source>(req->source) : std::nullopt;
    bool serviced = false;
    if (req) {
        uint64_t token = encodeToken(req->source, req->from_prefetch);
        bool accept = mCacheInterface->canAcceptRequest(req->line_addr);
        if (!accept) {
            ++mNumMshrFullCycles;
        } else if (mBackend && mCacheInterface->willNeedFill(req->line_addr) && !mBackend->canAccept(req->line_addr, /*is_write=*/false) &&
                   std::find(mVictims.begin(), mVictims.end(), req->line_addr) == mVictims.end()) {
            accept = false;
            ++mNumMemQueueFullCycles;
        }
        if (accept) {
            uint64_t before = mCacheInterface->numCoalesced();
            const auto probe = mCacheInterface->probe(req->line_addr);
            mServicingWrite = req->is_write;
            mCacheInterface->loadRequest(token, req->line_addr, mCurrentCycle, /*exclusive=*/false, /*demand=*/!req->from_prefetch);
            mServicingWrite = false;
            if (mCacheInterface->numCoalesced() > before) ++mNumCoalesced;
            // The stream detector watches the whole L1D miss stream, L1 prefetches included.
            if (req->source == Source::DCACHE) mPfHost->train(probe, req->line_addr, 0, /*is_store=*/false, prefetch::CacheLevel::L2, mCurrentCycle);
            serviced = true;

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
    if (mTracer && winner) {
        // Only log losses for the source that lost arbitration (the other source)
        if (*winner == Source::ICACHE && !mDcachePending.empty()) {
            // D-cache lost this arbitration - only log the front entry (the one that competed)
            const auto& pr = mDcachePending.front();
            uint32_t waited = static_cast<uint32_t>(mCurrentCycle - pr.arrive_cycle);
            mTracer->recordL2ArbLose(mCurrentCycle, static_cast<uint8_t>(Source::DCACHE), pr.line_addr, waited);
        } else if (*winner == Source::DCACHE && !mIcachePending.empty()) {
            // I-cache lost this arbitration - only log the front entry (the one that competed)
            const auto& pr = mIcachePending.front();
            uint32_t waited = static_cast<uint32_t>(mCurrentCycle - pr.arrive_cycle);
            mTracer->recordL2ArbLose(mCurrentCycle, static_cast<uint8_t>(Source::ICACHE), pr.line_addr, waited);
        }
    }

    // Prefetches use the tag port only in cycles demand left it idle.
    mPfHost->tick(
        [this](cpu::address_t addr) {
            if (mBackend && mCacheInterface->willNeedFill(addr) && !mBackend->canAccept(addr, /*is_write=*/false))
                return cpu::CacheInterface::PrefetchOutcome::NoMshr;
            return mCacheInterface->prefetchRequest(addr, mCurrentCycle, mPfMshrReserve);
        },
        !serviced);
}

}  // namespace memory
