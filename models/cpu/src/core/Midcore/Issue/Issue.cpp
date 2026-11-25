// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Tenstorrent USA, Inc.

#include "Midcore/Issue/Issue.hpp"

#include <algorithm>
#include <iostream>
#include <sstream>

#include "Common/FpOpClass.hpp"
#include "Common/ParamChoice.hpp"
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

constexpr const char* kUopTypeNames[] = {"ALU", "Mul", "Div", "Branch", "Load", "Store", "FpOp", "VecOp", "Fence"};
static_assert(std::size(kUopTypeNames) == core::kNumUopTypes);

}  // namespace

namespace midcore {

Issue::Issue(sparta::TreeNode* node, const IssueParams* params)
    : sparta::Unit(node),
      mScoreboard(params->num_phys_regs),
      mRegReadLatency(params->regfile_read_latency),
      mIntDividers(params->int_div_units),
      mFpDividers(params->fp_div_units),
      mIntDivInterval(params->int_div_interval),
      mFpDivSInterval(params->fp_div_s_interval),
      mFpDivDInterval(params->fp_div_d_interval),
      mNumIssued(&unit_stat_set_, "num_issued", "Total instructions issued", sparta::Counter::COUNT_NORMAL),
      mNumStallCycles(&unit_stat_set_, "num_stall_cycles", "Cycles with uops in the schedulers and none issued", sparta::Counter::COUNT_NORMAL),
      mNumLoadHitWakeups(&unit_stat_set_, "num_load_hit_wakeups", "Load destinations woken from the D-cache hit, ahead of write-back",
                         sparta::Counter::COUNT_NORMAL),
      mNumPortConflicts(&unit_stat_set_, "num_port_conflicts", "Ready uop-cycles held because no free issue port accepts the uop type",
                        sparta::Counter::COUNT_NORMAL),
      mNumIntDivBusy(&unit_stat_set_, "num_int_div_busy", "Ready divide uop-cycles held because every integer divider was busy", sparta::Counter::COUNT_NORMAL),
      mNumFpDivBusy(&unit_stat_set_, "num_fp_div_busy", "Ready FP divide/sqrt uop-cycles held because every FP divider was busy",
                    sparta::Counter::COUNT_NORMAL),
      mNumIntDivIssued(&unit_stat_set_, "num_int_div_issued", "Divides issued to an integer divider (busy int_div_interval cycles each)",
                       sparta::Counter::COUNT_NORMAL),
      mNumFpDivIssued(&unit_stat_set_, "num_fp_div_issued", "FP and vector divides and square roots issued to an FP divider", sparta::Counter::COUNT_NORMAL),
      mNumDepLocalRoutes(&unit_stat_set_, "num_dep_local_routes", "Times dependency locality routing succeeded", sparta::Counter::COUNT_NORMAL),
      mNumDepLocalFallback(&unit_stat_set_, "num_dep_local_fallback", "Times dependency locality routing fell back", sparta::Counter::COUNT_NORMAL),
      mNumTypeAffinityRoutes(&unit_stat_set_, "num_type_affinity_routes", "Times type affinity routing used", sparta::Counter::COUNT_NORMAL),
      mNumLeastOccupiedRoutes(&unit_stat_set_, "num_least_occupied_routes", "Times least-occupied routing used", sparta::Counter::COUNT_NORMAL),
      mNumRoundRobinRoutes(&unit_stat_set_, "num_round_robin_routes", "Times round-robin routing used", sparta::Counter::COUNT_NORMAL),
      mNumOldestSelected(&unit_stat_set_, "num_oldest_selected", "Uops issued with no older ready uop left waiting in their scheduler",
                         sparta::Counter::COUNT_NORMAL),
      mNumNotOldestSelected(&unit_stat_set_, "num_not_oldest_selected", "Uops issued ahead of an older ready uop in their scheduler that could not issue",
                            sparta::Counter::COUNT_NORMAL),
      mNumSquashed(&unit_stat_set_, "num_squashed", "Instructions squashed due to flush", sparta::Counter::COUNT_NORMAL) {
    in_port.registerConsumerHandler(CREATE_SPARTA_HANDLER_WITH_DATA(Issue, receivePackets_, std::vector<core::RenamedPacket>));
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
    } else {
        sparta_assert(false, params->selection.getLocation() << " = '" << sel << "'; expected one of: age_matrix, fifo, random");
    }

