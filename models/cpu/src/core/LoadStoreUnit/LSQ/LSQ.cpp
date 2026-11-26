// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Tenstorrent USA, Inc.

#include "LoadStoreUnit/LSQ/LSQ.hpp"

#include <algorithm>

#include "sparta/utils/MathUtils.hpp"

#include "Common/ParamChoice.hpp"
#include "Common/SpeculationUtils.hpp"
#include "LoadStoreUnit/BackendMemoryStructures/BackendMemoryStructures.hpp"
#include "Logging.hpp"
#include "Midcore/Writeback/Writeback.hpp"
#include "Mmu/Mmu.hpp"

namespace midcore {

LSQ::LSQ(sparta::TreeNode* node, const LSQParams* params)
    : sparta::Unit(node),
      mNumBanks(params->num_banks),
      mForwardingEnabled(params->store_forwarding),
      mForwardingLatency(params->forwarding_latency),
      mLoadPorts(params->load_ports),
      mStorePorts(params->store_ports),
      mSsitClearInterval(params->ssit_clear_interval),
      mStoreSets(params->ssit_entries, params->lfst_entries),
      mNumLoads(&unit_stat_set_, "num_loads", "Total load instructions", sparta::Counter::COUNT_NORMAL),
      mNumStores(&unit_stat_set_, "num_stores", "Total store instructions", sparta::Counter::COUNT_NORMAL),
      mNumForwards(&unit_stat_set_, "num_forwards", "Loads satisfied by store forwarding", sparta::Counter::COUNT_NORMAL),
      mNumSquashed(&unit_stat_set_, "num_squashed", "Instructions squashed due to flush", sparta::Counter::COUNT_NORMAL),
      mNumAtomics(&unit_stat_set_, "num_atomics", "Atomics (AMO, LR, SC) performed in the D-cache", sparta::Counter::COUNT_NORMAL),
      mNumFences(&unit_stat_set_, "num_fences", "Fences completed", sparta::Counter::COUNT_NORMAL),
      mNumViolations(&unit_stat_set_, "num_ordering_violations", "Loads that read before an older overlapping store resolved its address",
                     sparta::Counter::COUNT_NORMAL),
      mNumStaleResponses(&unit_stat_set_, "num_stale_responses", "D-cache responses dropped: a squashed load's access whose tag a newer load reuses",
                         sparta::Counter::COUNT_NORMAL),
      mNumStoreSetWaits(&unit_stat_set_, "num_store_set_waits", "Loads held for a store the store-set predictor named", sparta::Counter::COUNT_NORMAL),
      mNumOverlapWaits(&unit_stat_set_, "num_overlap_waits", "Loads held until an older overlapping store they cannot forward from writes the cache",
                       sparta::Counter::COUNT_NORMAL),
      mNumBarrierWaits(&unit_stat_set_, "num_barrier_waits", "Loads held behind an older fence or acquire atomic", sparta::Counter::COUNT_NORMAL),
      mNumLoadPortStalls(&unit_stat_set_, "num_load_port_stalls", "Load-cycles a ready load found no free load port", sparta::Counter::COUNT_NORMAL),
      mNumStorePortStalls(&unit_stat_set_, "num_store_port_stalls", "Cycles a retired store found no free store port", sparta::Counter::COUNT_NORMAL),
      mMaxLqOccupancy(&unit_stat_set_, "max_lq_occupancy", "Peak total load-queue occupancy", sparta::Counter::COUNT_LATEST),
      mMaxSqOccupancy(&unit_stat_set_, "max_sq_occupancy", "Peak total store-queue occupancy", sparta::Counter::COUNT_LATEST),
      mSumLqOccupancy(&unit_stat_set_, "sum_lq_occupancy", "Sum of load-queue occupancy per tick (mean = /cpu_cycles)", sparta::Counter::COUNT_NORMAL),
      mSumSqOccupancy(&unit_stat_set_, "sum_sq_occupancy", "Sum of store-queue occupancy per tick (mean = /cpu_cycles)", sparta::Counter::COUNT_NORMAL) {
    uint32_t lq_cap = params->load_queue_capacity;
    uint32_t sq_cap = params->store_queue_capacity;
    const std::string dep = core::checkChoice(params->mem_dep_predictor, {"store_sets", "blind", "none"});
    mMemDepMode = dep == "store_sets" ? MemDepMode::StoreSets : dep == "blind" ? MemDepMode::Blind : MemDepMode::None;
    sparta_assert(mLoadPorts > 0 && mStorePorts > 0, "LSQ load_ports and store_ports must be at least 1");
    sparta_assert(params->ssit_entries > 0 && params->lfst_entries > 0, "LSQ ssit_entries and lfst_entries must be at least 1");
    sparta_assert(lq_cap > 0 && sq_cap > 0, "lsq.params.load_queue_capacity and store_queue_capacity must be at least 1");
    // Rename's LQ/SQ credits are one pool, so a bank could fill while credits remain.
    sparta_assert(mNumBanks == 1, params->num_banks.getLocation() << " = " << mNumBanks << "; only 1 is supported");

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
        const auto claim = claimArrived_(ipkt.pkt.fetch_seq);
        if (!claim) {
            continue;
        }

        const bool is_fence = ipkt.uop_type == core::UopType::Fence;
        const bool is_atomic = ipkt.pkt.inst_class == cpu::InstClass::Atomic;
        cpu::address_t addr = 0;
        uint8_t size = 4;
        std::vector<cpu::address_t> lines;
        std::vector<MemRange> ranges;
        bool is_lr = false;
        if (ipkt.pkt.inst && !is_fence) {
            const auto& rec = ipkt.pkt.inst->getTraceRecord();
            if (!rec.physAddrs.empty()) addr = rec.physAddrs[0] ? rec.physAddrs[0] : rec.virtAddrs[0];
            if (rec.dataSize) size = static_cast<uint8_t>(rec.dataSize);
            lines = linesTouched_(rec);
            ranges = rangesTouched_(rec);
            is_lr = is_atomic && rec.assembly.starts_with("lr.");
        }
        // A vector access with no active element (vl=0 or all masked off) touches no memory.
        const auto* pac = ipkt.pkt.inst ? ipkt.pkt.inst->getPerfPtr().get() : nullptr;
        const bool no_access = lines.empty() && pac && (pac->isVectorLoad() || pac->isVectorStore());
        if (lines.empty() && !is_fence && !no_access) lines.push_back((addr >> mLineShift) << mLineShift);

        bool is_store = (ipkt.uop_type == core::UopType::Store || is_fence);

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
            auto& lq = mLoadQueues[bankFor(addr)];
            sparta_assert(!lq->isFull(), "LSQ load queue full despite rename credit");
            auto& e = lq->enqueue(ipkt, addr, size, std::move(lines));
            e.ranges = std::move(ranges);
            e.dep_store_seq = claim->dep_seq;
            if (no_access) e.forwarded = true;  // completes next tick with no cache request
            ++mNumLoads;
        } else if (is_store) {
            auto& sq = mStoreQueues[bankFor(addr)];
            sparta_assert(!sq->isFull(), "LSQ store queue full despite rename credit");
            auto& e = sq->enqueue(ipkt, addr, size, std::move(lines));
            if (!is_lr) e.ranges = std::move(ranges);  // LR writes nothing
            e.data_ready_cycle = getClock()->currentCycle();
            e.pred_seq = claim->dep_seq;
            e.is_fence = is_fence;
            e.is_atomic = is_atomic;
            e.acquire = claim->barrier && is_atomic;
            if (!is_fence) ++mNumStores;
            if (!e.ranges.empty()) {
                if (mMemDepMode == MemDepMode::StoreSets) mStoreSets.issueStore(claim->pc, ipkt.pkt.fetch_seq);
                checkOrderingViolation_(e);
            }
        }

