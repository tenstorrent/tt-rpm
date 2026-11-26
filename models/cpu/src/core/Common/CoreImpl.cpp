// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Tenstorrent USA, Inc.

#include "Common/CoreImpl.hpp"

#include <fstream>
#include <functional>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "sparta/app/Simulation.hpp"
#include "sparta/simulation/Clock.hpp"
#include "sparta/simulation/ClockManager.hpp"
#include "sparta/simulation/ResourceFactory.hpp"
#include "sparta/simulation/ResourceTreeNode.hpp"
#include "sparta/simulation/TreeNode.hpp"

#include "Common/Core.hpp"
#include "Common/FlushArbiter.hpp"
#include "Common/L1RequestAggregator.hpp"
#include "Common/PipelineVisualizer.hpp"
#include "Common/SpeculationConfig.hpp"
#include "Common/WritePortArbiter.hpp"
#include "Common/WritebackBuffer.hpp"
#include "Frontend/BranchPredictor/BranchPredictor.hpp"
#include "Frontend/DecodeQueue/DecodeQueue.hpp"
#include "Frontend/DecodeStructures/DecodeStructures.hpp"
#include "Frontend/FetchQueue/FetchQueue.hpp"
#include "Frontend/FetchStructures/FetchStructures.hpp"
#include "Frontend/FrontendMemoryStructures/FrontendMemoryStructures.hpp"
#include "LoadStoreUnit/BackendMemoryStructures/BackendMemoryStructures.hpp"
#include "LoadStoreUnit/LSQ/LSQ.hpp"
#include "MemoryHierarchy/CacheTracer.hpp"
#include "MemoryHierarchy/Dram/Dram.hpp"
#include "MemoryHierarchy/L2Cache.hpp"
#include "Midcore/Execute/Execute.hpp"
#include "Midcore/Issue/Issue.hpp"
#include "Midcore/Rename/Rename.hpp"
#include "Midcore/Writeback/Writeback.hpp"
#include "Mmu/Mmu.hpp"
#include "Pipeline.hpp"
#include "models/cpu/common/ExecutionDriver.hpp"
#include "models/cpu/common/SnapshotUtil.hpp"

namespace core {

void registerCoreResources(sparta::ResourceSet* rs) {
    rs->addResourceFactory<sparta::ResourceFactory<core::Core, core::Core::CoreParams>>();
    rs->addResourceFactory<sparta::ResourceFactory<frontend::FetchStructures, frontend::FetchStructuresParams>>();
    rs->addResourceFactory<sparta::ResourceFactory<frontend::FrontendMemoryStructures, frontend::FrontendMemoryStructuresParams>>();
    rs->addResourceFactory<sparta::ResourceFactory<frontend::FetchQueue, frontend::FetchQueueParams>>();
    rs->addResourceFactory<sparta::ResourceFactory<frontend::DecodeStructures, frontend::DecodeStructuresParams>>();
    rs->addResourceFactory<sparta::ResourceFactory<frontend::DecodeQueue, frontend::DecodeQueueParams>>();
    rs->addResourceFactory<sparta::ResourceFactory<frontend::BranchPredictor, frontend::BranchPredictorParams>>();
    rs->addResourceFactory<sparta::ResourceFactory<midcore::Rename, midcore::RenameParams>>();
    rs->addResourceFactory<sparta::ResourceFactory<midcore::Issue, midcore::IssueParams>>();
    rs->addResourceFactory<sparta::ResourceFactory<midcore::Execute, midcore::ExecuteParams>>();
    rs->addResourceFactory<sparta::ResourceFactory<midcore::LSQ, midcore::LSQParams>>();
    rs->addResourceFactory<sparta::ResourceFactory<midcore::BackendMemoryStructures, midcore::BackendMemoryStructuresParams>>();
    rs->addResourceFactory<sparta::ResourceFactory<midcore::Writeback, midcore::WritebackParams>>();
    rs->addResourceFactory<sparta::ResourceFactory<memory::L2Cache, memory::L2CacheParams>>();
    rs->addResourceFactory<sparta::ResourceFactory<memory::Dram, memory::DramParams>>();
    rs->addResourceFactory<sparta::ResourceFactory<core::PipelineClock, core::PipelineClock::PipelineClockParams>>();
    rs->addResourceFactory<sparta::ResourceFactory<core::FlushArbiter, core::FlushArbiterParams>>();
    rs->addResourceFactory<sparta::ResourceFactory<core::Mmu, core::MmuParams>>();
}

namespace {

// Helper unit whose only job is to run a callback during Sparta's late-bind
// phase (used by init()): internal port binding must happen *after* every unit's
// onBindTreeEarly_() has registered its auto-precedence consumer events.
class PhaseBinder : public sparta::Unit {
   public:
    explicit PhaseBinder(sparta::TreeNode* node)
        : sparta::Unit(node) {}
    std::function<void()> on_late;

