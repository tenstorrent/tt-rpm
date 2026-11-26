// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Tenstorrent USA, Inc.

#pragma once

#include <deque>
#include <iostream>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "sparta/events/UniqueEvent.hpp"
#include "sparta/ports/DataPort.hpp"
#include "sparta/simulation/ParameterSet.hpp"
#include "sparta/simulation/Unit.hpp"
#include "sparta/statistics/Counter.hpp"

#include "Logging.hpp"
#include "MemoryHierarchy/Cache.hpp"
#include "MmioRouter.hpp"
#include "MsiProtocolEngine.hpp"
#include "models/cpu/common/Coherence.hpp"
#include "models/cpu/common/MemInterface.hpp"

// Shared cluster cache: the cluster-level last-level cache, shared by all cores.
//
// One standardized mem::MemSlave per attached core; requests are demultiplexed by
// requester_id. Structural, tag-only: it models capacity / associativity /
// replacement and cross-core shared residency (all harts share one address space,
// so one core's fill is visible to another core's access to the same line). It never
// holds line data; cross-core invalidation exists only with coherence_enabled (MSI
// directory below). A hit answers in hit_latency, a miss in miss_latency (flat
// backing memory).
class ClusterCache : public sparta::Unit {
   public:
    ClusterCache(sparta::TreeNode* node, uint32_t num_requesters, uint32_t hit_latency = 12, uint32_t miss_latency = 100, uint64_t cache_size_kb = 2048,
                 uint32_t associativity = 8, uint64_t line_size = 64, bool coherence_enabled = false, uint32_t coherence_latency = 0,
                 const sparta::Clock* cache_clock = nullptr)
        : sparta::Unit(node),
          mNumRequesters(num_requesters),
          mHitLatency(hit_latency),
          mMissLatency(miss_latency),
          mCoherenceEnabled(coherence_enabled),
          mCoherenceLatency(coherence_latency),
          mNumRequests(&unit_stat_set_, "cluster_requests", "Total requests to cluster cache", sparta::Counter::COUNT_NORMAL),
          mNumWritebacks(&unit_stat_set_, "cluster_writebacks", "Total writebacks to cluster cache", sparta::Counter::COUNT_NORMAL),
          mNumHits(&unit_stat_set_, "cluster_hits", "Shared cluster cache hits", sparta::Counter::COUNT_NORMAL),
          mNumMisses(&unit_stat_set_, "cluster_misses", "Shared cluster cache misses", sparta::Counter::COUNT_NORMAL),
          mNumReadShared(&unit_stat_set_, "cluster_read_shared", "REQ ReadShared (load/ifetch fill)", sparta::Counter::COUNT_NORMAL),
          mNumReadUnique(&unit_stat_set_, "cluster_read_unique", "REQ ReadUnique (store-miss RFO fill)", sparta::Counter::COUNT_NORMAL),
          mNumCleanUnique(&unit_stat_set_, "cluster_clean_unique", "REQ CleanUnique (store-to-clean upgrade)", sparta::Counter::COUNT_NORMAL),
          mNumReadOnce(&unit_stat_set_, "cluster_read_once", "REQ ReadOnce (non-caching read)", sparta::Counter::COUNT_NORMAL),
          mNumWriteUnique(&unit_stat_set_, "cluster_write_unique", "REQ WriteUnique (non-caching write)", sparta::Counter::COUNT_NORMAL),
          mNumSnoops(&unit_stat_set_, "cluster_snoops", "Coherence snoop-invalidates sent to RNs", sparta::Counter::COUNT_NORMAL),
          mNumGrantS(&unit_stat_set_, "cluster_grant_s", "Shared (S) grants", sparta::Counter::COUNT_NORMAL),
          mNumGrantM(&unit_stat_set_, "cluster_grant_m", "Exclusive/Modified (M) grants", sparta::Counter::COUNT_NORMAL),
          mNumEvictSnoops(&unit_stat_set_, "cluster_evict_snoops", "Inclusion back-invalidates on shared-cache eviction", sparta::Counter::COUNT_NORMAL),
          mNumMmio(&unit_stat_set_, "cluster_mmio", "MMIO requests peeled to devices (never allocated)", sparta::Counter::COUNT_NORMAL),
          mSyncDrain(&unit_event_set_, "sync_drain", CREATE_SPARTA_HANDLER(ClusterCache, drainSync)) {
        cpu::SimpleCache::Config ccfg;
        ccfg.cacheSizeKb = cache_size_kb;
        ccfg.lineSize = line_size;
        ccfg.associativity = associativity;
        ccfg.replacementPolicy = "lru";
        ccfg.sizeParam = "cluster_cache_size_kb";
        ccfg.lineSizeParam = "line_size";
        ccfg.associativityParam = "cluster_cache_associativity";
        mCache = std::make_unique<cpu::SimpleCache>(ccfg);

        // The Home Node owns a pluggable coherence ProtocolEngine. With coherence disabled the
        // shared cache is tag-only (NullProtocolEngine, never driven). Enabled, the MSI engine
        // drives the per-line directory (mDirectory) and emits snoop-invalidates to sharers.
        sparta_assert(!mCoherenceEnabled || mNumRequesters <= 64,
                      "coherence directory tracks sharers in a 64-bit vector; got " << mNumRequesters << " requesters");
        if (mCoherenceEnabled)
            mProtocol = std::make_unique<mem::MsiProtocolEngine>();
        else
            mProtocol = std::make_unique<mem::NullProtocolEngine>();

        // The shared cache sits in its own clock domain; each per-core slave is a
        // clock-crossing (sync) face so cores may run at different frequencies. A
        // per-core ClusterBridge on the core clock binds to these.
        const sparta::Clock* clk = cache_clock ? cache_clock : getClock();
        mPerCoreRequests.resize(mNumRequesters, 0);
        mLastReqSeq.resize(mNumRequesters);
        mPerCoreWritebacks.resize(mNumRequesters, 0);
        for (uint32_t i = 0; i < mNumRequesters; ++i) {
            auto slave = std::make_unique<mem::SyncMemSlave>(&unit_port_set_, "core" + std::to_string(i), clk);
            slave->req_in->registerConsumerHandler(CREATE_SPARTA_HANDLER_WITH_DATA(ClusterCache, handleReq, mem::Request));
            slave->wb_in->registerConsumerHandler(CREATE_SPARTA_HANDLER_WITH_DATA(ClusterCache, handleWriteback, mem::Writeback));
            slave->snprsp_in->registerConsumerHandler(CREATE_SPARTA_HANDLER_WITH_DATA(ClusterCache, handleSnoopResponse, mem::SnoopResponse));
            // In the same cycle a core's writeback is older than its request: it dropped the line, then missed on it.
            slave->wb_in->precedes(*slave->req_in);
            mSlaves.push_back(std::move(slave));
        }
    }

