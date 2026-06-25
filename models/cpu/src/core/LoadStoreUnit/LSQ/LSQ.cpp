#include "LoadStoreUnit/LSQ/LSQ.hpp"

#include <algorithm>

#include "sparta/utils/MathUtils.hpp"

#include "Common/SpeculationUtils.hpp"
#include "LoadStoreUnit/BackendMemoryStructures/BackendMemoryStructures.hpp"
#include "Logging.hpp"

namespace midcore {

LSQ::LSQ(sparta::TreeNode* node, const LSQParams* params)
    : sparta::Unit(node),
      mNumBanks(params->num_banks),
      mForwardingEnabled(params->store_forwarding),
      mForwardingLatency(params->forwarding_latency),
      mNumLoads(&unit_stat_set_, "num_loads", "Total load instructions", sparta::Counter::COUNT_NORMAL),
      mNumStores(&unit_stat_set_, "num_stores", "Total store instructions", sparta::Counter::COUNT_NORMAL),
      mNumForwards(&unit_stat_set_, "num_forwards", "Loads satisfied by store forwarding", sparta::Counter::COUNT_NORMAL),
      mNumSquashed(&unit_stat_set_, "num_squashed", "Instructions squashed due to flush", sparta::Counter::COUNT_NORMAL),
      mMaxLqOccupancy(&unit_stat_set_, "max_lq_occupancy", "Peak total load-queue occupancy", sparta::Counter::COUNT_LATEST),
      mMaxSqOccupancy(&unit_stat_set_, "max_sq_occupancy", "Peak total store-queue occupancy", sparta::Counter::COUNT_LATEST),
      mSumLqOccupancy(&unit_stat_set_, "sum_lq_occupancy", "Sum of load-queue occupancy per tick (mean = /cpu_cycles)", sparta::Counter::COUNT_NORMAL),
      mSumSqOccupancy(&unit_stat_set_, "sum_sq_occupancy", "Sum of store-queue occupancy per tick (mean = /cpu_cycles)", sparta::Counter::COUNT_NORMAL) {
    // Determine capacities (use new params if set, else fall back to legacy)
    uint32_t lq_cap = params->load_queue_capacity;
    uint32_t sq_cap = params->store_queue_capacity;

    // If num_banks > 1, each bank gets its own LQ and SQ
    // Divide capacity among banks
    uint32_t lq_per_bank = (lq_cap + mNumBanks - 1) / mNumBanks;
    uint32_t sq_per_bank = (sq_cap + mNumBanks - 1) / mNumBanks;

    mLoadQueues.reserve(mNumBanks);
    mStoreQueues.reserve(mNumBanks);
    for (uint32_t i = 0; i < mNumBanks; ++i) {
        mLoadQueues.push_back(std::make_unique<LoadQueue>(lq_per_bank));
        mStoreQueues.push_back(std::make_unique<StoreQueue>(sq_per_bank));
    }

    // Rename claims one per memory uop in program order; the credit
    // guarantees a free slot at execute, so no overflow buffer is needed.
    for (uint32_t i = 0; i < mNumBanks; ++i) {
        mLoadCreditCap += static_cast<int32_t>(mLoadQueues[i]->capacity());
        mStoreCreditCap += static_cast<int32_t>(mStoreQueues[i]->capacity());
    }
    mLoadCredits = mLoadCreditCap;
    mStoreCredits = mStoreCreditCap;

    // Bank index calculation: compute log2 of cache line size for address shift
    mLineShift = sparta::utils::floor_log2(static_cast<uint32_t>(params->cache_line_size));
    mBankMask = mNumBanks - 1;
    sparta_assert(mNumBanks == 1 || (mNumBanks & mBankMask) == 0, "LSQ num_banks must be a power of 2, got " << mNumBanks);

    in_port.registerConsumerHandler(CREATE_SPARTA_HANDLER_WITH_DATA(LSQ, receivePackets_, std::vector<core::IssuePacket>));
    response_in.registerConsumerHandler(CREATE_SPARTA_HANDLER_WITH_DATA(LSQ, receiveResponse_, core::MemResponse));
    flush_in.registerConsumerHandler(CREATE_SPARTA_HANDLER_WITH_DATA(LSQ, receiveFlush_, core::FlushRequest));

    ILOG("[lsq] Initialized: num_banks=" << mNumBanks << " lq_cap=" << lq_cap << " sq_cap=" << sq_cap << " forwarding=" << mForwardingEnabled);
}

uint32_t LSQ::bankFor(cpu::address_t addr) const {
    if (mNumBanks <= 1) return 0;
    return static_cast<uint32_t>((addr >> mLineShift) & mBankMask);
}

void LSQ::receivePackets_(const std::vector<core::IssuePacket>& pkts) {
    for (const auto& ipkt : pkts) {
        // If this tag was marked as flushed (had pending D-cache request), clear the mark
        // since the tag is now being reused for a correct-path instruction
        mFlushedPendingTags.erase(ipkt.pkt.tag);

        // Check if this packet should be squashed according to the most recent flush.
        // This handles race conditions where packets arrive in the same cycle as a flush.
        // Use fetch_seq for filtering: packets with fetch_seq <= flush's branch_fetch_seq and
        // tag > branch_tag are stale wrong-path instructions that should be squashed.
        if (mLastFlush && ipkt.pkt.fetch_seq <= mLastFlush->branch_fetch_seq && ipkt.pkt.tag > mLastFlush->branch_tag) {
            // Squashed in flight: it will never become an LSQ entry, so refund the
            // credit it claimed at rename (no-op if a flush already refunded it).
            claimSquashed_(ipkt.pkt.fetch_seq);
            continue;  // Skip this packet - depth=0 late arrival
        }

        // This claimed op has now reached the LSQ. If its claim is gone, a flush already
        // refunded its credit (it was squashed in flight) -- it must NOT enqueue, or it would
        // occupy an LQ/SQ slot with no backing credit and over-subscribe the queue (tripping
        // the "full despite rename credit" assert). Drop it. Otherwise its credit stays
        // consumed by the LQ/SQ entry it is about to occupy.
        if (!claimArrived_(ipkt.pkt.fetch_seq)) {
            continue;
        }

        // Extract address from trace record
        cpu::address_t addr = 0;
        uint8_t size = 4;
        if (ipkt.pkt.inst) {
            const auto& vas = ipkt.pkt.inst->getTraceRecord().virtAddrs;
            if (!vas.empty()) addr = vas[0];
        }

        bool is_store = (ipkt.uop_type == core::UopType::Store);

        // Check for and handle duplicate tags BEFORE dispatch.
        // Duplicate tags occur when a late wrong-path instruction with the same tag was
        // already in the LSQ but should have been flushed. Remove the stale entry first.
        auto dup_pos =
            std::lower_bound(mCompletionOrder.begin(), mCompletionOrder.end(), ipkt.pkt.tag, [](const CompletionEntry& ce, uint64_t t) { return ce.tag < t; });
        if (dup_pos != mCompletionOrder.end() && dup_pos->tag == ipkt.pkt.tag) {
            // Remove stale entry from load/store queues; the stale op held an
            // LQ/SQ entry, so removing it returns its credit.
            if (!dup_pos->is_store) {
                mLoadCompletedTags.erase(dup_pos->tag);
                for (uint32_t b = 0; b < mNumBanks; ++b) {
                    if (mLoadQueues[b]->removeByTag(dup_pos->tag)) returnLoadCredit();
                }
            } else {
                for (uint32_t b = 0; b < mNumBanks; ++b) {
                    if (mStoreQueues[b]->removeByTag(dup_pos->tag)) returnStoreCredit();
                }
            }
            mCompletionOrder.erase(dup_pos);
        }

        // Dispatch to load/store queues. A credit was claimed at rename, so the
        // target queue is guaranteed to have room.
        if (ipkt.uop_type == core::UopType::Load) {
            dispatchLoad(ipkt, addr, size);
            ++mNumLoads;
        } else if (ipkt.uop_type == core::UopType::Store) {
            dispatchStore(ipkt, addr, size);
            ++mNumStores;
        }

        // Add to completion order - ALWAYS, even when going to pending.
        // This ensures ROB knows about all instructions in program order.
        // OOO dispatch means packets may arrive out of order, so insert sorted by tag.
        CompletionEntry new_entry{
            .tag = ipkt.pkt.tag, .rob_token = ipkt.rob_token, .is_store = is_store, .completed = is_store};  // Stores complete immediately
        auto insert_pos = std::lower_bound(mCompletionOrder.begin(), mCompletionOrder.end(), new_entry,
                                           [](const CompletionEntry& a, const CompletionEntry& b) { return a.tag < b.tag; });
        mCompletionOrder.insert(insert_pos, new_entry);

        if (mVis) mVis->onLsqEnter(ipkt.pkt.fetch_seq, getClock()->currentCycle());
        if (mVis && is_store) mVis->onLsqExit(ipkt.pkt.fetch_seq, getClock()->currentCycle());
    }
}

void LSQ::dispatchLoad(const core::IssuePacket& ipkt, cpu::address_t addr, uint8_t size) {
    auto& lq = mLoadQueues[bankFor(addr)];
    // A load credit was claimed at rename, so a slot is guaranteed. (Flat credits
    // assume a single bank; multi-bank per-bank room is a Phase-2 concern.)
    sparta_assert(!lq->isFull(), "LSQ load queue full despite rename credit (multi-bank without Phase 2?)");
    lq->enqueue(ipkt, addr, size);
}

void LSQ::dispatchStore(const core::IssuePacket& ipkt, cpu::address_t addr, uint8_t size) {
    auto& sq = mStoreQueues[bankFor(addr)];
    sparta_assert(!sq->isFull(), "LSQ store queue full despite rename credit (multi-bank without Phase 2?)");
    sq->enqueue(ipkt, addr, size);
}

void LSQ::receiveResponse_(const core::MemResponse& resp) {
    // Check if this is a late response for a flushed request - ignore it
    if (mFlushedPendingTags.count(resp.tag)) {
        // Don't remove from mFlushedPendingTags yet - wait for tag reuse
        return;
    }

    // Find the load that this response is for
    for (uint32_t bank = 0; bank < mNumBanks; ++bank) {
        auto* entry = mLoadQueues[bank]->find(resp.tag);
        if (!entry) continue;

        entry->response_received = true;
        if (mVis) mVis->onLsqExit(entry->ipkt.pkt.fetch_seq, getClock()->currentCycle());
        if (mVis && resp.miss) {
            mVis->onDcacheMiss(entry->ipkt.pkt.fetch_seq, getClock()->currentCycle());
        }

        // If arbiter disabled, complete immediately
        if (!mArbiter || !mArbiter->enabled()) {
            if (!entry->ipkt.phys_dsts.empty()) {
                std::vector<core::PhysRegRef> completions;
                for (const auto& dst : entry->ipkt.phys_dsts) completions.push_back(dst);
                completion_out.send(completions, 1);
            }
            entry->written_back = true;
        }

        // Mark load as completed (O(1) set insert instead of O(n) deque scan)
        mLoadCompletedTags.insert(resp.tag);

        return;
    }

    ILOG("[lsq] Response for unknown tag=" << resp.tag);
}

void LSQ::receiveFlush_(const core::FlushRequest& req) {
    ILOG("[lsq] cycle=" << getClock()->currentCycle() << " RECEIVED flush: branch_tag=" << req.branch_tag
                        << " branch_depth=" << static_cast<int>(req.branch_depth));

    // Store this flush for filtering late-arriving packets
    // Always update to the most recent flush - we need to track the latest misprediction
    // to filter late-arriving wrong-path instructions issued after that branch
    mLastFlush = req;

    // Squash predicate: tag/depth-based shouldSquash plus a fetch-sequence backstop. fetch_seq
    // is monotonic and never reused, so an op fetched after the resolving branch (larger
    // fetch_seq) is unambiguously on the squashed wrong path even when its model tag has been
    // reused or its depth decremented. Without this, such an op is neither squashed nor
    // credit-refunded here -- it leaks its rename credit, eventually tripping the
    // "full despite rename credit" assert.
    auto should_squash = [&req](const core::PipelinePacket& p) {
        if (core::shouldSquash(p, req)) return true;
        return req.branch_fetch_seq != 0 && p.fetch_seq > req.branch_fetch_seq;
    };

    uint32_t total_squashed = 0;

    // Squash from load queues (only speculative/uncommitted loads)
    for (uint32_t bank = 0; bank < mNumBanks; ++bank) {
        auto& lq = mLoadQueues[bank];
        std::vector<uint64_t> to_remove;
        for (auto& entry : *lq) {
            if (should_squash(entry.ipkt.pkt)) {
                to_remove.push_back(entry.ipkt.pkt.tag);
                // If D-cache request is in flight, track the tag so we can ignore late responses
                if (entry.request_sent && !entry.response_received) {
                    mFlushedPendingTags.insert(entry.ipkt.pkt.tag);
                }
            }
        }
        for (uint64_t tag : to_remove) {
            lq->removeByTag(tag);
            returnLoadCredit();  // squashed LQ entry frees its claimed credit
            mLoadCompletedTags.erase(tag);
            // Also remove from completion order to prevent zombie entries
            auto co_it =
                std::lower_bound(mCompletionOrder.begin(), mCompletionOrder.end(), tag, [](const CompletionEntry& ce, uint64_t t) { return ce.tag < t; });
            if (co_it != mCompletionOrder.end() && co_it->tag == tag) {
                mCompletionOrder.erase(co_it);
            }
            ++total_squashed;
        }
    }

    // Squash from store queues (only uncommitted stores)
    for (uint32_t bank = 0; bank < mNumBanks; ++bank) {
        auto& sq = mStoreQueues[bank];
        std::vector<uint64_t> to_remove;
        for (auto& entry : *sq) {
            if (!entry.committed && should_squash(entry.ipkt.pkt)) {
                to_remove.push_back(entry.ipkt.pkt.tag);
            }
        }
        for (uint64_t tag : to_remove) {
            sq->removeByTag(tag);
            returnStoreCredit();  // squashed SQ entry frees its claimed credit
            // Also remove from completion order to prevent zombie entries
            auto co_it =
                std::lower_bound(mCompletionOrder.begin(), mCompletionOrder.end(), tag, [](const CompletionEntry& ce, uint64_t t) { return ce.tag < t; });
            if (co_it != mCompletionOrder.end() && co_it->tag == tag) {
                mCompletionOrder.erase(co_it);
            }
            ++total_squashed;
        }
    }

    // Refund credits for ops that claimed at rename but were squashed before ever
    // reaching the LSQ (they will never become an LQ/SQ entry, so their credit
    // would otherwise leak). Presence-guarded against the arrival-skip path so a
    // claim is refunded exactly once.
    for (auto it = mClaimedNotArrived.begin(); it != mClaimedNotArrived.end();) {
        // it->first is the claim's fetch_seq (monotonic, never reused): a claim fetched after
        // the resolving branch is on the squashed path even if its tag/depth no longer match.
        bool squashed = core::shouldSquash(it->second.tag, it->second.depth, req) || (req.branch_fetch_seq != 0 && it->first > req.branch_fetch_seq);
        if (squashed) {
            if (it->second.is_store)
                returnStoreCredit();
            else
                returnLoadCredit();
            it = mClaimedNotArrived.erase(it);
        } else {
            ++it;
        }
    }

    // Squash from completion order tracking (legacy fallback for tag > branch_tag)
    // Note: This is now mostly redundant since we remove from mCompletionOrder when
    // removing from load/store/pending queues above. Kept for safety.
    auto co_it = mCompletionOrder.begin();
    while (co_it != mCompletionOrder.end()) {
        if (co_it->tag > req.branch_tag) {
            co_it = mCompletionOrder.erase(co_it);
        } else {
            ++co_it;
        }
    }

    mNumSquashed += total_squashed;
    ILOG("[lsq] Squashed " << total_squashed << " entries from LSQ");
}

bool LSQ::checkStoreForwarding(LoadQueue::Entry& load) {
    if (!mForwardingEnabled) return false;

    uint32_t bank = bankFor(load.address);
    auto& sq = mStoreQueues[bank];

    // Check store queue for forwarding (only stores older than this load)
    const auto* st = sq->findForwardingOlderThan(load.address, load.access_size, load.ipkt.pkt.tag);
    if (st) {
        load.forwarded = true;
        load.forward_cycles = mForwardingLatency;
        ++mNumForwards;
        ILOG("[lsq] Store forwarding: load tag=" << load.ipkt.pkt.tag << " from store tag=" << st->ipkt.pkt.tag);
        return true;
    }

    // Also check D-cache store buffer if available
    if (mDcache) {
        // D-cache now has store buffer forwarding check
        // This requires the cache interface to be updated
    }

    return false;
}

void LSQ::tryIssueLoads() {
    if (!mDcache) return;

    for (uint32_t bank = 0; bank < mNumBanks; ++bank) {
        for (auto& entry : *mLoadQueues[bank]) {
            if (entry.request_sent || entry.forwarded || entry.response_received) continue;

            // Try store forwarding first
            if (checkStoreForwarding(entry)) {
                continue;  // Forwarded, no cache access needed
            }

            // Try to send to D-cache
            core::MemRequest req;
            req.tag = entry.ipkt.pkt.tag;
            req.pc = entry.ipkt.pkt.pc;
            req.address = entry.address;
            req.access_size = entry.access_size;
            req.is_store = false;

            if (mDcache->canAcceptRequest(req.address)) {
                request_out.send(req, 0);
                entry.request_sent = true;
            }
        }
    }
}

void LSQ::tryDrainStores() {
    if (!mDcache) return;

    for (uint32_t bank = 0; bank < mNumBanks; ++bank) {
        auto* entry = mStoreQueues[bank]->nextDrainable();
        if (!entry) continue;

        core::MemRequest req;
        req.tag = entry->ipkt.pkt.tag;
        req.pc = entry->ipkt.pkt.pc;
        req.address = entry->address;
        req.access_size = entry->access_size;
        req.is_store = true;

        if (mDcache->canAcceptStore()) {
            request_out.send(req, 0);
            entry->sent_to_cache = true;
            ILOG("[lsq] Store tag=" << entry->ipkt.pkt.tag << " drained to cache");
        }
    }
}

void LSQ::drainCompletedEntries() {
    mRobCompletionsBuf.clear();

    // Drain completed entries in order.
    // Note: Stores are marked "completed" immediately so ROB completion is sent early,
    // allowing Writeback to retire them and call commitStore(). The store queue entry
    // is only removed AFTER the store has been committed and drained to cache.
    //
    // We iterate over entries, skipping ones that are not yet removable (a store
    // awaiting cache drain, or a load awaiting write-back) so younger ready entries
    // can still drain past them.

    size_t skip_count = 0;
    while (skip_count < mCompletionOrder.size()) {
        auto it = mCompletionOrder.begin() + skip_count;
        auto& ce = *it;

        // Check completion: stores are completed immediately (ce.completed),
        // loads use mLoadCompletedTags set (O(1) lookup instead of O(n) scan)
        bool is_completed = ce.completed || mLoadCompletedTags.count(ce.tag);

        // Stop at incomplete entries that are NOT in pending
        // (they're in a queue and need to execute first)
        if (!is_completed) break;

        bool can_remove = false;

        if (ce.is_store) {
            // Stores: Can only remove from completion order after committed AND sent to cache.
            for (uint32_t b = 0; b < mNumBanks; ++b) {
                if (mStoreQueues[b]->canRemove(ce.tag)) {
                    mStoreQueues[b]->removeByTag(ce.tag);
                    returnStoreCredit();  // SQ entry drained: free the claimed credit
                    can_remove = true;
                    break;
                }
            }

            // Send ROB completion to allow Writeback to retire and call commitStore()
            if (!ce.rob_sent) {
                mRobCompletionsBuf.push_back(ce.rob_token);
                ce.rob_sent = true;
            }

            if (!can_remove) {
                // Store is in SQ but not yet drainable (committed && sent_to_cache).
                // Skip this entry to allow younger instructions to drain.
                ++skip_count;
                continue;
            }
        } else {
            // Loads: Send ROB completion when data is received.
            // This allows Writeback to retire the instruction.
            // The LQ entry is only REMOVED when written_back=true (after arbiter grants).

            // Send ROB completion if not already sent
            if (!ce.rob_sent) {
                mRobCompletionsBuf.push_back(ce.rob_token);
                ce.rob_sent = true;
            }

            // Try to remove from LQ (only when written_back=true)
            for (uint32_t b = 0; b < mNumBanks; ++b) {
                if (mLoadQueues[b]->canRemove(ce.tag)) {
                    mLoadQueues[b]->removeByTag(ce.tag);
                    returnLoadCredit();  // LQ entry written back: free the claimed credit
                    can_remove = true;
                    break;
                }
            }

            if (!can_remove) {
                // Load is in LQ, ROB completion sent, but not yet written back.
                // Skip this entry to allow younger instructions to drain.
                ++skip_count;
                continue;
            }
        }

        if (can_remove) {
            mLoadCompletedTags.erase(ce.tag);
            mCompletionOrder.erase(it);
            // Don't increment skip_count since we removed an element
        } else {
            ++skip_count;
        }
    }

    if (!mRobCompletionsBuf.empty()) {
        rob_complete_out.send(mRobCompletionsBuf, 1);
    }
}

void LSQ::tick() {
    // Advance forwarding countdowns
    for (auto& lq : mLoadQueues) {
        lq->tickForwardCountdowns();

        // Check if any forwarded loads are now ready
        for (auto& entry : *lq) {
            if (entry.forwarded && entry.forward_cycles == 0 && !entry.response_received) {
                entry.response_received = true;  // Treat forwarded as "received"

                if (!mArbiter || !mArbiter->enabled()) {
                    if (!entry.ipkt.phys_dsts.empty()) {
                        std::vector<core::PhysRegRef> completions;
                        for (const auto& dst : entry.ipkt.phys_dsts) completions.push_back(dst);
                        completion_out.send(completions, 1);
                    }
                    entry.written_back = true;
                }

                // Mark load as completed (O(1) set insert)
                mLoadCompletedTags.insert(entry.ipkt.pkt.tag);
            }
        }
    }

    // Try to issue pending loads
    tryIssueLoads();

    // Try to drain committed stores
    tryDrainStores();

    // Submit ready loads to write-port arbiter
    if (mArbiter && mArbiter->enabled()) {
        for (auto& lq : mLoadQueues) {
            for (auto& entry : *lq) {
                if ((entry.response_received || (entry.forwarded && entry.forward_cycles == 0)) && !entry.written_back) {
                    WriteCandidate cand;
                    cand.source_id = kLsqSourceId;
                    cand.tag = entry.ipkt.pkt.tag;
                    cand.fetch_seq = entry.ipkt.pkt.fetch_seq;
                    cand.phys_dsts = entry.ipkt.phys_dsts;
                    cand.rob_token = entry.ipkt.rob_token;
                    mArbiter->submit(cand);
                }
            }
        }
    }

    // Drain completed entries
    drainCompletedEntries();

    // Phase 0 observability: record occupancy after this cycle's work.
    sampleOccupancyStats();
}

void LSQ::sampleOccupancyStats() {
    uint64_t lq = 0, sq = 0;
    for (uint32_t i = 0; i < mNumBanks; ++i) {
        lq += mLoadQueues[i]->size();
        sq += mStoreQueues[i]->size();
    }

    if (lq > mMaxLqOccupancy.get()) mMaxLqOccupancy = lq;
    if (sq > mMaxSqOccupancy.get()) mMaxSqOccupancy = sq;

    mSumLqOccupancy += lq;
    mSumSqOccupancy += sq;
}

void LSQ::claimLoad(uint64_t fetch_seq, uint64_t tag, uint8_t depth) {
    // Rename only calls this after canClaimLoad(), so a credit is available.
    --mLoadCredits;
    mClaimedNotArrived[fetch_seq] = ClaimInfo{tag, depth, /*is_store=*/false};
}

void LSQ::claimStore(uint64_t fetch_seq, uint64_t tag, uint8_t depth) {
    --mStoreCredits;
    mClaimedNotArrived[fetch_seq] = ClaimInfo{tag, depth, /*is_store=*/true};
}

void LSQ::processWritePortGrants() {
    if (!mArbiter || !mArbiter->enabled()) return;

    mRegCompletionsBuf.clear();

    for (auto& lq : mLoadQueues) {
        for (auto& entry : *lq) {
            bool ready = (entry.response_received || (entry.forwarded && entry.forward_cycles == 0));
            bool granted = mArbiter->isGranted(entry.ipkt.pkt.tag);
            if (ready && !entry.written_back && granted) {
                for (const auto& dst : entry.ipkt.phys_dsts) mRegCompletionsBuf.push_back(dst);
                entry.written_back = true;
            }
        }
    }

    if (!mRegCompletionsBuf.empty()) {
        completion_out.send(mRegCompletionsBuf, 1);
    }
}

void LSQ::commitStore(uint64_t tag) {
    for (auto& sq : mStoreQueues) {
        if (auto* entry = sq->find(tag)) {
            entry->committed = true;
            return;
        }
    }
}

}  // namespace midcore