    std::string mode = core::checkChoice(params->mode, {"unified", "partitioned"});
    uint32_t iq_capacity = params->issue_queue_capacity;
    uint32_t iq_width = params->issue_width;

    const std::vector<std::string> port_specs = params->issue_ports;
    if (mode == "partitioned") {
        sparta_assert(port_specs.empty(), params->issue_ports.getLocation()
                                              << " applies to unified mode only; partitioned mode takes ports from scheduler_configs");
        // Parse scheduler_configs: "name:capacity:types" per entry
        std::vector<std::string> sched_strs = params->scheduler_configs;
        sparta_assert(!sched_strs.empty(), params->scheduler_configs.getLocation() << " is empty; partitioned mode needs at least one scheduler");
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
        configurePartitioned(sched_configs);

        // Parse routing priorities
        std::vector<std::string> prio_strs = params->routing_priorities;
        RoutingConfig rcfg;
        if (!prio_strs.empty()) {
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
                else
                    sparta_assert(false, params->routing_priorities.getLocation()
                                             << ": unknown '" << p << "'; expected dependency_locality, type_affinity, least_occupied or round_robin");
            }
        }
        rcfg.dependency_occupancy_limit = static_cast<float>(static_cast<double>(params->routing_dep_occupancy_limit));
        configureRouting(rcfg);
    } else {
        buildUnified(iq_capacity, iq_width, port_specs);
    }

    // Read all remaining params (unused in this mode)
    if (mode != "partitioned") {
        (void)params->scheduler_configs.getValue();
        (void)params->routing_priorities.getValue();
        (void)static_cast<double>(params->routing_dep_occupancy_limit);
    }

    ILOG("[issue] mode=" << mode << " selection=" << sel << " regfile_read_latency=" << mRegReadLatency << " schedulers=" << mSchedulers.size());

    // Initialize pending reservations tracking
    mPendingReservations.assign(mSchedulers.size(), 0);
}

void Issue::buildUnified(uint32_t capacity, uint32_t width, const std::vector<std::string>& port_specs) {
    mSchedulers.clear();

    IssueScheduler sched;
    sched.name = "unified";
    sched.init(capacity);

    // All types are primary for unified scheduler
    sched.primary_types = {core::UopType::ALU,   core::UopType::Mul,  core::UopType::Div,   core::UopType::Branch, core::UopType::Load,
                           core::UopType::Store, core::UopType::FpOp, core::UopType::VecOp, core::UopType::Fence};

    sparta_assert(port_specs.empty() || port_specs.size() == width,
                  "issue.params.issue_ports lists " << port_specs.size() << " ports but issue.params.issue_width is " << width);
    for (uint32_t i = 0; i < width; ++i) {
        IssuePort port;
        port.name = "unified_p" + std::to_string(i);
        port.served_types = port_specs.empty() ? sched.primary_types : parseUopTypeList(port_specs[i]);
        sched.ports.push_back(std::move(port));
    }
    for (size_t t = 0; t < kNumUopTypes; ++t) {
        bool served = false;
        for (const auto& p : sched.ports) served |= p.canServe(static_cast<core::UopType>(t));
        sparta_assert(served, "issue.params.issue_ports: no port accepts uop type " << kUopTypeNames[t]);
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

        ILOG("[issue] scheduler '" << cfg.name << "': capacity=" << cfg.capacity << " ports=" << sched.ports.size() << " primary_types=" << cfg.types.size());

        mSchedulers.push_back(std::move(sched));
        ++sched_idx;
    }

    // Verify all types have a primary scheduler
    for (size_t i = 0; i < kNumUopTypes; ++i) {
        sparta_assert(mSchedulerForType[i] != kNoScheduler, "UopType " << i << " not assigned to any scheduler");
    }

    // Resize pending reservations to match the new scheduler count.
    mPendingReservations.assign(mSchedulers.size(), 0);
}