    // The per-core clock-crossing interface a ClusterBridge binds to.
    mem::SyncMemSlave& slave(uint32_t i) { return *mSlaves.at(i); }
    uint32_t numRequesters() const { return mNumRequesters; }

    // MMIO BAR registry: requests in a registered range are peeled to the device handler
    // before the tag path (never allocated, never in the directory).
    cluster::MmioRouter& mmio() { return mMmio; }

    // Print shared-cache traffic (total + hit/miss + per-requester requests / writebacks).
    void reportStats() const {
        const uint64_t reqs = mNumRequests.get();
        const uint64_t fills = mNumHits.get() + mNumMisses.get();  // reqs also counts upgrades and MMIO
        const double hitrate = fills ? 100.0 * static_cast<double>(mNumHits.get()) / static_cast<double>(fills) : 0.0;
        std::cout << "\n=== Shared Cluster Cache Traffic ===\n";
        std::cout << "  coherence: " << (mCoherenceEnabled ? "enabled" : "disabled (tag-only)") << "\n";
        std::cout << "  Total: requests=" << reqs << "  hits=" << mNumHits.get() << "  misses=" << mNumMisses.get() << "  hit_rate=" << hitrate
                  << "%  writebacks=" << mNumWritebacks.get() << "\n";
        std::cout << "  REQ opcodes: ReadShared=" << mNumReadShared.get() << "  ReadUnique=" << mNumReadUnique.get()
                  << "  CleanUnique=" << mNumCleanUnique.get() << "  ReadOnce=" << mNumReadOnce.get() << "  WriteUnique=" << mNumWriteUnique.get() << "\n";
        std::cout << "  MMIO: peeled=" << mNumMmio.get() << " (forwarded to devices, never allocated)\n";
        if (mCoherenceEnabled)
            std::cout << "  Coherence: snoops=" << mNumSnoops.get() << " (evict=" << mNumEvictSnoops.get() << ")"
                      << "  grants(S)=" << mNumGrantS.get() << "  grants(M)=" << mNumGrantM.get() << "  directory_lines=" << mDirectory.size()
                      << "  coherence_latency=" << mCoherenceLatency << "\n";
        for (uint32_t i = 0; i < mNumRequesters; ++i) {
            std::cout << "  requester " << i << ": requests=" << mPerCoreRequests[i] << "  writebacks=" << mPerCoreWritebacks[i] << "\n";
        }
    }

