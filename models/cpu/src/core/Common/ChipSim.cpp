#include "ChipSim.hpp"

#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <vector>

#include "sparta/simulation/Clock.hpp"
#include "sparta/simulation/ClockManager.hpp"
#include "sparta/simulation/ResourceFactory.hpp"
#include "sparta/simulation/ResourceTreeNode.hpp"
#include "sparta/simulation/TreeNode.hpp"

#include "Common/BypassNetwork.hpp"
#include "Common/Core.hpp"
#include "Common/FlushArbiter.hpp"
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
#include "Logging.hpp"
#include "MemoryHierarchy/CacheTracer.hpp"
#include "MemoryHierarchy/L2Cache.hpp"
#include "Midcore/Execute/Execute.hpp"
#include "Midcore/Issue/Issue.hpp"
#include "Midcore/Rename/Rename.hpp"
#include "Midcore/Writeback/Writeback.hpp"
#include "Pipeline.hpp"
#include "models/cpu/common/ExecutionDriver.hpp"
#include "models/cpu/common/SnapshotUtil.hpp"
#include "models/cpu/common/whisper_include_fix.hpp"

namespace {

#define ADD_TO_TREE(parent, child_name, child_type)                                                                                                \
    auto* child_name##_tn = new sparta::ResourceTreeNode(parent, #child_name, sparta::TreeNode::GROUP_NAME_NONE, sparta::TreeNode::GROUP_IDX_NONE, \
                                                         #child_name, getResourceSet()->getResourceFactory(child_type::name));                     \
    (void)child_name##_tn;

#define ADD_TO_TREE_WITH_ID(parent, child_name, id, child_type)                                                                                       \
    auto* child_name##_tn = new sparta::ResourceTreeNode(parent, #child_name + std::to_string(id), #child_name, id, #child_name + std::to_string(id), \
                                                         getResourceSet()->getResourceFactory(child_type::name));                                     \
    (void)child_name##_tn;

}  // namespace

// ============================================================================
// Constructor
// ============================================================================

ChipSim::ChipSim(sparta::Scheduler* scheduler)
    : sparta::app::Simulation("core_cpu", scheduler) {
    auto* rs = getResourceSet();
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
    rs->addResourceFactory<sparta::ResourceFactory<core::PipelineClock, core::PipelineClock::PipelineClockParams>>();
    rs->addResourceFactory<sparta::ResourceFactory<core::FlushArbiter, core::FlushArbiterParams>>();
}

ChipSim::~ChipSim() = default;

// ============================================================================
// ExecutionDriver setup
// ============================================================================

void ChipSim::initExecutionDriver(const std::string& target_command, const std::string& trace_filename) {
    if (target_command.empty() && trace_filename.empty()) return;

    auto* driver_node = new sparta::TreeNode(getRoot()->getChild("core0"), cpu::ExecutionDriver::name, "ExecutionDriver");
    auto* params = new cpu::ExecutionDriver::ExecutionDriverParamSet(driver_node);
    params->target_command = target_command;

    if (!trace_filename.empty()) {
        auto snapshot_folder = cpu::generateSnapshotFolderName(trace_filename);
        if (snapshot_folder.empty()) {
            throw std::runtime_error("Trace-driven mode: could not resolve snapshot folder from: " + trace_filename);
        }
        params->snapshot_foldername = snapshot_folder;
        params->enable_snapshot_usage = true;
    }

    mExecutionDriver = new cpu::ExecutionDriver(driver_node, params);
    mExecutionDriver->setId(0);
    mExecutionDriver->setTraceFileName(trace_filename);

    if (!mExecutionDriver->doSetup(0)) {
        std::cerr << "[chipsim] Failed to setup ExecutionDriver\n";
        mExecutionDriver = nullptr;
    } else {
        auto mode = trace_filename.empty() ? "elf" : "trace";
        auto& source = trace_filename.empty() ? target_command : trace_filename;
        std::cout << "[chipsim] ExecutionDriver initialized (" << mode << "): " << source << "\n";
    }
}

// ============================================================================
// buildTree_() — Declare tree structure only.
// ============================================================================

