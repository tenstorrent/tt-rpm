// SPDX-FileCopyrightText: © 2026 Tenstorrent AI ULC
//
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>
#include <ostream>
#include <string>
#include <vector>

namespace cpu {

// ─────────────────────────────────────────────────────────────────
// CacheTracer — passive observer for CacheInterface and L2Cache.
//
// Records structured events during simulation; outputs them at the
// end via dump().  Zero overhead when pointer is null in callers.
//
// Event lifecycle for a single request:
//
//   Hit:      Request → Hit → ... → Response(miss=false)
//   New miss: Request → Miss → MshrAlloc → [FillStart] → FillDone
//                     → Response(miss=true)
//   Coalesce: Request → Coalesce → Response(miss=true)
//              (Response fires at same cycle as the parent MSHR)
// ─────────────────────────────────────────────────────────────────

enum class CacheEventType : uint8_t {
    Request,    // new request received (token, addr)
    Hit,        // tag-array hit       (token, set, way)
    Miss,       // tag-array miss      (token, set) → MSHR to be allocated
    Coalesce,   // miss coalesced into in-flight MSHR (token, mshr_id)
    MshrAlloc,  // MSHR slot allocated (mshr_id, line_addr)
    FillStart,  // L2 fill arrived; countdown started (mshr_id, line_addr)
    FillDone,   // countdown reached 0; line installed (mshr_id, set, way, eviction info)
    Response,   // completion callback fired (req_id, miss)
    L2ArbWin,   // L2: this source won arbitration this cycle
    L2ArbLose,  // L2: this source lost arbitration (wait_cycles recorded)
};

struct CacheEvent {
    uint64_t cycle{0};
    uint32_t req_id{0};  // per-request monotonic ID (set by newRequest())
    CacheEventType type{CacheEventType::Request};
    uint64_t token{0};  // caller's correlation ID (instr tag or base_pc)
    uint64_t addr{0};   // effective address or cache line address
    uint32_t set{0};
    uint32_t way{0};
    uint32_t mshr_id{0};
    uint8_t source{0};  // 0=icache 1=dcache (L2 events only)
    bool miss{false};   // Response: was this a miss?
    bool evict_valid{false};
    uint64_t evict_addr{0};   // FillDone: address of the evicted line (if valid)
    uint32_t wait_cycles{0};  // L2ArbLose: cycles the request has been waiting
};

class CacheTracer {
   public:
    // name:       human-readable label (e.g. "icache", "dcache", "l2cache")
    // max_events: cap on stored events (0 = unlimited)
    CacheTracer(std::string name, uint32_t max_events = 0);

    // Allocates a new req_id and records a Request event.
    // Returns the req_id so the caller can attach it to subsequent events.
    uint32_t newRequest(uint64_t cycle, uint64_t token, uint64_t addr);

    void recordHit(uint32_t req_id, uint64_t cycle, uint32_t set, uint32_t way);
    void recordMiss(uint32_t req_id, uint64_t cycle, uint32_t set);
    void recordCoalesce(uint32_t req_id, uint64_t cycle, uint32_t mshr_id);
    void recordMshrAlloc(uint32_t req_id, uint64_t cycle, uint32_t mshr_id, uint64_t line_addr);
    void recordFillStart(uint64_t cycle, uint32_t mshr_id, uint64_t line_addr);
    void recordFillDone(uint64_t cycle, uint32_t mshr_id, uint64_t line_addr, uint32_t set, uint32_t way, bool evict_valid, uint64_t evict_addr);
    void recordResponse(uint32_t req_id, uint64_t cycle, bool miss);

    // L2-specific arbitration events.
    void recordL2ArbWin(uint64_t cycle, uint8_t source, uint64_t line_addr);
    void recordL2ArbLose(uint64_t cycle, uint8_t source, uint64_t line_addr, uint32_t wait_cycles);

    // Output.  format: "log" | "waterfall" | "summary"
    void dump(std::ostream& os, const std::string& format) const;

    const std::string& getName() const { return mName; }
    bool hasEvents() const { return !mEvents.empty(); }
    bool isFull() const { return mMax > 0 && mEvents.size() >= mMax; }

   private:
    void dumpLog(std::ostream& os) const;
    void dumpWaterfall(std::ostream& os) const;
    void dumpSummary(std::ostream& os) const;

    void append(CacheEvent ev) {
        if (!isFull()) mEvents.push_back(std::move(ev));
    }

    static const char* eventName(CacheEventType t);
    static const char* sourceName(uint8_t src);

    std::string mName;
    uint32_t mMax;
    std::vector<CacheEvent> mEvents;
    uint32_t mNextReqId{0};
};

}  // namespace cpu