   private:
    void handleReq(const mem::Request& req) {
        ++mNumRequests;
        if (req.requester_id >= mNumRequesters) {
            ILOG("[cluster-cache] cycle " << getClock()->currentCycle() << " dropping request with bad requester " << req.requester_id);
            return;
        }
        ++mPerCoreRequests[req.requester_id];

        // MMIO peel: register-granular, non-cacheable. Forward to the device and ack
        // immediately so the requester completes; never allocate a line, never touch the
        // directory, on either the coherent or the tag-only path.
        if (mMmio.isMmio(req.line_addr)) {
            ++mNumMmio;
            mMmio.notify(req);
            mem::Response resp;
            resp.line_addr = req.line_addr;
            resp.requester_id = req.requester_id;
            resp.source = req.source;
            resp.txn_id = req.txn_id;
            resp.resp_opcode = mem::RespOpcode::CompAck;
            enqueueResp(resp, 0);
            return;
        }

        if (mCoherenceEnabled) {
            handleReqCoherent(req);
            return;
        }

        const cpu::address_t line_addr = req.line_addr;

        // Without coherence nothing is shared: fills are granted exclusive, and a CleanUnique (from a
        // level that only had S to give) is granted at once. Record it and touch for LRU.
        mem::Response resp;
        resp.line_addr = req.line_addr;
        resp.requester_id = req.requester_id;
        resp.source = req.source;  // echoed so the core can demux the fill to the right L1
        resp.txn_id = req.txn_id;
        resp.granted_state = req.opcode == mem::ReqOpcode::ReadShared ? mem::LineState::E : mem::LineState::M;
        if (isNonCaching(req.opcode)) resp.granted_state = mem::LineState::I;
        countOpcode(req.opcode);
        if (req.opcode == mem::ReqOpcode::CleanUnique) {
            if (mCache->isHit(line_addr)) mCache->touch(*mCache->getLine(line_addr));
            ILOG("[cluster-cache] cycle " << getClock()->currentCycle() << " cpu " << req.requester_id << " line 0x" << std::hex << req.line_addr << std::dec
                                          << " UPGRADE (CleanUnique)");
            resp.resp_opcode = mem::RespOpcode::CompUpgrade;
            enqueueResp(resp, mHitLatency);
            return;
        }

        bool hit = mCache->isHit(line_addr);
        if (hit) {
            mCache->touch(*mCache->getLine(line_addr));
            ++mNumHits;
        } else {
            ++mNumMisses;
            // Allocate the line (tag-only: victim is simply replaced, no data/writeback).
            cpu::CacheLine& victim = mCache->getLineForReplacement(line_addr);
            mCache->allocate(victim, line_addr);
        }
        if (req.opcode == mem::ReqOpcode::WriteUnique) mCache->getLine(line_addr)->setModified(true);
        const uint32_t latency = hit ? mHitLatency : mMissLatency;
        ILOG("[cluster-cache] cycle " << getClock()->currentCycle() << " cpu " << req.requester_id << " line 0x" << std::hex << req.line_addr << std::dec
                                      << (hit ? " HIT" : " MISS") << " (lat " << latency << ")");
        enqueueResp(resp, latency);
    }