void ChipSim::buildTree_() {
    ADD_TO_TREE_WITH_ID(getRoot(), core, 0, core::Core);

    ADD_TO_TREE(core_tn, fetch, frontend::FetchStructures);
    ADD_TO_TREE(core_tn, icache, frontend::FrontendMemoryStructures);
    ADD_TO_TREE(core_tn, fetch_queue, frontend::FetchQueue);
    ADD_TO_TREE(core_tn, decode, frontend::DecodeStructures);
    ADD_TO_TREE(core_tn, decode_queue, frontend::DecodeQueue);
    ADD_TO_TREE(core_tn, branch_predictor, frontend::BranchPredictor);
    ADD_TO_TREE(core_tn, rename, midcore::Rename);
    ADD_TO_TREE(core_tn, issue, midcore::Issue);
    ADD_TO_TREE(core_tn, execute, midcore::Execute);
    ADD_TO_TREE(core_tn, lsq, midcore::LSQ);
    ADD_TO_TREE(core_tn, dcache, midcore::BackendMemoryStructures);
    ADD_TO_TREE(core_tn, writeback, midcore::Writeback);
    ADD_TO_TREE(core_tn, l2cache, memory::L2Cache);
    ADD_TO_TREE(core_tn, pipeline_clock, core::PipelineClock);
    ADD_TO_TREE(core_tn, flush_arbiter, core::FlushArbiter);

    std::cout << "[chipsim] Tree built\n";
}

// ============================================================================
// configureTree_() — Called after parameters are auto-applied from -c config.
//   Set up clocks, ExecutionDriver, and cross-unit parameter coordination.
// ============================================================================

void ChipSim::configureTree_() {
    // Set up CPU clock domain
    if (mCpuFreqGHz > 0) {
        auto cpu_clock = getClockManager().makeClock("cpu_clock", getClockManager().getRoot(), mCpuFreqGHz * 1000.0);
        getRoot()->getChild("core0")->setClock(cpu_clock.get());
        getClockManager().normalize();
        std::cout << "[chipsim] CPU clock: " << mCpuFreqGHz << " GHz (period=" << cpu_clock->getPeriod() << ")\n";
    } else {
        getRoot()->getChild("core0")->setClock(getClockManager().getRoot().get());
    }

    // Create ExecutionDriver from CLI overrides
    initExecutionDriver(mTargetCommandOverride, mTraceFilenameOverride);

    // Cross-unit coordination: set fetch initial_pc from ExecutionDriver
    if (mExecutionDriver) {
        auto* fetch_pc = getRoot()->getChild("core0.fetch.params.initial_pc");
        if (fetch_pc) {
            fetch_pc->getAs<sparta::ParameterBase>()->setValueFromString(std::to_string(mExecutionDriver->getInitPc()));
        }
    }

    std::cout << "[chipsim] Tree configured\n";
}

// ============================================================================
// bindTree_() — Wire ports, resolve pointers, configure extensions.
// ============================================================================

void ChipSim::bindTree_() {
    auto* core_tn = getRoot()->getChild("core0");
    sparta_assert(core_tn, "core0 tree node not found");
    bindCore(core_tn);
    std::cout << "[chipsim] All ports bound\n";
}

