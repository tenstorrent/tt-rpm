// SPDX-FileCopyrightText: © 2026 Tenstorrent AI ULC
//
// SPDX-License-Identifier: Apache-2.0

#include "Midcore/Issue/Issue.hpp"

#include <algorithm>
#include <iostream>
#include <sstream>

#include "Common/BypassNetwork.hpp"
#include "Common/SpeculationUtils.hpp"
#include "Logging.hpp"
#include "Midcore/Execute/Execute.hpp"
#include "Midcore/Rename/Rename.hpp"

namespace {

core::UopType parseUopType(const std::string& s) {
    if (s == "ALU") return core::UopType::ALU;
    if (s == "Mul") return core::UopType::Mul;
    if (s == "Div") return core::UopType::Div;
    if (s == "Branch") return core::UopType::Branch;
    if (s == "Load") return core::UopType::Load;
    if (s == "Store") return core::UopType::Store;
    if (s == "FpOp") return core::UopType::FpOp;
    if (s == "VecOp") return core::UopType::VecOp;
    if (s == "Fence") return core::UopType::Fence;
    sparta_assert(false, "Unknown UopType: " << s);
    return core::UopType::ALU;
}

std::vector<core::UopType> parseUopTypeList(const std::string& str) {
    std::vector<core::UopType> types;
    std::istringstream ss(str);
    std::string token;
    while (std::getline(ss, token, '+')) {
        if (!token.empty()) types.push_back(parseUopType(token));
    }
    return types;
}

}  // namespace

