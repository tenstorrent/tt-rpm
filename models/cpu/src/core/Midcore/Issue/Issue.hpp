// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Tenstorrent USA, Inc.

#pragma once

#include <array>
#include <bitset>
#include <deque>
#include <optional>
#include <random>
#include <string>
#include <vector>

#include "sparta/ports/DataPort.hpp"
#include "sparta/simulation/ParameterSet.hpp"
#include "sparta/simulation/Unit.hpp"
#include "sparta/statistics/Counter.hpp"

#include "Common/PipelinePacket.hpp"
#include "Common/PipelineVisualizer.hpp"
#include "Midcore/Issue/AgeMatrix.hpp"

namespace midcore {

class Execute;
class BypassNetwork;
class PhysicalRegisterFile;

// ============================================================================
// Routing Priority — used in priority-based load balancing
// ============================================================================
enum class RoutingPriority : uint8_t {
    DependencyLocality,  // Route to producer's scheduler if eligible
    TypeAffinity,        // Route to primary scheduler for this UopType
    LeastOccupied,       // Route to least-full eligible scheduler
    RoundRobin           // Cycle through eligible schedulers
};

// ============================================================================
// Selection Mode — how to pick from ready instructions
// ============================================================================
enum class SelectionMode : uint8_t {
    AgeMatrix,  // Use age matrix to find oldest ready (default)
    FIFO,       // Use queue order (front-to-back)
    Random      // Random selection among ready
};

// ============================================================================
// Routing Config — priority chain for load balancing
// ============================================================================
struct RoutingConfig {
    std::vector<RoutingPriority> priorities{RoutingPriority::DependencyLocality, RoutingPriority::TypeAffinity, RoutingPriority::LeastOccupied};
    float dependency_occupancy_limit{0.9f};  // Skip dep routing if scheduler >90% full
    float overflow_threshold{0.8f};          // Consider alternatives when >80% full
};

// ============================================================================
// Issue Port — one issue slot within a scheduler
// ============================================================================
struct IssuePort {
    std::string name;
    std::vector<core::UopType> served_types;
    bool used_this_cycle{false};

    bool canServe(core::UopType t) const {
        for (auto st : served_types)
            if (st == t) return true;
        return false;
    }

    void reset() { used_this_cycle = false; }
};

// ============================================================================
// IQ Entry — slot-based storage for age matrix integration
// ============================================================================
struct IQEntry {
    std::optional<core::RenamedPacket> pkt;  // Empty = free slot
    uint32_t slot_idx{0};                    // Index in the slots array
};

// ============================================================================
// Issue Scheduler — one partitioned queue with age matrix selection
// ============================================================================
struct IssueScheduler {
    std::string name;
    uint32_t capacity{16};
    std::vector<IssuePort> ports;

    // Types this scheduler primarily handles
    std::vector<core::UopType> primary_types;
    // Additional types it can accept (for dependency routing)
    std::vector<core::UopType> accepts_types;

    // Slot-based storage with age matrix
    std::vector<IQEntry> slots;
    AgeMatrix ages{16};

    void init(uint32_t cap) {
        capacity = cap;
        slots.resize(cap);
        for (uint32_t i = 0; i < cap; ++i) {
            slots[i].slot_idx = i;
        }
        ages = AgeMatrix(cap);
    }

    bool isFull() const { return ages.count() >= capacity; }
    uint32_t size() const { return ages.count(); }
    float occupancy() const { return static_cast<float>(size()) / capacity; }

    // Allocate a slot and insert packet, returns slot index or kInvalidSlot
    uint32_t insert(const core::RenamedPacket& rpkt) {
        uint32_t slot = ages.findFreeSlot();
        if (slot == kInvalidSlot) return kInvalidSlot;
        slots[slot].pkt = rpkt;
        ages.allocate(slot);
        return slot;
    }

    // Remove entry from slot
    void remove(uint32_t slot) {
        if (slot >= capacity) return;
        slots[slot].pkt.reset();
        ages.deallocate(slot);
    }