void ChipSim::bindCore(sparta::TreeNode* core_tn) {
    auto* core_unit = core_tn->getResourceAs<core::Core>();
    bool ooo_enabled = core_unit->oooEnabled();

    auto* fetch = core_tn->getChild("fetch")->getResourceAs<frontend::FetchStructures>();
    auto* icache = core_tn->getChild("icache")->getResourceAs<frontend::FrontendMemoryStructures>();
    auto* fetch_queue = core_tn->getChild("fetch_queue")->getResourceAs<frontend::FetchQueue>();
    auto* decode = core_tn->getChild("decode")->getResourceAs<frontend::DecodeStructures>();
    auto* decode_queue = core_tn->getChild("decode_queue")->getResourceAs<frontend::DecodeQueue>();
    auto* bp = core_tn->getChild("branch_predictor")->getResourceAs<frontend::BranchPredictor>();
    auto* rename = core_tn->getChild("rename")->getResourceAs<midcore::Rename>();
    auto* issue = core_tn->getChild("issue")->getResourceAs<midcore::Issue>();
    auto* execute = core_tn->getChild("execute")->getResourceAs<midcore::Execute>();
    auto* lsq = core_tn->getChild("lsq")->getResourceAs<midcore::LSQ>();
    auto* dcache = core_tn->getChild("dcache")->getResourceAs<midcore::BackendMemoryStructures>();
    auto* writeback = core_tn->getChild("writeback")->getResourceAs<midcore::Writeback>();
    auto* l2 = core_tn->getChild("l2cache")->getResourceAs<memory::L2Cache>();
    auto* clock = core_tn->getChild("pipeline_clock")->getResourceAs<core::PipelineClock>();
    auto* flush_arbiter = core_tn->getChild("flush_arbiter")->getResourceAs<core::FlushArbiter>();

    // ExecutionDriver wiring
    fetch->setExecutionDriver(mExecutionDriver);
    writeback->setExecutionDriver(mExecutionDriver);
    if (mInstructionLimit > 0) writeback->setInstructionLimit(mInstructionLimit);

    // Port bindings: Frontend
    // Fetch → ICache (request with embedded packets)
    fetch->request_out.bind(icache->request_in);
    icache->response_out.bind(fetch->response_in);

    // Fetch → BranchPredictor (prediction request - PARALLEL with ICache)
    fetch->predict_request_out.bind(bp->predict_in);

    // Check if we're bypassing queues (simpler wrong-path bring-up)
    auto* core_ps = core_tn->getChild("params");
    bool bypass_queues = core_ps->getChild("bypass_queues")->getAs<sparta::ParameterBase>()->getValueAsString() == "true";

    if (bypass_queues) {
        // Direct mode: ICache → Decode → Rename (bypass FetchQueue and DecodeQueue)
        // BranchPredictor → Decode (predictions route directly to Decode)
        bp->predict_out.bind(decode->branch_predict_in);
        icache->packets_to_queue_out.bind(decode->packets_in);
        decode->out_port.bind(rename->packets_in);
        decode->setDirectMode(true);
        decode->setRename(rename);
        rename->setDirectMode(true);
        std::cout << "[chipsim] Bypass queues ENABLED: ICache -> Decode -> Rename (direct)\n";
        std::cout << "[chipsim] BP predictions routed to Decode (bypass FetchQueue)\n";
    } else {
        // Normal mode: BranchPredictor → FetchQueue (predictions)
        bp->predict_out.bind(fetch_queue->branch_predict_in);
        // Normal mode with queues
        // ICache → FetchQueue (packets after cache latency)
        icache->packets_to_queue_out.bind(fetch_queue->packets_in);

        // Decode → DecodeQueue (decoded uops)
        decode->out_port.bind(decode_queue->decode_in);

        // Direct pointers for pull model
        icache->setFetchQueue(fetch_queue);
        decode->setFetchQueue(fetch_queue);
        decode->setDecodeQueue(decode_queue);
        rename->setDecodeQueue(decode_queue);
    }

    // BranchPredictor → FlushArbiter (wrong-path speculation triggers)
    bp->flush_out.bind(flush_arbiter->bp_flush_in);

    // Port bindings: OOO vs In-order
    if (ooo_enabled) {
        rename->out_port.bind(issue->in_port);
        issue->out_port.bind(execute->in_port);
        execute->completion_out.bind(issue->completion_exe_in);
        lsq->completion_out.bind(issue->completion_lsq_in);
        rename->setDownstream(issue);
        issue->setDownstream(execute);
    } else {
        rename->inorder_out.bind(execute->in_port);
        execute->completion_out.bind(rename->completion_exe_in);
        lsq->completion_out.bind(rename->completion_lsq_in);
        rename->setDownstreamExecute(execute);
    }

    // Port bindings: Memory
    execute->mem_out.bind(lsq->in_port);
    lsq->request_out.bind(dcache->request_in);
    dcache->response_out.bind(lsq->response_in);

    // Port bindings: ROB completion
    execute->rob_complete_out.bind(writeback->rob_complete_exe_in);
    lsq->rob_complete_out.bind(writeback->rob_complete_lsq_in);
    writeback->commit_out.bind(rename->commit_in);

    // Downstream pointers
    fetch->setCache(icache);
    rename->setROB(&writeback->rob());
    rename->setLSQ(lsq);
    execute->setDownstream(lsq);
    lsq->setCache(dcache);
    writeback->setLsq(lsq);
    writeback->setRename(rename);

    // Execute → FlushArbiter (misprediction flush)
    execute->mispred_flush_out.bind(flush_arbiter->exe_flush_in);

    // Writeback → FlushArbiter (trap/interrupt redirect)
    writeback->trap_flush_out.bind(flush_arbiter->retire_flush_in);

    // FlushArbiter → Pipeline stages (broadcast flush)
    flush_arbiter->flush_to_fq_out.bind(fetch_queue->flush_in);
    flush_arbiter->flush_to_icache_out.bind(icache->flush_in);
    flush_arbiter->flush_to_decode_out.bind(decode->flush_in);
    flush_arbiter->flush_to_decode_queue_out.bind(decode_queue->flush_in);
    flush_arbiter->flush_to_issue_out.bind(issue->flush_in);
    flush_arbiter->flush_to_execute_out.bind(execute->flush_in);
    flush_arbiter->flush_to_lsq_out.bind(lsq->flush_in);
    flush_arbiter->flush_to_rename_out.bind(rename->flush_in);
    flush_arbiter->flush_to_writeback_out.bind(writeback->flush_in);

    // FlushArbiter → Fetch (redirects)
    flush_arbiter->redirect_to_fetch_out.bind(fetch->bp_redirect_in);
    flush_arbiter->mispred_redirect_to_fetch_out.bind(fetch->mispred_redirect_in);

    // Execute → Fetch, Writeback, Issue (correct branch resolution - depth decrement)
    execute->branch_resolved_out.bind(fetch->branch_resolved_in);
    execute->branch_resolved_out.bind(writeback->branch_resolved_in);
    execute->branch_resolved_out.bind(issue->branch_resolved_in);

    // Writeback → Rename (freed registers from squash, checkpoint release)
    writeback->freed_regs_out.bind(rename->freed_regs_in);
    writeback->checkpoint_release_out.bind(rename->checkpoint_release_in);

    // L2 cache (enabled param on L2Cache unit)
    bool l2_enabled = l2->isEnabled();
    if (l2_enabled && l2) {
        icache->fill_request_out.bind(l2->icache_request_in);
        l2->icache_response_out.bind(icache->fill_response_in);
        dcache->fill_request_out.bind(l2->dcache_request_in);
        l2->dcache_response_out.bind(dcache->fill_response_in);
        dcache->writeback_out.bind(l2->dcache_writeback_in);
        l2->icache_invalidate_out.bind(icache->invalidate_in);
        l2->dcache_invalidate_out.bind(dcache->invalidate_in);
        icache->setL2(l2);
        dcache->setL2(l2);
    }

    // Write-port arbiter (params on Execute)
    {
        auto* exe_ps = core_tn->getChild("execute.params");
        std::string wp_mode = exe_ps->getChild("write_port_mode")->getAs<sparta::ParameterBase>()->getValueAsString();
        uint32_t wp_count = std::stoul(exe_ps->getChild("write_port_count")->getAs<sparta::ParameterBase>()->getValueAsString());
        mWritePortArbiter = std::make_unique<midcore::WritePortArbiter>();

        if (wp_mode == "mapped") {
            // Parse write_port_mapping from Execute params
            auto& mapping_strs = exe_ps->getChild("write_port_mapping")->getAs<sparta::Parameter<std::vector<std::string>>>()->getValue();
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
            mWritePortArbiter->buildMapped(mapping);
        } else {
            mWritePortArbiter->buildUnified(wp_count, execute->numGroups());
        }
        execute->setArbiter(mWritePortArbiter.get());
        lsq->setArbiter(mWritePortArbiter.get());
    }

    // Bypass network (params on Issue — paths parsed by ChipSim since BypassNetwork is owned here)
    {
        auto* iss_ps = core_tn->getChild("issue.params");
        midcore::BypassNetwork::Config bn_cfg;
        bn_cfg.enabled = iss_ps->getChild("bypass_network_enabled")->getAs<sparta::ParameterBase>()->getValueAsString() == "true";
        bn_cfg.regfile_read_latency =
            static_cast<uint8_t>(std::stoul(iss_ps->getChild("bypass_regfile_read_latency")->getAs<sparta::ParameterBase>()->getValueAsString()));

        if (bn_cfg.enabled) {
            // Read bypass_paths vector<string> from the Issue parameter
            auto& paths_param = *iss_ps->getChild("bypass_paths")->getAs<sparta::Parameter<std::vector<std::string>>>();
            for (const auto& path_str : paths_param.getValue()) {
                // Format: "producer:consumer:latency"
                std::istringstream ss(path_str);
                std::string producer, consumer, lat_str;
                std::getline(ss, producer, ':');
                std::getline(ss, consumer, ':');
                std::getline(ss, lat_str, ':');
                midcore::BypassPathConfig path;
                path.producer = producer;
                path.consumer = consumer;
                path.latency = lat_str.empty() ? 0 : static_cast<uint8_t>(std::stoul(lat_str));
                bn_cfg.paths.push_back(path);
            }
        }
        mBypassNetwork = std::make_unique<midcore::BypassNetwork>();
        mBypassNetwork->configure(bn_cfg, execute->numGroups());
        execute->setBypassNetwork(mBypassNetwork.get());
        issue->setBypassNetwork(mBypassNetwork.get());
    }

    // Writeback buffer (params on Execute)
    {
        auto* exe_ps = core_tn->getChild("execute.params");
        bool wb_enabled = exe_ps->getChild("writeback_buffer_enabled")->getAs<sparta::ParameterBase>()->getValueAsString() == "true";
        if (wb_enabled) {
            midcore::WritebackBuffer::Config wb_cfg;
            wb_cfg.capacity = std::stoul(exe_ps->getChild("writeback_buffer_capacity")->getAs<sparta::ParameterBase>()->getValueAsString());
            wb_cfg.drain_width = std::stoul(exe_ps->getChild("writeback_buffer_drain_width")->getAs<sparta::ParameterBase>()->getValueAsString());
            wb_cfg.writeback_latency =
                static_cast<uint8_t>(std::stoul(exe_ps->getChild("writeback_buffer_latency")->getAs<sparta::ParameterBase>()->getValueAsString()));
            mWritebackBuffer = std::make_unique<midcore::WritebackBuffer>();
            mWritebackBuffer->configure(wb_cfg);
            execute->setWritebackBuffer(mWritebackBuffer.get());
        }
    }

    // Register file banking (params on Rename)
    {
        auto* rn_ps = core_tn->getChild("rename.params");
        bool banking = rn_ps->getChild("regfile_banking_enabled")->getAs<sparta::ParameterBase>()->getValueAsString() == "true";
        if (banking) {
            midcore::PhysicalRegisterFile::BankConfig bank_cfg;
            bank_cfg.enabled = true;
            bank_cfg.num_banks = std::stoul(rn_ps->getChild("regfile_num_banks")->getAs<sparta::ParameterBase>()->getValueAsString());
            bank_cfg.reads_per_bank = std::stoul(rn_ps->getChild("regfile_reads_per_bank")->getAs<sparta::ParameterBase>()->getValueAsString());
            bank_cfg.writes_per_bank = std::stoul(rn_ps->getChild("regfile_writes_per_bank")->getAs<sparta::ParameterBase>()->getValueAsString());
            rename->getPrf().configureBanking(bank_cfg);
            issue->setPhysicalRegisterFile(&rename->getPrf());
        }
    }

    // Issue partitioned mode is now self-configured in Issue constructor from params

    // Pipeline clock wiring
    clock->setStages(fetch, icache, fetch_queue, decode, decode_queue, bp, rename, issue, execute, lsq, dcache, writeback);
    if (mWritePortArbiter) clock->setArbiter(mWritePortArbiter.get());
    if (l2_enabled && l2) clock->setL2(l2);
    clock->setFlushArbiter(flush_arbiter);

    // Speculation config (params on Core)
    {
        core::SpeculationConfig spec_cfg;
        spec_cfg.enabled = core_ps->getChild("wrong_path_enabled")->getAs<sparta::ParameterBase>()->getValueAsString() == "true";
        spec_cfg.max_depth = static_cast<uint8_t>(std::stoul(core_ps->getChild("wrong_path_max_depth")->getAs<sparta::ParameterBase>()->getValueAsString()));
        core::setSpeculationConfig(spec_cfg);

        // Configure static misprediction penalty (used when wrong_path_enabled=false)
        uint32_t mispred_penalty = std::stoul(core_ps->getChild("misprediction_penalty_cycles")->getAs<sparta::ParameterBase>()->getValueAsString());
        fetch->setMispredictionPenalty(mispred_penalty);

        if (spec_cfg.enabled) {
            std::cout << "[chipsim] Wrong-path speculation ENABLED (max_depth=" << static_cast<int>(spec_cfg.max_depth) << ")\n";
        } else if (mispred_penalty > 0) {
            std::cout << "[chipsim] Wrong-path speculation DISABLED, using static misprediction penalty of " << mispred_penalty << " cycles\n";
        }
    }

    // Visualizer (params on Core)
    {
        bool vis_enabled = core_ps->getChild("visualizer_enabled")->getAs<sparta::ParameterBase>()->getValueAsString() == "true";
        if (vis_enabled) {
            uint32_t max_instr = std::stoul(core_ps->getChild("visualizer_max_instructions")->getAs<sparta::ParameterBase>()->getValueAsString());
            std::string output_file = core_ps->getChild("visualizer_output_file")->getAs<sparta::ParameterBase>()->getValueAsString();
            std::string format = core_ps->getChild("visualizer_format")->getAs<sparta::ParameterBase>()->getValueAsString();
            bool color = core_ps->getChild("visualizer_color")->getAs<sparta::ParameterBase>()->getValueAsString() == "true";
            mVisualizer = std::make_unique<core::PipelineVisualizer>(max_instr, output_file, format, color);
            auto* v = mVisualizer.get();
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
            if (mWritePortArbiter) mWritePortArbiter->setVisualizer(v);

            // Debug mode: detailed per-cycle output for a specific cycle range
            bool debug_enabled = core_ps->getChild("visualizer_debug_enabled")->getAs<sparta::ParameterBase>()->getValueAsString() == "true";
            if (debug_enabled) {
                std::string debug_file = core_ps->getChild("visualizer_debug_file")->getAs<sparta::ParameterBase>()->getValueAsString();
                uint64_t start_cycle = std::stoull(core_ps->getChild("visualizer_debug_start_cycle")->getAs<sparta::ParameterBase>()->getValueAsString());
                uint64_t end_cycle = std::stoull(core_ps->getChild("visualizer_debug_end_cycle")->getAs<sparta::ParameterBase>()->getValueAsString());
                v->enableDebugMode(debug_file, start_cycle, end_cycle);
            }
        }
    }

    // Cache viewer (params on Core)
    {
        bool cv_enabled = core_ps->getChild("cache_viewer_enabled")->getAs<sparta::ParameterBase>()->getValueAsString() == "true";
        if (cv_enabled) {
            mCacheViewerFormat = core_ps->getChild("cache_viewer_format")->getAs<sparta::ParameterBase>()->getValueAsString();
            mCacheViewerOutfile = core_ps->getChild("cache_viewer_output_file")->getAs<sparta::ParameterBase>()->getValueAsString();
            mIcacheTracer = std::make_unique<cpu::CacheTracer>("icache", 0);
            icache->setTracer(mIcacheTracer.get());
            mDcacheTracer = std::make_unique<cpu::CacheTracer>("dcache", 0);
            dcache->setTracer(mDcacheTracer.get());
            if (l2_enabled && l2) {
                mL2Tracer = std::make_unique<cpu::CacheTracer>("l2cache", 0);
                l2->setTracer(mL2Tracer.get());
            }
        }
    }
}

