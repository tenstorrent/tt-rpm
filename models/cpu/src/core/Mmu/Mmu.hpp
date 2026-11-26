// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Tenstorrent USA, Inc.

#pragma once

#include <array>
#include <map>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

#include "sparta/ports/DataPort.hpp"
#include "sparta/simulation/ParameterSet.hpp"
#include "sparta/simulation/Unit.hpp"
#include "sparta/statistics/Counter.hpp"

#include "Common/PipelinePacket.hpp"
#include "common/TLB.hpp"
#include "models/cpu/common/ExecutionDriver.hpp"

namespace midcore {
class BackendMemoryStructures;
}

namespace core {

class MmuParams : public sparta::ParameterSet {
   public:
    MmuParams(sparta::TreeNode* n)
        : sparta::ParameterSet(n) {}

    PARAMETER(bool, enabled, true, "Model TLB and page-walk timing (false: translation is free)")
    PARAMETER(std::string, tlb_type, "setassoc", "TLB algorithm for the ITLB, DTLB and L2 TLB")
    // L1 TLBs: rocket-chip ICacheParams/DCacheParams defaults (1 set x 32 ways).
    PARAMETER(uint32_t, itlb_entries, 32, "L1 instruction TLB entries")
    PARAMETER(uint32_t, itlb_ways, 32, "L1 instruction TLB ways (= entries: fully associative)")
    PARAMETER(uint32_t, dtlb_entries, 32, "L1 data TLB entries")
    PARAMETER(uint32_t, dtlb_ways, 32, "L1 data TLB ways (= entries: fully associative)")
    PARAMETER(uint32_t, l2tlb_entries, 1024, "Unified L2 TLB entries (0 = none)")
    PARAMETER(uint32_t, l2tlb_ways, 8, "Unified L2 TLB ways")
    PARAMETER(uint32_t, l2tlb_latency, 8, "Cycles an L1 TLB miss waits for an L2 TLB hit")
    PARAMETER(uint32_t, num_walkers, 2, "Concurrent page-table walks")
    PARAMETER(uint32_t, pwc_entries, 16, "Page-walk cache entries per non-leaf level (0 = none)")
};

// ITLB, DTLB, unified L2 TLB and a hardware page-table walker. Whisper translates functionally;
// this unit only times it, loading the PTE addresses of Whisper's own walk through the L1D.
class Mmu : public sparta::Unit {
   public:
    static constexpr char name[] = "mmu";

    Mmu(sparta::TreeNode* node, const MmuParams* params);

    sparta::DataOutPort<MemRequest> pte_request_out{&unit_port_set_, "pte_request_out"};
    sparta::DataInPort<MemResponse> pte_response_in{&unit_port_set_, "pte_response_in"};
    sparta::DataInPort<FlushRequest> flush_in{&unit_port_set_, "flush_in"};

    void setExecutionDriver(cpu::ExecutionDriver* driver);
    void setCache(midcore::BackendMemoryStructures* dcache) { mDcache = dcache; }

    // Whether fetch may use the translation of `va` this cycle. Polled every cycle until true.
    bool fetchReady(cpu::address_t va);
    // Whether a load or store may use its translation this cycle. Polled until true.
    bool dataReady(const PipelinePacket& pkt, bool is_store);

    void tick();

    uint64_t numItlbMisses() const { return mItlbMisses.get(); }
    uint64_t numDtlbMisses() const { return mDtlbMisses.get(); }
    uint64_t numL2TlbMisses() const { return mL2TlbMisses.get(); }
    uint64_t numWalks() const { return mWalksStarted.get(); }
    uint64_t numSuperpageWalks() const { return mWalksSuperpage.get(); }
    uint64_t numWalkCycles() const { return mWalkCycles.get(); }
    uint64_t numPwcHits() const { return mPwcHits.get(); }

    // PTE loads share the D-cache response port with the LSQ; this bit keeps their tags apart.
    static constexpr uint64_t kPteTagBit = 1ULL << 63;