// ============================================================================
// Producer Tracking (for dependency-aware routing)
// ============================================================================

void Issue::recordProducerScheduler(const std::vector<core::PhysRegRef>& dsts, uint8_t sched_idx) {
    for (const auto& dst : dsts) {
        if (!core::isRegFileType(dst.type)) continue;
        size_t type_idx = core::regTypeIndex(dst.type);
        if (dst.phys_reg < mProducerScheduler[type_idx].size()) {
            mProducerScheduler[type_idx][dst.phys_reg] = sched_idx;
        }
    }
}

void Issue::clearProducerScheduler(const std::vector<core::PhysRegRef>& dsts) {
    for (const auto& dst : dsts) {
        if (!core::isRegFileType(dst.type)) continue;
        size_t type_idx = core::regTypeIndex(dst.type);
        if (dst.phys_reg < mProducerScheduler[type_idx].size()) {
            mProducerScheduler[type_idx][dst.phys_reg] = kNoScheduler;
        }
    }
}

uint8_t Issue::getProducerScheduler(const core::PhysRegRef& src) const {
    if (!core::isRegFileType(src.type)) return kNoScheduler;
    size_t type_idx = core::regTypeIndex(src.type);
    if (src.phys_reg < mProducerScheduler[type_idx].size()) {
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

        if (result != kNoScheduler && hasRoom_(result)) {
            ILOG("[issue] tag=" << rpkt.pkt.tag << " routed to scheduler " << static_cast<int>(result) << " (" << mSchedulers[result].name << ") via "
                                << routingPriorityName(prio));
            return result;
        }
    }

    // Fallback: the first scheduler with room that accepts the type; the primary one is listed first.
    const uint8_t ti = static_cast<uint8_t>(rpkt.uop_type);
    for (uint8_t fallback : mEligibleSchedulers[ti]) {
        if (!hasRoom_(fallback)) continue;
        ILOG("[issue] tag=" << rpkt.pkt.tag << " routed to scheduler " << static_cast<int>(fallback) << " (" << mSchedulers[fallback].name << ") via fallback");
        return fallback;
    }
    sparta_assert(false, "no issue scheduler has room for uop type " << kUopTypeNames[ti] << "; Rename must check isReadyForType first");
    return kNoScheduler;
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
    if (occupancy_(hint) > mRoutingConfig.dependency_occupancy_limit) {
        ILOG("[issue] dependency_locality: tag=" << rpkt.pkt.tag << " producer_sched=" << sched.name << " over occupancy limit");
        ++mNumDepLocalFallback;
        return kNoScheduler;
    }

    ++mNumDepLocalRoutes;
    return hint;
}

uint8_t Issue::selectByType(core::UopType t) {
    uint8_t primary = mSchedulerForType[static_cast<uint8_t>(t)];
    if (primary != kNoScheduler && hasRoom_(primary)) {
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
        if (!hasRoom_(si)) continue;
        float occ = occupancy_(si);
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
        if (hasRoom_(si)) {
            counter = static_cast<uint8_t>((idx + 1) % eligible.size());
            ++mNumRoundRobinRoutes;
            return si;
        }
    }

    return kNoScheduler;
}

uint8_t Issue::reserveSlot(const core::RenamedPacket& rpkt) {
    uint8_t si = selectScheduler(rpkt);
    ++mPendingReservations[si];
    return si;
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

        uint8_t sched_idx = rpkt.scheduler;
        sparta_assert(sched_idx < mSchedulers.size(), "Invalid scheduler index");

        // Insert into scheduler
        uint32_t slot = mSchedulers[sched_idx].insert(rpkt);
        sparta_assert(slot != kInvalidSlot, "Failed to insert into scheduler");

        // Record producer location for future dependents
        recordProducerScheduler(rpkt.phys_dsts, sched_idx);

        if (mVis) mVis->onIqEnter(rpkt.pkt.fetch_seq, getClock()->currentCycle());
    }
}