    // MSI-directory path (coherence enabled): update tags, drive the engine, emit snoops, grant.
    void handleReqCoherent(const mem::Request& req) {
        const cpu::address_t line_addr = req.line_addr;
        mLastReqSeq[req.requester_id][line_addr] = req.rn_seq;
        const bool is_fill = req.opcode != mem::ReqOpcode::CleanUnique;
        countOpcode(req.opcode);

        // Tag update: fills install (allocate on miss); an upgrade only touches its resident line.
        bool hit = mCache->isHit(line_addr);
        if (is_fill) {
            if (hit) {
                mCache->touch(*mCache->getLine(line_addr));
                ++mNumHits;
            } else {
                ++mNumMisses;
                cpu::CacheLine& victim = mCache->getLineForReplacement(line_addr);
                // Inclusion: a line absent from the shared cache must be absent from every L1, so
                // back-invalidate the evicted line's sharers before dropping its directory entry.
                if (victim.isValid()) evictLine(victim.getAddress(), req.source);
                mCache->allocate(victim, line_addr);
            }
            if (req.opcode == mem::ReqOpcode::WriteUnique) mCache->getLine(line_addr)->setModified(true);
        } else if (hit) {
            mCache->touch(*mCache->getLine(line_addr));
        }

        // Directory + protocol engine decide the reaction (snoops + grant).
        mem::DirEntry& entry = mDirectory[line_addr];
        mem::Reaction reaction = mProtocol->onRequest(req.requester_id, req, entry);

        const uint32_t latency = (is_fill && !hit) ? mMissLatency : mHitLatency;

        // When a grant needs snoops, hold it in the transaction tracker until every snooped RN
        // acknowledges on SNPRSP, then release it (handleSnoopResponse). A unique txn tags this
        // request's snoops so their acks correlate back to the held grant. With no snoops, grant
        // immediately. Snoops with no held grant (inclusion eviction) carry txn=0 and their acks
        // are ignored.
        uint64_t txn = 0;
        if (reaction.send_response && !reaction.snoops.empty()) {
            txn = ++mNextTxn;
            // A coherence transaction (invalidating remote sharers / fetching an owned line) costs
            // an extra coherence_latency on top of the fill latency.
            mPendingGrants[txn] = PendingGrant{reaction.response, latency + mCoherenceLatency, static_cast<uint32_t>(reaction.snoops.size())};
        }
        for (auto snp : reaction.snoops) {
            if (snp.requester_id < mNumRequesters) {
                snp.txn_id = txn;
                enqueueSnoop(snp);
                ++mNumSnoops;
            } else if (txn != 0) {
                // Target out of range: won't ack, so don't wait on it.
                --mPendingGrants[txn].pending;
            }
        }
        if (reaction.send_response && reaction.snoops.empty()) {
            countGrant(reaction.response.granted_state);
            enqueueResp(reaction.response, latency);
        }
        ILOG("[cluster-cache] cycle " << getClock()->currentCycle() << " cpu " << req.requester_id << " line 0x" << std::hex << req.line_addr << std::dec
                                      << " op=" << static_cast<int>(req.opcode) << " snoops=" << reaction.snoops.size() << (hit ? " HIT" : " MISS"));
    }

    // A snooped RN acknowledged an invalidate. Advance the held grant for its transaction; when
    // the last outstanding snoop acks, release the grant. Acks with no pending transaction
    // (txn=0 upgrades / inclusion evictions, or an already-completed txn) are ignored.
    void handleSnoopResponse(const mem::SnoopResponse& rsp) {
        auto it = mPendingGrants.find(rsp.txn_id);
        if (it == mPendingGrants.end()) return;
        if (it->second.pending > 0 && --it->second.pending == 0) {
            PendingGrant& g = it->second;
            countGrant(g.response.granted_state);
            enqueueResp(g.response, g.latency);
            mPendingGrants.erase(it);
        }
    }