// ============================================================================
// run()
// ============================================================================

void ChipSim::run(uint64_t run_time) {
    auto* clock = getRoot()->getChild("core0.pipeline_clock")->getResourceAs<core::PipelineClock>();
    clock->start();

    sparta::app::Simulation::run(run_time);

    if (mVisualizer) mVisualizer->dump();

    auto dumpTracer = [&](const std::unique_ptr<cpu::CacheTracer>& tracer) {
        if (!tracer || !tracer->hasEvents()) return;
        if (mCacheViewerOutfile == "stderr")
            tracer->dump(std::cerr, mCacheViewerFormat);
        else if (mCacheViewerOutfile == "stdout")
            tracer->dump(std::cout, mCacheViewerFormat);
        else {
            std::ofstream ofs(mCacheViewerOutfile, std::ios::app);
            if (ofs)
                tracer->dump(ofs, mCacheViewerFormat);
            else
                tracer->dump(std::cerr, mCacheViewerFormat);
        }
    };

    if (mIcacheTracer || mDcacheTracer || mL2Tracer) {
        if (!mCacheViewerOutfile.empty() && mCacheViewerOutfile != "stderr" && mCacheViewerOutfile != "stdout") {
            std::ofstream ofs(mCacheViewerOutfile, std::ios::trunc);
        }
        dumpTracer(mIcacheTracer);
        dumpTracer(mDcacheTracer);
        dumpTracer(mL2Tracer);
    }
}