   private:
    void onBindTreeLate_() override {
        if (on_late) on_late();
    }
};

std::string paramStr(sparta::TreeNode* core_tn, const std::string& path) { return core_tn->getChild(path)->getAs<sparta::ParameterBase>()->getValueAsString(); }
bool paramBool(sparta::TreeNode* core_tn, const std::string& path) { return paramStr(core_tn, path) == "true"; }
uint64_t paramU64(sparta::TreeNode* core_tn, const std::string& path) { return std::stoull(paramStr(core_tn, path)); }

}  // namespace

// ============================================================================
// Impl — holds every unit pointer and the non-Unit helpers.
// ============================================================================
struct CoreImpl::Impl {
    sparta::TreeNode* parent = nullptr;
    sparta::TreeNode* core_tn = nullptr;  // the core::Core ResourceTreeNode
    Config cfg;

    cpu::ExecutionDriver* driver = nullptr;

    bool l2_enabled = false;  // l2cache.params.enabled, resolved at build time

    core::L1RequestAggregator* aggregator = nullptr;  // per-core L1 merge, below L2/cluster
    PhaseBinder* binder = nullptr;

    // Non-Unit helpers owned here.
    std::unique_ptr<midcore::WritePortArbiter> arbiter;
    std::unique_ptr<midcore::WritebackBuffer> wb_buffer;
    std::unique_ptr<core::PipelineVisualizer> visualizer;
    core::SpeculationConfig spec_cfg;  // the speculating units hold a pointer to it
    std::unique_ptr<cpu::CacheTracer> icache_tracer;
    std::unique_ptr<cpu::CacheTracer> dcache_tracer;
    std::unique_ptr<cpu::CacheTracer> l2_tracer;
    std::string cache_viewer_format{"log"};
    std::string cache_viewer_outfile{"stderr"};

    // The tree nodes this core created and the units on them.
    std::vector<std::unique_ptr<sparta::Unit>> owned_units;
    std::vector<std::unique_ptr<sparta::TreeNode>> owned_nodes;

    explicit Impl(sparta::TreeNode* p, const Config& c)
        : parent(p),
          cfg(c) {}

    // Units before the nodes they sit on, children before parents, all before the helpers the units point at.
    ~Impl() {
        while (!owned_units.empty()) owned_units.pop_back();
        while (!owned_nodes.empty()) owned_nodes.pop_back();
    }

    template <typename T>
    T* ownNode(T* n) {
        owned_nodes.emplace_back(n);
        return n;
    }
    template <typename T>
    T* ownUnit(T* u) {
        owned_units.emplace_back(u);
        return u;
    }

    sparta::ResourceSet* resources() { return cfg.sim->getResourceSet(); }

    sparta::ResourceTreeNode* addUnit(sparta::TreeNode* parent_node, const std::string& name, const char* res_name) {
        return ownNode(new sparta::ResourceTreeNode(parent_node, name, sparta::TreeNode::GROUP_NAME_NONE, sparta::TreeNode::GROUP_IDX_NONE, name,
                                                    resources()->getResourceFactory(res_name)));
    }

    template <typename T>
    T* res(const std::string& child) {
        return core_tn->getChild(child)->getResourceAs<T>();
    }

