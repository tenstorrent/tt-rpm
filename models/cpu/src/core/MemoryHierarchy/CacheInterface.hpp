// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Tenstorrent USA, Inc.

#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <unordered_map>
#include <vector>

#include "MemoryHierarchy/Cache.hpp"
#include "MemoryHierarchy/CacheTracer.hpp"

namespace cpu {

// ─────────────────────────────────────────────────────────────────────────────
// CacheInterface — MSHR controller wrapping a SimpleCache tag array.
//
// Features:
//  1. Proper fill timing — lines installed only when fill completes.
//  2. Miss coalescing — multiple requests to the same line share one MSHR.
//  3. Non-blocking — hits bypass outstanding misses (hit-under-miss).
//  4. Store buffer — absorbs stores, enables store-to-load forwarding.
//  5. Banking — per-bank port limits for realistic contention modeling.
//  6. Write-back, write-allocate.
//
// Fill modes:
//  • Countdown (external_fill=false): MSHR counts down hit_lat + miss_lat.
//  • External (external_fill=true): waits for fillComplete(), then hit_lat.
// ─────────────────────────────────────────────────────────────────────────────

class CacheInterface {
   public:
    struct Config {
        uint32_t mshr_capacity{4};
        uint32_t hit_latency{1};
        uint32_t miss_latency{10};
        bool external_fill{false};

        // Store buffer
        uint32_t store_buffer_capacity{8};

        // Banking (1 = single-ported, no banking)
        uint32_t num_banks{1};
        uint32_t reads_per_bank_per_cycle{1};
        uint32_t writes_per_bank_per_cycle{1};
        bool fill_occupies_write_port{false};

        // Uncacheable MMIO range (e.g. an accelerator BAR): size 0 = disabled. Stores into
        // the range bypass the tag array entirely and are forwarded downstream every time.
        uint64_t uncacheable_base{0};
        uint64_t uncacheable_size{0};

        SimpleCache::Config cache;
    };

    // Completion entry for queue-based completion model
    struct CompletionEntry {
        uint64_t token;
        address_t line_addr;
        bool miss;
    };

    // Completion callback: (token, line_addr, was_miss) - optional, if null uses completion
    // queue. line_addr is delivered explicitly so callers never recover it from the token.
    using Callback = std::function<void(uint64_t token, address_t line_addr, bool miss)>;
    // Fill request callback: fired when new MSHR allocated (external_fill mode). write_allocate
    // distinguishes a store-miss fill (wants write permission) from a load/ifetch-miss fill, so
    // the caller can label the outbound request (read vs write intent); prefetch marks a prefetch's fill.
    using FillRequestCb = std::function<void(address_t line_addr, bool write_allocate, bool prefetch)>;
    // Upgrade request callback: a store or atomic needs ownership of a line granted shared. The
    // write waits in an MSHR until fillComplete() grants it; a line granted E or M never asks.
    using UpgradeRequestCb = std::function<void(address_t line_addr)>;
    // Eviction callback: fired when a line is evicted (for inclusivity/writeback)
    using EvictionCb = std::function<void(address_t line_addr, bool dirty)>;
    // Writeback callback: fired when dirty line evicted (write-back mode)
    using WritebackCb = std::function<void(address_t line_addr)>;
    // Store-drain callback: fired when a buffered store actually accesses the
    // cache (drains), carrying its true hit/miss. Lets observers see the store's
    // real cache outcome rather than its store-buffer-insert state.
    using StoreDrainCb = std::function<void(address_t addr, address_t pc, bool miss)>;
    // Fill-done callback: fired when a line is installed; prefetched=true if the
    // fill was triggered by a prefetch rather than a demand miss.
    using FillDoneCb = std::function<void(address_t line_addr, bool prefetched)>;
    // Writeback gate: a fill that would evict a dirty line waits until this returns true.
    using WritebackGateCb = std::function<bool()>;

    // Constructor - callback is optional (if null, use drainCompletions() instead)
    CacheInterface(const Config& cfg, Callback cb = nullptr);
    ~CacheInterface() = default;

    // ── Callback setters ─────────────────────────────────────────────────────
    void setFillRequestCallback(FillRequestCb cb);
    void setUpgradeRequestCallback(UpgradeRequestCb cb);

    // Data-carrying shadow. PeekFn reads `len` bytes of physical
    // memory at `addr` from Whisper (the oracle). When enabled: fill seeds the shadow line from
    // Whisper, a store re-seeds its line, and a load HIT compares the shadow line vs Whisper and
    // counts mismatches (stale-line reads that coherence should have prevented).
    using PeekFn = std::function<void(address_t addr, uint8_t* buf, uint32_t len)>;
    void setDataCarrying(bool on, PeekFn peek) {
        mDataCarrying = on;
        mPeekFn = std::move(peek);
    }
    uint64_t numShadowMismatches() const { return mShadowMismatches; }
    uint64_t numShadowChecks() const { return mShadowChecks; }