   private:
    enum Side : uint8_t { kInst = 0, kData = 1 };

    // Waits either for a cycle (L2 TLB hit) or for a walk to finish.
    struct Ticket {
        uint64_t ready_cycle{0};
        uint64_t walk_id{0};
    };
    struct DataTicket {
        Ticket t;
        uint64_t tag{0};
        uint8_t depth{0};
    };
    struct Walk {
        cpu::address_t va{0};
        tlb::asid_t asid{0};
        uint8_t sides{0};  // bitmask of the L1 TLBs to fill
        cpu::ExecutionDriver::PageWalk pw;
        uint32_t next{0};  // index into pw.pte_addrs of the next PTE to load
        uint32_t refs{0};  // live requesters
        bool started{false};
        bool outstanding{false};
        bool stale{false};         // an sfence.vma retired while in flight: do not fill
        uint64_t queued_cycle{0};  // earliest start
        uint64_t start_cycle{0};
    };
    // An L2 TLB hit fills the L1 TLB only once its lookup latency has passed.
    struct L1Fill {
        uint64_t ready_cycle{0};
        Side side{kInst};
        tlb::TlbEntry e;
    };
    struct PwcEntry {
        bool valid{false};
        uint64_t tag{0};
        tlb::asid_t asid{0};
        uint32_t levels{0};
        uint64_t last_use{0};
    };

    std::optional<Ticket> start_(Side side, cpu::address_t va, bool is_store, uint64_t tag);
    bool ready_(const Ticket& t) const;
    void release_(const Ticket& t);
    void complete_(std::map<uint64_t, Walk>::iterator it);
    uint32_t pwcSkip_(const Walk& w);
    void pwcFill_(const Walk& w, uint32_t level);
    void receivePteResponse_(const MemResponse& resp);
    void receiveFlush_(const FlushRequest& req);
    void sfence_(const cpu::ExecutionDriver::SfenceVma& f);
    uint64_t now_() const { return getClock()->currentCycle(); }

    const bool mEnabled;
    const uint32_t mL2Latency;
    const uint32_t mNumWalkers;
    std::array<std::unique_ptr<tlb::TlbInterface>, 2> mL1;  // indexed by Side
    std::unique_ptr<tlb::TlbInterface> mL2;                 // null when l2tlb_entries == 0
    std::vector<std::vector<PwcEntry>> mPwc;                // [non-leaf level from the root][entry]
    uint64_t mPwcClock{0};

    cpu::ExecutionDriver* mDriver{nullptr};
    midcore::BackendMemoryStructures* mDcache{nullptr};

    std::vector<L1Fill> mL1Fills;
    std::map<uint64_t, Walk> mWalks;  // queued and active, by id (so in arrival order)
    uint64_t mNextWalkId{0};
    uint32_t mActiveWalks{0};
    std::optional<Ticket> mFetchTicket;
    cpu::address_t mFetchVa{0};
    std::unordered_map<uint64_t, DataTicket> mDataTickets;  // by fetch_seq

    sparta::Counter mItlbHits;
    sparta::Counter mItlbMisses;
    sparta::Counter mDtlbHits;
    sparta::Counter mDtlbMisses;
    sparta::Counter mL2TlbHits;
    sparta::Counter mL2TlbMisses;
    sparta::Counter mWalksStarted;
    sparta::Counter mWalksCoalesced;
    sparta::Counter mWalksFaulted;
    sparta::Counter mWalksSuperpage;
    sparta::Counter mWalksCancelled;
    sparta::Counter mWalksOrphaned;
    sparta::Counter mWalkCycles;
    sparta::Counter mWalkQueueCycles;
    sparta::Counter mPteLoads;
    sparta::Counter mPwcHits;
    sparta::Counter mPwcMisses;
    sparta::Counter mItlbStallCycles;
    sparta::Counter mDtlbWaitCycles;
    sparta::Counter mSfences;
};

}  // namespace core