void Issue::receiveLsqCompletions_(const std::vector<core::PhysRegRef>& completions) {
    // Load write-back: wakes loads that wakeOnLoadData() did not (misses, forwarded, multi-line).
    const uint64_t cycle = getClock()->currentCycle();
    for (const auto& ref : completions) {
        mScoreboard.readyAt(ref.phys_reg, ref.type, cycle);
    }
}

void Issue::wakeOnLoadData(const std::vector<core::PhysRegRef>& dsts, uint64_t data_cycle) {
    // A consumer issued at c executes at c + regfile_read_latency + 1.
    const uint64_t issue_cycle = data_cycle > mRegReadLatency ? data_cycle - mRegReadLatency - 1 : 0;
    for (const auto& ref : dsts) {
        mScoreboard.readyAt(ref.phys_reg, ref.type, issue_cycle);
    }
    ++mNumLoadHitWakeups;
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

    for (auto it = mRegReadPipe.begin(); it != mRegReadPipe.end();) {
        if (core::shouldSquash(it->second.pkt, req)) {
            if (mVis) mVis->onSquash(it->second.pkt.fetch_seq, cycle);
            it = mRegReadPipe.erase(it);
            ++total_squashed;
        } else {
            ++it;
        }
    }

    // A squashed divide frees its unit, as Execute drops it on the same predicate.
    for (auto* units : {&mIntDividers, &mFpDividers}) {
        for (auto& u : *units) {
            if (u.busy_until > cycle && core::shouldSquash(u.holder, req)) u.busy_until = 0;
        }
    }

    mNumSquashed += total_squashed;
    ILOG("[issue] cycle " << cycle << " Squashed " << total_squashed << " entries");
}