        // Add to completion order so the ROB learns about every memory op in program order.
        // OOO dispatch means packets may arrive out of order, so insert sorted by tag.
        // Plain stores complete once translated (this first query starts the walk on a TLB miss);
        // fences and atomics when they are performed.
        CompletionEntry new_entry{.tag = ipkt.pkt.tag,
                                  .rob_token = ipkt.rob_token,
                                  .is_store = is_store,
                                  .completed = is_store && !is_fence && !is_atomic && (!mMmu || mMmu->dataReady(ipkt.pkt, true))};
        auto insert_pos = std::lower_bound(mCompletionOrder.begin(), mCompletionOrder.end(), new_entry,
                                           [](const CompletionEntry& a, const CompletionEntry& b) { return a.tag < b.tag; });
        mCompletionOrder.insert(insert_pos, new_entry);

        if (mVis) mVis->onLsqEnter(ipkt.pkt.fetch_seq, getClock()->currentCycle());
        if (mVis && is_store && !is_fence && !is_atomic) mVis->onLsqExit(ipkt.pkt.fetch_seq, getClock()->currentCycle());
    }
}

// Physical addresses, so separate address spaces don't alias in shared caches; masked vector
// elements touch nothing.
std::vector<cpu::address_t> LSQ::linesTouched_(const WhisperUtil::TraceRecord& rec) const {
    std::vector<cpu::address_t> lines;
    const uint64_t size = rec.dataSize ? rec.dataSize : 1;
    for (size_t i = 0; i < rec.virtAddrs.size(); ++i) {
        if (i < rec.maskedAddrs.size() && rec.maskedAddrs[i]) continue;
        const cpu::address_t a = (i < rec.physAddrs.size() && rec.physAddrs[i]) ? rec.physAddrs[i] : rec.virtAddrs[i];
        for (cpu::address_t l = a >> mLineShift; l <= (a + size - 1) >> mLineShift; ++l) {
            if (std::find(lines.begin(), lines.end(), l << mLineShift) == lines.end()) lines.push_back(l << mLineShift);
        }
    }
    return lines;
}

