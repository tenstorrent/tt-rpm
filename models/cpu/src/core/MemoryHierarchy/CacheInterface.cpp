#include "MemoryHierarchy/CacheInterface.hpp"

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
      mWritePolicy(cfg.write_policy),
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

void CacheInterface::setEvictionCallback(EvictionCb cb) { mEvictionCb = std::move(cb); }

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
        if (!m.active) return &m;
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

bool CacheInterface::isStoreBufferFull() const { return mStoreBuffer.size() >= mStoreBufferCapacity; }

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

bool CacheInterface::loadRequest(uint64_t token, address_t addr, uint64_t cycle) {
    // Check bank availability for read port
    if (!tryConsumeReadPort(addr)) {
        if (mTracer) {
            // Record bank conflict event
        }
        return false;
    }

    // Tag-array hit
    auto* line = mTagArray->getLine(addr);
    if (line) {
        mTagArray->touch(*line);
        ++mNumHits;

        uint32_t req_id = 0;
        if (mTracer) {
            req_id = mTracer->newRequest(cycle, token, addr);
            uint32_t set = mTagArray->setIndex(addr);
            uint32_t way = mTagArray->getLineWay(addr, line);
            mTracer->recordHit(req_id, cycle, set, way);
        }
        mPendingHits.push_back({token, mHitLatency, req_id});
        return true;
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
        mshr->waiters.push_back({token, req_id});
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
    mshr->waiters = {{token, req_id}};
    mshr->fill_pending = mExternalFill;
    mMSHRMap[line_addr] = mshrIndex(mshr);
    ++mNumMisses;

    if (mExternalFill) {
        if (mFillRequestCb) mFillRequestCb(line_addr);
    } else {
        mshr->fill_cycles_remaining = mHitLatency + mMissLatency;
    }

    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Store request
// ─────────────────────────────────────────────────────────────────────────────

bool CacheInterface::storeRequest(uint64_t token, address_t addr, uint8_t size, uint64_t cycle) {
    if (mStoreBuffer.size() >= mStoreBufferCapacity) {
        return false;  // store buffer full
    }

    uint32_t req_id = 0;
    if (mTracer) {
        req_id = mTracer->newRequest(cycle, token, addr);
        // Record store-buffer enqueue event (could add a new event type)
    }

    mStoreBuffer.push_back({addr, size, token, req_id});
    ++mNumStoreBufferHits;  // stores always "hit" from the pipeline's perspective
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Legacy request (backward compatibility)
// ─────────────────────────────────────────────────────────────────────────────

void CacheInterface::request(uint64_t token, address_t addr, uint64_t cycle) {
    // Legacy call — treat as a load, ignore return value (old behavior)
    loadRequest(token, addr, cycle);
}

// ─────────────────────────────────────────────────────────────────────────────
// Fill complete (external fill mode)
// ─────────────────────────────────────────────────────────────────────────────

void CacheInterface::fillComplete(address_t line_addr, uint64_t cycle) {
    MSHREntry* mshr = findMSHR(lineAddr(line_addr));
    sparta_assert(mshr && mshr->active && mshr->fill_pending, "CacheInterface::fillComplete — no matching pending MSHR");

    if (mTracer) {
        mTracer->recordFillStart(cycle, mshrIndex(mshr), lineAddr(line_addr));
    }
    mshr->fill_pending = false;
    mshr->fill_cycles_remaining = mHitLatency;
}

// ─────────────────────────────────────────────────────────────────────────────
// Invalidate line (for inclusive L2 back-invalidation)
// ─────────────────────────────────────────────────────────────────────────────

void CacheInterface::invalidateLine(address_t addr, uint64_t cycle) {
    auto* line = mTagArray->getLine(addr);
    if (!line) return;

    // Record eviction info before invalidating
    if (mTracer) {
        uint32_t set = mTagArray->setIndex(addr);
        uint32_t way = mTagArray->getLineWay(addr, line);
        mTracer->recordFillDone(cycle, UINT32_MAX, addr, set, way, true, addr);
    }

    line->invalidate();
}

// ─────────────────────────────────────────────────────────────────────────────
// Tick — advance all pipelines
// ─────────────────────────────────────────────────────────────────────────────

void CacheInterface::tick(uint64_t cycle) {
    // Reset bank state at the start of each cycle
    resetBankState();

    // Drain store buffer (one entry per cycle per bank's write port)
    drainStoreBuffer(cycle);

    // Advance MSHR fills
    advanceMSHRs(cycle);

    // Advance hit pipeline
    advanceHitPipeline(cycle);
}

void CacheInterface::drainStoreBuffer(uint64_t cycle) {
    if (mStoreBuffer.empty()) return;

    auto& sb = mStoreBuffer.front();

    // Check write port availability
    if (!tryConsumeWritePort(sb.addr)) {
        return;  // bank conflict, retry next cycle
    }

    // Perform the store
    address_t line_addr = lineAddr(sb.addr);
    auto* line = mTagArray->getLine(sb.addr);

    if (line) {
        // Write hit
        mTagArray->touch(*line);
        if (mWritePolicy == WritePolicy::WRITE_BACK) {
            line->setModified(true);
        }
        // Write-through: would fire mWritebackCb here for L2 write
    } else {
        // Write miss
        if (mWritePolicy == WritePolicy::WRITE_BACK) {
            // Write-allocate: need to fetch the line first
            // Check if MSHR already exists for this line
            MSHREntry* mshr = findMSHR(line_addr);
            if (!mshr) {
                // Allocate new MSHR for write-allocate
                mshr = allocMSHR();
                if (!mshr) {
                    // No MSHR available, store stays in buffer
                    return;
                }
                mshr->active = true;
                mshr->line_addr = line_addr;
                mshr->fill_pending = mExternalFill;
                mMSHRMap[line_addr] = mshrIndex(mshr);
                ++mNumMisses;

                if (mExternalFill) {
                    if (mFillRequestCb) mFillRequestCb(line_addr);
                } else {
                    mshr->fill_cycles_remaining = mHitLatency + mMissLatency;
                }
            }
            // The store will be replayed when the fill completes
            // For simplicity, we still dequeue it now (line will be dirty when filled)
        }
        // Write-through + no-write-allocate: just forward to L2 (not modeled here)
    }

    if (mTracer) {
        mTracer->recordResponse(sb.req_id, cycle, /*miss=*/false);
    }

    mStoreBuffer.pop_front();
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

            // Get eviction info before allocate
            uint32_t set_idx = mTagArray->setIndex(mshr.line_addr);
            uint32_t way_idx = mTagArray->getVictimWay(mshr.line_addr);
            auto& victim = mTagArray->getLineForReplacement(mshr.line_addr);
            bool evict_valid = victim.isValid();
            address_t evict_addr = victim.getAddress();
            bool evict_dirty = victim.isModified();

            // Fire eviction/writeback callbacks before replacing
            if (evict_valid) {
                if (mEvictionCb) {
                    mEvictionCb(evict_addr, evict_dirty);
                }
                if (evict_dirty && mWritebackCb) {
                    mWritebackCb(evict_addr);
                }
            }

            mTagArray->allocate(victim, mshr.line_addr);

            if (mTracer) {
                mTracer->recordFillDone(cycle, mshr_id, mshr.line_addr, set_idx, way_idx, evict_valid, evict_addr);
            }

            // Fire completion callbacks or queue completions for all waiters
            for (const auto& w : mshr.waiters) {
                if (mTracer) mTracer->recordResponse(w.req_id, cycle, /*miss=*/true);
                if (mCallback) {
                    mCallback(w.token, /*miss=*/true);
                } else {
                    mCompletions.push_back({w.token, /*miss=*/true});
                }
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
            mCallback(h.token, /*miss=*/false);
        } else {
            mCompletions.push_back({h.token, /*miss=*/false});
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