namespace midcore {

Issue::Issue(sparta::TreeNode* node, const IssueParams* params)
    : sparta::Unit(node),
      mScoreboard(params->num_phys_regs),
      mSpeculativeWakeupEnabled(params->speculative_wakeup),
      mNumIssued(&unit_stat_set_, "num_issued", "Total instructions issued", sparta::Counter::COUNT_NORMAL),
      mNumStallCycles(&unit_stat_set_, "num_stall_cycles", "Cycles issue stalled on RAW hazard", sparta::Counter::COUNT_NORMAL),
      mNumSpeculativeWakeups(&unit_stat_set_, "num_speculative_wakeups", "Speculative wakeups fired", sparta::Counter::COUNT_NORMAL),
      mNumDepLocalRoutes(&unit_stat_set_, "num_dep_local_routes", "Times dependency locality routing succeeded", sparta::Counter::COUNT_NORMAL),
      mNumDepLocalFallback(&unit_stat_set_, "num_dep_local_fallback", "Times dependency locality routing fell back", sparta::Counter::COUNT_NORMAL),
      mNumTypeAffinityRoutes(&unit_stat_set_, "num_type_affinity_routes", "Times type affinity routing used", sparta::Counter::COUNT_NORMAL),
      mNumLeastOccupiedRoutes(&unit_stat_set_, "num_least_occupied_routes", "Times least-occupied routing used", sparta::Counter::COUNT_NORMAL),
      mNumRoundRobinRoutes(&unit_stat_set_, "num_round_robin_routes", "Times round-robin routing used", sparta::Counter::COUNT_NORMAL),
      mNumOldestSelected(&unit_stat_set_, "num_oldest_selected", "Times oldest ready was selected", sparta::Counter::COUNT_NORMAL),
      mNumNotOldestSelected(&unit_stat_set_, "num_not_oldest_selected", "Times non-oldest ready was selected", sparta::Counter::COUNT_NORMAL),
      mNumSquashed(&unit_stat_set_, "num_squashed", "Instructions squashed due to flush", sparta::Counter::COUNT_NORMAL) {
    in_port.registerConsumerHandler(CREATE_SPARTA_HANDLER_WITH_DATA(Issue, receivePackets_, std::vector<core::RenamedPacket>));
    completion_exe_in.registerConsumerHandler(CREATE_SPARTA_HANDLER_WITH_DATA(Issue, receiveExeCompletions_, std::vector<core::PhysRegRef>));
    completion_lsq_in.registerConsumerHandler(CREATE_SPARTA_HANDLER_WITH_DATA(Issue, receiveLsqCompletions_, std::vector<core::PhysRegRef>));
    flush_in.registerConsumerHandler(CREATE_SPARTA_HANDLER_WITH_DATA(Issue, receiveFlush_, core::FlushRequest));
    branch_resolved_in.registerConsumerHandler(CREATE_SPARTA_HANDLER_WITH_DATA(Issue, receiveBranchResolved_, core::BranchResolved));

    // Initialize producer tracking
    for (auto& vec : mProducerScheduler) {
        vec.assign(params->num_phys_regs, kNoScheduler);
    }

    // Parse selection mode
    std::string sel = params->selection;
    if (sel == "age_matrix" || sel.empty()) {
        mSelectionMode = SelectionMode::AgeMatrix;
    } else if (sel == "fifo") {
        mSelectionMode = SelectionMode::FIFO;
    } else if (sel == "random") {
        mSelectionMode = SelectionMode::Random;
    }

    std::string mode = params->mode;
    uint32_t iq_capacity = params->issue_queue_capacity;
    uint32_t iq_width = params->issue_width;

    if (mode == "partitioned") {
        // Parse scheduler_configs: "name:capacity:types" per entry
        std::vector<std::string> sched_strs = params->scheduler_configs;
        std::vector<SchedulerConfig> sched_configs;
        for (const auto& s : sched_strs) {
            std::istringstream ss(s);
            std::string sname, cap_str, types_str;
            std::getline(ss, sname, ':');
            std::getline(ss, cap_str, ':');
            std::getline(ss, types_str, ':');
            SchedulerConfig scfg;
            scfg.name = sname;
            scfg.capacity = cap_str.empty() ? 16 : std::stoul(cap_str);
            scfg.types = parseUopTypeList(types_str);
            // Default: one port per unit serving all types
            uint32_t ports_per = std::max(1u, iq_width / static_cast<uint32_t>(sched_strs.size()));
            for (uint32_t p = 0; p < ports_per; ++p) {
                SchedulerPortConfig pcfg;
                pcfg.name = sname + "_p" + std::to_string(p);
                pcfg.types = scfg.types;
                scfg.ports.push_back(std::move(pcfg));
            }
            sched_configs.push_back(std::move(scfg));
        }
        if (!sched_configs.empty()) {
            configurePartitioned(sched_configs);
        }

        // Parse routing priorities
        std::vector<std::string> prio_strs = params->routing_priorities;
        if (!prio_strs.empty()) {
            RoutingConfig rcfg;
            rcfg.priorities.clear();
            for (const auto& p : prio_strs) {
                if (p == "dependency_locality")
                    rcfg.priorities.push_back(RoutingPriority::DependencyLocality);
                else if (p == "type_affinity")
                    rcfg.priorities.push_back(RoutingPriority::TypeAffinity);
                else if (p == "least_occupied")
                    rcfg.priorities.push_back(RoutingPriority::LeastOccupied);
                else if (p == "round_robin")
                    rcfg.priorities.push_back(RoutingPriority::RoundRobin);
            }
            rcfg.dependency_occupancy_limit = static_cast<float>(static_cast<double>(params->routing_dep_occupancy_limit));
            rcfg.overflow_threshold = static_cast<float>(static_cast<double>(params->routing_overflow_threshold));
            configureRouting(rcfg);
        }
    } else {
        buildUnified(iq_capacity, iq_width);
    }

    // Read all remaining params (consumed by ChipSim during binding or unused in this mode)
    (void)static_cast<bool>(params->bypass_network_enabled);
    (void)static_cast<uint32_t>(params->bypass_regfile_read_latency);
    (void)params->bypass_paths.getValue();
    if (mode != "partitioned") {
        (void)params->scheduler_configs.getValue();
        (void)params->routing_priorities.getValue();
        (void)static_cast<double>(params->routing_dep_occupancy_limit);
        (void)static_cast<double>(params->routing_overflow_threshold);
    }

    ILOG("[issue] mode=" << mode << " selection=" << sel << " speculative_wakeup=" << mSpeculativeWakeupEnabled << " schedulers=" << mSchedulers.size());

    // Initialize pending reservations tracking
    mPendingReservations.assign(mSchedulers.size(), 0);
}

void Issue::buildUnified(uint32_t capacity, uint32_t width) {
    mSchedulers.clear();

    IssueScheduler sched;
    sched.name = "unified";
    sched.init(capacity);

    // All types are primary for unified scheduler
    sched.primary_types = {core::UopType::ALU,   core::UopType::Mul,  core::UopType::Div,   core::UopType::Branch, core::UopType::Load,
                           core::UopType::Store, core::UopType::FpOp, core::UopType::VecOp, core::UopType::Fence};

    for (uint32_t i = 0; i < width; ++i) {
        IssuePort port;
        port.name = "unified_p" + std::to_string(i);
        port.served_types = sched.primary_types;
        sched.ports.push_back(std::move(port));
    }

    mSchedulers.push_back(std::move(sched));

    // All types map to scheduler 0
    mSchedulerForType.fill(0);
    for (size_t t = 0; t < kNumUopTypes; ++t) {
        mEligibleSchedulers[t] = {0};
    }

    ILOG("[issue] unified mode: capacity=" << capacity << " ports=" << width);
}

void Issue::configurePartitioned(const std::vector<SchedulerConfig>& configs) { buildPartitioned(configs); }

void Issue::buildPartitioned(const std::vector<SchedulerConfig>& configs) {
    mSchedulers.clear();
    mSchedulerForType.fill(kNoScheduler);
    for (auto& vec : mEligibleSchedulers) {
        vec.clear();
    }

    uint8_t sched_idx = 0;
    for (const auto& cfg : configs) {
        IssueScheduler sched;
        sched.name = cfg.name;
        sched.init(cfg.capacity);
        sched.primary_types = cfg.types;
        sched.accepts_types = cfg.accepts;

        for (const auto& port_cfg : cfg.ports) {
            IssuePort port;
            port.name = port_cfg.name;
            port.served_types = port_cfg.types;
            sched.ports.push_back(std::move(port));
        }

        // Set primary scheduler for each type
        for (auto t : cfg.types) {
            uint8_t ti = static_cast<uint8_t>(t);
            if (mSchedulerForType[ti] == kNoScheduler) {
                mSchedulerForType[ti] = sched_idx;
            }
            mEligibleSchedulers[ti].push_back(sched_idx);
        }

        // Add to eligible list for accepted types too
        for (auto t : cfg.accepts) {
            uint8_t ti = static_cast<uint8_t>(t);
            mEligibleSchedulers[ti].push_back(sched_idx);
        }

        ILOG("[issue] scheduler '" << cfg.name << "': capacity=" << cfg.capacity << " ports=" << sched.ports.size() << " primary_types=" << cfg.types.size()
                                   << " accepts=" << cfg.accepts.size());

        mSchedulers.push_back(std::move(sched));
        ++sched_idx;
    }

    // Verify all types have a primary scheduler
    for (size_t i = 0; i < kNumUopTypes; ++i) {
        sparta_assert(mSchedulerForType[i] != kNoScheduler, "UopType " << i << " not assigned to any scheduler");
    }

    // Resize pending reservations to match the new scheduler count.
    // This is deferred here (not in the constructor) because partitioned
    // schedulers are set up after construction via configurePartitioned().
    mPendingReservations.assign(mSchedulers.size(), 0);
}

// ============================================================================
// Producer Tracking (for dependency-aware routing)
// ============================================================================

void Issue::recordProducerScheduler(const std::vector<core::PhysRegRef>& dsts, uint8_t sched_idx) {
    for (const auto& dst : dsts) {
        size_t type_idx = static_cast<size_t>(dst.type);
        if (type_idx < kNumRegTypes && dst.phys_reg < mProducerScheduler[type_idx].size()) {
            mProducerScheduler[type_idx][dst.phys_reg] = sched_idx;
        }
    }
}

void Issue::clearProducerScheduler(const std::vector<core::PhysRegRef>& dsts) {
    for (const auto& dst : dsts) {
        size_t type_idx = static_cast<size_t>(dst.type);
        if (type_idx < kNumRegTypes && dst.phys_reg < mProducerScheduler[type_idx].size()) {
            mProducerScheduler[type_idx][dst.phys_reg] = kNoScheduler;
        }
    }
}

uint8_t Issue::getProducerScheduler(const core::PhysRegRef& src) const {
    size_t type_idx = static_cast<size_t>(src.type);
    if (type_idx < kNumRegTypes && src.phys_reg < mProducerScheduler[type_idx].size()) {
        return mProducerScheduler[type_idx][src.phys_reg];
    }
    return kNoScheduler;
}

// ============================================================================
// Routing Logic (Entry Path)
// ============================================================================

static const char* routingPriorityName(RoutingPriority p) {
    switch (p) {
        case RoutingPriority::DependencyLocality:
            return "dependency_locality";
        case RoutingPriority::TypeAffinity:
            return "type_affinity";
        case RoutingPriority::LeastOccupied:
            return "least_occupied";
        case RoutingPriority::RoundRobin:
            return "round_robin";
    }
    return "unknown";
}

uint8_t Issue::selectScheduler(const core::RenamedPacket& rpkt) {
    for (RoutingPriority prio : mRoutingConfig.priorities) {
        uint8_t result = kNoScheduler;

        switch (prio) {
            case RoutingPriority::DependencyLocality:
                result = selectByDependency(rpkt);
                break;
            case RoutingPriority::TypeAffinity:
                result = selectByType(rpkt.uop_type);
                break;
            case RoutingPriority::LeastOccupied:
                result = selectLeastOccupied(rpkt.uop_type);
                break;
            case RoutingPriority::RoundRobin:
                result = selectRoundRobin(rpkt.uop_type);
                break;
        }

        if (result != kNoScheduler && !mSchedulers[result].isFull()) {
            ILOG("[issue] tag=" << rpkt.pkt.tag << " routed to scheduler " << static_cast<int>(result) << " (" << mSchedulers[result].name << ") via "
                                << routingPriorityName(prio));
            return result;
        }
    }

    // Ultimate fallback: primary scheduler for type
    uint8_t fallback = mSchedulerForType[static_cast<uint8_t>(rpkt.uop_type)];
    ILOG("[issue] tag=" << rpkt.pkt.tag << " routed to scheduler " << static_cast<int>(fallback) << " (" << mSchedulers[fallback].name << ") via fallback");
    return fallback;
}

uint8_t Issue::selectByDependency(const core::RenamedPacket& rpkt) {
    uint8_t hint = rpkt.producer_scheduler;
    if (hint == kNoScheduler || hint >= mSchedulers.size()) {
        ILOG("[issue] dependency_locality: tag=" << rpkt.pkt.tag << " has no producer hint");
        return kNoScheduler;
    }

    auto& sched = mSchedulers[hint];

    // Check if scheduler can accept this type
    if (!sched.canAccept(rpkt.uop_type)) {
        ILOG("[issue] dependency_locality: tag=" << rpkt.pkt.tag << " producer_sched=" << sched.name << " cannot accept type");
        ++mNumDepLocalFallback;
        return kNoScheduler;
    }

    // Check occupancy limit
    if (sched.occupancy() > mRoutingConfig.dependency_occupancy_limit) {
        ILOG("[issue] dependency_locality: tag=" << rpkt.pkt.tag << " producer_sched=" << sched.name << " over occupancy limit");
        ++mNumDepLocalFallback;
        return kNoScheduler;
    }

    ++mNumDepLocalRoutes;
    return hint;
}

uint8_t Issue::selectByType(core::UopType t) {
    uint8_t primary = mSchedulerForType[static_cast<uint8_t>(t)];
    if (primary != kNoScheduler && !mSchedulers[primary].isFull()) {
        ++mNumTypeAffinityRoutes;
        return primary;
    }
    return kNoScheduler;
}

uint8_t Issue::selectLeastOccupied(core::UopType t) {
    const auto& eligible = mEligibleSchedulers[static_cast<uint8_t>(t)];
    if (eligible.empty()) return kNoScheduler;

    uint8_t best = kNoScheduler;
    float best_occ = 2.0f;  // > 1.0 so first non-full wins

    for (uint8_t si : eligible) {
        auto& sched = mSchedulers[si];
        if (sched.isFull()) continue;
        float occ = sched.occupancy();
        if (occ < best_occ) {
            best_occ = occ;
            best = si;
        }
    }

    if (best != kNoScheduler) {
        ++mNumLeastOccupiedRoutes;
    }
    return best;
}

uint8_t Issue::selectRoundRobin(core::UopType t) {
    uint8_t ti = static_cast<uint8_t>(t);
    const auto& eligible = mEligibleSchedulers[ti];
    if (eligible.empty()) return kNoScheduler;

    uint8_t& counter = mRoundRobinCounters[ti];
    size_t start = counter % eligible.size();

    for (size_t i = 0; i < eligible.size(); ++i) {
        size_t idx = (start + i) % eligible.size();
        uint8_t si = eligible[idx];
        if (!mSchedulers[si].isFull()) {
            counter = static_cast<uint8_t>((idx + 1) % eligible.size());
            ++mNumRoundRobinRoutes;
            return si;
        }
    }

    return kNoScheduler;
}

// ============================================================================
// Packet Reception
// ============================================================================

void Issue::receivePackets_(const std::vector<core::RenamedPacket>& pkts) {
    // Clear pending reservations now that we're receiving the actual packets
    clearPendingReservations();

    uint64_t cycle = getClock()->currentCycle();

    // Flush state is active for packets arriving in the same cycle or next cycle
    // (packets from Rename have 1 cycle latency, flush has 0 cycle latency)
    bool within_flush_window = mFlushActive && (cycle <= mFlushCycle + 1);

    // Clear flush state after the window expires
    if (mFlushActive && cycle > mFlushCycle + 1) {
        mFlushActive = false;
    }

    for (const auto& rpkt : pkts) {
        // Filter packets using fetch_seq for stale instruction detection
        // This handles race conditions where depth=0 instructions arrive after a BP redirect
        if (mFlushActive && rpkt.pkt.fetch_seq <= mActiveFlush.branch_fetch_seq && rpkt.pkt.tag > mActiveFlush.branch_tag) {
            continue;  // Skip this packet
        }

        // Filter packets that should be squashed by an active flush
        if (within_flush_window && core::shouldSquash(rpkt.pkt, mActiveFlush)) {
            continue;  // Skip this packet
        }

        // Check for duplicate tags - remove old entry if present
        // This handles tag reuse when Whisper is flushed but depth=0 instructions
        // in the pipeline are protected from squashing by shouldSquash.
        for (auto& sched : mSchedulers) {
            for (uint32_t i = 0; i < sched.capacity; ++i) {
                if (sched.slots[i].pkt && sched.slots[i].pkt->pkt.tag == rpkt.pkt.tag) {
                    clearProducerScheduler(sched.slots[i].pkt->phys_dsts);
                    sched.remove(i);
                    ++mNumSquashed;
                }
            }
        }

        // Mark destinations as not ready
        for (const auto& dst : rpkt.phys_dsts) {
            mScoreboard.markNotReady(dst.phys_reg, dst.type);
        }

        // Select scheduler using priority chain
        uint8_t sched_idx = selectScheduler(rpkt);
        sparta_assert(sched_idx < mSchedulers.size(), "Invalid scheduler index");

        // Insert into scheduler
        uint32_t slot = mSchedulers[sched_idx].insert(rpkt);
        sparta_assert(slot != kInvalidSlot, "Failed to insert into scheduler");

        // Record producer location for future dependents
        recordProducerScheduler(rpkt.phys_dsts, sched_idx);

        if (mVis) mVis->onIqEnter(rpkt.pkt.fetch_seq, getClock()->currentCycle());
    }
}

void Issue::receiveExeCompletions_(const std::vector<core::PhysRegRef>& completions) {
    // When speculative wakeup is enabled, it owns the timing for non-memory ops
    if (mSpeculativeWakeupEnabled) return;

    for (const auto& ref : completions) {
        mScoreboard.markReady(ref.phys_reg, ref.type);
    }
}

void Issue::receiveLsqCompletions_(const std::vector<core::PhysRegRef>& completions) {
    // Load completions from LSQ - always mark scoreboard
    for (const auto& ref : completions) {
        mScoreboard.markReady(ref.phys_reg, ref.type);
    }
}

void Issue::receiveFlush_(const core::FlushRequest& req) {
    uint64_t cycle = getClock()->currentCycle();
    ILOG("[issue] cycle " << cycle << " RECEIVED flush: branch_tag=" << req.branch_tag << " branch_depth=" << static_cast<int>(req.branch_depth)
                          << " source=" << static_cast<int>(req.source));

    // Store flush state to filter packets arriving in the same cycle
    // Only update if this is a "stronger" flush (lower tag at same depth, or Execute vs BP)
    bool should_update = !mFlushActive || req.source == core::FlushSource::Execute || (req.branch_tag < mActiveFlush.branch_tag);
    if (should_update) {
        mFlushActive = true;
        mFlushCycle = cycle;
        mActiveFlush = req;
    }

    uint32_t total_squashed = 0;
    for (auto& sched : mSchedulers) {
        total_squashed += sched.squash([&req, cycle, this](const core::RenamedPacket& pkt) {
            bool should_sq = core::shouldSquash(pkt.pkt, req);
            if (should_sq) {
                ILOG("[issue] cycle " << cycle << " SQUASHING tag=" << pkt.pkt.tag << " depth=" << static_cast<int>(pkt.pkt.wrong_path_depth));
                if (mVis) mVis->onSquash(pkt.pkt.fetch_seq, cycle);
            }
            return should_sq;
        });
    }

    // Clear pending wakeups for squashed instructions
    auto it = mPendingWakeups.begin();
    while (it != mPendingWakeups.end()) {
        // Pending wakeups don't have tags - clear all to be safe
        // A more refined approach would track tags in PendingWakeup
        ++it;
    }

    mNumSquashed += total_squashed;
    ILOG("[issue] cycle " << cycle << " Squashed " << total_squashed << " entries");
}

void Issue::receiveBranchResolved_(const core::BranchResolved& resolved) {
    // When a branch at depth D resolves correctly, decrement depth of younger entries
    uint64_t cycle = getClock()->currentCycle();
    uint32_t num_decremented = 0;

    for (auto& sched : mSchedulers) {
        num_decremented += sched.decrementDepth(resolved.branch_tag, resolved.resolved_depth);
    }

    if (num_decremented > 0) {
        ILOG("[issue] cycle " << cycle << " BRANCH RESOLVED: tag=" << resolved.branch_tag << " depth=" << static_cast<int>(resolved.resolved_depth)
                              << " decremented " << num_decremented << " entries");
    }
}

// ============================================================================
// Speculative Wakeup
// ============================================================================

void Issue::processSpeculativeWakeups() {
    if (!mSpeculativeWakeupEnabled) return;

    for (auto it = mPendingWakeups.begin(); it != mPendingWakeups.end();) {
        if (--it->cycles_remaining == 0) {
            for (const auto& dst : it->phys_dsts) {
                mScoreboard.markReady(dst.phys_reg, dst.type);
            }
            ++mNumSpeculativeWakeups;
            it = mPendingWakeups.erase(it);
        } else {
            ++it;
        }
    }
}

void Issue::scheduleSpeculativeWakeup(const core::IssuePacket& ipkt) {
    if (!mSpeculativeWakeupEnabled) return;
    if (isMemoryType(ipkt.uop_type)) return;
    if (ipkt.phys_dsts.empty()) return;

    // Compute wakeup latency based on bypass network availability.
    //
    // Timeline for a dependent consumer:
    //   Producer issues at cycle N
    //   Producer completes execution at N + exec_latency
    //
    //   WITH BYPASS (enabled, path exists):
    //     Result available to consumer at: N + exec_latency + bypass_path_latency
    //     Typically bypass_path_latency = 0 (same cycle as exec completes)
    //
    //   WITHOUT BYPASS (disabled, or no path):
    //     Producer writes to PRF, consumer reads from PRF
    //     Result available at: N + exec_latency + prf_read_latency
    //     Typically prf_read_latency = 1 (one extra cycle to read from regfile)
    //
    uint8_t base_latency = ipkt.latency;  // Execution latency
    uint8_t additional_latency = 0;

    if (mBypassNetwork && mBypassNetwork->enabled()) {
        // Get minimum bypass latency for this producer type
        uint8_t producer_fu = static_cast<uint8_t>(ipkt.uop_type);
        uint8_t bypass_lat = mBypassNetwork->getMinBypassLatency(producer_fu);

        if (bypass_lat != BypassNetwork::kNoBypass) {
            // Bypass path exists - use bypass latency (typically 0)
            additional_latency = bypass_lat;
        } else {
            // No bypass path configured for this FU - fall back to PRF read
            additional_latency = mBypassNetwork->regfileReadLatency();
        }
    } else {
        // Bypass disabled - consumer must wait for PRF read
        // Use a default of 1 cycle if no bypass network configured
        additional_latency = mBypassNetwork ? mBypassNetwork->regfileReadLatency() : 1;
    }

    uint8_t wakeup_latency = base_latency + additional_latency;

    mPendingWakeups.emplace_back(PendingWakeup{.phys_dsts = ipkt.phys_dsts, .cycles_remaining = wakeup_latency});
}

// ============================================================================
// Selection Logic (Exit Path)
// ============================================================================

void Issue::selectFromScheduler(IssueScheduler& sched) {
    switch (mSelectionMode) {
        case SelectionMode::AgeMatrix:
            selectAgeMatrix(sched);
            break;
        case SelectionMode::FIFO:
            selectFIFO(sched);
            break;
        case SelectionMode::Random:
            selectRandom(sched);
            break;
    }
}

bool Issue::sourcesReadyWithBypass(const std::vector<core::PhysRegRef>& phys_srcs) const {
    // With the new bypass-aware speculative wakeup, the scoreboard is updated
    // at the appropriate time (accounting for bypass path latency or PRF read latency).
    // Selection just needs to check the scoreboard.
    return mScoreboard.sourcesReady(phys_srcs);
}

void Issue::issueInstruction(IssueScheduler& sched, uint32_t slot, IssuePort& port) {
    auto& entry = sched.slots[slot];
    sparta_assert(entry.pkt.has_value(), "Selected empty slot");
    const auto& rpkt = *entry.pkt;

    port.used_this_cycle = true;
    mDownstream->reserveSlot(rpkt.uop_type);

    if (mPrf && mPrf->bankingEnabled()) {
        mPrf->consumeReads(rpkt.phys_srcs);
    }

    auto ipkt = core::IssuePacket::from(rpkt);

    ILOG("[issue] selected tag=" << ipkt.pkt.tag << " (slot=" << slot << ") from " << sched.name);

    if (mVis) mVis->onIssue(ipkt.pkt.fetch_seq, getClock()->currentCycle());

    scheduleSpeculativeWakeup(ipkt);

    mToIssueBuf.push_back(std::move(ipkt));
    sched.remove(slot);
}

void Issue::selectAgeMatrix(IssueScheduler& sched) {
    // Build ready mask - consider both scoreboard and bypass network
    // Only iterate over valid (occupied) slots using the age matrix valid mask
    std::bitset<kMaxIQSlots> ready_mask;
    const auto& valid_mask = sched.ages.validMask();
    for (uint32_t slot = 0; slot < sched.capacity; ++slot) {
        if (!valid_mask.test(slot)) continue;
        if (sourcesReadyWithBypass(sched.slots[slot].pkt->phys_srcs)) {
            ready_mask.set(slot);
        }
    }

    size_t num_ready = ready_mask.count();
    if (num_ready > 0) {
        ILOG("[issue] age_matrix selection: " << num_ready << " ready in " << sched.name);
    }

    // Repeatedly select oldest ready until no more ports or no more ready
    while (ready_mask.any()) {
        uint32_t oldest_slot = sched.ages.selectOldest(ready_mask);
        if (oldest_slot == kInvalidSlot) break;

        const auto& rpkt = *sched.slots[oldest_slot].pkt;

        IssuePort* port = sched.findAvailablePort(rpkt.uop_type);
        if (!port) {
            ready_mask.reset(oldest_slot);
            ++mNumNotOldestSelected;
            continue;
        }

        if (!mDownstream->isReadyForType(rpkt.uop_type)) {
            ready_mask.reset(oldest_slot);
            ++mNumNotOldestSelected;
            continue;
        }

        if (mPrf && mPrf->bankingEnabled() && !mPrf->canReadRegs(rpkt.phys_srcs)) {
            ready_mask.reset(oldest_slot);
            ++mNumNotOldestSelected;
            continue;
        }

        issueInstruction(sched, oldest_slot, *port);
        ready_mask.reset(oldest_slot);
        ++mNumOldestSelected;
    }
}

void Issue::selectFIFO(IssueScheduler& sched) {
    // Iterate slots in order (0 to N-1), treating lower index as "older"
    // This is a simplified FIFO that doesn't perfectly match deque ordering
    // but is consistent with slot-based storage
    for (uint32_t slot = 0; slot < sched.capacity; ++slot) {
        auto& entry = sched.slots[slot];
        if (!entry.pkt.has_value()) continue;

        const auto& rpkt = *entry.pkt;

        if (!sourcesReadyWithBypass(rpkt.phys_srcs)) continue;

        IssuePort* port = sched.findAvailablePort(rpkt.uop_type);
        if (!port) continue;

        if (!mDownstream->isReadyForType(rpkt.uop_type)) continue;

        if (mPrf && mPrf->bankingEnabled() && !mPrf->canReadRegs(rpkt.phys_srcs)) continue;

        issueInstruction(sched, slot, *port);
    }
}

void Issue::selectRandom(IssueScheduler& sched) {
    // Build list of ready slots (considering bypass network)
    std::vector<uint32_t> ready_slots;
    for (uint32_t slot = 0; slot < sched.capacity; ++slot) {
        if (!sched.slots[slot].pkt.has_value()) continue;
        if (sourcesReadyWithBypass(sched.slots[slot].pkt->phys_srcs)) {
            ready_slots.push_back(slot);
        }
    }

    std::shuffle(ready_slots.begin(), ready_slots.end(), mRng);

    for (uint32_t slot : ready_slots) {
        auto& entry = sched.slots[slot];
        if (!entry.pkt.has_value()) continue;

        const auto& rpkt = *entry.pkt;

        IssuePort* port = sched.findAvailablePort(rpkt.uop_type);
        if (!port) continue;

        if (!mDownstream->isReadyForType(rpkt.uop_type)) continue;

        if (mPrf && mPrf->bankingEnabled() && !mPrf->canReadRegs(rpkt.phys_srcs)) continue;

        issueInstruction(sched, slot, *port);
    }
}

// ============================================================================
// Main Tick
// ============================================================================

void Issue::tick() {
    processSpeculativeWakeups();

    // Reset register file bank usage for this cycle
    if (mPrf && mPrf->bankingEnabled()) {
        mPrf->resetBanks();
    }

    // Tick bypass network
    if (mBypassNetwork && mBypassNetwork->enabled()) {
        mBypassNetwork->tick(getClock()->currentCycle());
    }

    mToIssueBuf.clear();

    for (auto& sched : mSchedulers) {
        sched.resetPorts();
        selectFromScheduler(sched);
    }

    if (mToIssueBuf.empty()) {
        bool has_work = false;
        for (const auto& s : mSchedulers) {
            if (s.size() > 0) {
                has_work = true;
                break;
            }
        }
        if (has_work) ++mNumStallCycles;
    } else {
        mNumIssued += mToIssueBuf.size();
        out_port.send(mToIssueBuf, 1);
    }
}

}  // namespace midcore
