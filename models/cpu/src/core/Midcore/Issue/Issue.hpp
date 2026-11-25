// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Tenstorrent USA, Inc.

#pragma once

#include <algorithm>
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

    // Slot-based storage with age matrix
    std::vector<IQEntry> slots;
    AgeMatrix ages{16};

    void init(uint32_t cap) {
        sparta_assert(cap >= 1 && cap <= kMaxIQSlots, "issue scheduler '" << name << "' capacity is " << cap << "; it must be in [1, " << kMaxIQSlots
                                                                          << "] (issue.params.issue_queue_capacity or scheduler_configs)");
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

    // Decrement depth for entries younger than the resolved branch and at depth > its depth.
    // Younger by fetch_seq: model tags are reused after a flush.
    uint32_t decrementDepth(const core::BranchResolved& resolved) {
        uint32_t count = 0;
        for (uint32_t i = 0; i < capacity; ++i) {
            if (!slots[i].pkt) continue;
            const auto& p = slots[i].pkt->pkt;
            const bool younger = resolved.branch_fetch_seq != 0 ? p.fetch_seq > resolved.branch_fetch_seq : p.tag > resolved.branch_tag;
            if (younger && p.wrong_path_depth > resolved.resolved_depth) {
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

    // Least capable free port first, so a plain ALU op does not take the only Mul/Div port.
    IssuePort* findAvailablePort(core::UopType t) {
        IssuePort* best = nullptr;
        for (auto& p : ports) {
            if (!p.used_this_cycle && p.canServe(t) && (!best || p.served_types.size() < best->served_types.size())) best = &p;
        }
        return best;
    }
};

// ============================================================================
// Unpipelined unit (divider) — busy from issue until its result is produced
// ============================================================================
struct UnpipelinedUnit {
    uint64_t busy_until{0};
    core::PipelinePacket holder;
};

// ============================================================================
// Scheduler Config — parsed from the scheduler_configs parameter
// ============================================================================
struct SchedulerPortConfig {
    std::string name;
    std::vector<core::UopType> types;
};

struct SchedulerConfig {
    std::string name;
    uint32_t capacity{16};
    std::vector<core::UopType> types;  // Primary types for this scheduler
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
    PARAMETER(std::vector<std::string>, issue_ports, std::vector<std::string>{},
              "Unified mode: uop types each issue port accepts, e.g. ALU+Branch (empty = issue_width ports that accept any type)")
    PARAMETER(uint32_t, regfile_read_latency, 1, "Register-read stages between issue and execute")
    PARAMETER(uint32_t, int_div_units, 1, "Unpipelined integer dividers (0 = pipelined)")
    PARAMETER(uint32_t, fp_div_units, 1, "Unpipelined FP divide/sqrt units (0 = pipelined)")
    PARAMETER(uint32_t, int_div_interval, 10, "Cycles an integer divide holds its divider (0 = its full latency)")
    PARAMETER(uint32_t, fp_div_s_interval, 3, "Cycles a half/single FP divide or sqrt holds its divider (0 = its full latency)")
    PARAMETER(uint32_t, fp_div_d_interval, 4, "Cycles a double FP divide or sqrt, or a vector one with SEW 64, holds its divider (0 = its full latency)")
    PARAMETER(std::string, selection, "age_matrix", "Selection mode: age_matrix | fifo | random")
    PARAMETER(uint32_t, num_phys_regs, 128, "Number of physical registers (for scoreboard sizing)")
    PARAMETER(std::vector<std::string>, scheduler_configs, std::vector<std::string>{}, "Partitioned scheduler configs: name:capacity:types")
    PARAMETER(std::vector<std::string>, routing_priorities, std::vector<std::string>{}, "Routing priorities for partitioned mode")
    PARAMETER(double, routing_dep_occupancy_limit, 0.9, "Dependency routing occupancy limit")
};

// ============================================================================
// Issue Unit
// ============================================================================
class Issue : public sparta::Unit {
    // Per physical register, the first cycle a consumer may issue and still meet the value on the bypass.
    class PhysicalScoreboard {
        static constexpr size_t kNumRegTypes = 3;
        static constexpr uint64_t kNever = UINT64_MAX;
        static size_t idx(core::RegType t) { return core::regTypeIndex(t); }

        std::array<std::vector<uint64_t>, kNumRegTypes> mReadyCycle;

       public:
        explicit PhysicalScoreboard(uint32_t num_physical_registers) {
            sparta_assert(num_physical_registers > 32 && num_physical_registers <= 65536,
                          "issue.params.num_phys_regs must be in (32, 65536], got " << num_physical_registers);
            for (size_t t = 0; t < kNumRegTypes; ++t) {
                mReadyCycle[t].assign(num_physical_registers, kNever);
                for (uint32_t i = 0; i < 32; ++i) mReadyCycle[t][i] = 0;
            }
        }

        void markNotReady(uint16_t phys_reg, core::RegType type) { mReadyCycle[idx(type)][phys_reg] = kNever; }

        // Never delays a wakeup already scheduled earlier.
        void readyAt(uint16_t phys_reg, core::RegType type, uint64_t cycle) {
            uint64_t& r = mReadyCycle[idx(type)][phys_reg];
            r = std::min(r, cycle);
        }

        bool sourcesReady(const std::vector<core::PhysRegRef>& phys_srcs, uint64_t cycle) const {
            for (const auto& src : phys_srcs)
                if (mReadyCycle[idx(src.type)][src.phys_reg] > cycle) return false;
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

    sparta::DataInPort<std::vector<core::PhysRegRef>> completion_lsq_in{&unit_port_set_, "completion_lsq_in"};

    // Flush support
    sparta::DataInPort<core::FlushRequest> flush_in{&unit_port_set_, "flush_in"};

    // Correct branch resolution - decrements depth
    sparta::DataInPort<core::BranchResolved> branch_resolved_in{&unit_port_set_, "branch_resolved_in"};

    void tick();

    bool isReadyForType(core::UopType t) const {
        for (uint8_t si : mEligibleSchedulers[static_cast<uint8_t>(t)])
            if (hasRoom_(si)) return true;
        return false;
    }

    bool isReady() const {
        for (size_t i = 0; i < mSchedulers.size(); ++i) {
            if ((mSchedulers[i].size() + mPendingReservations[i]) >= mSchedulers[i].capacity) return false;
        }
        return true;
    }

    // Routes the uop at dispatch and reserves its slot there; Rename carries the index in RenamedPacket::scheduler.
    uint8_t reserveSlot(const core::RenamedPacket& rpkt);

    void clearPendingReservations() { mPendingReservations.assign(mPendingReservations.size(), 0); }

    void setDownstream(midcore::Execute* execute) { mDownstream = execute; }
    void setVisualizer(core::PipelineVisualizer* v) { mVis = v; }
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

    // A load's data reaches the bypass at data_cycle: wake its consumers so they arrive there with it.
    void wakeOnLoadData(const std::vector<core::PhysRegRef>& dsts, uint64_t data_cycle);

    // In-order mode, where Rename issues straight to Execute: claims a port and any divider the uop needs.
    bool issueInorder(const core::DecodePacket& dp);

   private:
    void receivePackets_(const std::vector<core::RenamedPacket>& pkts);
    void receiveLsqCompletions_(const std::vector<core::PhysRegRef>& completions);
    void receiveFlush_(const core::FlushRequest& req);
    void receiveBranchResolved_(const core::BranchResolved& resolved);

    void buildUnified(uint32_t capacity, uint32_t width, const std::vector<std::string>& port_specs);
    void buildPartitioned(const std::vector<SchedulerConfig>& configs);

    // Free unpipelined unit for a ready uop: nullptr if it needs none, or sets blocked if all are busy.
    UnpipelinedUnit* unpipelinedUnitFor(core::UopType t, const core::PipelinePacket& pkt, bool& blocked);
    void occupyUnit_(UnpipelinedUnit& unit, core::UopType t, const core::PipelinePacket& pkt, uint8_t latency);
    bool canIssue(IssueScheduler& sched, const core::RenamedPacket& rpkt, IssuePort*& port, UnpipelinedUnit*& unit);
    void sendRegReadStage();

    // Slots already taken or reserved by this cycle's dispatches
    uint32_t occupied_(uint8_t si) const { return mSchedulers[si].size() + mPendingReservations[si]; }
    bool hasRoom_(uint8_t si) const { return occupied_(si) < mSchedulers[si].capacity; }
    float occupancy_(uint8_t si) const { return static_cast<float>(occupied_(si)) / mSchedulers[si].capacity; }

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

    // Counts slot as oldest or not oldest among the ready uops still waiting, then removes it from them.
    void countSelection_(const IssueScheduler& sched, uint32_t slot, std::bitset<kMaxIQSlots>& waiting);

    // Common post-selection logic: issue a single instruction from a scheduler slot
    void issueInstruction(IssueScheduler& sched, uint32_t slot, IssuePort& port, UnpipelinedUnit* unit);

    std::vector<IssueScheduler> mSchedulers;
    std::vector<uint32_t> mPendingReservations;                            // Track same-cycle dispatch reservations
    std::array<uint8_t, kNumUopTypes> mSchedulerForType{};                 // Primary scheduler per type
    std::array<std::vector<uint8_t>, kNumUopTypes> mEligibleSchedulers{};  // All eligible schedulers per type

    PhysicalScoreboard mScoreboard;
    midcore::Execute* mDownstream{nullptr};
    core::PipelineVisualizer* mVis{nullptr};
    PhysicalRegisterFile* mPrf{nullptr};

    // Selection and routing config
    SelectionMode mSelectionMode{SelectionMode::AgeMatrix};
    RoutingConfig mRoutingConfig;
    std::array<uint8_t, kNumUopTypes> mRoundRobinCounters{};  // Per-type RR state

    // Producer tracking for dependency-aware routing
    // mProducerScheduler[type_idx][phys_reg] = scheduler_idx or 255
    static constexpr size_t kNumRegTypes = 3;
    std::array<std::vector<uint8_t>, kNumRegTypes> mProducerScheduler;

    uint32_t mRegReadLatency{1};
    std::deque<std::pair<uint64_t, core::IssuePacket>> mRegReadPipe;  // (cycle it leaves for Execute, uop)
    std::vector<UnpipelinedUnit> mIntDividers;                        // empty = pipelined
    std::vector<UnpipelinedUnit> mFpDividers;
    uint32_t mIntDivInterval{0};
    uint32_t mFpDivSInterval{0};
    uint32_t mFpDivDInterval{0};
    uint64_t mInorderPortCycle{UINT64_MAX};  // cycle the ports were last reset for issueInorder

    // Flush state tracking - to filter incoming packets in same cycle as flush
    bool mFlushActive{false};
    uint64_t mFlushCycle{0};
    core::FlushRequest mActiveFlush;

    std::mt19937 mRng{42};  // Per-instance RNG for random selection

    std::vector<core::IssuePacket> mToIssueBuf;

    // Statistics
    sparta::Counter mNumIssued;
    sparta::Counter mNumStallCycles;
    sparta::Counter mNumLoadHitWakeups;
    sparta::Counter mNumPortConflicts;
    sparta::Counter mNumIntDivBusy;
    sparta::Counter mNumFpDivBusy;
    sparta::Counter mNumIntDivIssued;
    sparta::Counter mNumFpDivIssued;
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
