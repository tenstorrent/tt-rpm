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
        if (node["instr_size"]) cfg.instr_size = node["instr_size"].as<uint64_t>(DEFAULT_INSTR_SIZE);
        if (node["mFetchwidth"]) cfg.mFetchwidth = node["mFetchwidth"].as<uint64_t>(DEFAULT_FETCH_WIDTH);
        if (node["decode_width"]) cfg.decode_width = node["decode_width"].as<uint64_t>(DEFAULT_DECODE_WIDTH);
        if (node["ls_width"]) cfg.ls_width = node["ls_width"].as<uint64_t>(DEFAULT_LS_WIDTH);
        if (node["l1_load_ports"]) cfg.l1_load_ports = node["l1_load_ports"].as<uint64_t>(DEFAULT_LOAD_PORTS);
        if (node["l2_load_ports"]) cfg.l2_load_ports = node["l2_load_ports"].as<uint64_t>(DEFAULT_LOAD_PORTS);
        if (node["dram_load_ports"]) cfg.dram_load_ports = node["dram_load_ports"].as<uint64_t>(DEFAULT_LOAD_PORTS);
        if (node["target_command"]) cfg.target_command = node["target_command"].as<std::string>("");
        cfg.pipeline_width = std::min({cfg.mFetchwidth, cfg.decode_width, cfg.ls_width, cfg.l1_load_ports, cfg.l2_load_ports, cfg.dram_load_ports});
        if (cfg.pipeline_width == 0) cfg.pipeline_width = 1;
    } catch (...) {
    }
    return cfg;
}
}  // namespace

TopSim::TopSim(sparta::Scheduler* scheduler)
    : sparta::app::Simulation("simple_cpu", scheduler) {}

void TopSim::configureTree_() {}

void TopSim::bindTree_() {}

void TopSim::buildTree_() {
    PipelineConfig cfg = loadPipelineConfig();
    // Use pipeline_width so we never send more than any stage can accept (no dropped packets).
    mFetch = new FetchUnit(new sparta::TreeNode(getRoot(), "fetch", "Fetch unit"), cfg.instr_size, cfg.pipeline_width);

    // Optional: create ExecutionDriver (Whisper) when target_command is set in config
    if (!cfg.target_command.empty()) {
        sparta::TreeNode* driver_node = new sparta::TreeNode(getRoot(), cpu::ExecutionDriver::name, "ExecutionDriver");
        cpu::ExecutionDriver::ExecutionDriverParamSet params(driver_node);
        params.target_command = cfg.target_command;
        mExecutionDriver = new cpu::ExecutionDriver(driver_node, &params);
        mExecutionDriver->setId(0);
        if (mExecutionDriver->doSetup(0)) {
            mFetch->setExecutionDriver(mExecutionDriver);
        }
    }

    auto* mid = new MidCore(new sparta::TreeNode(getRoot(), "midcore", "Mid core"));
    auto* ls = new LSUnit(new sparta::TreeNode(getRoot(), "ls", "Load/store unit"));
    auto* l1_cache = new CacheUnit(new sparta::TreeNode(getRoot(), "l1_cache", "L1 Cache"));
    auto* l2_cache = new CacheUnit(new sparta::TreeNode(getRoot(), "l2_cache", "L2 Cache"));
    auto* dram = new DRAM(new sparta::TreeNode(getRoot(), "dram", "DRAM"));

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
