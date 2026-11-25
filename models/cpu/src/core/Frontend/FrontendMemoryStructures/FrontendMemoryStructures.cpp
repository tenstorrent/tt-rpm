// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Tenstorrent USA, Inc.

#include "Frontend/FrontendMemoryStructures/FrontendMemoryStructures.hpp"

#include <iostream>

#include "Common/L1RequestAggregator.hpp"
#include "Common/ParamChoice.hpp"
#include "Common/PipelineVisualizer.hpp"
#include "Common/SpeculationUtils.hpp"
#include "Frontend/DecodeStructures/DecodeStructures.hpp"
#include "Frontend/FetchQueue/FetchQueue.hpp"
#include "Logging.hpp"
#include "MemoryHierarchy/L2Cache.hpp"
#include "prefetch/common/PrefetcherRegistry.hpp"
#include "prefetch/prefetcher/src/InstructionPrefetcherInterface.hpp"

namespace frontend {

FrontendMemoryStructures::FrontendMemoryStructures(sparta::TreeNode* node, const FrontendMemoryStructuresParams* params)
    : sparta::Unit(node),
      mNumHits(&unit_stat_set_, "num_hits", "I-cache hits", sparta::Counter::COUNT_NORMAL),
      mNumMisses(&unit_stat_set_, "num_misses", "I-cache misses", sparta::Counter::COUNT_NORMAL),
      mNumCoalesced(&unit_stat_set_, "num_coalesced", "I-cache coalesced misses", sparta::Counter::COUNT_NORMAL) {
    mHitRate = params->hit_rate;
    mHitLatency = params->hit_latency;
    mMissLatency = params->miss_latency;
    mMshrCapacity = params->mshr_capacity;
    mStructuralMode = core::checkChoice(params->cache_mode, {"probabilistic", "structural"}) == "structural";
    mRng.seed(params->rng_seed);

    mPfMshrReserve = params->prefetch_mshr_reserve;
    prefetch::PrefetcherConfig pf_cfg;
    pf_cfg.line_size = static_cast<uint32_t>(params->cache_line_size);
    pf_cfg.degree = params->prefetch_degree;
    core::PrefetchHost::Config host_cfg;
    host_cfg.line_size = pf_cfg.line_size;
    host_cfg.queue_capacity = params->prefetch_queue_capacity;
    if (params->prefetcher_type.getValue() != "none") {
        sparta_assert(mPfMshrReserve < params->mshr_capacity, params->prefetch_mshr_reserve.getLocation()
                                                                  << " (" << mPfMshrReserve << ") must be below " << params->mshr_capacity.getLocation() << " ("
                                                                  << params->mshr_capacity.getValue() << ") with a prefetcher");
        sparta_assert(host_cfg.queue_capacity > 0, params->prefetch_queue_capacity.getLocation() << " must be nonzero with a prefetcher");
        sparta_assert(pf_cfg.degree > 0, params->prefetch_degree.getLocation() << " must be nonzero with a prefetcher");
        auto pf = prefetch::Registry<prefetch::InstructionPrefetcherInterface, prefetch::PrefetcherConfig>::instance().create(
            params->prefetcher_type.getValue(), pf_cfg);
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

    if (mStructuralMode) {
        buildCacheInterface();
    }

    request_in.registerConsumerHandler(CREATE_SPARTA_HANDLER_WITH_DATA(FrontendMemoryStructures, receiveRequest, core::FetchRequest));
    fill_response_in.registerConsumerHandler(CREATE_SPARTA_HANDLER_WITH_DATA(FrontendMemoryStructures, receiveFillResponse, mem::Response));
    invalidate_in.registerConsumerHandler(CREATE_SPARTA_HANDLER_WITH_DATA(FrontendMemoryStructures, receiveInvalidate, mem::Invalidate));
    flush_in.registerConsumerHandler(CREATE_SPARTA_HANDLER_WITH_DATA(FrontendMemoryStructures, receiveFlush_, core::FlushRequest));
}

void FrontendMemoryStructures::buildCacheInterface() {
    // Create CacheInterface WITHOUT callback - we use completion queue for controlled processing
    mCacheInterface = std::make_unique<cpu::CacheInterface>(mCacheInterfaceCfg, nullptr);

    if (mCacheInterfaceCfg.external_fill) {
        mCacheInterface->setFillRequestCallback([this](cpu::address_t line_addr, bool /*write_allocate*/, bool prefetch) {
            mem::Request req;
            req.line_addr = line_addr;
            req.source = mem::CacheSource::ICACHE;
            req.opcode = mem::ReqOpcode::ReadShared;  // instruction fetches are always reads
            req.from_prefetch = prefetch;
            fill_request_out.send(req, 1);
        });
    }

    if (mPfHost) {
        mPfHost->bind(mCacheInterface.get());
        mCacheInterface->setEvictionCallback([this](cpu::address_t line_addr, bool) { mPfHost->onEviction(line_addr, getClock()->currentCycle()); });
        mCacheInterface->setFillDoneCallback(
            [this](cpu::address_t line_addr, bool prefetched) { mPfHost->onFill(line_addr, prefetched, getClock()->currentCycle()); });
    }

    // Re-attach tracer if one was set before the interface was rebuilt.
    if (mTracer) mCacheInterface->setTracer(mTracer);
}

void FrontendMemoryStructures::setUpstream(core::L1RequestAggregator* agg) {
    mAgg = agg;
    if (mStructuralMode && agg) {
        mCacheInterfaceCfg.external_fill = true;
        buildCacheInterface();
    }
}

size_t FrontendMemoryStructures::pendingPacketCount() const {
    size_t count = 0;
    if (mStructuralMode) {
        for (const auto& [seq, pkts] : mStructuralPacketsBySeq) count += pkts.size();
    } else {
        for (const auto& pend : mPendingRequests) count += pend.request.packets.size();
    }
    return count;
}

bool FrontendMemoryStructures::downstreamFull() const {
    // In-flight = packets inside the icache (accepted, not yet forwarded) plus packets sent
    // this tick (1-cycle port flight). The structural forward gate in tick() is the hard
    // overflow guard for the one fetch group this check cannot size in advance.
    const size_t in_flight = pendingPacketCount() + mPacketsSentThisTick;
    if (mFetchQueue) return in_flight >= mFetchQueue->available();
    if (mBypassDecode) return in_flight + mBypassDecode->numPendingPackets() >= mBypassCapacity;
    return false;
}

bool FrontendMemoryStructures::canAcceptRequest(cpu::address_t addr) const {
    if (mStructuralMode) {
        if (!mCacheInterface->canAcceptRequest(addr)) return false;
        // If a new MSHR would be allocated, also check L2 has room for the fill.
        if (mAgg && mCacheInterface->willNeedFill(addr) && !mAgg->canAccept(mem::CacheSource::ICACHE)) return false;
        return true;
    }
    return mOutstandingMisses < mMshrCapacity;
}

void FrontendMemoryStructures::receiveRequest(const core::FetchRequest& request) {
    uint64_t cycle = getClock()->currentCycle();

    ILOG("[icache] cycle " << cycle << " RECEIVED " << request.packets.size() << " packets");

    // Mark ICache start for visualization
    if (mVis) {
        for (const auto& pkt : request.packets) {
            mVis->onIcacheStart(pkt.fetch_seq, cycle);
        }
    }

    if (mStructuralMode) {
        // Assign sequence number to preserve fetch order
        uint64_t fetch_seq = mNextFetchSeq++;

        // Store packets indexed by fetch_seq (unique key, never overwrites)
        mStructuralPacketsBySeq[fetch_seq] = request.packets;

        // Track that this fetch_seq is waiting on this address
        mSeqsWaitingOnAddr[request.base_pc].push_back(fetch_seq);

        // Queue request - will be submitted to cache in tick() respecting bank limits
        PendingRequest pend_req;
        pend_req.token = request.base_pc;
        pend_req.fetch_seq = fetch_seq;
        mStructuralPendingRequests.push_back(pend_req);
        return;
    }

    // Probabilistic mode
    bool hit = mHitRate >= mDist(mRng);
    ++(hit ? mNumHits : mNumMisses);
    uint32_t latency = hit ? mHitLatency : mHitLatency + mMissLatency;
    if (!hit) ++mOutstandingMisses;
    mPendingRequests.push_back({request, latency, hit});
}

void FrontendMemoryStructures::receiveFillResponse(const mem::Response& resp) {
    if (mCacheInterface) {
        if (resp.granted_state == mem::LineState::I)
            mCacheInterface->invalidateLine(resp.line_addr, getClock()->currentCycle());  // serve the waiters, keep nothing
        mCacheInterface->fillComplete(resp.line_addr, getClock()->currentCycle());
    }
}

void FrontendMemoryStructures::receiveInvalidate(const mem::Invalidate& req) {
    if (mCacheInterface) {
        mCacheInterface->invalidateLine(req.line_addr, getClock()->currentCycle());
        ILOG("[icache] back-invalidate line=0x" << std::hex << req.line_addr);
    }
}

void FrontendMemoryStructures::receiveFlush_(const core::FlushRequest& req) {
    uint64_t cycle = getClock()->currentCycle();

    ILOG("[icache] cycle=" << cycle << " FLUSH received: branch_tag=" << req.branch_tag << " branch_depth=" << (int)req.branch_depth);

    // Cancel in-flight packet vectors heading to the FetchQueue that belong to the squashed
    // (wrong) path. Doing this synchronously at flush time is unambiguous: the re-fetch with
    // reused tags has not happened yet, so shouldSquash(tag,depth) is exact. This replaces
    // the downstream time-based flush barrier. forwardPackets() splits each fetch group at
    // branch/jump boundaries, so a vector is entirely squashable or entirely not.
    uint32_t cancelled_to_fq = packets_to_queue_out.cancelIf([&req](const std::vector<core::PipelinePacket>& pkts) {
        if (pkts.empty()) return false;
        for (const auto& p : pkts)
            if (!core::shouldSquash(p, req)) return false;
        return true;
    });
    if (cancelled_to_fq > 0) ILOG("[icache] cycle " << cycle << " cancelled " << cancelled_to_fq << " in-flight packet vector(s) to FetchQueue");

    // Immediately remove pending fetches that should be squashed
    if (!mStructuralMode) {
        size_t before = mPendingRequests.size();
        auto it = mPendingRequests.begin();
        while (it != mPendingRequests.end()) {
            // A fetch group can hold the redirecting branch and older instructions: keep those.
            std::erase_if(it->request.packets, [&](const core::PipelinePacket& pkt) {
                if (!core::shouldSquash(pkt, req)) return false;
                ILOG("[icache] cycle " << cycle << " SQUASHING tag=" << pkt.tag << " depth=" << (int)pkt.wrong_path_depth);
                if (mVis) mVis->onSquash(pkt.fetch_seq, cycle);
                return true;
            });
            if (it->request.packets.empty()) {
                if (!it->hit) --mOutstandingMisses;
                it = mPendingRequests.erase(it);
            } else {
                ++it;
            }
        }
        size_t cleared = before - mPendingRequests.size();
        ILOG("[icache] cycle " << cycle << " FLUSH done: tag=" << req.branch_tag << " cleared=" << cleared);
    }

    // Clear structural mode pending packets that should be flushed
    if (mStructuralMode) {
        size_t cleared = 0;

        // Check each fetch_seq's packets for squashing
        auto it = mStructuralPacketsBySeq.begin();
        while (it != mStructuralPacketsBySeq.end()) {
            std::erase_if(it->second, [&req](const core::PipelinePacket& pkt) { return core::shouldSquash(pkt, req); });
            if (it->second.empty()) {
                // Remember this sequence was flushed
                mFlushedSequences.insert(it->first);
                ++cleared;
                it = mStructuralPacketsBySeq.erase(it);
            } else {
                ++it;
            }
        }

        // Clear matching requests from the pending request queue
        auto req_it = mStructuralPendingRequests.begin();
        while (req_it != mStructuralPendingRequests.end()) {
            if (mStructuralPacketsBySeq.find(req_it->fetch_seq) == mStructuralPacketsBySeq.end()) {
                req_it = mStructuralPendingRequests.erase(req_it);
            } else {
                ++req_it;
            }
        }

        // PERF: Do NOT scan mSeqsWaitingOnAddr here. That map is keyed by every
        // unique cache-line base PC ever fetched and grows monotonically; scanning
        // it on every flush made receiveFlush_ O(unique_addrs) per call and the
        // simulator O(N^2) in run length. Stale entries for flushed seqs are
        // filtered lazily when their completions arrive (tick() checks
        // mStructuralPacketsBySeq.find(fetch_seq) before enqueueing), so this
        // defensive scan is unnecessary for correctness.

        // Also clear matching completions from the pending queue
        auto comp_it = mPendingCompletions.begin();
        while (comp_it != mPendingCompletions.end()) {
            if (mStructuralPacketsBySeq.find(comp_it->fetch_seq) == mStructuralPacketsBySeq.end()) {
                comp_it = mPendingCompletions.erase(comp_it);
            } else {
                ++comp_it;
            }
        }

        ILOG("[icache] cleared " << cleared << " structural pending packet sets");
    }
}

bool FrontendMemoryStructures::canForwardPackets(const core::FetchRequest& req) const {
    if (req.packets.empty()) return true;

    if (mFetchQueue) {
        size_t available_space = mFetchQueue->available();
        // Account for packets already sent this tick but not yet received by FetchQueue
        if (available_space <= mPacketsSentThisTick) {
            ILOG("[icache] cannot forward: FetchQueue has " << available_space << " slots, already sent " << mPacketsSentThisTick << " this tick");
            return false;
        }
        size_t effective_space = available_space - mPacketsSentThisTick;
        if (effective_space < req.packets.size()) {
            ILOG("[icache] cannot forward: FetchQueue effective space " << effective_space << " slots, need " << req.packets.size());
            return false;
        }
    }
    return true;
}

void FrontendMemoryStructures::forwardPackets(const core::FetchRequest& req, bool miss) {
    if (req.packets.empty()) return;

    uint64_t cycle = getClock()->currentCycle();
    ILOG("[icache] cycle " << cycle << " FORWARDING " << req.packets.size() << " packets");

    // Track packets we're about to send
    mPacketsSentThisTick += req.packets.size();

    // Mark ICache end for visualization
    if (mVis) {
        for (const auto& pkt : req.packets) {
            mVis->onIcacheEnd(pkt.fetch_seq, cycle);
            if (miss) {
                mVis->onIcacheMiss(pkt.fetch_seq, cycle);
            }
        }
    }

    // Split the fetch group at control-flow boundaries: each payload ends at a branch/jump.
    // A fetch tick has a single wrong_path_depth and depth changes only at branches, so every
    // sub-vector is uniform in (depth, side-of-any-future-flushed-branch). That makes the
    // all-or-nothing flush cancelIf on this port exact -- a payload straddling a branch would
    // be kept whole (older correct packet not squashable), leaking the younger wrong-path
    // packets in it downstream.
    std::vector<core::PipelinePacket> sub;
    sub.reserve(req.packets.size());
    for (const auto& pkt : req.packets) {
        sub.push_back(pkt);
        if (pkt.inst_class == cpu::InstClass::Branch || pkt.inst_class == cpu::InstClass::Jump) {
            packets_to_queue_out.send(sub, 1);
            sub.clear();
        }
    }
    if (!sub.empty()) packets_to_queue_out.send(sub, 1);
    ILOG("[icache] forwarding " << req.packets.size() << " packets to FetchQueue (miss=" << miss << ")");
}

void FrontendMemoryStructures::sendResponseProb() {
    auto& entry = mPendingRequests.front();

    // Check if we can forward packets (downstream has space)
    if (!canForwardPackets(entry.request)) {
        ILOG("[icache] stalled: cannot forward packets, keeping in pending queue");
        return;
    }

    if (!entry.hit) --mOutstandingMisses;

    core::FetchResponse response;
    response.base_pc = entry.request.base_pc;
    response.miss = !entry.hit;
    response_out.send(response, 1);

    forwardPackets(entry.request, !entry.hit);
    mPendingRequests.pop_front();
}

void FrontendMemoryStructures::tick() {
    uint64_t cycle = getClock()->currentCycle();

    mPacketsSentThisTick = 0;

    if (mStructuralMode) {
        mCacheInterface->tick(cycle);

        // Submit pending requests to cache (respecting bank limits)
        while (!mStructuralPendingRequests.empty()) {
            auto& pend_req = mStructuralPendingRequests.front();

            // Check if this request was flushed while waiting
            if (mStructuralPacketsBySeq.find(pend_req.fetch_seq) == mStructuralPacketsBySeq.end()) {
                // Flushed - skip it
                mStructuralPendingRequests.pop_front();
                continue;
            }

            // Fetch's canAcceptRequest() check does not reserve the MSHR, and a prefetch may
            // have taken it since; wait for one rather than assert in loadRequest().
            if (!mCacheInterface->canAcceptRequest(pend_req.token)) break;

            // Try to submit to cache
            uint64_t before = mCacheInterface->numCoalesced();
            const auto probe = mPfHost ? mCacheInterface->probe(pend_req.token) : cpu::CacheInterface::Probe{};
            bool accepted = mCacheInterface->loadRequest(pend_req.token, pend_req.token, cycle);
            if (!accepted) {
                // Bank conflict - stop trying this cycle, will retry next cycle
                break;
            }
            if (mPfHost) mPfHost->train(probe, pend_req.token, pend_req.token, /*is_store=*/false, prefetch::CacheLevel::L1I, cycle);
            if (mCacheInterface->numCoalesced() > before) ++mNumCoalesced;
            mStructuralPendingRequests.pop_front();
        }

        // Issue prefetches after demand fetches so they never starve demand traffic.
        if (mPfHost) {
            mPfHost->tick([this, cycle](cpu::address_t addr) {
                if (mAgg && mCacheInterface->willNeedFill(addr) && !mAgg->canAccept(mem::CacheSource::ICACHE))
                    return cpu::CacheInterface::PrefetchOutcome::NoMshr;
                return mCacheInterface->prefetchRequest(addr, cycle, mPfMshrReserve);
            });
        }

        // Drain completions from CacheInterface and process them
        auto completions = mCacheInterface->drainCompletions();

        // Queue completions from CacheInterface
        for (const auto& comp : completions) {
            // Find all fetch_seqs waiting on this address
            auto it = mSeqsWaitingOnAddr.find(comp.token);
            if (it == mSeqsWaitingOnAddr.end() || it->second.empty()) {
                // No one waiting - skip
                continue;
            }

            // Create completion for each waiting fetch_seq
            for (uint64_t fetch_seq : it->second) {
                // Check if this fetch_seq's packets still exist (not flushed)
                if (mStructuralPacketsBySeq.find(fetch_seq) == mStructuralPacketsBySeq.end()) {
                    continue;  // Flushed
                }

                PendingCompletion pend;
                pend.fetch_seq = fetch_seq;
                pend.miss = comp.miss;
                mPendingCompletions.push_back(pend);
            }

            // PERF: Erase the entry rather than just clearing the vector. Leaving
            // an empty entry behind would keep mSeqsWaitingOnAddr growing without
            // bound across the run; if the same address is fetched again, it
            // will be re-inserted by receiveRequest().
            mSeqsWaitingOnAddr.erase(it);
        }

        // Forward completions in fetch order
        while (!mPendingCompletions.empty()) {
            // Check if next sequence was flushed (and not yet completed)
            if (mFlushedSequences.count(mNextForwardSeq)) {
                // This sequence was flushed - skip it
                mFlushedSequences.erase(mNextForwardSeq);
                ++mNextForwardSeq;
                continue;
            }

            // Find completion with next sequence number
            auto pend_it = std::find_if(mPendingCompletions.begin(), mPendingCompletions.end(),
                                        [this](const PendingCompletion& pc) { return pc.fetch_seq == mNextForwardSeq; });

            if (pend_it == mPendingCompletions.end()) {
                // Next sequence not yet completed, can't forward more
                break;
            }

            // Look up packets by fetch_seq
            auto pkt_it = mStructuralPacketsBySeq.find(pend_it->fetch_seq);
            if (pkt_it == mStructuralPacketsBySeq.end()) {
                // Flushed while waiting - skip this sequence number
                mPendingCompletions.erase(pend_it);
                ++mNextForwardSeq;
                continue;
            }

            // FetchQueue backpressure: hold this completion (in fetch order) if the
            // FetchQueue can't accept it this cycle, accounting for packets already
            // forwarded this tick that are still in flight.
            if (mFetchQueue) {
                size_t avail = mFetchQueue->available();
                if (avail <= mPacketsSentThisTick || (avail - mPacketsSentThisTick) < pkt_it->second.size()) {
                    break;  // retry next cycle, preserving fetch order
                }
            }

            // Update stats
            ++(pend_it->miss ? mNumMisses : mNumHits);

            // Send response to Fetch (use first packet's PC for base_pc)
            core::FetchResponse resp;
            resp.base_pc = pkt_it->second.empty() ? 0 : pkt_it->second.front().pc;
            resp.miss = pend_it->miss;
            response_out.send(resp, 1);

            // Forward packets to Decode
            core::FetchRequest stored_req;
            stored_req.base_pc = resp.base_pc;
            stored_req.packets = std::move(pkt_it->second);
            forwardPackets(stored_req, pend_it->miss);
            mStructuralPacketsBySeq.erase(pkt_it);

            // Move to next sequence
            mPendingCompletions.erase(pend_it);
            ++mNextForwardSeq;
        }
        return;
    }

    // Probabilistic mode
    for (auto& entry : mPendingRequests) {
        if (entry.cycles_to_complete > 0) --entry.cycles_to_complete;
    }

    while (!mPendingRequests.empty() && mPendingRequests.front().cycles_to_complete == 0) {
        size_t before_size = mPendingRequests.size();
        sendResponseProb();
        // If sendResponseProb couldn't forward (downstream full), break to avoid infinite loop
        if (mPendingRequests.size() == before_size) {
            ILOG("[icache] tick: stalled, downstream not ready");
            break;
        }
    }
}

}  // namespace frontend