    // Squash entries matching the predicate
    template <typename Pred>
    uint32_t squash(Pred should_squash) {
        uint32_t count = 0;
        for (uint32_t i = 0; i < capacity; ++i) {
            if (slots[i].pkt && should_squash(*slots[i].pkt)) {
                remove(i);
                ++count;
            }
        }
        return count;
    }

    // Decrement depth for entries younger than branch_tag and at depth > resolved_depth
    uint32_t decrementDepth(uint64_t branch_tag, uint8_t resolved_depth) {
        uint32_t count = 0;
        for (uint32_t i = 0; i < capacity; ++i) {
            if (slots[i].pkt && slots[i].pkt->pkt.tag > branch_tag && slots[i].pkt->pkt.wrong_path_depth > resolved_depth) {
                --slots[i].pkt->pkt.wrong_path_depth;
                ++count;
            }
        }
        return count;
    }

    // Check if this scheduler can accept a given UopType
    bool canAccept(core::UopType t) const {
        for (auto pt : primary_types)
            if (pt == t) return true;
        for (auto at : accepts_types)
            if (at == t) return true;
        return false;
    }

    // Check if this is the primary scheduler for a type
    bool isPrimaryFor(core::UopType t) const {
        for (auto pt : primary_types)
            if (pt == t) return true;
        return false;
    }

    void resetPorts() {
        for (auto& p : ports) p.reset();
    }

    IssuePort* findAvailablePort(core::UopType t) {
        for (auto& p : ports) {
            if (!p.used_this_cycle && p.canServe(t)) return &p;
        }
        return nullptr;
    }
};

// ============================================================================
// Pending Wakeup — speculative wakeup scheduled for future cycle
// ============================================================================
struct PendingWakeup {
    std::vector<core::PhysRegRef> phys_dsts;
    uint8_t cycles_remaining;
};

// ============================================================================
// Scheduler Config — passed from CoreTop after YAML parsing
// ============================================================================
struct SchedulerPortConfig {
    std::string name;
    std::vector<core::UopType> types;
};

struct SchedulerConfig {
    std::string name;
    uint32_t capacity{16};
    std::vector<core::UopType> types;    // Primary types for this scheduler
    std::vector<core::UopType> accepts;  // Additional types it can accept (dependency routing)
    std::vector<SchedulerPortConfig> ports;
};

// ============================================================================
// Issue Parameters — Sparta parameter set
// ============================================================================
class IssueParams : public sparta::ParameterSet {
   public:
    IssueParams(sparta::TreeNode* n)
        : sparta::ParameterSet(n) {}

    PARAMETER(uint32_t, issue_queue_capacity, 16, "Issue queue capacity (unified mode)")
    PARAMETER(uint32_t, issue_width, 4, "Max instructions issued per cycle (unified mode)")
    PARAMETER(std::string, mode, "unified", "Issue mode: unified | partitioned")
    PARAMETER(bool, speculative_wakeup, false, "Enable speculative wakeup for non-memory ops")
    PARAMETER(std::string, selection, "age_matrix", "Selection mode: age_matrix | fifo | random")
    PARAMETER(uint32_t, num_phys_regs, 128, "Number of physical registers (for scoreboard sizing)")
    PARAMETER(bool, bypass_network_enabled, false, "Enable bypass network")
    PARAMETER(uint32_t, bypass_regfile_read_latency, 1, "Regfile read latency when bypass disabled")
    PARAMETER(std::vector<std::string>, bypass_paths, std::vector<std::string>{}, "Bypass paths: producer:consumer:latency")
    PARAMETER(std::vector<std::string>, scheduler_configs, std::vector<std::string>{}, "Partitioned scheduler configs: name:capacity:types")
    PARAMETER(std::vector<std::string>, routing_priorities, std::vector<std::string>{}, "Routing priorities for partitioned mode")
    PARAMETER(double, routing_dep_occupancy_limit, 0.9, "Dependency routing occupancy limit")
    PARAMETER(double, routing_overflow_threshold, 0.8, "Overflow threshold for routing")
};

// ============================================================================
// Issue Unit
// ============================================================================
class Issue : public sparta::Unit {
    class PhysicalScoreboard {
        static constexpr size_t kNumRegTypes = 3;
        static size_t idx(core::RegType t) { return core::regTypeIndex(t); }