    // Uncacheable MMIO range query (size 0 = disabled).
    bool isUncacheable(address_t addr) const { return mUncacheableSize != 0 && addr >= mUncacheableBase && addr < mUncacheableBase + mUncacheableSize; }
    uint64_t numMmioStores() const { return mNumMmioStores; }
    void setEvictionCallback(EvictionCb cb);
    void setWritebackCallback(WritebackCb cb);
    void setStoreDrainCallback(StoreDrainCb cb);
    void setFillDoneCallback(FillDoneCb cb);
    void setWritebackGate(WritebackGateCb cb) { mWritebackGate = std::move(cb); }
    void setTracer(CacheTracer* t);

    // Switch between countdown (false) and external-fill (true) miss handling.
    // Must be set before the first request is serviced.
    void setExternalFill(bool v) { mExternalFill = v; }

    // ── Backpressure queries ─────────────────────────────────────────────────

    // Can this address be accepted? (MSHR available or coalescing possible)
    bool canAcceptRequest(address_t addr) const;

    // Would this address allocate a new MSHR? (for L2 backpressure check)
    bool willNeedFill(address_t addr) const;

    uint32_t storeBufferRoom() const { return mStoreBufferCapacity - static_cast<uint32_t>(mStoreBuffer.size()); }
    bool storeBufferEmpty() const { return mStoreBuffer.empty(); }
    // Buffered stores vs [addr, addr + size): 0 none, 1 one store covers it, 2 partial overlap.
    int storeBufferOverlap(address_t addr, uint32_t size) const;

    // ── Request interface ────────────────────────────────────────────────────

    // Submit a load request. Returns true if accepted, false if bank-conflicted.
    // When false, caller should retry next cycle. exclusive (atomics) leaves the line Modified.
    // A non-demand access (page walk, upper-level prefetch) is not a demand miss, and a prefetch it
    // touches first counts as neither useful nor useless.
    bool loadRequest(uint64_t token, address_t addr, uint64_t cycle, bool exclusive = false, bool demand = true);

    // Submit a store request. Returns true if accepted into store buffer.
    bool storeRequest(uint64_t token, address_t addr, address_t pc, uint8_t size, uint64_t cycle);

    // Prefetch a line: install on fill but never complete to the core. No-op if
    // already cached or already in flight. A prefetch never takes the last
    // demand_reserve MSHRs, so demand misses keep priority.
    enum class PrefetchOutcome : uint8_t { Issued, Filtered, NoMshr };  // Filtered: present, in flight or uncacheable
    PrefetchOutcome prefetchRequest(address_t addr, uint64_t cycle = 0, uint32_t demand_reserve = 0);

    // Side-effect-free demand lookup, for training a prefetcher on the access stream.
    struct Probe {
        bool hit{false};
        bool in_flight{false};
        bool prefetched{false};  // the line or the MSHR it would merge into came from a prefetch
    };
    Probe probe(address_t addr) const;

    // ── Store buffer forwarding ──────────────────────────────────────────────

    // Check if a load can be satisfied from the store buffer.
    // Returns true if a buffered store fully covers [addr, addr + size).
    bool checkStoreBufferForward(address_t addr, uint8_t size) const;

    // ── Fill / Invalidate ────────────────────────────────────────────────────

    // Called when the next level delivers a fill or an ownership grant (external_fill mode).
    // unique: the line was granted E or M, so it may be written without asking again.
    void fillComplete(address_t line_addr, uint64_t cycle = 0, bool unique = false);

    // Invalidate a line (for inclusive L2 back-invalidation).
    void invalidateLine(address_t addr, uint64_t cycle = 0);

    // ── Tick ─────────────────────────────────────────────────────────────────

    // Advance one cycle: drain store buffer, advance MSHRs, drain hit pipeline.
    void tick(uint64_t cycle = 0);

    // ── Completion Queue (alternative to callback) ───────────────────────────

    // Drain all completions from this cycle. Use this instead of callback for
    // controlled completion processing (e.g., with backpressure checks).
    std::vector<CompletionEntry> drainCompletions();

    // ── Stats ────────────────────────────────────────────────────────────────

    uint32_t outstandingMisses() const;
    uint64_t numHits() const { return mNumHits; }
    uint64_t numMisses() const { return mNumMisses; }
    uint64_t numCoalesced() const { return mNumCoalesced; }
    uint64_t numStoreBufferHits() const { return mNumStoreBufferHits; }
    uint64_t numPrefetches() const { return mNumPrefetches; }
    uint64_t numUsefulPrefetches() const { return mNumUsefulPrefetches; }
    uint64_t numLatePrefetches() const { return mNumLatePrefetches; }
    uint64_t numUselessPrefetches() const { return mNumUselessPrefetches; }
    uint64_t numPollutionMisses() const { return mNumPollutionMisses; }
    uint64_t numNonDemandPrefetchHits() const { return mNumNonDemandPrefetchHits; }
    uint64_t numUpgrades() const { return mNumUpgrades; }

    // ── Accessors for tag array (for external queries) ───────────────────────

    const SimpleCache* tagArray() const { return mTagArray.get(); }
    uint64_t lineSize() const { return mLineSize; }