    static bool isNonCaching(mem::ReqOpcode op) { return op == mem::ReqOpcode::ReadOnce || op == mem::ReqOpcode::WriteUnique; }

    void countOpcode(mem::ReqOpcode op) {
        switch (op) {
            case mem::ReqOpcode::ReadShared:
                ++mNumReadShared;
                break;
            case mem::ReqOpcode::ReadUnique:
                ++mNumReadUnique;
                break;
            case mem::ReqOpcode::CleanUnique:
                ++mNumCleanUnique;
                break;
            case mem::ReqOpcode::ReadOnce:
                ++mNumReadOnce;
                break;
            case mem::ReqOpcode::WriteUnique:
                ++mNumWriteUnique;
                break;
        }
    }

    // A non-caching requester is granted I, which is not a grant.
    void countGrant(mem::LineState s) {
        if (s == mem::LineState::M)
            ++mNumGrantM;
        else if (s == mem::LineState::S)
            ++mNumGrantS;
    }

    // Inclusion back-invalidation: when the shared cache evicts a line, drop it from every
    // sharer's L1 and remove the directory entry (keeps the directory inclusive of the tags).
    void evictLine(cpu::address_t line_addr, mem::CacheSource src) {
        auto it = mDirectory.find(line_addr);
        if (it == mDirectory.end()) return;
        uint64_t sharers = it->second.sharers;
        for (mem::RnId t = 0; sharers != 0; ++t, sharers >>= 1) {
            if ((sharers & 1) && t < mNumRequesters) {
                mem::Snoop s;
                s.line_addr = line_addr;
                s.requester_id = t;
                s.source = src;
                s.opcode = mem::SnoopOpcode::SnpInvalidate;
                enqueueSnoop(s);
                ++mNumSnoops;
                ++mNumEvictSnoops;
            }
        }
        mDirectory.erase(it);
        for (auto& seen : mLastReqSeq) seen.erase(line_addr);
    }

    void handleWriteback(const mem::Writeback& wb) {
        ++mNumWritebacks;
        if (wb.requester_id < mNumRequesters) ++mPerCoreWritebacks[wb.requester_id];
        // Writebacks and requests use separate channels, and a clock crossing can slide one by a cycle: a
        // request the RN sent after this writeback may already have been granted, so the entry must keep it.
        mem::Writeback w = wb;
        if (wb.requester_id < mNumRequesters) {
            const auto& seen = mLastReqSeq[wb.requester_id];
            if (auto s = seen.find(wb.line_addr); s != seen.end() && s->second > wb.rn_seq) w.keep_sharer = true;
        }
        if (auto it = mDirectory.find(wb.line_addr); it != mDirectory.end()) mProtocol->onWriteback(wb.requester_id, w, it->second);
        // A miss goes on to memory: allocating would evict a victim, breaking inclusion unless it
        // were back-invalidated, which can trigger yet another writeback.
        if (auto* line = mCache->getLine(wb.line_addr)) line->setModified(true);
        ILOG("[cluster-cache] cycle " << getClock()->currentCycle() << " writeback from cpu " << wb.requester_id << " line 0x" << std::hex << wb.line_addr
                                      << std::dec);
    }

    // Model the access latency in the cache domain (a per-packet ready cycle), then cross
    // the SyncOutPort with a fixed 1-cycle delay -- a variable sync delay would break the
    // port's monotonic per-cycle send when the cache clocks slower than a core.
    void enqueueResp(const mem::Response& r, uint32_t latency) {
        mRespQ.push_back({r, getClock()->currentCycle() + latency});
        scheduleSyncDrain();
    }
    void enqueueSnoop(const mem::Snoop& s) {
        mSnpQ.push_back({s, getClock()->currentCycle()});  // snoops carry no access latency
        scheduleSyncDrain();
    }