std::vector<MemRange> LSQ::rangesTouched_(const WhisperUtil::TraceRecord& rec) const {
    std::vector<MemRange> ranges;
    const uint32_t size = rec.dataSize ? rec.dataSize : 1;
    for (size_t i = 0; i < rec.virtAddrs.size(); ++i) {
        if (i < rec.maskedAddrs.size() && rec.maskedAddrs[i]) continue;
        ranges.push_back({(i < rec.physAddrs.size() && rec.physAddrs[i]) ? rec.physAddrs[i] : rec.virtAddrs[i], size});
    }
    return ranges;
}

void LSQ::receiveResponse_(const core::MemResponse& resp) {
    // Find the load that this response is for
    for (uint32_t bank = 0; bank < mNumBanks; ++bank) {
        auto* entry = mLoadQueues[bank]->find(resp.tag);
        if (!entry) continue;
        if (entry->req_id != resp.req_id) {
            ++mNumStaleResponses;
            return;
        }

        if (entry->responses_pending > 0) --entry->responses_pending;
        if (entry->responses_pending > 0 || !entry->request_sent) {
            if (mVis && resp.miss) mVis->onDcacheMiss(entry->ipkt.pkt.fetch_seq, getClock()->currentCycle());
            return;
        }
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

    for (auto& sq : mStoreQueues) {
        auto* e = sq->find(resp.tag);
        if (!e || !e->is_atomic || e->responses_pending == 0 || e->req_id != resp.req_id) continue;
        if (--e->responses_pending > 0) return;
        // An atomic bypasses the write-port arbiter, like Execute's store destinations.
        if (!e->ipkt.phys_dsts.empty()) completion_out.send(e->ipkt.phys_dsts, 1);
        finishSqEntry_(*e);
        ++mNumAtomics;
        if (mVis) mVis->onLsqExit(e->ipkt.pkt.fetch_seq, getClock()->currentCycle());
        return;
    }

    ILOG("[lsq] Response for unknown tag=" << resp.tag);
}

const std::vector<core::PhysRegRef>* LSQ::singleAccessLoadDsts(uint64_t tag, uint64_t req_id) const {
    for (const auto& lq : mLoadQueues) {
        if (const auto* entry = lq->find(tag)) return entry->lines.size() == 1 && entry->req_id == req_id ? &entry->ipkt.phys_dsts : nullptr;
    }
    return nullptr;
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
            if (should_squash(entry.ipkt.pkt)) to_remove.push_back(entry.ipkt.pkt.tag);
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
            if (!entry.committed && should_squash(entry.ipkt.pkt)) to_remove.push_back(entry.ipkt.pkt.tag);
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

    mNumSquashed += total_squashed;
    ILOG("[lsq] Squashed " << total_squashed << " entries from LSQ");
}

// A fence or acquire atomic is a barrier: younger loads wait until it is performed.
uint64_t LSQ::oldestBarrierTag_() const {
    uint64_t oldest = UINT64_MAX;
    for (const auto& [seq, c] : mClaimedNotArrived)
        if (c.barrier) oldest = std::min(oldest, c.tag);
    for (const auto& sq : mStoreQueues)
        for (const auto& e : *sq)
            if ((e.is_fence || e.acquire) && !e.sent_to_cache) oldest = std::min(oldest, e.ipkt.pkt.tag);
    return oldest;
}

// A store has resolved once its address is known and so has every older store of its set.
bool LSQ::storeResolved_(uint64_t seq) const {
    while (seq != StoreSetPredictor::kNone) {
        if (mClaimedNotArrived.count(seq)) return false;
        const StoreQueue::Entry* found = nullptr;
        for (const auto& sq : mStoreQueues) {
            for (const auto& e : *sq) {
                if (e.ipkt.pkt.fetch_seq == seq) {
                    found = &e;
                    break;
                }
            }
            if (found) break;
        }
        if (!found) return true;  // retired or squashed
        seq = found->pred_seq;
    }
    return true;
}

const StoreQueue::Entry* LSQ::youngestOlderStore_(const LoadQueue::Entry& load) const {
    const StoreQueue::Entry* best = nullptr;
    for (const auto& sq : mStoreQueues) {
        const auto* st = sq->youngestOverlappingOlderThan(load.ranges, load.ipkt.pkt.tag);
        if (st && (!best || st->ipkt.pkt.tag > best->ipkt.pkt.tag)) best = st;
    }
    return best;
}

// Ordering checks before a load reads the cache. On Forward, src is the store-queue entry
// that supplies the data, or nullptr for the store buffer.
LSQ::LoadStep LSQ::orderLoad_(LoadQueue::Entry& load, uint64_t barrier_tag, const StoreQueue::Entry*& src) {
    auto wait = [&load](WaitReason r, sparta::Counter& c) {
        if (!(load.waits_counted & r)) ++c;
        load.waits_counted |= r;
        return LoadStep::Wait;
    };
    if (load.ipkt.pkt.tag > barrier_tag) return wait(kWaitBarrier, mNumBarrierWaits);
    if (mMemDepMode == MemDepMode::StoreSets && !storeResolved_(load.dep_store_seq)) return wait(kWaitStoreSet, mNumStoreSetWaits);

    src = youngestOlderStore_(load);
    // A performed atomic has written the cache, after every older store drained.
    if (src && src->is_atomic && src->sent_to_cache) src = nullptr;
    if (src) {
        const auto& st = src->ranges;
        const auto& ld = load.ranges;
        const bool covers =
            !src->is_atomic && st.size() == 1 && ld.size() == 1 && st[0].addr <= ld[0].addr && st[0].addr + st[0].size >= ld[0].addr + ld[0].size;
        // A partial overlap cannot forward: the load reads the cache after the store writes it.
        if (!covers || !mForwardingEnabled) return wait(kWaitOverlap, mNumOverlapWaits);
        return LoadStep::Forward;
    }
    int sb = 0;
    for (const auto& r : load.ranges) sb = std::max(sb, mDcache->storeBufferOverlap(r.addr, r.size));
    if (sb == 0) return LoadStep::ReadCache;
    // A retired store not yet in the cache: forward a full cover, else wait for the write.
    if (sb == 2 || !mForwardingEnabled || load.ranges.size() != 1) return wait(kWaitOverlap, mNumOverlapWaits);
    return LoadStep::Forward;
}

void LSQ::tryIssueLoads() {
    if (!mDcache) return;

    const uint64_t barrier = oldestBarrierTag_();
    const uint64_t now = getClock()->currentCycle();
    for (uint32_t bank = 0; bank < mNumBanks; ++bank) {
        for (auto& entry : *mLoadQueues[bank]) {
            if (entry.request_sent || entry.forwarded || entry.response_received) continue;
            if (mMmu && !mMmu->dataReady(entry.ipkt.pkt, false)) continue;
            const StoreQueue::Entry* src = nullptr;
            const LoadStep step = entry.next_line == 0 ? orderLoad_(entry, barrier, src) : LoadStep::ReadCache;
            if (step == LoadStep::Wait) continue;
            if (mLoadPortsLeft == 0) {
                ++mNumLoadPortStalls;
                continue;
            }

            if (step == LoadStep::Forward) {
                --mLoadPortsLeft;
                entry.forwarded = true;
                entry.forward_cycles = mForwardingLatency + (src && src->data_ready_cycle > now ? src->data_ready_cycle - now : 0);
                entry.fwd_from_sq = src != nullptr;
                entry.fwd_store_tag = src ? src->ipkt.pkt.tag : 0;
                ++mNumForwards;
                ILOG("[lsq] Store forwarding: load tag=" << entry.ipkt.pkt.tag << " from_sq=" << entry.fwd_from_sq << " store tag=" << entry.fwd_store_tag);
                continue;
            }

            // One D-cache request per line touched; the load completes on the last response.
            if (entry.next_line == 0) entry.req_id = ++mNextReqId;
            while (entry.next_line < entry.lines.size() && mLoadPortsLeft > 0 && mDcache->canAcceptRequest(entry.lines[entry.next_line])) {
                core::MemRequest req;
                req.tag = entry.ipkt.pkt.tag;
                req.req_id = entry.req_id;
                req.pc = entry.ipkt.pkt.pc;
                req.address = entry.next_line == 0 ? entry.address : entry.lines[entry.next_line];
                req.access_size = entry.access_size;
                req.is_store = false;
                request_out.send(req, 0);
                ++entry.next_line;
                ++entry.responses_pending;
                --mLoadPortsLeft;
            }
            entry.request_sent = entry.next_line == entry.lines.size();
        }
    }
}

// The oldest load that read st's bytes before st's address was known is replayed from
// the ROB head; loads that forwarded from a younger covering store read the right data.
void LSQ::checkOrderingViolation_(const StoreQueue::Entry& st) {
    if (mMemDepMode == MemDepMode::None) return;
    LoadQueue::Entry* oldest = nullptr;
    for (auto& lq : mLoadQueues) {
        for (auto& ld : *lq) {
            if (ld.ipkt.pkt.tag <= st.ipkt.pkt.tag) continue;
            if (ld.next_line == 0 && !ld.forwarded && !ld.response_received) continue;
            if (ld.fwd_from_sq && ld.fwd_store_tag > st.ipkt.pkt.tag) continue;
            if (!overlaps(ld.ranges, st.ranges)) continue;
            if (!oldest || ld.ipkt.pkt.tag < oldest->ipkt.pkt.tag) oldest = &ld;
        }
    }
    if (!oldest) return;
    if (!oldest->violation) ++mNumViolations;
    oldest->violation = true;
    if (mMemDepMode == MemDepMode::StoreSets) mStoreSets.train(oldest->ipkt.pkt.pc, st.ipkt.pkt.pc);
    ILOG("[lsq] Ordering violation: load tag=" << oldest->ipkt.pkt.tag << " read before store tag=" << st.ipkt.pkt.tag);
}

bool LSQ::hasOrderingViolation(uint64_t tag) const {
    for (const auto& lq : mLoadQueues)
        if (const auto* e = lq->find(tag)) return e->violation;
    return false;
}

// Every older store, fence and atomic has left the SQ and the store buffer has drained.
bool LSQ::olderStoresDrained_(uint64_t tag) const {
    for (const auto& sq : mStoreQueues)
        for (const auto& e : *sq)
            if (e.ipkt.pkt.tag < tag) return false;
    return mDcache->storeBufferEmpty();
}

void LSQ::finishSqEntry_(StoreQueue::Entry& e) {
    e.sent_to_cache = true;
    auto it =
        std::lower_bound(mCompletionOrder.begin(), mCompletionOrder.end(), e.ipkt.pkt.tag, [](const CompletionEntry& ce, uint64_t t) { return ce.tag < t; });
    if (it != mCompletionOrder.end() && it->tag == e.ipkt.pkt.tag) it->completed = true;
}

// A fence completes once every older memory op has been performed: older loads have their
// data, and older stores have left the SQ and the store buffer.
void LSQ::completeFences_() {
    for (auto& sq : mStoreQueues) {
        for (auto& e : *sq) {
            if (!e.is_fence || e.sent_to_cache) continue;
            const uint64_t tag = e.ipkt.pkt.tag;
            bool older_pending = false;
            for (const auto& [seq, c] : mClaimedNotArrived) older_pending |= c.tag < tag;
            for (const auto& lq : mLoadQueues)
                for (const auto& ld : *lq) older_pending |= ld.ipkt.pkt.tag < tag && !mLoadCompletedTags.count(ld.ipkt.pkt.tag);
            if (older_pending || !olderStoresDrained_(tag)) continue;
            finishSqEntry_(e);
            ++mNumFences;
            if (mVis) mVis->onLsqExit(e.ipkt.pkt.fetch_seq, getClock()->currentCycle());
        }
    }
}

// Atomics are performed non-speculatively at the ROB head, after older stores drain, as a
// load that takes the line exclusive and writes it.
void LSQ::issueAtomics_() {
    for (auto& sq : mStoreQueues) {
        for (auto& e : *sq) {
            if (!e.is_atomic || e.request_sent) continue;
            const bool at_head = mRob ? (!mRob->empty() && mRob->head().fetch_seq == e.ipkt.pkt.fetch_seq)
                                      : (!mCompletionOrder.empty() && mCompletionOrder.front().tag == e.ipkt.pkt.tag);
            if (!at_head || !olderStoresDrained_(e.ipkt.pkt.tag)) continue;
            if (mMmu && !mMmu->dataReady(e.ipkt.pkt, true)) continue;
            if (mLoadPortsLeft == 0) {
                ++mNumLoadPortStalls;
                continue;
            }
            if (!mDcache->canAcceptRequest(e.lines.front())) continue;
            core::MemRequest req;
            req.tag = e.ipkt.pkt.tag;
            req.pc = e.ipkt.pkt.pc;
            req.address = e.address;
            req.access_size = e.access_size;
            req.exclusive = true;
            req.req_id = e.req_id = ++mNextReqId;
            request_out.send(req, 0);
            --mLoadPortsLeft;
            e.request_sent = true;
            e.responses_pending = 1;
        }
    }
}

void LSQ::tryDrainStores() {
    if (!mDcache) return;

    uint32_t ports = mStorePorts;
    // Stores sent this cycle reach the store buffer after this returns, so count its room here.
    uint32_t room = mDcache->storeBufferRoom();
    for (uint32_t bank = 0; bank < mNumBanks; ++bank) {
        while (auto* entry = mStoreQueues[bank]->nextDrainable()) {
            if (room == 0) return;
            if (ports == 0) {
                ++mNumStorePortStalls;
                return;
            }
            --ports;

            while (entry->next_line < entry->lines.size() && room > 0) {
                core::MemRequest req;
                req.tag = entry->ipkt.pkt.tag;
                req.pc = entry->ipkt.pkt.pc;
                req.address = entry->next_line == 0 ? entry->address : entry->lines[entry->next_line];
                req.access_size = entry->access_size;
                req.is_store = true;
                request_out.send(req, 0);
                ++entry->next_line;
                --room;
            }
            if (entry->next_line < entry->lines.size()) return;
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
        if (ce.is_store && !ce.completed) ce.completed = storeTranslated_(ce.tag);

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
            // The LQ entry is only REMOVED once written back and retired.

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
                // Load is in LQ, ROB completion sent, but not yet written back or retired.
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

bool LSQ::storeTranslated_(uint64_t tag) {
    for (auto& sq : mStoreQueues) {
        // Fences and atomics complete when performed, not on translation.
        if (auto* e = sq->find(tag)) return !e->is_fence && !e->is_atomic && (!mMmu || mMmu->dataReady(e->ipkt.pkt, true));
    }
    return true;
}

void LSQ::tick() {
    if (mSsitClearInterval && getClock()->currentCycle() % mSsitClearInterval == 0) mStoreSets.clear();

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

    // Atomics first: one at the ROB head is the oldest memory op.
    mLoadPortsLeft = mLoadPorts;
    if (mDcache) {
        completeFences_();
        issueAtomics_();
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

    // Record occupancy after this cycle's work.
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

void LSQ::claim(const core::DecodePacket& dp) {
    // Rename only calls this after canClaimLoad()/canClaimStore(), so a credit is available.
    const auto& pkt = dp.pkt;
    ClaimInfo c{.tag = pkt.tag, .depth = pkt.wrong_path_depth, .is_store = dp.uop_type != core::UopType::Load, .pc = pkt.pc};
    const bool is_atomic = pkt.inst_class == cpu::InstClass::Atomic;
    const bool is_lr = is_atomic && pkt.inst && pkt.inst->getTraceRecord().assembly.starts_with("lr.");
    // A fence is treated as rw,rw; an atomic with aq set orders every younger load.
    c.barrier = dp.uop_type == core::UopType::Fence || (is_atomic && pkt.inst && ((pkt.inst->getTraceRecord().inst >> 26) & 1));
    if (mMemDepMode == MemDepMode::StoreSets) {
        if (!c.is_store)
            c.dep_seq = mStoreSets.dispatchLoad(pkt.pc);
        else if (dp.uop_type == core::UopType::Store && !is_lr)
            c.dep_seq = mStoreSets.dispatchStore(pkt.pc, pkt.fetch_seq);
    }
    --(c.is_store ? mStoreCredits : mLoadCredits);
    mClaimedNotArrived[pkt.fetch_seq] = c;
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

void LSQ::commitLoad(uint64_t tag) {
    for (auto& lq : mLoadQueues) {
        if (auto* entry = lq->find(tag)) {
            entry->committed = true;
            return;
        }
    }
}

}  // namespace midcore
