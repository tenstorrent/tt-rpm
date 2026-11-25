// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Tenstorrent USA, Inc.

#include "TopSim.hpp"

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <fstream>

#include "sparta/simulation/TreeNode.hpp"

#include "CacheUnit.hpp"
#include "DRAM.hpp"
#include "FetchUnit.hpp"
#include "LSUnit.hpp"
#include "MidCore.hpp"
#include "models/cpu/common/ExecutionDriver.hpp"
#include "models/cpu/common/whisper_include_fix.hpp"

namespace {
constexpr uint64_t DEFAULT_INSTR_SIZE = 4;
constexpr uint64_t DEFAULT_FETCH_WIDTH = 2;
constexpr uint64_t DEFAULT_DECODE_WIDTH = 2;
constexpr uint64_t DEFAULT_LS_WIDTH = 2;
constexpr uint64_t DEFAULT_LOAD_PORTS = 2;
const char* CONFIG_FILE = "config.yaml";

struct PipelineConfig {
    uint64_t instr_size = DEFAULT_INSTR_SIZE;
    uint64_t mFetchwidth = DEFAULT_FETCH_WIDTH;
    uint64_t decode_width = DEFAULT_DECODE_WIDTH;
    uint64_t ls_width = DEFAULT_LS_WIDTH;
    uint64_t l1_load_ports = DEFAULT_LOAD_PORTS;
    uint64_t l2_load_ports = DEFAULT_LOAD_PORTS;
    uint64_t dram_load_ports = DEFAULT_LOAD_PORTS;
    std::string target_command;  // If non-empty, use ExecutionDriver/Whisper for fetch
    // Effective width: min of all stages so we never send more than any stage can accept (no drops).
    uint64_t pipeline_width = DEFAULT_FETCH_WIDTH;
};

PipelineConfig loadPipelineConfig() {
    PipelineConfig cfg;
    std::ifstream f(CONFIG_FILE);
    if (!f.good()) return cfg;
    try {
        YAML::Node node = YAML::Load(f);
        if (node["instr_size"]) cfg.instr_size = node["instr_size"].as<uint64_t>();
        if (node["fetch_width"]) cfg.mFetchwidth = node["fetch_width"].as<uint64_t>();
        if (node["decode_width"]) cfg.decode_width = node["decode_width"].as<uint64_t>();
        if (node["ls_width"]) cfg.ls_width = node["ls_width"].as<uint64_t>();
        if (node["l1_load_ports"]) cfg.l1_load_ports = node["l1_load_ports"].as<uint64_t>();
        if (node["l2_load_ports"]) cfg.l2_load_ports = node["l2_load_ports"].as<uint64_t>();
        if (node["dram_load_ports"]) cfg.dram_load_ports = node["dram_load_ports"].as<uint64_t>();
        if (node["target_command"]) cfg.target_command = node["target_command"].as<std::string>();
        cfg.pipeline_width = std::min({cfg.mFetchwidth, cfg.decode_width, cfg.ls_width, cfg.l1_load_ports, cfg.l2_load_ports, cfg.dram_load_ports});
        if (cfg.pipeline_width == 0) cfg.pipeline_width = 1;
    } catch (const YAML::Exception& e) {
        throw sparta::SpartaException(CONFIG_FILE) << ": " << e.what();
    }
    return cfg;
}
}  // namespace

TopSim::TopSim(sparta::Scheduler* scheduler, const std::string& target_elf)
    : sparta::app::Simulation("simple_cpu", scheduler),
      mTargetElf(target_elf) {}

TopSim::~TopSim() { getRoot()->enterTeardown(); }

void TopSim::configureTree_() {}

void TopSim::bindTree_() {}

void TopSim::buildTree_() {
    PipelineConfig cfg = loadPipelineConfig();
    if (!mTargetElf.empty()) cfg.target_command = mTargetElf;
    // Nodes (and the driver's ParameterSet) go in to_delete_, which the base destroys after mUnits.
    auto node = [this](sparta::TreeNode* n) { return to_delete_.emplace_back(n).get(); };
    auto own = [this](auto* unit) {
        mUnits.emplace_back(unit);
        return unit;
    };

    // Use pipeline_width so we never send more than any stage can accept (no dropped packets).
    mFetch = own(new FetchUnit(node(new sparta::TreeNode(getRoot(), "fetch", "Fetch unit")), cfg.instr_size, cfg.pipeline_width));

    // Optional: create ExecutionDriver (Whisper) when target_command is set in config
    if (!cfg.target_command.empty()) {
        sparta::TreeNode* driver_node = node(new sparta::TreeNode(getRoot(), cpu::ExecutionDriver::name, "ExecutionDriver"));
        auto* params = new cpu::ExecutionDriver::ExecutionDriverParamSet(driver_node);
        node(params);
        params->target_command = cfg.target_command;
        mExecutionDriver = own(new cpu::ExecutionDriver(driver_node, params));
        mExecutionDriver->setId(0);
        const bool ok = mExecutionDriver->doSetup(0);
        sparta_assert(ok, "could not set up Whisper for target_command " << cfg.target_command);
        mFetch->setExecutionDriver(mExecutionDriver);
    }

    auto* mid = own(new MidCore(node(new sparta::TreeNode(getRoot(), "midcore", "Mid core"))));
    auto* ls = own(new LSUnit(node(new sparta::TreeNode(getRoot(), "ls", "Load/store unit"))));
    auto* l1_cache = own(new CacheUnit(node(new sparta::TreeNode(getRoot(), "l1_cache", "L1 Cache"))));
    auto* l2_cache = own(new CacheUnit(node(new sparta::TreeNode(getRoot(), "l2_cache", "L2 Cache"))));
    auto* dram = own(new DRAM(node(new sparta::TreeNode(getRoot(), "dram", "DRAM"))));

    mFetch->inst_out.bind(mid->inst_in);
    mid->op_out.bind(ls->op_in);
    if (mExecutionDriver) {
        ls->setExecutionDriver(mExecutionDriver);
    }
    ls->mem_req_out.bind(l1_cache->req_in);
    l1_cache->req_out.bind(l2_cache->req_in);
    l2_cache->req_out.bind(dram->req_in);

    dram->resp_out.bind(l2_cache->resp_in);
    l2_cache->resp_out.bind(l1_cache->resp_in);
    l1_cache->resp_out.bind(ls->resp_in);
}

void TopSim::run(uint64_t run_time) {
    // Send first instruction after DAG is finalized (cannot send in onBindTreeEarly_)
    if (mFetch) {
        mFetch->sendInitial();
    }
    sparta::app::Simulation::run(run_time);
}
