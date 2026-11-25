// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Tenstorrent USA, Inc.

#pragma once

#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "sparta/ports/DataPort.hpp"
#include "sparta/simulation/ParameterSet.hpp"
#include "sparta/simulation/Unit.hpp"
#include "sparta/statistics/Counter.hpp"

#include "Common/PipelinePacket.hpp"
#include "Common/PipelineVisualizer.hpp"
#include "Common/WritePortArbiter.hpp"
#include "LoadStoreUnit/LoadQueue.hpp"
#include "LoadStoreUnit/StoreQueue.hpp"
#include "LoadStoreUnit/StoreSetPredictor.hpp"

namespace core {
class Mmu;
}

namespace midcore {

class BackendMemoryStructures;
class ReorderBuffer;

// ─────────────────────────────────────────────────────────────────────────────
// LSQ — Load Store Queue (actually Load-Store Unit)
//
// Manages separate load and store queues with support for:
//  - Store-to-load forwarding from the SQ
//  - Memory-dependence prediction (store sets) and ordering-violation replay
//  - Fences, and atomics performed at the ROB head with the line held exclusive
//  - Per-cycle load and store port limits
//  - Per-bank queues (when num_banks > 1)
//  - Write-port arbitration for load completions
//  - In-order ROB completion
// ─────────────────────────────────────────────────────────────────────────────

class LSQParams : public sparta::ParameterSet {
   public:
    LSQParams(sparta::TreeNode* n)
        : sparta::ParameterSet(n) {}

    PARAMETER(uint32_t, load_queue_capacity, 32, "Load queue capacity")
    PARAMETER(uint32_t, store_queue_capacity, 32, "Store queue capacity")
    PARAMETER(bool, store_forwarding, true, "Enable store-to-load forwarding")
    PARAMETER(uint32_t, forwarding_latency, 1, "Cycles for forwarded load to complete")
    PARAMETER(uint32_t, load_ports, 2, "Loads per cycle that can access the D-cache or forward")
    PARAMETER(uint32_t, store_ports, 1, "Retired stores written to the D-cache store buffer per cycle")
    PARAMETER(std::string, mem_dep_predictor, "store_sets", "Loads vs unknown-address older stores: store_sets | blind | none")
    PARAMETER(uint32_t, ssit_entries, 4096, "Store-set ID table entries")
    PARAMETER(uint32_t, lfst_entries, 128, "Last-fetched-store table entries (number of store sets)")
    PARAMETER(uint64_t, ssit_clear_interval, 1000000, "Cycles between store-set table clears (0 = never)")
    PARAMETER(uint32_t, num_banks, 1, "Number of banks (1 = no banking)")
    PARAMETER(uint32_t, cache_line_size, 64, "Cache line size in bytes (for bank indexing)")
};

class LSQ : public sparta::Unit {
   public:
    static constexpr char name[] = "lsq";

    LSQ(sparta::TreeNode* node, const LSQParams* params);

    // ── Ports ────────────────────────────────────────────────────────────────

    sparta::DataInPort<std::vector<core::IssuePacket>> in_port{&unit_port_set_, "packets_in"};

    sparta::DataOutPort<core::MemRequest> request_out{&unit_port_set_, "request_out"};

    sparta::DataInPort<core::MemResponse> response_in{&unit_port_set_, "response_in"};

    sparta::DataOutPort<std::vector<core::PhysRegRef>> completion_out{&unit_port_set_, "completion_out"};

    sparta::DataOutPort<std::vector<core::ROBToken>> rob_complete_out{&unit_port_set_, "rob_complete_out"};

    // Flush support
    sparta::DataInPort<core::FlushRequest> flush_in{&unit_port_set_, "flush_in"};

    // ── Public interface ─────────────────────────────────────────────────────

    void tick();
    void processWritePortGrants();

    void setCache(midcore::BackendMemoryStructures* dcache) { mDcache = dcache; }
    void setMmu(core::Mmu* mmu) { mMmu = mmu; }
    void setArbiter(WritePortArbiter* arb) { mArbiter = arb; }
    void setVisualizer(core::PipelineVisualizer* v) { mVis = v; }
    void setROB(const midcore::ReorderBuffer* rob) { mRob = rob; }