        std::array<std::vector<uint8_t>, kNumRegTypes> mReady;

       public:
        explicit PhysicalScoreboard(uint32_t num_physical_registers) {
            for (size_t t = 0; t < kNumRegTypes; ++t) {
                mReady[t].assign(num_physical_registers, 0);
                for (uint32_t i = 0; i < 32; ++i) mReady[t][i] = 1;
            }
        }

        void markNotReady(uint16_t phys_reg, core::RegType type) { mReady[idx(type)][phys_reg] = 0; }

        void markReady(uint16_t phys_reg, core::RegType type) { mReady[idx(type)][phys_reg] = 1; }

        bool sourcesReady(const std::vector<core::PhysRegRef>& phys_srcs) const {
            for (const auto& src : phys_srcs)
                if (!mReady[idx(src.type)][src.phys_reg]) return false;
            return true;
        }
    };

   public:
    static constexpr char name[] = "issue";
    static constexpr size_t kNumUopTypes = core::kNumUopTypes;
    static constexpr uint8_t kNoScheduler = 255;  // Sentinel: UopType not assigned to any scheduler

    Issue(sparta::TreeNode* node, const IssueParams* params);

    sparta::DataInPort<std::vector<core::RenamedPacket>> in_port{&unit_port_set_, "packets_in"};

    sparta::DataOutPort<std::vector<core::IssuePacket>> out_port{&unit_port_set_, "packets_out"};

    sparta::DataInPort<std::vector<core::PhysRegRef>> completion_exe_in{&unit_port_set_, "completion_exe_in"};

    sparta::DataInPort<std::vector<core::PhysRegRef>> completion_lsq_in{&unit_port_set_, "completion_lsq_in"};

    // Flush support
    sparta::DataInPort<core::FlushRequest> flush_in{&unit_port_set_, "flush_in"};

    // Correct branch resolution - decrements depth
    sparta::DataInPort<core::BranchResolved> branch_resolved_in{&unit_port_set_, "branch_resolved_in"};

    void tick();

    bool isReadyForType(core::UopType t) const {
        uint8_t si = mSchedulerForType[static_cast<uint8_t>(t)];
        const auto& sched = mSchedulers[si];
        // Account for pending reservations from same-cycle dispatches
        return (sched.size() + mPendingReservations[si]) < sched.capacity;
    }

    bool isReady() const {
        for (size_t i = 0; i < mSchedulers.size(); ++i) {
            if ((mSchedulers[i].size() + mPendingReservations[i]) >= mSchedulers[i].capacity) return false;
        }
        return true;
    }

    void reserveSlot(core::UopType t) {
        uint8_t si = mSchedulerForType[static_cast<uint8_t>(t)];
        ++mPendingReservations[si];
    }

    void clearPendingReservations() { mPendingReservations.assign(mPendingReservations.size(), 0); }

    void setDownstream(midcore::Execute* execute) { mDownstream = execute; }
    void setVisualizer(core::PipelineVisualizer* v) { mVis = v; }
    void setBypassNetwork(BypassNetwork* bn) { mBypassNetwork = bn; }
    void setPhysicalRegisterFile(PhysicalRegisterFile* prf) { mPrf = prf; }

    void configurePartitioned(const std::vector<SchedulerConfig>& configs);
    void configureRouting(const RoutingConfig& cfg) { mRoutingConfig = cfg; }

    // Called by Rename to record where a physical register's producer went
    void recordProducerScheduler(const std::vector<core::PhysRegRef>& dsts, uint8_t sched_idx);
    // Called on writeback to clear producer tracking
    void clearProducerScheduler(const std::vector<core::PhysRegRef>& dsts);
    // Get the scheduler a physical register's producer is in (255 if unknown)
    uint8_t getProducerScheduler(const core::PhysRegRef& src) const;

    uint64_t numIssued() const { return mNumIssued.get(); }
    uint64_t numStallCycles() const { return mNumStallCycles.get(); }
    uint64_t numSpeculativeWakeups() const { return mNumSpeculativeWakeups.get(); }