    void buildUnits();
    void configure();
    void initDriver();
    void wireInternal();
    void finish();
};

void CoreImpl::Impl::buildUnits() {
    const uint32_t id = cfg.index;
    // The per-core node *is* the core::Core unit (preserves ChipSim's tree layout:
    // top.core0 == core unit, stages are its children).
    auto* core_node = ownNode(new sparta::ResourceTreeNode(parent, "core" + std::to_string(id), "core", id, "core" + std::to_string(id),
                                                           resources()->getResourceFactory(core::Core::name)));
    core_tn = core_node;

    addUnit(core_node, "fetch", frontend::FetchStructures::name);
    addUnit(core_node, "icache", frontend::FrontendMemoryStructures::name);
    addUnit(core_node, "fetch_queue", frontend::FetchQueue::name);
    addUnit(core_node, "decode", frontend::DecodeStructures::name);
    addUnit(core_node, "decode_queue", frontend::DecodeQueue::name);
    addUnit(core_node, "branch_predictor", frontend::BranchPredictor::name);
    addUnit(core_node, "rename", midcore::Rename::name);
    addUnit(core_node, "issue", midcore::Issue::name);
    addUnit(core_node, "execute", midcore::Execute::name);
    addUnit(core_node, "lsq", midcore::LSQ::name);
    addUnit(core_node, "dcache", midcore::BackendMemoryStructures::name);
    addUnit(core_node, "mmu", core::Mmu::name);
    addUnit(core_node, "writeback", midcore::Writeback::name);
    addUnit(core_node, "l2cache", memory::L2Cache::name);
    addUnit(core_node, "dram", memory::Dram::name);
    addUnit(core_node, "pipeline_clock", core::PipelineClock::name);
    addUnit(core_node, "flush_arbiter", core::FlushArbiter::name);

    // Apply host-supplied parameter overrides now that every node (and its
    // ParameterSet) exists, so topology decisions below see final values.
    for (const auto& [path, value] : cfg.param_overrides) {
        core_tn->getChild(path)->getAs<sparta::ParameterBase>()->setValueFromString(value);
    }

    // The per-core L1RequestAggregator merges the two L1 streams and presents one tagged
    // port to the next level. It exists whenever there is a next level below the L1s --
    // a private L2 and/or the shared cluster cache.
    l2_enabled = paramBool(core_node, "l2cache.params.enabled");
    if (cfg.external_cache || l2_enabled) {
        aggregator = ownUnit(new core::L1RequestAggregator(ownNode(new sparta::TreeNode(core_node, "l1_aggregator", "L1 request aggregator"))));
    }
}

void CoreImpl::Impl::initDriver() {
    const std::string& target_command = cfg.program;
    if (target_command.empty() && cfg.snapshot_folder.empty()) return;

    auto* driver_node = ownNode(new sparta::TreeNode(core_tn, cpu::ExecutionDriver::name, "ExecutionDriver"));
    auto* params = ownNode(new cpu::ExecutionDriver::ExecutionDriverParamSet(driver_node));
    params->target_command = target_command;
    params->shared_hart_count = cfg.shared_harts;
    params->private_system = cfg.private_system;
    params->whisper_flags = cfg.whisper_flags;

    if (!cfg.snapshot_folder.empty()) {
        // Execution-driven from a snapshot: Whisper runs forward from the restored state.
        auto snapshot_folder = cpu::generateSnapshotFolderName(cfg.snapshot_folder);
        if (snapshot_folder.empty()) {
            throw std::runtime_error("Snapshot mode: could not resolve snapshot folder from: " + cfg.snapshot_folder);
        }
        params->snapshot_foldername = snapshot_folder;
        params->enable_snapshot_usage = true;
    }

    driver = ownUnit(new cpu::ExecutionDriver(driver_node, params));
    driver->setId(cfg.index);

    if (!driver->doSetup(cfg.index)) {
        throw std::runtime_error(core_tn->getLocation() + ": ExecutionDriver setup failed");
    }
}

void CoreImpl::Impl::configure() {
    // Clock domain.
    if (cfg.cpu_freq_ghz > 0) {
        auto cpu_clock =
            cfg.sim->getClockManager().makeClock("cpu_clock_" + std::to_string(cfg.index), cfg.sim->getClockManager().getRoot(), cfg.cpu_freq_ghz * 1000.0);
        core_tn->setClock(cpu_clock.get());
        cfg.sim->getClockManager().normalize();
    } else {
        core_tn->setClock(cfg.sim->getClockManager().getRoot().get());
    }

    // Match the cache that egresses to the external shared cache to its line size.
    if (cfg.external_cache && !l2_enabled) {
        // No private L2: the L1s talk to the external cache directly, so they must
        // be structural (fills routed over ports) and line-matched to the cluster.
        for (const char* l1 : {"icache", "dcache"}) {
            core_tn->getChild(std::string(l1) + ".params.cache_mode")->getAs<sparta::ParameterBase>()->setValueFromString("structural");
            core_tn->getChild(std::string(l1) + ".params.cache_line_size")->getAs<sparta::ParameterBase>()->setValueFromString(std::to_string(cfg.line_size));
        }
    } else if (cfg.external_cache && l2_enabled) {
        // Private L2 egresses to the external cache: line-match the L2.
        core_tn->getChild("l2cache.params.cache_line_size")->getAs<sparta::ParameterBase>()->setValueFromString(std::to_string(cfg.line_size));
    }

    initDriver();

    // Cross-unit coordination: fetch starts at the program's entry PC.
    if (driver) {
        auto* fetch_pc = core_tn->getChild("fetch.params.initial_pc");
        if (fetch_pc) fetch_pc->getAs<sparta::ParameterBase>()->setValueFromString(std::to_string(driver->getInitPc()));
    }
}

void CoreImpl::Impl::wireInternal() {
    auto* core_unit = core_tn->getResourceAs<core::Core>();
    bool ooo_enabled = core_unit->oooEnabled();

    auto* fetch = res<frontend::FetchStructures>("fetch");
    auto* icache = res<frontend::FrontendMemoryStructures>("icache");
    auto* fetch_queue = res<frontend::FetchQueue>("fetch_queue");
    auto* decode = res<frontend::DecodeStructures>("decode");
    auto* decode_queue = res<frontend::DecodeQueue>("decode_queue");
    auto* bp = res<frontend::BranchPredictor>("branch_predictor");
    auto* rename = res<midcore::Rename>("rename");
    auto* issue = res<midcore::Issue>("issue");
    auto* execute = res<midcore::Execute>("execute");
    auto* lsq = res<midcore::LSQ>("lsq");
    auto* dcache = res<midcore::BackendMemoryStructures>("dcache");
    auto* mmu = res<core::Mmu>("mmu");
    auto* writeback = res<midcore::Writeback>("writeback");
    auto* l2 = res<memory::L2Cache>("l2cache");
    auto* dram = res<memory::Dram>("dram");
    auto* clock = res<core::PipelineClock>("pipeline_clock");
    auto* flush_arbiter = res<core::FlushArbiter>("flush_arbiter");

    // ── ISS driver ───────────────────────────────────────────────────────────
    fetch->setExecutionDriver(driver);
    writeback->setExecutionDriver(driver);

    // Data-carrying L1 shadow: give the dcache a side-effect-free peek into
    // Whisper (the oracle). On fill it seeds the shadow line; on a load hit it compares the shadow
    // vs Whisper and counts stale-line reads (a coherence hazard). Whisper stays authoritative.
    if (cfg.data_carrying && driver) {
        dcache->setDataCarrying(true, [drv = driver](cpu::address_t addr, uint8_t* buf, uint32_t len) { drv->peekPhysBytes(addr, buf, len); });
    }
    if (cfg.instruction_limit > 0) writeback->setInstructionLimit(cfg.instruction_limit);

    // ── Frontend ───────────────────────────────────────────────────────────────
    fetch->request_out.bind(icache->request_in);
    icache->response_out.bind(fetch->response_in);
    fetch->predict_request_out.bind(bp->predict_in);

    bool bypass_queues = paramBool(core_tn, "params.bypass_queues");
    if (bypass_queues) {
        bp->predict_out.bind(decode->branch_predict_in);
        icache->packets_to_queue_out.bind(decode->packets_in);
        decode->out_port.bind(rename->packets_in);
        decode->setDirectMode(true);
        decode->setRename(rename);
        rename->setDirectMode(true);
        icache->setBypassDecode(decode, fetch_queue->capacity());
    } else {
        bp->predict_out.bind(fetch_queue->branch_predict_in);
        icache->packets_to_queue_out.bind(fetch_queue->packets_in);
        decode->out_port.bind(decode_queue->decode_in);
        icache->setFetchQueue(fetch_queue);
        decode->setFetchQueue(fetch_queue);
        decode->setDecodeQueue(decode_queue);
        rename->setDecodeQueue(decode_queue);
    }

    bp->flush_out.bind(flush_arbiter->bp_flush_in);
    bp->taken_bubble_out.bind(fetch->taken_bubble_in);

    // ── OOO vs in-order mid-core ─────────────────────────────────────────────
    rename->setOooEnabled(ooo_enabled);
    if (ooo_enabled) {
        rename->out_port.bind(issue->in_port);
        issue->out_port.bind(execute->in_port);
        lsq->completion_out.bind(issue->completion_lsq_in);
        rename->setDownstream(issue);
        issue->setDownstream(execute);
        dcache->setLoadHitCallback([lsq, issue](uint64_t tag, uint64_t req_id, uint64_t data_cycle) {
            const auto* dsts = lsq->singleAccessLoadDsts(tag, req_id);
            if (dsts && !dsts->empty()) issue->wakeOnLoadData(*dsts, data_cycle);
        });
    } else {
        rename->inorder_out.bind(execute->in_port);
        execute->completion_out.bind(rename->completion_exe_in);
        lsq->completion_out.bind(rename->completion_lsq_in);
        rename->setDownstreamExecute(execute);
        rename->setDownstream(issue);  // issue ports and dividers only
        dcache->setLoadHitCallback([lsq, rename](uint64_t tag, uint64_t req_id, uint64_t data_cycle) {
            const auto* dsts = lsq->singleAccessLoadDsts(tag, req_id);
            if (dsts && !dsts->empty()) rename->wakeOnLoadData(*dsts, data_cycle);
        });
    }

    // ── Memory path ──────────────────────────────────────────────────────────
    execute->mem_out.bind(lsq->in_port);
    lsq->request_out.bind(dcache->request_in);
    dcache->response_out.bind(lsq->response_in);
    // Page walks load PTEs through the L1D like any load.
    mmu->pte_request_out.bind(dcache->request_in);
    dcache->response_out.bind(mmu->pte_response_in);
    flush_arbiter->flush_to_lsq_out.bind(mmu->flush_in);
    mmu->setExecutionDriver(driver);
    mmu->setCache(dcache);
    fetch->setMmu(mmu);
    lsq->setMmu(mmu);

    // ── ROB completion / commit ──────────────────────────────────────────────
    execute->rob_complete_out.bind(writeback->rob_complete_exe_in);
    lsq->rob_complete_out.bind(writeback->rob_complete_lsq_in);
    writeback->commit_out.bind(rename->commit_in);

    fetch->setCache(icache);
    rename->setROB(&writeback->rob());
    rename->setLSQ(lsq);
    execute->setDownstream(lsq);
    lsq->setCache(dcache);
    lsq->setROB(&writeback->rob());
    writeback->setLsq(lsq);

    // ── Flush / redirect fabric ──────────────────────────────────────────────
    execute->mispred_flush_out.bind(flush_arbiter->exe_flush_in);
    decode->misfetch_flush_out.bind(flush_arbiter->exe_flush_in);
    writeback->trap_flush_out.bind(flush_arbiter->retire_flush_in);
    flush_arbiter->flush_to_fq_out.bind(fetch_queue->flush_in);
    flush_arbiter->flush_to_icache_out.bind(icache->flush_in);
    flush_arbiter->flush_to_decode_out.bind(decode->flush_in);
    flush_arbiter->flush_to_decode_queue_out.bind(decode_queue->flush_in);
    flush_arbiter->flush_to_issue_out.bind(issue->flush_in);
    flush_arbiter->flush_to_execute_out.bind(execute->flush_in);
    flush_arbiter->flush_to_lsq_out.bind(lsq->flush_in);
    flush_arbiter->flush_to_rename_out.bind(rename->flush_in);
    flush_arbiter->flush_to_writeback_out.bind(writeback->flush_in);
    flush_arbiter->redirect_to_fetch_out.bind(fetch->bp_redirect_in);
    flush_arbiter->mispred_redirect_to_fetch_out.bind(fetch->mispred_redirect_in);
    // Restore the branch predictor's speculative history when a misprediction flush redirects fetch.
    flush_arbiter->mispred_redirect_to_fetch_out.bind(bp->mispred_redirect_in);
    flush_arbiter->setFetch(fetch);

    execute->branch_resolved_out.bind(fetch->branch_resolved_in);
    execute->branch_resolved_out.bind(writeback->branch_resolved_in);
    execute->branch_resolved_out.bind(issue->branch_resolved_in);

    // Commit-time branch-predictor training: Writeback signals each retired branch to the predictor.
    writeback->branch_retire_out.bind(bp->train_in);
    writeback->branch_retire_out.bind(flush_arbiter->branch_retire_in);

    writeback->freed_regs_out.bind(rename->freed_regs_in);
    writeback->checkpoint_release_out.bind(rename->checkpoint_release_in);

    // ── Last-level cache ─────────────────────────────────────────────────────────
    // Use the L2 resource here (valid at late-bind, reflects `-c` for ChipSim);
    // it matches the build-time l2_enabled used for bridge creation in the cluster.
    const bool l2_on = l2->isEnabled();
    bool l2_used = false;
    if (aggregator) {
        // The L1s connect to the per-core aggregator (one MemSlave each). Their
        // individual fill ports act as a MemMaster on the L1 side.
        auto& aggI = aggregator->l1Slave(mem::CacheSource::ICACHE);
        auto& aggD = aggregator->l1Slave(mem::CacheSource::DCACHE);
        icache->fill_request_out.bind(aggI.req_in);
        aggI.resp_out.bind(icache->fill_response_in);
        aggI.inval_out.bind(icache->invalidate_in);
        dcache->fill_request_out.bind(aggD.req_in);
        aggD.resp_out.bind(dcache->fill_response_in);
        dcache->writeback_out.bind(aggD.wb_in);
        aggD.inval_out.bind(dcache->invalidate_in);
        icache->setUpstream(aggregator);
        dcache->setUpstream(aggregator);

        if (l2_on) {
            // Private L2 below the aggregator. The aggregator's master drives the L2's
            // downstream slave; backpressure defers to the L2's per-source queues.
            l2_used = true;
            mem::connect(aggregator->up(), l2->down);
            aggregator->setL2(l2);
            // Each outstanding L1 miss holds an MSHR, so this bounds the L2 queues with no credits.
            for (const char* l1 : {"icache", "dcache"}) {
                sparta_assert(paramStr(core_tn, std::string(l1) + ".params.cache_mode") != "structural" ||
                                  paramU64(core_tn, std::string(l1) + ".params.mshr_capacity") <= l2->queueCapacity(),
                              "l2cache.params.queue_capacity (" << l2->queueCapacity() << ") must be at least " << l1 << ".params.mshr_capacity");
            }
            if (cfg.external_cache) {
                // L2 misses/writebacks egress to the shared cluster cache (its upstream
                // master is exposed via memMaster()).
                l2->enableExternalFill();
            } else if (dram->isEnabled()) {
                l2->setMemBackend(dram);
                clock->setDram(dram);
            }
        }
        // else: the aggregator's upstream master is exposed via memMaster() and binds
        // straight to the shared cluster cache (no private L2).
    }

    // Issue's scoreboard is indexed by Rename's physical register numbers.
    sparta_assert(paramU64(core_tn, "rename.params.num_phys_regs") == paramU64(core_tn, "issue.params.num_phys_regs"),
                  "rename.params.num_phys_regs and issue.params.num_phys_regs must be equal");

    // ── Write-port arbiter (params on Execute) ────────────────────────────────
    {
        std::string wp_mode = paramStr(core_tn, "execute.params.write_port_mode");
        uint32_t wp_count = static_cast<uint32_t>(paramU64(core_tn, "execute.params.write_port_count"));
        arbiter = std::make_unique<midcore::WritePortArbiter>();
        if (wp_mode == "mapped") {
            auto& mapping_strs = core_tn->getChild("execute.params.write_port_mapping")->getAs<sparta::Parameter<std::vector<std::string>>>()->getValue();
            std::vector<std::vector<uint8_t>> mapping;
            for (const auto& port_str : mapping_strs) {
                std::vector<uint8_t> sources;
                std::istringstream ss(port_str);
                std::string tok;
                while (std::getline(ss, tok, '+')) {
                    if (tok == "lsq")
                        sources.push_back(midcore::kLsqSourceId);
                    else if (!tok.empty())
                        sources.push_back(static_cast<uint8_t>(std::stoi(tok)));
                }
                mapping.push_back(std::move(sources));
            }
            arbiter->buildMapped(mapping);
        } else {
            sparta_assert(wp_mode == "unified", "execute.params.write_port_mode = '" << wp_mode << "'; expected one of: unified, mapped");
            arbiter->buildUnified(wp_count, execute->numGroups());
        }
        execute->setArbiter(arbiter.get());
        lsq->setArbiter(arbiter.get());
    }

    // ── Writeback buffer (params on Execute) ──────────────────────────────────
    if (paramBool(core_tn, "execute.params.writeback_buffer_enabled")) {
        midcore::WritebackBuffer::Config wb_cfg;
        wb_cfg.capacity = static_cast<uint32_t>(paramU64(core_tn, "execute.params.writeback_buffer_capacity"));
        wb_cfg.drain_width = static_cast<uint32_t>(paramU64(core_tn, "execute.params.writeback_buffer_drain_width"));
        const uint64_t wb_lat = paramU64(core_tn, "execute.params.writeback_buffer_latency");
        sparta_assert(wb_cfg.capacity > 0 && wb_cfg.drain_width > 0,
                      "execute.params.writeback_buffer_capacity and writeback_buffer_drain_width must be nonzero");
        sparta_assert(wb_lat <= UINT8_MAX, "execute.params.writeback_buffer_latency must be at most 255, got " << wb_lat);
        wb_cfg.writeback_latency = static_cast<uint8_t>(wb_lat);
        wb_buffer = std::make_unique<midcore::WritebackBuffer>();
        wb_buffer->configure(wb_cfg);
        execute->setWritebackBuffer(wb_buffer.get());
    }

    // ── Register file banking (params on Rename) ──────────────────────────────
    if (paramBool(core_tn, "rename.params.regfile_banking_enabled")) {
        midcore::PhysicalRegisterFile::BankConfig bank_cfg;
        bank_cfg.enabled = true;
        bank_cfg.num_banks = static_cast<uint32_t>(paramU64(core_tn, "rename.params.regfile_num_banks"));
        bank_cfg.reads_per_bank = static_cast<uint32_t>(paramU64(core_tn, "rename.params.regfile_reads_per_bank"));
        rename->getPrf().configureBanking(bank_cfg);
        issue->setPhysicalRegisterFile(&rename->getPrf());
    }

    // ── Pipeline clock ─────────────────────────────────────────────────────────
    clock->setStages(fetch, icache, fetch_queue, decode, decode_queue, bp, rename, issue, execute, lsq, dcache, writeback);
    if (arbiter) clock->setArbiter(arbiter.get());
    if (l2_used) clock->setL2(l2);
    clock->setMmu(mmu);
    clock->setFlushArbiter(flush_arbiter);

    // ── Speculation config (params on Core) ────────────────────────────────────
    {
        spec_cfg.enabled = paramBool(core_tn, "params.wrong_path_enabled");
        uint64_t max_depth = paramU64(core_tn, "params.wrong_path_max_depth");
        sparta_assert(max_depth <= UINT8_MAX, core_tn->getLocation() << ".params.wrong_path_max_depth must be at most 255, got " << max_depth);
        spec_cfg.max_depth = static_cast<uint8_t>(max_depth);
        // Wrong-path speculation requires OOO rename; it has no effect in-order.
        if (spec_cfg.enabled && !ooo_enabled) {
            std::cerr << "[core " << cfg.index
                      << "] WARNING: wrong_path_enabled=true has no effect with ooo_enabled=false "
                         "(the in-order pipeline does not speculate); forcing wrong_path_enabled=false.\n";
            spec_cfg.enabled = false;
        }
        fetch->setSpeculationConfig(&spec_cfg);
        fetch->setFollowWrongPathBranches(bp->predictsTargets());
        rename->setSpeculationConfig(&spec_cfg);
        writeback->setSpeculationConfig(&spec_cfg);
        const uint64_t penalty = paramU64(core_tn, "params.misprediction_penalty_cycles");
        sparta_assert(!spec_cfg.enabled || penalty == 0, core_tn->getLocation() << ".params.misprediction_penalty_cycles = " << penalty
                                                                                << "; it applies only with wrong_path_enabled: false and must be 0 otherwise");
        fetch->setMispredictionPenalty(static_cast<uint32_t>(penalty));
    }

    // ── Visualizer (params on Core) ────────────────────────────────────────────
    if (paramBool(core_tn, "params.visualizer_enabled")) {
        uint32_t max_instr = static_cast<uint32_t>(paramU64(core_tn, "params.visualizer_max_instructions"));
        std::string output_file = paramStr(core_tn, "params.visualizer_output_file");
        std::string format = paramStr(core_tn, "params.visualizer_format");
        bool color = paramBool(core_tn, "params.visualizer_color");
        visualizer = std::make_unique<core::PipelineVisualizer>(max_instr, output_file, format, color);
        auto* v = visualizer.get();
        fetch->setVisualizer(v);
        icache->setVisualizer(v);
        fetch_queue->setVisualizer(v);
        decode->setVisualizer(v);
        decode_queue->setVisualizer(v);
        rename->setVisualizer(v);
        issue->setVisualizer(v);
        execute->setVisualizer(v);
        lsq->setVisualizer(v);
        writeback->setVisualizer(v);
        clock->setVisualizer(v);
        if (arbiter) arbiter->setVisualizer(v);
        if (paramBool(core_tn, "params.visualizer_debug_enabled")) {
            std::string debug_file = paramStr(core_tn, "params.visualizer_debug_file");
            uint64_t start_cycle = paramU64(core_tn, "params.visualizer_debug_start_cycle");
            uint64_t end_cycle = paramU64(core_tn, "params.visualizer_debug_end_cycle");
            v->enableDebugMode(debug_file, start_cycle, end_cycle);
        }
    }

    // ── Cache viewer (params on Core) ──────────────────────────────────────────
    if (paramBool(core_tn, "params.cache_viewer_enabled")) {
        cache_viewer_format = paramStr(core_tn, "params.cache_viewer_format");
        cache_viewer_outfile = paramStr(core_tn, "params.cache_viewer_output_file");
        icache_tracer = std::make_unique<cpu::CacheTracer>("icache", 0);
        icache->setTracer(icache_tracer.get());
        dcache_tracer = std::make_unique<cpu::CacheTracer>("dcache", 0);
        dcache->setTracer(dcache_tracer.get());
        if (l2_used) {
            l2_tracer = std::make_unique<cpu::CacheTracer>("l2cache", 0);
            l2->setTracer(l2_tracer.get());
        }
    }
}

void CoreImpl::Impl::finish() {
    if (visualizer) visualizer->dump();

    auto dumpTracer = [&](const std::unique_ptr<cpu::CacheTracer>& tracer) {
        if (!tracer || !tracer->hasEvents()) return;
        if (cache_viewer_outfile == "stderr")
            tracer->dump(std::cerr, cache_viewer_format);
        else if (cache_viewer_outfile == "stdout")
            tracer->dump(std::cout, cache_viewer_format);
        else {
            std::ofstream ofs(cache_viewer_outfile, std::ios::app);
            if (ofs)
                tracer->dump(ofs, cache_viewer_format);
            else
                tracer->dump(std::cerr, cache_viewer_format);
        }
    };

    if (icache_tracer || dcache_tracer || l2_tracer) {
        if (!cache_viewer_outfile.empty() && cache_viewer_outfile != "stderr" && cache_viewer_outfile != "stdout") {
            std::ofstream ofs(cache_viewer_outfile, std::ios::trunc);
        }
        dumpTracer(icache_tracer);
        dumpTracer(dcache_tracer);
        dumpTracer(l2_tracer);
    }
}

// ============================================================================
// CoreImpl — public surface, delegates to Impl.
// ============================================================================
CoreImpl::CoreImpl(sparta::TreeNode* parent, const Config& cfg)
    : mImpl(std::make_unique<Impl>(parent, cfg)) {}

CoreImpl::~CoreImpl() = default;

void CoreImpl::buildUnits_() { mImpl->buildUnits(); }
void CoreImpl::configure_() { mImpl->configure(); }
void CoreImpl::wireInternal_() { mImpl->wireInternal(); }
void CoreImpl::finish_() { mImpl->finish(); }

void CoreImpl::init() {
    mImpl->buildUnits();
    mImpl->configure();
    // Defer internal wiring to the late-bind phase (after auto-precedence).
    mImpl->binder = mImpl->ownUnit(new PhaseBinder(mImpl->ownNode(new sparta::TreeNode(mImpl->core_tn, "binder", "Late-phase internal binder"))));
    mImpl->binder->on_late = [this]() { mImpl->wireInternal(); };
}

uint32_t CoreImpl::numMemMasters() const {
    return mImpl->cfg.external_cache ? 1 : 0;  // one tagged port per core (or no egress)
}

mem::MemMaster& CoreImpl::memMaster(uint32_t) {
    // The level talking to the cluster: the L2's master if present, else the aggregator's.
    return mImpl->l2_enabled ? mImpl->res<memory::L2Cache>("l2cache")->up : mImpl->aggregator->up();
}

const sparta::Clock* CoreImpl::clock() const {
    // The per-core clock set in configure() (cpu_freq clock, or root). Node lookup only.
    return mImpl->core_tn->getClock();
}

void CoreImpl::setRequesterId(uint32_t, uint32_t requester_id) {
    // The level connected to the cluster stamps the (per-core) requester id. The aggregator
    // always stamps (it owns the L2-off path); the L2 also stamps when present.
    if (mImpl->aggregator) mImpl->aggregator->setRequesterId(requester_id);
    if (mImpl->l2_enabled) mImpl->res<memory::L2Cache>("l2cache")->setRequesterId(requester_id);
}

void CoreImpl::start() {
    auto* clock = mImpl->res<core::PipelineClock>("pipeline_clock");
    clock->start();
}

cpu::ExecutionDriver* CoreImpl::executionDriver() { return mImpl->driver; }

}  // namespace core