void Issue::receiveBranchResolved_(const core::BranchResolved& resolved) {
    // When a branch at depth D resolves correctly, decrement depth of younger entries
    uint64_t cycle = getClock()->currentCycle();
    uint32_t num_decremented = 0;

    for (auto& sched : mSchedulers) {
        num_decremented += sched.decrementDepth(resolved);
    }

    if (num_decremented > 0) {
        ILOG("[issue] cycle " << cycle << " BRANCH RESOLVED: tag=" << resolved.branch_tag << " depth=" << static_cast<int>(resolved.resolved_depth)
                              << " decremented " << num_decremented << " entries");
    }
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

UnpipelinedUnit* Issue::unpipelinedUnitFor(core::UopType t, const core::PipelinePacket& pkt, bool& blocked) {
    std::vector<UnpipelinedUnit>* units = nullptr;
    if (t == core::UopType::Div) {
        units = &mIntDividers;
    } else if (pkt.inst && ((t == core::UopType::FpOp && core::fpOpClass(pkt.inst->getTraceRecord().inst) == core::FpOpClass::DivSqrt) ||
                            (t == core::UopType::VecOp && core::vecFpIsDivSqrt(pkt.inst->getTraceRecord().inst)))) {
        units = &mFpDividers;
    }
    if (!units || units->empty()) return nullptr;

    const uint64_t cycle = getClock()->currentCycle();
    for (auto& u : *units) {
        if (u.busy_until <= cycle) return &u;
    }
    blocked = true;
    ++(units == &mIntDividers ? mNumIntDivBusy : mNumFpDivBusy);
    return nullptr;
}

bool Issue::canIssue(IssueScheduler& sched, const core::RenamedPacket& rpkt, IssuePort*& port, UnpipelinedUnit*& unit) {
    bool blocked = false;
    unit = unpipelinedUnitFor(rpkt.uop_type, rpkt.pkt, blocked);
    if (blocked) return false;
    port = sched.findAvailablePort(rpkt.uop_type);
    if (!port) {
        ++mNumPortConflicts;
        return false;
    }
    if (!mDownstream->isReadyForType(rpkt.uop_type)) return false;
    if (mPrf && mPrf->bankingEnabled() && !mPrf->canReadRegs(rpkt.phys_srcs)) return false;
    return true;
}

// The interval is the divider's issue interval (reciprocal throughput); the result still takes the full latency.
void Issue::occupyUnit_(UnpipelinedUnit& unit, core::UopType t, const core::PipelinePacket& pkt, uint8_t latency) {
    uint32_t interval = mIntDivInterval;
    if (t == core::UopType::Div) {
        ++mNumIntDivIssued;
    } else {
        interval = pkt.inst->getFpDivPrecision() == cpu::FpDivPrecision::Single ? mFpDivSInterval : mFpDivDInterval;
        ++mNumFpDivIssued;
    }
    unit.busy_until = getClock()->currentCycle() + (interval ? interval : latency);
    unit.holder = pkt;
}

bool Issue::issueInorder(const core::DecodePacket& dp) {
    const uint64_t cycle = getClock()->currentCycle();
    if (mInorderPortCycle != cycle) {
        for (auto& s : mSchedulers) s.resetPorts();
        mInorderPortCycle = cycle;
    }
    bool blocked = false;
    UnpipelinedUnit* unit = unpipelinedUnitFor(dp.uop_type, dp.pkt, blocked);
    if (blocked) return false;
    IssuePort* port = mSchedulers[mSchedulerForType[static_cast<uint8_t>(dp.uop_type)]].findAvailablePort(dp.uop_type);
    if (!port) {
        ++mNumPortConflicts;
        return false;
    }
    port->used_this_cycle = true;
    if (unit) occupyUnit_(*unit, dp.uop_type, dp.pkt, dp.latency);
    ++mNumIssued;
    return true;
}

void Issue::issueInstruction(IssueScheduler& sched, uint32_t slot, IssuePort& port, UnpipelinedUnit* unit) {
    auto& entry = sched.slots[slot];
    sparta_assert(entry.pkt.has_value(), "Selected empty slot");
    const auto& rpkt = *entry.pkt;
    const uint64_t cycle = getClock()->currentCycle();

    port.used_this_cycle = true;
    mDownstream->reserveSlot(rpkt.uop_type);
    if (unit) occupyUnit_(*unit, rpkt.uop_type, rpkt.pkt, rpkt.latency);

    if (mPrf && mPrf->bankingEnabled()) {
        mPrf->consumeReads(rpkt.phys_srcs);
    }

    auto ipkt = core::IssuePacket::from(rpkt);

    ILOG("[issue] selected tag=" << ipkt.pkt.tag << " (slot=" << slot << ") from " << sched.name);

    if (mVis) mVis->onIssue(ipkt.pkt.fetch_seq, cycle);

    // Fixed-latency results reach consumers on the bypass; loads and atomics wake when their data is known.
    if (ipkt.uop_type != core::UopType::Load && ipkt.pkt.inst_class != cpu::InstClass::Atomic) {
        for (const auto& dst : ipkt.phys_dsts) mScoreboard.readyAt(dst.phys_reg, dst.type, cycle + ipkt.latency);
    }

    mToIssueBuf.push_back(std::move(ipkt));
    sched.remove(slot);
}

void Issue::countSelection_(const IssueScheduler& sched, uint32_t slot, std::bitset<kMaxIQSlots>& waiting) {
    ++(sched.ages.selectOldest(waiting) == slot ? mNumOldestSelected : mNumNotOldestSelected);
    waiting.reset(slot);
}

void Issue::selectAgeMatrix(IssueScheduler& sched) {
    // Build ready mask - consider both scoreboard and bypass network
    // Only iterate over valid (occupied) slots using the age matrix valid mask
    const uint64_t cycle = getClock()->currentCycle();
    std::bitset<kMaxIQSlots> ready_mask;
    const auto& valid_mask = sched.ages.validMask();
    for (uint32_t slot = 0; slot < sched.capacity; ++slot) {
        if (!valid_mask.test(slot)) continue;
        if (mScoreboard.sourcesReady(sched.slots[slot].pkt->phys_srcs, cycle)) {
            ready_mask.set(slot);
        }
    }

    size_t num_ready = ready_mask.count();
    if (num_ready > 0) {
        ILOG("[issue] age_matrix selection: " << num_ready << " ready in " << sched.name);
    }

    // Repeatedly select oldest ready until no more ports or no more ready
    auto waiting = ready_mask;
    while (ready_mask.any()) {
        uint32_t oldest_slot = sched.ages.selectOldest(ready_mask);
        if (oldest_slot == kInvalidSlot) break;

        IssuePort* port = nullptr;
        UnpipelinedUnit* unit = nullptr;
        if (!canIssue(sched, *sched.slots[oldest_slot].pkt, port, unit)) {
            ready_mask.reset(oldest_slot);
            continue;
        }

        countSelection_(sched, oldest_slot, waiting);
        issueInstruction(sched, oldest_slot, *port, unit);
        ready_mask.reset(oldest_slot);
    }
}

void Issue::selectFIFO(IssueScheduler& sched) {
    // Iterate slots in order (0 to N-1), treating lower index as "older"
    // This is a simplified FIFO that doesn't perfectly match deque ordering
    // but is consistent with slot-based storage
    const uint64_t cycle = getClock()->currentCycle();
    std::bitset<kMaxIQSlots> waiting;
    for (uint32_t slot = 0; slot < sched.capacity; ++slot) {
        if (sched.slots[slot].pkt && mScoreboard.sourcesReady(sched.slots[slot].pkt->phys_srcs, cycle)) waiting.set(slot);
    }
    for (uint32_t slot = 0; slot < sched.capacity; ++slot) {
        auto& entry = sched.slots[slot];
        if (!entry.pkt.has_value()) continue;

        const auto& rpkt = *entry.pkt;

        if (!mScoreboard.sourcesReady(rpkt.phys_srcs, cycle)) continue;

        IssuePort* port = nullptr;
        UnpipelinedUnit* unit = nullptr;
        if (!canIssue(sched, rpkt, port, unit)) continue;

        waiting.set(slot);  // may have been woken this cycle by a zero-latency producer
        countSelection_(sched, slot, waiting);
        issueInstruction(sched, slot, *port, unit);
    }
}

void Issue::selectRandom(IssueScheduler& sched) {
    // Build list of ready slots (considering bypass network)
    const uint64_t cycle = getClock()->currentCycle();
    std::vector<uint32_t> ready_slots;
    for (uint32_t slot = 0; slot < sched.capacity; ++slot) {
        if (!sched.slots[slot].pkt.has_value()) continue;
        if (mScoreboard.sourcesReady(sched.slots[slot].pkt->phys_srcs, cycle)) {
            ready_slots.push_back(slot);
        }
    }

    std::shuffle(ready_slots.begin(), ready_slots.end(), mRng);
    std::bitset<kMaxIQSlots> waiting;
    for (uint32_t slot : ready_slots) waiting.set(slot);

    for (uint32_t slot : ready_slots) {
        auto& entry = sched.slots[slot];
        if (!entry.pkt.has_value()) continue;

        IssuePort* port = nullptr;
        UnpipelinedUnit* unit = nullptr;
        if (!canIssue(sched, *entry.pkt, port, unit)) continue;

        countSelection_(sched, slot, waiting);
        issueInstruction(sched, slot, *port, unit);
    }
}

// ============================================================================
// Main Tick
// ============================================================================

void Issue::tick() {
    // Reset register file bank usage for this cycle
    if (mPrf && mPrf->bankingEnabled()) {
        mPrf->resetBanks();
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
    }
    sendRegReadStage();
}

void Issue::sendRegReadStage() {
    const uint64_t cycle = getClock()->currentCycle();
    for (auto& ipkt : mToIssueBuf) mRegReadPipe.emplace_back(cycle + mRegReadLatency, std::move(ipkt));
    mToIssueBuf.clear();
    while (!mRegReadPipe.empty() && mRegReadPipe.front().first <= cycle) {
        mToIssueBuf.push_back(std::move(mRegReadPipe.front().second));
        mRegReadPipe.pop_front();
    }
    if (!mToIssueBuf.empty()) out_port.send(mToIssueBuf, 1);
}

}  // namespace midcore
