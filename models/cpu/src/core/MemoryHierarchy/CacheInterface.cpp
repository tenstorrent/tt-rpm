// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Tenstorrent USA, Inc.

#include "MemoryHierarchy/CacheInterface.hpp"

#include <cstring>

#include "sparta/utils/MathUtils.hpp"
#include "sparta/utils/SpartaAssert.hpp"

namespace cpu {

// ─────────────────────────────────────────────────────────────────────────────
// Construction
// ─────────────────────────────────────────────────────────────────────────────

CacheInterface::CacheInterface(const Config& cfg, Callback cb)
    : mCallback(std::move(cb)),
      mHitLatency(cfg.hit_latency),
      mMissLatency(cfg.miss_latency),
      mExternalFill(cfg.external_fill),
      mUncacheableBase(cfg.uncacheable_base),
      mUncacheableSize(cfg.uncacheable_size),
      mStoreBufferCapacity(cfg.store_buffer_capacity),
      mNumBanks(cfg.num_banks),
      mReadsPerBank(cfg.reads_per_bank_per_cycle),
      mWritesPerBank(cfg.writes_per_bank_per_cycle),
      mFillOccupiesWritePort(cfg.fill_occupies_write_port),
      mTagArray(std::make_unique<SimpleCache>(cfg.cache)),
      mLineSize(cfg.cache.lineSize),
      mLineShift(sparta::utils::floor_log2(cfg.cache.lineSize)),
      mMSHRs(cfg.mshr_capacity),
      mBankState(cfg.num_banks) {}

// ─────────────────────────────────────────────────────────────────────────────
// Callback setters
// ─────────────────────────────────────────────────────────────────────────────

void CacheInterface::setFillRequestCallback(FillRequestCb cb) { mFillRequestCb = std::move(cb); }
void CacheInterface::setUpgradeRequestCallback(UpgradeRequestCb cb) { mUpgradeRequestCb = std::move(cb); }

void CacheInterface::setEvictionCallback(EvictionCb cb) { mEvictionCb = std::move(cb); }
void CacheInterface::setStoreDrainCallback(StoreDrainCb cb) { mStoreDrainCb = std::move(cb); }
void CacheInterface::setFillDoneCallback(FillDoneCb cb) { mFillDoneCb = std::move(cb); }

void CacheInterface::setWritebackCallback(WritebackCb cb) { mWritebackCb = std::move(cb); }

void CacheInterface::setTracer(CacheTracer* t) { mTracer = t; }

// ─────────────────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────────────────

address_t CacheInterface::lineAddr(address_t addr) const noexcept { return addr & ~(mLineSize - 1); }

uint32_t CacheInterface::bankIndex(address_t addr) const noexcept {
    if (mNumBanks <= 1) return 0;
    return static_cast<uint32_t>((addr >> mLineShift) & (mNumBanks - 1));
}

CacheInterface::MSHREntry* CacheInterface::findMSHR(address_t line_addr) {
    auto it = mMSHRMap.find(line_addr);
    if (it != mMSHRMap.end()) return &mMSHRs[it->second];
    return nullptr;
}

const CacheInterface::MSHREntry* CacheInterface::findMSHRConst(address_t line_addr) const {
    auto it = mMSHRMap.find(line_addr);
    if (it != mMSHRMap.end()) return &mMSHRs[it->second];
    return nullptr;
}

CacheInterface::MSHREntry* CacheInterface::allocMSHR() {
    for (auto& m : mMSHRs)
        if (!m.active) {
            m.invalidated = false;  // fresh transaction: clear any stale snoop-during-fill mark
            m.prefetch = false;
            m.dirty_on_fill = false;
            m.exclusive = m.unique = false;
            return &m;
        }
    return nullptr;
}

uint32_t CacheInterface::mshrIndex(const MSHREntry* m) const noexcept { return static_cast<uint32_t>(m - mMSHRs.data()); }

bool CacheInterface::tryConsumeReadPort(address_t addr) {
    if (mNumBanks <= 1) return true;  // no banking → always succeed
    uint32_t bank = bankIndex(addr);
    if (mBankState[bank].reads_used >= mReadsPerBank) {
        ++mNumBankConflicts;
        return false;
    }
    ++mBankState[bank].reads_used;
    return true;
}

bool CacheInterface::tryConsumeWritePort(address_t addr) {
    if (mNumBanks <= 1) return true;
    uint32_t bank = bankIndex(addr);
    if (mBankState[bank].writes_used >= mWritesPerBank) {
        ++mNumBankConflicts;
        return false;
    }
    ++mBankState[bank].writes_used;
    return true;
}

void CacheInterface::resetBankState() {
    for (auto& bs : mBankState) {
        bs.reads_used = 0;
        bs.writes_used = 0;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Backpressure queries
// ─────────────────────────────────────────────────────────────────────────────

bool CacheInterface::canAcceptRequest(address_t addr) const {
    address_t line = lineAddr(addr);
    if (mMSHRMap.count(line)) return true;   // can coalesce
    return mMSHRMap.size() < mMSHRs.size();  // free MSHR slot available
}

bool CacheInterface::willNeedFill(address_t addr) const {
    if (mTagArray->getLine(addr)) return false;  // tag-array hit
    return findMSHRConst(lineAddr(addr)) == nullptr;
}

CacheInterface::Probe CacheInterface::probe(address_t addr) const {
    Probe p;
    if (const auto* line = mTagArray->peekLine(addr)) {
        p.hit = true;
        p.prefetched = line->isPrefetched();
    } else if (const auto* m = findMSHRConst(lineAddr(addr))) {
        p.in_flight = true;
        p.prefetched = m->prefetch;
    }
    return p;
}

uint32_t CacheInterface::pollutionIndex(address_t line_addr) const noexcept {
    const uint64_t blk = line_addr >> mLineShift;
    return static_cast<uint32_t>((blk ^ (blk >> 12)) & (mPollutionFilter.size() - 1));
}

void CacheInterface::demandMiss(address_t line_addr) {
    ++mNumMisses;
    if (mPollutionFilter[pollutionIndex(line_addr)]) ++mNumPollutionMisses;
}

// CleanUnique for a line held shared; fillComplete() delivers the grant.
void CacheInterface::requestOwnership(MSHREntry& mshr) {
    sparta_assert(mExternalFill, "CacheInterface: a line without a next level is always unique");
    mshr.fill_pending = true;
    ++mNumUpgrades;
    if (mUpgradeRequestCb) mUpgradeRequestCb(mshr.line_addr);
}

void CacheInterface::claimPrefetch(MSHREntry& mshr) {
    if (!mshr.prefetch) return;
    ++mNumLatePrefetches;
    mshr.prefetch = false;
}

int CacheInterface::storeBufferOverlap(address_t addr, uint32_t size) const {
    int result = 0;
    for (auto it = mStoreBuffer.rbegin(); it != mStoreBuffer.rend(); ++it) {
        if (it->addr >= addr + size || addr >= it->addr + it->size) continue;
        if (it->addr <= addr && it->addr + it->size >= addr + size) return result == 0 ? 1 : 2;
        result = 2;
    }
    return result;
}

// ─────────────────────────────────────────────────────────────────────────────
// Store buffer forwarding
// ─────────────────────────────────────────────────────────────────────────────

bool CacheInterface::checkStoreBufferForward(address_t addr, uint8_t size) const {
    // Search backwards (most recent store first) for a store that fully covers
    // the load range [addr, addr + size).
    for (auto it = mStoreBuffer.rbegin(); it != mStoreBuffer.rend(); ++it) {
        if (it->addr <= addr && (it->addr + it->size) >= (addr + size)) {
            return true;
        }
    }
    return false;
}

// ─────────────────────────────────────────────────────────────────────────────
// Load request
// ─────────────────────────────────────────────────────────────────────────────

bool CacheInterface::loadRequest(uint64_t token, address_t addr, uint64_t cycle, bool exclusive, bool demand) {
    // Check bank availability for read port
    if (!tryConsumeReadPort(addr)) {
        return false;
    }

    // Tag-array hit. A write to a line held shared waits in an MSHR for ownership instead.
    auto* line = mTagArray->getLine(addr);
    const bool needs_ownership = line && exclusive && !line->isUnique();
    if (line && !needs_ownership) {
        mTagArray->touch(*line);
        ++mNumHits;
        if (line->isPrefetched()) {  // demand hit on a prefetched line -> useful
            ++(demand ? mNumUsefulPrefetches : mNumNonDemandPrefetchHits);
            line->setPrefetched(false);
        }

        // Self-check: on a hit the shadow line should still match Whisper. If it does
        // not, this core read a STALE cached line — a coherence violation the directory should
        // have prevented (with coherence off, no invalidation refreshed it). Whisper remains the
        // architectural oracle; this only counts the hazard.
        if (mDataCarrying && mPeekFn) {
            ++mShadowChecks;
            uint8_t whis[kMaxLineBytes];
            mPeekFn(lineAddr(addr), whis, kMaxLineBytes);
            if (std::memcmp(whis, line->data(), kMaxLineBytes) != 0) ++mShadowMismatches;
        }

        if (exclusive) {
            line->setModified(true);
            // As a store drain does: re-seed the writer's copy from Whisper.
            if (mDataCarrying && mPeekFn) {
                uint8_t b[kMaxLineBytes];
                mPeekFn(lineAddr(addr), b, kMaxLineBytes);
                line->seedData(b);
            }
        }

        uint32_t req_id = 0;
        if (mTracer) {
            req_id = mTracer->newRequest(cycle, token, addr);
            uint32_t set = mTagArray->setIndex(addr);
            uint32_t way = mTagArray->getLineWay(addr, line);
            mTracer->recordHit(req_id, cycle, set, way);
        }
        mPendingHits.push_back({token, mHitLatency, req_id, lineAddr(addr)});
        return true;
    }

    if (needs_ownership) {
        mTagArray->touch(*line);
        if (line->isPrefetched()) {
            ++(demand ? mNumUsefulPrefetches : mNumNonDemandPrefetchHits);
            line->setPrefetched(false);
        }
    }

    // Miss: check for coalescing
    address_t line_addr = lineAddr(addr);
    MSHREntry* mshr = findMSHR(line_addr);
    if (mshr) {
        uint32_t mshr_id = mshrIndex(mshr);
        uint32_t req_id = 0;
        if (mTracer) {
            req_id = mTracer->newRequest(cycle, token, addr);
            mTracer->recordMiss(req_id, cycle, mTagArray->setIndex(addr));
            mTracer->recordCoalesce(req_id, cycle, mshr_id);
        }
        if (demand) {
            claimPrefetch(*mshr);
        } else if (mshr->prefetch) {
            ++mNumNonDemandPrefetchHits;
            mshr->prefetch = false;
        }
        mshr->waiters.push_back({token, req_id, exclusive});
        mshr->exclusive |= exclusive;
        ++mNumCoalesced;
        return true;
    }

    // New miss: allocate MSHR
    mshr = allocMSHR();
    sparta_assert(mshr != nullptr, "CacheInterface::loadRequest — no free MSHR; caller must check canAcceptRequest()");

    uint32_t mshr_id = mshrIndex(mshr);
    uint32_t req_id = 0;
    if (mTracer) {
        req_id = mTracer->newRequest(cycle, token, addr);
        mTracer->recordMiss(req_id, cycle, mTagArray->setIndex(addr));
        mTracer->recordMshrAlloc(req_id, cycle, mshr_id, line_addr);
    }

    mshr->active = true;
    mshr->line_addr = line_addr;
    mshr->waiters = {{token, req_id, exclusive}};
    mshr->fill_pending = mExternalFill;
    mshr->exclusive = exclusive;
    mshr->unique = !mExternalFill;
    mMSHRMap[line_addr] = mshrIndex(mshr);
    if (needs_ownership) {
        requestOwnership(*mshr);
        return true;
    }
    if (demand) demandMiss(line_addr);

    if (mExternalFill) {
        if (mFillRequestCb) mFillRequestCb(line_addr, /*write_allocate=*/exclusive, /*prefetch=*/false);  // load/ifetch miss, or atomic RFO
    } else {
        mshr->fill_cycles_remaining = mHitLatency + mMissLatency;
    }

    return true;
}

CacheInterface::PrefetchOutcome CacheInterface::prefetchRequest(address_t addr, uint64_t cycle, uint32_t demand_reserve) {
    (void)cycle;
    if (isUncacheable(addr) || mTagArray->getLine(addr)) return PrefetchOutcome::Filtered;

    address_t line_addr = lineAddr(addr);
    if (findMSHR(line_addr)) return PrefetchOutcome::Filtered;  // already in flight (demand or prefetch)

    if (mMSHRMap.size() + demand_reserve >= mMSHRs.size()) return PrefetchOutcome::NoMshr;
    MSHREntry* mshr = allocMSHR();

    mshr->active = true;
    mshr->line_addr = line_addr;
    mshr->waiters.clear();  // no waiter -> fill installs the line but completes to no one
    mshr->prefetch = true;
    mshr->fill_pending = mExternalFill;
    mshr->unique = !mExternalFill;
    mMSHRMap[line_addr] = mshrIndex(mshr);
    ++mNumPrefetches;

    if (mExternalFill) {
        if (mFillRequestCb) mFillRequestCb(line_addr, /*write_allocate=*/false, /*prefetch=*/true);  // prefetch is read-allocate
    } else {
        mshr->fill_cycles_remaining = mHitLatency + mMissLatency;
    }
    return PrefetchOutcome::Issued;
}

// ─────────────────────────────────────────────────────────────────────────────
// Store request
// ─────────────────────────────────────────────────────────────────────────────

bool CacheInterface::storeRequest(uint64_t token, address_t addr, address_t pc, uint8_t size, uint64_t cycle) {
    if (mStoreBuffer.size() >= mStoreBufferCapacity) {
        return false;  // store buffer full
    }

    uint32_t req_id = 0;
    if (mTracer) {
        req_id = mTracer->newRequest(cycle, token, addr);
    }

    mStoreBuffer.push_back({addr, pc, size, token, req_id});
    ++mNumStoreBufferHits;  // stores always "hit" from the pipeline's perspective
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Fill complete (external fill mode)
// ─────────────────────────────────────────────────────────────────────────────

void CacheInterface::fillComplete(address_t line_addr, uint64_t cycle, bool unique) {
    MSHREntry* mshr = findMSHR(lineAddr(line_addr));
    sparta_assert(mshr && mshr->active && mshr->fill_pending, "CacheInterface::fillComplete — no matching pending MSHR");

    if (mTracer) {
        mTracer->recordFillStart(cycle, mshrIndex(mshr), lineAddr(line_addr));
    }
    mshr->fill_pending = false;
    mshr->unique = unique;
    mshr->fill_cycles_remaining = mHitLatency;
}

// ─────────────────────────────────────────────────────────────────────────────
// Invalidate line (for inclusive L2 back-invalidation)
// ─────────────────────────────────────────────────────────────────────────────

void CacheInterface::invalidateLine(address_t addr, uint64_t cycle) {
    // A fill or ownership grant in flight raced the snoop. If we do nothing, it installs a line
    // that should have been invalidated -> the snoop is lost and the core keeps a stale or
    // wrongly-owned copy. Mark the MSHR so it does not retain the line; the next access re-misses.
    if (MSHREntry* mshr = findMSHR(lineAddr(addr))) mshr->invalidated = true;
    auto* line = mTagArray->getLine(addr);
    if (!line) return;

    // Record eviction info before invalidating
    if (mTracer) {
        uint32_t set = mTagArray->setIndex(addr);
        uint32_t way = mTagArray->getLineWay(addr, line);
        mTracer->recordFillDone(cycle, UINT32_MAX, addr, set, way, true, addr);
    }

    if (line->isPrefetched()) ++mNumUselessPrefetches;
    if (line->isModified() && mWritebackCb) mWritebackCb(line->getAddress());  // the only copy of the data
    line->invalidate();
}

// ─────────────────────────────────────────────────────────────────────────────
// Tick — advance all pipelines
// ─────────────────────────────────────────────────────────────────────────────

void CacheInterface::tick(uint64_t cycle) {
    // Reset bank state at the start of each cycle
    resetBankState();

    drainStoreBuffer(cycle);

    // Advance MSHR fills
    advanceMSHRs(cycle);

    // Advance hit pipeline
    advanceHitPipeline(cycle);
}

// In order, as many stores as the write ports take: writes_per_bank_per_cycle unbanked, else per bank.
void CacheInterface::drainStoreBuffer(uint64_t cycle) {
    for (uint32_t n = 0; !mStoreBuffer.empty() && (mNumBanks > 1 || n < mWritesPerBank); ++n) {
        if (!drainOneStore(cycle)) return;
    }
}

bool CacheInterface::drainOneStore(uint64_t cycle) {
    auto& sb = mStoreBuffer.front();

    // Check write port availability
    if (!tryConsumeWritePort(sb.addr)) {
        return false;  // bank conflict, retry next cycle
    }

    // Perform the store
    address_t line_addr = lineAddr(sb.addr);

    // Uncacheable MMIO (accelerator BAR): never allocate, never mark dirty — forward every
    // store downstream as an RFO. Repetition matters: a cached M line would suppress all
    // downstream traffic, and a doorbell is exactly repeated stores to the same address.
    if (isUncacheable(sb.addr)) {
        ++mNumMmioStores;
        if (mFillRequestCb) mFillRequestCb(line_addr, /*write_allocate=*/true, /*prefetch=*/false);
        if (mStoreDrainCb) mStoreDrainCb(sb.addr, sb.pc, /*miss=*/true);
        mStoreBuffer.pop_front();
        return true;
    }

    auto* line = mTagArray->getLine(sb.addr);
    const bool store_miss = (line == nullptr);

    if (line) {
        mTagArray->touch(*line);
        if (line->isPrefetched()) {  // demand store hit on a prefetched line -> useful
            ++mNumUsefulPrefetches;
            line->setPrefetched(false);
        }
    }
    if (line && line->isUnique()) {
        // Write hit
        line->setModified(true);
        // Store-merge (simplified): re-seed the shadow line from Whisper, which now holds the
        // committed store value, so the writer's own cached copy stays fresh.
        if (mDataCarrying && mPeekFn) {
            uint8_t b[kMaxLineBytes];
            mPeekFn(line_addr, b, kMaxLineBytes);
            line->seedData(b);
        }
    } else {
        // Write miss, or a write to a line held shared: the store waits in an MSHR for the line or ownership
        MSHREntry* mshr = findMSHR(line_addr);
        if (mshr) {
            claimPrefetch(*mshr);
        } else {
            mshr = allocMSHR();
            if (!mshr) {
                // No MSHR available, store stays in buffer
                return false;
            }
            mshr->active = true;
            mshr->line_addr = line_addr;
            mshr->fill_pending = mExternalFill;
            mshr->unique = !mExternalFill;
            mMSHRMap[line_addr] = mshrIndex(mshr);

            if (line) {
                requestOwnership(*mshr);
            } else {
                demandMiss(line_addr);
                if (mExternalFill) {
                    if (mFillRequestCb) mFillRequestCb(line_addr, /*write_allocate=*/true, /*prefetch=*/false);  // store miss (RFO)
                } else {
                    mshr->fill_cycles_remaining = mHitLatency + mMissLatency;
                }
            }
        }
        // The store leaves the buffer now; the fill or ownership grant installs the line dirty.
        mshr->dirty_on_fill = true;
    }

    if (mTracer) {
        mTracer->recordResponse(sb.req_id, cycle, /*miss=*/false);
    }

    if (mStoreDrainCb) mStoreDrainCb(sb.addr, sb.pc, store_miss);

    mStoreBuffer.pop_front();
    return true;
}

void CacheInterface::advanceMSHRs(uint64_t cycle) {
    for (auto& mshr : mMSHRs) {
        if (!mshr.active || mshr.fill_pending) continue;
        if (mshr.fill_cycles_remaining > 0) --mshr.fill_cycles_remaining;
        if (mshr.fill_cycles_remaining == 0) {
            // Check if fill can consume a write port (if configured)
            if (mFillOccupiesWritePort && !tryConsumeWritePort(mshr.line_addr)) {
                // Defer fill to next cycle
                mshr.fill_cycles_remaining = 1;
                continue;
            }

            uint32_t mshr_id = mshrIndex(&mshr);

            // An ownership grant usually finds its line still resident: nothing to evict.
            CacheLine* held = mTagArray->getLine(mshr.line_addr);
            const bool fresh = held == nullptr;
            // Get eviction info before allocate
            uint32_t set_idx = mTagArray->setIndex(mshr.line_addr);
            uint32_t way_idx = fresh ? mTagArray->getVictimWay(mshr.line_addr) : mTagArray->getLineWay(mshr.line_addr, held);
            auto& victim = fresh ? mTagArray->getLineForReplacement(mshr.line_addr) : *held;
            bool evict_valid = fresh && victim.isValid();
            address_t evict_addr = victim.getAddress();
            bool evict_dirty = victim.isModified();
            if (evict_valid && evict_dirty && mWritebackGate && !mWritebackGate()) {
                mshr.fill_cycles_remaining = 1;
                continue;
            }

            // Fire eviction/writeback callbacks before replacing
            if (evict_valid) {
                if (victim.isPrefetched()) {
                    ++mNumUselessPrefetches;
                } else if (mshr.prefetch) {
                    mPollutionFilter[pollutionIndex(evict_addr)] = true;
                }
                if (mEvictionCb) {
                    mEvictionCb(evict_addr, evict_dirty);
                }
                if (evict_dirty && mWritebackCb) {
                    mWritebackCb(evict_addr);
                }
            }

            if (fresh) mTagArray->allocate(victim, mshr.line_addr);

            // A store or atomic waiting here writes the line only once it is granted E or M.
            const bool writes = mshr.exclusive || mshr.dirty_on_fill;
            const bool await_ownership = writes && !mshr.unique;
            victim.setUnique(mshr.unique);
            if (writes && mshr.unique) victim.setModified(true);
            // Fill seeds the shadow line from Whisper (the oracle) at the moment of fill.
            if (mDataCarrying && mPeekFn) {
                uint8_t b[kMaxLineBytes];
                mPeekFn(mshr.line_addr, b, kMaxLineBytes);
                victim.seedData(b);
            }

            // A coherence snoop invalidated this line while the fill was outstanding: satisfy the
            // in-flight waiters (their value comes from Whisper regardless) but do NOT retain the
            // line, so the next access re-misses and re-fetches a coherent copy.
            if (mshr.invalidated) victim.invalidate();

            // A prefetch no demand load coalesced onto: tag the line so a later demand
            // hit counts as a useful prefetch.
            const bool was_prefetch = mshr.prefetch;
            if (was_prefetch) mPollutionFilter[pollutionIndex(mshr.line_addr)] = false;
            victim.setPrefetched(was_prefetch);
            if (fresh && mFillDoneCb) mFillDoneCb(mshr.line_addr, was_prefetch);

            if (mTracer) {
                mTracer->recordFillDone(cycle, mshr_id, mshr.line_addr, set_idx, way_idx, evict_valid, evict_addr);
            }

            // Fire completion callbacks or queue completions for all waiters
            std::vector<Waiter> writers;
            for (const auto& w : mshr.waiters) {
                if (await_ownership && w.exclusive) {
                    writers.push_back(w);
                    continue;
                }
                if (mTracer) mTracer->recordResponse(w.req_id, cycle, /*miss=*/true);
                if (mCallback) {
                    mCallback(w.token, mshr.line_addr, /*miss=*/true);
                } else {
                    mCompletions.push_back({w.token, mshr.line_addr, /*miss=*/true});
                }
            }
            // Installed shared: the readers are done; the writers keep this MSHR for the upgrade.
            if (await_ownership) {
                mshr.waiters = std::move(writers);
                mshr.invalidated = mshr.prefetch = false;
                requestOwnership(mshr);
                continue;
            }

            mMSHRMap.erase(mshr.line_addr);
            mshr.active = false;
            mshr.waiters.clear();
        }
    }
}

void CacheInterface::advanceHitPipeline(uint64_t cycle) {
    for (auto& h : mPendingHits) {
        if (h.cycles_remaining > 0) --h.cycles_remaining;
    }

    while (!mPendingHits.empty() && mPendingHits.front().cycles_remaining == 0) {
        const auto& h = mPendingHits.front();
        if (mTracer) mTracer->recordResponse(h.req_id, cycle, /*miss=*/false);
        if (mCallback) {
            mCallback(h.token, h.line_addr, /*miss=*/false);
        } else {
            mCompletions.push_back({h.token, h.line_addr, /*miss=*/false});
        }
        mPendingHits.pop_front();
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Completion Queue
// ─────────────────────────────────────────────────────────────────────────────

std::vector<CacheInterface::CompletionEntry> CacheInterface::drainCompletions() {
    std::vector<CompletionEntry> result;
    std::swap(result, mCompletions);
    return result;
}

// ─────────────────────────────────────────────────────────────────────────────
// Stats
// ─────────────────────────────────────────────────────────────────────────────

uint32_t CacheInterface::outstandingMisses() const { return static_cast<uint32_t>(mMSHRMap.size()); }

}  // namespace cpu