    // Destinations of the live load with this tag if a single D-cache access completes it, else nullptr.
    const std::vector<core::PhysRegRef>* singleAccessLoadDsts(uint64_t tag, uint64_t req_id) const;

    // ── Commit notifications (called when the ROB retires a memory op) ───────

    void commitStore(uint64_t tag);
    void commitLoad(uint64_t tag);

    // The load must be replayed: an older store to its bytes resolved after it read them.
    bool hasOrderingViolation(uint64_t tag) const;

    bool canClaimLoad() const { return mLoadCredits > 0; }
    bool canClaimStore() const { return mStoreCredits > 0; }
    // Claims an LQ entry for a load, an SQ entry for a store, atomic or fence.
    void claim(const core::DecodePacket& dp);

    // ── Stats ────────────────────────────────────────────────────────────────

    uint64_t numLoads() const { return mNumLoads.get(); }
    uint64_t numStores() const { return mNumStores.get(); }
    uint64_t numForwards() const { return mNumForwards.get(); }
    size_t numCompletionOrder() const { return mCompletionOrder.size(); }

    // Diagnostics
    void dumpDiag(std::ostream& os) const {
        size_t lq_total = 0, sq_total = 0;
        for (uint32_t i = 0; i < mNumBanks; ++i) {
            lq_total += mLoadQueues[i]->size();
            sq_total += mStoreQueues[i]->size();
        }
        os << "  LSQ diag: lq_occ=" << lq_total << " sq_occ=" << sq_total << " load_credits=" << mLoadCredits << " store_credits=" << mStoreCredits
           << " completion_order=" << mCompletionOrder.size() << " load_completed_tags=" << mLoadCompletedTags.size()
           << " claimed_not_arrived=" << mClaimedNotArrived.size();
        if (!mCompletionOrder.empty()) {
            auto& front = mCompletionOrder.front();
            bool load_completed = mLoadCompletedTags.count(front.tag) > 0;
            os << " front_tag=" << front.tag << " front_is_store=" << front.is_store << " front_completed=" << front.completed
               << " front_load_completed=" << load_completed << " front_rob_sent=" << front.rob_sent;
            // Check if front is in LQ/SQ
            bool in_lq = false, in_sq = false;
            for (uint32_t i = 0; i < mNumBanks; ++i) {
                if (mLoadQueues[i]->find(front.tag)) in_lq = true;
                if (mStoreQueues[i]->find(front.tag)) in_sq = true;
            }
            os << " front_in_lq=" << in_lq << " front_in_sq=" << in_sq;
            // If front is a store in SQ, show committed/sent_to_cache
            if (front.is_store) {
                for (uint32_t i = 0; i < mNumBanks; ++i) {
                    if (auto* se = mStoreQueues[i]->find(front.tag)) {
                        os << " store_committed=" << se->committed << " store_sent_to_cache=" << se->sent_to_cache;
                        break;
                    }
                }
            }
        }
        os << "\n";
    }

   private:
    void receivePackets_(const std::vector<core::IssuePacket>& pkts);
    void receiveResponse_(const core::MemResponse& resp);
    void receiveFlush_(const core::FlushRequest& req);

    // Helpers
    uint32_t bankFor(cpu::address_t addr) const;
    std::vector<cpu::address_t> linesTouched_(const WhisperUtil::TraceRecord& rec) const;
    std::vector<MemRange> rangesTouched_(const WhisperUtil::TraceRecord& rec) const;
    void tryIssueLoads();
    void tryDrainStores();
    void drainCompletedEntries();
    void sampleOccupancyStats();
    bool storeTranslated_(uint64_t tag);

    enum WaitReason : uint8_t { kWaitBarrier = 1, kWaitStoreSet = 2, kWaitOverlap = 4 };
    enum class LoadStep : uint8_t { Wait, Forward, ReadCache };
    LoadStep orderLoad_(LoadQueue::Entry& load, uint64_t barrier_tag, const StoreQueue::Entry*& src);
    bool storeResolved_(uint64_t seq) const;
    const StoreQueue::Entry* youngestOlderStore_(const LoadQueue::Entry& load) const;
    uint64_t oldestBarrierTag_() const;
    bool olderStoresDrained_(uint64_t tag) const;
    void checkOrderingViolation_(const StoreQueue::Entry& st);
    void completeFences_();
    void issueAtomics_();
    void finishSqEntry_(StoreQueue::Entry& e);