   private:
    // ── Internal types ───────────────────────────────────────────────────────

    struct Waiter {
        uint64_t token;
        uint32_t req_id{0};
        bool exclusive{false};
    };

    struct MSHREntry {
        address_t line_addr{0};
        uint32_t fill_cycles_remaining{0};
        std::vector<Waiter> waiters;
        bool active{false};
        bool fill_pending{false};
        bool invalidated{false};    // a coherence snoop hit this line while its fill was outstanding
        bool prefetch{false};       // allocated by prefetchRequest and not yet claimed by a demand
        bool dirty_on_fill{false};  // a buffered store (write-allocate) is waiting on this line
        bool exclusive{false};      // an atomic waits on it: install the line Modified
        bool unique{false};         // the next level granted E or M
    };

    struct PendingHit {
        uint64_t token;
        uint32_t cycles_remaining;
        uint32_t req_id{0};
        address_t line_addr{0};
    };

    struct StoreBufferEntry {
        address_t addr{0};
        address_t pc{0};
        uint8_t size{0};
        uint64_t token{0};
        uint32_t req_id{0};
    };

    struct BankState {
        uint32_t reads_used{0};
        uint32_t writes_used{0};
    };

    // ── Helpers ──────────────────────────────────────────────────────────────

    address_t lineAddr(address_t addr) const noexcept;
    uint32_t bankIndex(address_t addr) const noexcept;

    MSHREntry* findMSHR(address_t line_addr);
    const MSHREntry* findMSHRConst(address_t line_addr) const;
    MSHREntry* allocMSHR();
    uint32_t mshrIndex(const MSHREntry* m) const noexcept;

    bool tryConsumeReadPort(address_t addr);
    bool tryConsumeWritePort(address_t addr);
    void resetBankState();

    void demandMiss(address_t line_addr);
    void requestOwnership(MSHREntry& mshr);
    void claimPrefetch(MSHREntry& mshr);
    uint32_t pollutionIndex(address_t line_addr) const noexcept;

    void drainStoreBuffer(uint64_t cycle);
    bool drainOneStore(uint64_t cycle);
    void advanceMSHRs(uint64_t cycle);
    void advanceHitPipeline(uint64_t cycle);

    // ── Members ──────────────────────────────────────────────────────────────

    Callback mCallback;
    FillRequestCb mFillRequestCb;
    UpgradeRequestCb mUpgradeRequestCb;
    EvictionCb mEvictionCb;
    WritebackCb mWritebackCb;
    StoreDrainCb mStoreDrainCb;
    FillDoneCb mFillDoneCb;
    WritebackGateCb mWritebackGate;
    CacheTracer* mTracer{nullptr};

    // Config
    uint32_t mHitLatency;
    uint32_t mMissLatency;
    bool mExternalFill;
    uint64_t mUncacheableBase;
    uint64_t mUncacheableSize;
    uint32_t mStoreBufferCapacity;
    uint32_t mNumBanks;
    uint32_t mReadsPerBank;
    uint32_t mWritesPerBank;
    bool mFillOccupiesWritePort;

    // Tag array
    std::unique_ptr<SimpleCache> mTagArray;
    uint64_t mLineSize;
    uint32_t mLineShift;  // log2(line_size) for bank index calculation

    // MSHRs
    std::vector<MSHREntry> mMSHRs;
    std::unordered_map<address_t, uint32_t> mMSHRMap;  // line_addr -> index in mMSHRs

    // Hit pipeline
    std::deque<PendingHit> mPendingHits;

    // Store buffer
    std::deque<StoreBufferEntry> mStoreBuffer;

    // Banking state (reset each tick)
    std::vector<BankState> mBankState;

    // Completion queue (used when mCallback is null)
    std::vector<CompletionEntry> mCompletions;

    // Stats
    uint64_t mNumHits{0};
    uint64_t mNumMisses{0};
    // Data-carrying shadow state.
    bool mDataCarrying{false};
    PeekFn mPeekFn;
    uint64_t mShadowMismatches{0};  // load hits where shadow line != Whisper (stale-line reads)
    uint64_t mShadowChecks{0};      // load hits checked (data-carrying on)
    uint64_t mNumCoalesced{0};
    uint64_t mNumStoreBufferHits{0};
    uint64_t mNumBankConflicts{0};
    uint64_t mNumPrefetches{0};
    uint64_t mNumUsefulPrefetches{0};
    uint64_t mNumLatePrefetches{0};     // demand merged into an in-flight prefetch
    uint64_t mNumUselessPrefetches{0};  // prefetched line evicted or invalidated before any demand use
    uint64_t mNumPollutionMisses{0};    // demand misses to lines a prefetch fill evicted
    uint64_t mNumNonDemandPrefetchHits{0};
    uint64_t mNumUpgrades{0};
    // Srinath et al. (HPCA 2007) pollution filter: set when a prefetch fill evicts a demand line.
    std::vector<bool> mPollutionFilter = std::vector<bool>(4096);
    uint64_t mNumMmioStores{0};  // stores forwarded to the uncacheable MMIO range
};

}  // namespace cpu