   private:
    void receivePackets_(const std::vector<core::RenamedPacket>& pkts);
    void receiveExeCompletions_(const std::vector<core::PhysRegRef>& completions);
    void receiveLsqCompletions_(const std::vector<core::PhysRegRef>& completions);
    void receiveFlush_(const core::FlushRequest& req);
    void receiveBranchResolved_(const core::BranchResolved& resolved);

    void buildUnified(uint32_t capacity, uint32_t width);
    void buildPartitioned(const std::vector<SchedulerConfig>& configs);

    void processSpeculativeWakeups();
    void scheduleSpeculativeWakeup(const core::IssuePacket& ipkt);

    // Routing: select scheduler for incoming instruction
    uint8_t selectScheduler(const core::RenamedPacket& rpkt);
    uint8_t selectByDependency(const core::RenamedPacket& rpkt);
    uint8_t selectByType(core::UopType t);
    uint8_t selectLeastOccupied(core::UopType t);
    uint8_t selectRoundRobin(core::UopType t);

    // Selection: pick ready instructions from a scheduler
    void selectFromScheduler(IssueScheduler& sched);
    void selectAgeMatrix(IssueScheduler& sched);
    void selectFIFO(IssueScheduler& sched);
    void selectRandom(IssueScheduler& sched);

    // Common post-selection logic: issue a single instruction from a scheduler slot
    void issueInstruction(IssueScheduler& sched, uint32_t slot, IssuePort& port);

    // Check if sources are ready (via scoreboard or bypass network)
    bool sourcesReadyWithBypass(const std::vector<core::PhysRegRef>& phys_srcs) const;

    static bool isMemoryType(core::UopType t) { return t == core::UopType::Load || t == core::UopType::Store; }

    std::vector<IssueScheduler> mSchedulers;
    std::vector<uint32_t> mPendingReservations;                            // Track same-cycle dispatch reservations
    std::array<uint8_t, kNumUopTypes> mSchedulerForType{};                 // Primary scheduler per type
    std::array<std::vector<uint8_t>, kNumUopTypes> mEligibleSchedulers{};  // All eligible schedulers per type

    PhysicalScoreboard mScoreboard;
    midcore::Execute* mDownstream{nullptr};
    core::PipelineVisualizer* mVis{nullptr};
    BypassNetwork* mBypassNetwork{nullptr};
    PhysicalRegisterFile* mPrf{nullptr};

    // Selection and routing config
    SelectionMode mSelectionMode{SelectionMode::AgeMatrix};
    RoutingConfig mRoutingConfig;
    std::array<uint8_t, kNumUopTypes> mRoundRobinCounters{};  // Per-type RR state

    // Producer tracking for dependency-aware routing
    // mProducerScheduler[type_idx][phys_reg] = scheduler_idx or 255
    static constexpr size_t kNumRegTypes = 3;
    std::array<std::vector<uint8_t>, kNumRegTypes> mProducerScheduler;

    bool mSpeculativeWakeupEnabled{false};
    std::deque<PendingWakeup> mPendingWakeups;

    // Flush state tracking - to filter incoming packets in same cycle as flush
    bool mFlushActive{false};
    uint64_t mFlushCycle{0};
    core::FlushRequest mActiveFlush;

    std::mt19937 mRng{42};  // Per-instance RNG for random selection

    std::vector<core::IssuePacket> mToIssueBuf;

    // Statistics
    sparta::Counter mNumIssued;
    sparta::Counter mNumStallCycles;
    sparta::Counter mNumSpeculativeWakeups;
    sparta::Counter mNumDepLocalRoutes;
    sparta::Counter mNumDepLocalFallback;
    sparta::Counter mNumTypeAffinityRoutes;
    sparta::Counter mNumLeastOccupiedRoutes;
    sparta::Counter mNumRoundRobinRoutes;
    sparta::Counter mNumOldestSelected;
    sparta::Counter mNumNotOldestSelected;
    sparta::Counter mNumSquashed;
};

}  // namespace midcore