    void returnLoadCredit() {
        if (mLoadCredits < mLoadCreditCap) ++mLoadCredits;
    }
    void returnStoreCredit() {
        if (mStoreCredits < mStoreCreditCap) ++mStoreCredits;
    }
    struct ClaimInfo {
        uint64_t tag{0};
        uint8_t depth{0};
        bool is_store{false};  // holds an SQ credit
        bool barrier{false};   // fence or acquire atomic: younger loads wait for it
        cpu::address_t pc{0};
        uint64_t dep_seq{StoreSetPredictor::kNone};  // load: predicted store; store: set predecessor
    };
    // Returns the live claim (and consumes it). Empty means a flush already refunded this
    // op's credit (it was squashed in flight) -- the caller must not enqueue it.
    std::optional<ClaimInfo> claimArrived_(uint64_t fetch_seq) {
        auto it = mClaimedNotArrived.find(fetch_seq);
        if (it == mClaimedNotArrived.end()) return std::nullopt;
        ClaimInfo c = it->second;
        mClaimedNotArrived.erase(it);
        return c;
    }
    void claimSquashed_(uint64_t fetch_seq) {
        auto it = mClaimedNotArrived.find(fetch_seq);
        if (it == mClaimedNotArrived.end()) return;
        if (it->second.is_store)
            returnStoreCredit();
        else
            returnLoadCredit();
        mClaimedNotArrived.erase(it);
    }

    // Per-bank load and store queues
    std::vector<std::unique_ptr<LoadQueue>> mLoadQueues;
    std::vector<std::unique_ptr<StoreQueue>> mStoreQueues;

    // In-order completion tracking (for ROB)
    struct CompletionEntry {
        uint64_t tag{0};
        core::ROBToken rob_token;
        bool is_store{false};
        bool completed{false};  // store: immediately true; load: true when response received
        bool rob_sent{false};   // ROB completion already sent (for stores that haven't drained yet)
    };
    std::deque<CompletionEntry> mCompletionOrder;
    std::unordered_set<uint64_t> mLoadCompletedTags;  // O(1) lookup for completed loads
    uint64_t mNextReqId{0};

    int32_t mLoadCredits{0};
    int32_t mStoreCredits{0};
    int32_t mLoadCreditCap{0};
    int32_t mStoreCreditCap{0};
    std::unordered_map<uint64_t, ClaimInfo> mClaimedNotArrived;

    // Track most recent flush for filtering late-arriving packets
    std::optional<core::FlushRequest> mLastFlush;

    // Config
    uint32_t mNumBanks;
    uint32_t mBankMask;
    uint32_t mLineShift;
    bool mForwardingEnabled;
    uint32_t mForwardingLatency;
    uint32_t mLoadPorts;
    uint32_t mStorePorts;
    enum class MemDepMode : uint8_t { StoreSets, Blind, None };
    MemDepMode mMemDepMode;
    uint64_t mSsitClearInterval;
    StoreSetPredictor mStoreSets;

    uint32_t mLoadPortsLeft{0};

    // External references
    midcore::BackendMemoryStructures* mDcache{nullptr};
    core::Mmu* mMmu{nullptr};
    WritePortArbiter* mArbiter{nullptr};
    core::PipelineVisualizer* mVis{nullptr};
    const midcore::ReorderBuffer* mRob{nullptr};

    // Buffers
    std::vector<core::ROBToken> mRobCompletionsBuf;
    std::vector<core::PhysRegRef> mRegCompletionsBuf;

    // Stats
    sparta::Counter mNumLoads;
    sparta::Counter mNumStores;
    sparta::Counter mNumForwards;
    sparta::Counter mNumSquashed;
    sparta::Counter mNumAtomics;
    sparta::Counter mNumFences;
    sparta::Counter mNumViolations;
    sparta::Counter mNumStaleResponses;
    sparta::Counter mNumStoreSetWaits;
    sparta::Counter mNumOverlapWaits;
    sparta::Counter mNumBarrierWaits;
    sparta::Counter mNumLoadPortStalls;
    sparta::Counter mNumStorePortStalls;

    sparta::Counter mMaxLqOccupancy;
    sparta::Counter mMaxSqOccupancy;
    sparta::Counter mSumLqOccupancy;
    sparta::Counter mSumSqOccupancy;
};

}  // namespace midcore
