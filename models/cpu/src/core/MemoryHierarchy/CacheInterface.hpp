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
//  6. Write policies — write-back (default) or write-through.
//
// Fill modes:
//  • Countdown (external_fill=false): MSHR counts down hit_lat + miss_lat.
//  • External (external_fill=true): waits for fillComplete(), then hit_lat.
// ─────────────────────────────────────────────────────────────────────────────

enum class WritePolicy : uint8_t {
    WRITE_BACK,    // Allocate on write-miss, mark dirty, writeback on evict
    WRITE_THROUGH  // Write to next level immediately, no dirty state
};

class CacheInterface {
   public:
    struct Config {
        uint32_t mshr_capacity{4};
        uint32_t hit_latency{1};
        uint32_t miss_latency{10};
        bool external_fill{false};

        // Store buffer
        uint32_t store_buffer_capacity{8};
        WritePolicy write_policy{WritePolicy::WRITE_BACK};

        // Banking (1 = single-ported, no banking)
        uint32_t num_banks{1};
        uint32_t reads_per_bank_per_cycle{1};
        uint32_t writes_per_bank_per_cycle{1};
        bool fill_occupies_write_port{false};

        SimpleCache::Config cache;
    };

    // Completion entry for queue-based completion model
    struct CompletionEntry {
        uint64_t token;
        bool miss;
    };

    // Completion callback: (token, was_miss) - optional, if null uses completion queue
    using Callback = std::function<void(uint64_t token, bool miss)>;
    // Fill request callback: fired when new MSHR allocated (external_fill mode)
    using FillRequestCb = std::function<void(address_t line_addr)>;
    // Eviction callback: fired when a line is evicted (for inclusivity/writeback)
    using EvictionCb = std::function<void(address_t line_addr, bool dirty)>;
    // Writeback callback: fired when dirty line evicted (write-back mode)
    using WritebackCb = std::function<void(address_t line_addr)>;

    // Constructor - callback is optional (if null, use drainCompletions() instead)
    CacheInterface(const Config& cfg, Callback cb = nullptr);
    ~CacheInterface() = default;

    // ── Callback setters ─────────────────────────────────────────────────────
    void setFillRequestCallback(FillRequestCb cb);
    void setEvictionCallback(EvictionCb cb);
    void setWritebackCallback(WritebackCb cb);
    void setTracer(CacheTracer* t);

    // ── Backpressure queries ─────────────────────────────────────────────────

    // Can this address be accepted? (MSHR available or coalescing possible)
    bool canAcceptRequest(address_t addr) const;

    // Would this address allocate a new MSHR? (for L2 backpressure check)
    bool willNeedFill(address_t addr) const;

    // Is the store buffer full?
    bool isStoreBufferFull() const;

    // ── Request interface ────────────────────────────────────────────────────

    // Submit a load request. Returns true if accepted, false if bank-conflicted.
    // When false, caller should retry next cycle.
    bool loadRequest(uint64_t token, address_t addr, uint64_t cycle);

    // Submit a store request. Returns true if accepted into store buffer.
    bool storeRequest(uint64_t token, address_t addr, uint8_t size, uint64_t cycle);

    // Legacy single-entry-point (treats as load). For backward compatibility.
    void request(uint64_t token, address_t addr, uint64_t cycle = 0);

    // ── Store buffer forwarding ──────────────────────────────────────────────

    // Check if a load can be satisfied from the store buffer.
    // Returns true if forwarding is possible; sets *forwarded_data if provided.
    bool checkStoreBufferForward(address_t addr, uint8_t size) const;

    // ── Fill / Invalidate ────────────────────────────────────────────────────

    // Called when L2 delivers a fill (external_fill mode).
    void fillComplete(address_t line_addr, uint64_t cycle = 0);

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

    // ── Accessors for tag array (for external queries) ───────────────────────

    const SimpleCache* tagArray() const { return mTagArray.get(); }
    uint64_t lineSize() const { return mLineSize; }

   private:
    // ── Internal types ───────────────────────────────────────────────────────

    struct Waiter {
        uint64_t token;
        uint32_t req_id{0};
    };

    struct MSHREntry {
        address_t line_addr{0};
        uint32_t fill_cycles_remaining{0};
        std::vector<Waiter> waiters;
        bool active{false};
        bool fill_pending{false};
    };

    struct PendingHit {
        uint64_t token;
        uint32_t cycles_remaining;
        uint32_t req_id{0};
    };

    struct StoreBufferEntry {
        address_t addr{0};
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

    void drainStoreBuffer(uint64_t cycle);
    void advanceMSHRs(uint64_t cycle);
    void advanceHitPipeline(uint64_t cycle);

    // ── Members ──────────────────────────────────────────────────────────────

    Callback mCallback;
    FillRequestCb mFillRequestCb;
    EvictionCb mEvictionCb;
    WritebackCb mWritebackCb;
    CacheTracer* mTracer{nullptr};

    // Config
    uint32_t mHitLatency;
    uint32_t mMissLatency;
    bool mExternalFill;
    WritePolicy mWritePolicy;
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
    uint64_t mNumCoalesced{0};
    uint64_t mNumStoreBufferHits{0};
    uint64_t mNumBankConflicts{0};
};

}  // namespace cpu