    void scheduleSyncDrain() {
        const auto* clk = getClock();
        const auto period = clk->getPeriod();
        auto to_edge = (period - (clk->currentTick() % period)) % period;
        if (to_edge == 0) to_edge = period;  // strictly-positive: land on the next edge
        mSyncDrain.scheduleRelativeTick(to_edge, clk->getScheduler());
    }

    void drainSync() {
        // Send ready packets, one per requester per cycle (the port sends once per cycle);
        // reschedule while any remain queued or not yet ready.
        const uint64_t now = getClock()->currentCycle();
        std::vector<bool> resp_used(mNumRequesters, false), snp_used(mNumRequesters, false);
        for (auto it = mRespQ.begin(); it != mRespQ.end();) {
            if (it->second > now || resp_used[it->first.requester_id]) {
                ++it;
                continue;
            }
            resp_used[it->first.requester_id] = true;
            mSlaves[it->first.requester_id]->resp_out->sendAndAllowSlide(it->first, kSyncCrossDelay);
            it = mRespQ.erase(it);
        }
        for (auto it = mSnpQ.begin(); it != mSnpQ.end();) {
            if (it->second > now || snp_used[it->first.requester_id]) {
                ++it;
                continue;
            }
            snp_used[it->first.requester_id] = true;
            mSlaves[it->first.requester_id]->inval_out->sendAndAllowSlide(it->first, kSyncCrossDelay);
            it = mSnpQ.erase(it);
        }
        if (!mRespQ.empty() || !mSnpQ.empty()) scheduleSyncDrain();
    }

    uint32_t mNumRequesters;
    uint32_t mHitLatency;
    uint32_t mMissLatency;
    bool mCoherenceEnabled;      // false => tag-only shared cache; selects the coherence engine when live
    uint32_t mCoherenceLatency;  // extra cycles added to a grant that required snoops
    std::unique_ptr<cpu::SimpleCache> mCache;
    std::unique_ptr<mem::ProtocolEngine> mProtocol;             // Home-Node coherence engine (Null when disabled)
    std::unordered_map<mem::addr_t, mem::DirEntry> mDirectory;  // per-line sharer/owner state (coherence on)
    // Per RN: rn_seq of its latest request per line.
    std::vector<std::unordered_map<mem::addr_t, uint64_t>> mLastReqSeq;

    // Outstanding-transaction tracker: a grant that is waiting on snoop acknowledgements. Keyed
    // by a unique per-request txn; released when `pending` snoops have all acked on SNPRSP.
    struct PendingGrant {
        mem::Response response;
        uint32_t latency;
        uint32_t pending;
    };
    uint64_t mNextTxn{0};
    std::unordered_map<uint64_t, PendingGrant> mPendingGrants;
    std::vector<std::unique_ptr<mem::SyncMemSlave>> mSlaves;
    std::vector<uint64_t> mPerCoreRequests;
    std::vector<uint64_t> mPerCoreWritebacks;
    sparta::Counter mNumRequests;
    sparta::Counter mNumWritebacks;
    sparta::Counter mNumHits;
    sparta::Counter mNumMisses;
    sparta::Counter mNumReadShared;
    sparta::Counter mNumReadUnique;
    sparta::Counter mNumCleanUnique;
    sparta::Counter mNumReadOnce;
    sparta::Counter mNumWriteUnique;
    sparta::Counter mNumSnoops;
    sparta::Counter mNumGrantS;
    sparta::Counter mNumGrantM;
    sparta::Counter mNumEvictSnoops;
    sparta::Counter mNumMmio;

    cluster::MmioRouter mMmio;  // BAR decode consulted before the tag path (see handleReq)

    // Posedge-aligned drain of outgoing sync responses/snoops (see enqueueResp/Snoop).
    static constexpr uint32_t kSyncCrossDelay = 1;  // fixed clock-crossing delay (cycles)
    sparta::UniqueEvent<sparta::SchedulingPhase::PortUpdate> mSyncDrain;
    std::deque<std::pair<mem::Response, uint64_t>> mRespQ;  // (response, ready cache-cycle)
    std::deque<std::pair<mem::Snoop, uint64_t>> mSnpQ;      // (snoop, ready cache-cycle)
};
