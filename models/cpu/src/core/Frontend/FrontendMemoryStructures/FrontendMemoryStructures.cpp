#include "Frontend/FrontendMemoryStructures/FrontendMemoryStructures.hpp"

#include <iostream>

#include "Common/PipelineVisualizer.hpp"
#include "Common/SpeculationUtils.hpp"
#include "Frontend/FetchQueue/FetchQueue.hpp"
#include "Logging.hpp"
#include "MemoryHierarchy/L2Cache.hpp"

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

    if (mStructuralMode) {
        buildCacheInterface();
    }

    request_in.registerConsumerHandler(CREATE_SPARTA_HANDLER_WITH_DATA(FrontendMemoryStructures, receiveRequest, core::FetchRequest));
    fill_response_in.registerConsumerHandler(CREATE_SPARTA_HANDLER_WITH_DATA(FrontendMemoryStructures, receiveFillResponse, core::FillResponse));
    invalidate_in.registerConsumerHandler(CREATE_SPARTA_HANDLER_WITH_DATA(FrontendMemoryStructures, receiveInvalidate, core::InvalidateRequest));
    flush_in.registerConsumerHandler(CREATE_SPARTA_HANDLER_WITH_DATA(FrontendMemoryStructures, receiveFlush_, core::FlushRequest));
}

void FrontendMemoryStructures::buildCacheInterface() {
    // Create CacheInterface WITHOUT callback - we use completion queue for controlled processing
    mCacheInterface = std::make_unique<cpu::CacheInterface>(mCacheInterfaceCfg, nullptr);

    if (mCacheInterfaceCfg.external_fill) {
        mCacheInterface->setFillRequestCallback([this](cpu::address_t line_addr) {
            core::FillRequest req;
            req.line_addr = line_addr;
            fill_request_out.send(req, 1);
        });
    }

    // Re-attach tracer if one was set before the interface was rebuilt.
    if (mTracer) mCacheInterface->setTracer(mTracer);
}

void FrontendMemoryStructures::setL2(memory::L2Cache* l2) {
    mL2 = l2;
    if (mStructuralMode && l2) {
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

bool FrontendMemoryStructures::canAcceptRequest(cpu::address_t addr) const {
    // FetchQueue backpressure to Fetch: reject once queued + in-flight packets have
    // claimed every FetchQueue entry. In-flight = packets inside the icache (accepted,
    // not yet forwarded) plus packets sent this tick (1-cycle port flight). The
    // structural forward gate in tick() is the hard overflow guard for the one fetch
    // group this check cannot size in advance.
    if (mFetchQueue) {
        const size_t in_flight = pendingPacketCount() + mPacketsSentThisTick;
        if (in_flight >= mFetchQueue->available()) return false;
    }

    if (mStructuralMode) {
        if (!mCacheInterface->canAcceptRequest(addr)) return false;
        // If a new MSHR would be allocated, also check L2 has room for the fill.
        if (mL2 && mCacheInterface->willNeedFill(addr) && !mL2->canAcceptIcacheRequest()) return false;
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

void FrontendMemoryStructures::receiveFillResponse(const core::FillResponse& resp) {
    if (mCacheInterface) {
        mCacheInterface->fillComplete(resp.line_addr, getClock()->currentCycle());
    }
}

void FrontendMemoryStructures::receiveInvalidate(const core::InvalidateRequest& req) {
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
            bool should_flush = false;
            for (const auto& pkt : it->request.packets) {
                if (core::shouldSquash(pkt, req)) {
                    ILOG("[icache] cycle " << cycle << " SQUASHING tag=" << pkt.tag << " depth=" << (int)pkt.wrong_path_depth);
                    if (mVis) mVis->onSquash(pkt.fetch_seq, cycle);
                    should_flush = true;
                    // Don't break - call onSquash for ALL packets in this request
                }
            }
            if (should_flush) {
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
            bool should_flush = false;
            for (const auto& pkt : it->second) {
                if (core::shouldSquash(pkt, req)) {
                    should_flush = true;
                    break;
                }
            }
            if (should_flush) {
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

            // Try to submit to cache
            uint64_t before = mCacheInterface->numCoalesced();
            bool accepted = mCacheInterface->loadRequest(pend_req.token, pend_req.token, cycle);
            if (!accepted) {
                // Bank conflict - stop trying this cycle, will retry next cycle
                break;
            }
            if (mCacheInterface->numCoalesced() > before) ++mNumCoalesced;
            mStructuralPendingRequests.pop_front();
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
