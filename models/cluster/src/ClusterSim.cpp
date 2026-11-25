// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Tenstorrent USA, Inc.

#include "ClusterSim.hpp"

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "sparta/simulation/Parameter.hpp"
#include "sparta/simulation/TreeNode.hpp"

#include "ClusterCache.hpp"
#include "Common/CoreImpl.hpp"
#include "models/cpu/accel/common/AttachSpec.hpp"
#include "models/cpu/accel/common/CompletionSink.hpp"
#include "models/cpu/accel/devices/DmaEngine.hpp"
#include "models/cpu/accel/frontends/RingFrontEnd.hpp"
#include "models/cpu/common/ExecutionDriver.hpp"
#include "models/cpu/common/SnapshotUtil.hpp"

namespace {

// Hart count of a Whisper snapshot: one `registers<k>` entry per hart, else 1 for a lone `registers`, else 0.
uint32_t snapshotHartCount(const std::string& folder) {
    constexpr std::string_view prefix = "registers";
    uint32_t numbered = 0;
    bool plain = false;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(folder, ec)) {
        const std::string name = entry.path().filename().string();
        if (!name.starts_with(prefix)) continue;
        const std::string_view suffix = std::string_view(name).substr(prefix.size());
        if (suffix.empty()) {
            plain = true;
        } else if (std::all_of(suffix.begin(), suffix.end(), [](unsigned char c) { return std::isdigit(c); })) {
            ++numbered;
        }
    }
    return numbered > 0 ? numbered : (plain ? 1 : 0);
}

// One fully-resolved core: its hartid (== tree node suffix == the hart it drives),
// its flattened CPU-config overrides, and its per-core topology/clock
struct CoreSpec {
    uint32_t hartid = 0;
    std::vector<std::pair<std::string, std::string>> overrides;  // flattened core_config
    double cpu_freq = 0.0;                                       // GHz; 0 => inherit root clock
    std::string type = "default";
};

// One accelerator to attach as a fabric peer (loose coupling: MMIO doorbell + ring).
struct AccelSpec {
    std::string type = "dma";  // device body (only the reference DmaEngine exists)
    uint64_t bar_base = 0;     // MMIO doorbell BAR (required); doorbell register (u64 ring tail) at bar_base+0
    uint64_t bar_size = 0x1000;
    uint64_t ring_base = 0;  // descriptor ring in guest memory (required; 32B descriptors)
    uint32_t ring_slots = 16;
    uint32_t max_outstanding = 8;
    uint32_t queue_depth = 8;   // command ingress credits; full => the ring walk stalls
    double clock_ghz = 0.0;     // device clock; 0 => inherit the shared-cache (fabric) clock
    uint32_t link_latency = 0;  // extra cycles each way device <-> fabric (on-die = 0)
};

// Configuration shared between the cluster harness and the OOO core factory.
struct SimConfig {
    cluster::Cluster::Config cluster;
    uint64_t line_size = 64;
    std::string program;               // ELF to execute on each core
    std::string snapshot;              // Whisper snapshot directory; execution-driven
    std::string core_config;           // optional default core CPU config (type-level fallback)
    std::string core_config_override;  // --core-config: replaces every type's core_config
    std::string config_dir;            // relative core_config paths in the file resolve against it
    uint64_t instruction_limit = 0;    // 0 = no limit
    double cpu_freq = 0.0;             // default clock GHz (0 = inherit); per-type may override
    double cache_freq = 0.0;           // shared-cache (fabric) clock GHz; 0 => 2.0 default
    bool verbose = false;
    std::vector<AccelSpec> accelerators;  // optional fabric peers (empty = cores only)
    // Required schema: a library of named CPU types and the ordered assignment of types
    // to cores. A default-constructed YAML::Node reads as "defined", so presence is
    // tracked by has_schema rather than by testing these nodes.
    bool has_schema = false;  // true iff both cpu_types and cores keys are present
    YAML::Node cpu_types;     // map: name -> {core_config, cpu_freq?}
    YAML::Node cores;         // sequence: {type, count | hartids}
};

// Flatten a core model's Sparta config file into (relative-path, value) overrides.
// The core config is keyed under a single `top.core<N>:` root (e.g. top.core0.icache.
// params.cache_size_kb); we strip that root so each path is relative to the per-core
// node and can be applied to any cluster core (core0, core1, ...).
void flattenNode(const YAML::Node& node, const std::string& prefix, std::vector<std::pair<std::string, std::string>>& out) {
    if (node.IsMap()) {
        for (const auto& kv : node) {
            const std::string key = kv.first.as<std::string>();
            flattenNode(kv.second, prefix.empty() ? key : prefix + "." + key, out);
        }
    } else if (node.IsScalar()) {
        out.emplace_back(prefix, node.Scalar());
    }
}

// A relative path from a cluster config file names a file beside it, so the config runs from any cwd.
std::string resolvePath(const std::string& dir, const std::string& p) {
    if (p.empty() || std::filesystem::path(p).is_absolute()) return p;
    return (std::filesystem::path(dir) / p).string();
}

// Reject keys the loader does not read, so a typo fails instead of leaving the default in place.
void checkKeys(const YAML::Node& map, std::initializer_list<const char*> known, const std::string& where) {
    for (const auto& kv : map) {
        const std::string key = kv.first.as<std::string>();
        if (std::none_of(known.begin(), known.end(), [&](const char* k) { return key == k; })) {
            throw std::runtime_error(where + key + ": unknown key");
        }
    }
}

// Read map[key] as T; a malformed value is an error naming the key, never a fallback.
template <typename T>
T get(const YAML::Node& map, const std::string& key, const std::string& where = "") {
    try {
        return map[key].as<T>();
    } catch (const YAML::Exception&) {
        throw std::runtime_error(where + key + ": invalid value '" + YAML::Dump(map[key]) + "'");
    }
}

double getFreq(const YAML::Node& map, const std::string& key, const std::string& where = "") {
    const double ghz = get<double>(map, key, where);
    if (ghz < 0.0) throw std::runtime_error(where + key + ": must be >= 0 GHz, got " + std::to_string(ghz));
    return ghz;
}

std::vector<std::pair<std::string, std::string>> loadCoreConfigOverrides(const std::string& path) {
    std::vector<std::pair<std::string, std::string>> overrides;
    if (path.empty()) return overrides;
    std::ifstream f(path);
    if (!f.good()) throw std::runtime_error("core_config file not found: " + path);
    YAML::Node root = YAML::Load(f);
    // Descend through the single top-level `top.core<N>` root; everything below it is
    // relative to the per-core tree node.
    for (const auto& kv : root) {
        flattenNode(kv.second, "", overrides);
    }
    return overrides;
}

// Parse a `cores[].hartids` entry into concrete hart ids. Accepts a scalar or sequence,
// where each token is an integer ("3") or an inclusive range ("2-5" -> 2,3,4,5).
std::vector<uint32_t> parseHartids(const YAML::Node& node) {
    std::vector<uint32_t> hartids;
    auto add_token = [&](const std::string& tok) {
        auto dash = tok.find('-');
        if (dash != std::string::npos && dash > 0) {
            uint32_t lo = std::stoul(tok.substr(0, dash));
            uint32_t hi = std::stoul(tok.substr(dash + 1));
            if (hi < lo) throw std::runtime_error("cores: invalid hartid range '" + tok + "'");
            for (uint32_t i = lo; i <= hi; ++i) hartids.push_back(i);
        } else {
            hartids.push_back(std::stoul(tok));
        }
    };
    if (node.IsSequence()) {
        for (const auto& e : node) add_token(e.as<std::string>());
    } else if (node.IsScalar()) {
        add_token(node.Scalar());
    }
    return hartids;
}

// Resolve the cpu_types + cores schema into a per-core spec list (sorted by hartid).
// The schema is required. Throws std::runtime_error on any invalid configuration.
// `shared`: the cores share one Whisper system, so a core's hartid is its hart index there.
std::vector<CoreSpec> buildCoreSpecs(const SimConfig& cfg, bool shared) {
    if (!cfg.has_schema) {
        throw std::runtime_error(
            "cluster config must define both 'cpu_types' and 'cores' "
            "(see config_heterogeneous.yaml)");
    }

    // Flatten each distinct core_config file at most once.
    std::map<std::string, std::vector<std::pair<std::string, std::string>>> flat_cache;
    auto flatten = [&](const std::string& path) -> const std::vector<std::pair<std::string, std::string>>& {
        auto it = flat_cache.find(path);
        if (it != flat_cache.end()) return it->second;
        return flat_cache.emplace(path, loadCoreConfigOverrides(path)).first->second;
    };

    std::vector<CoreSpec> specs;

    // Index the type library by name (values are the raw YAML nodes). Per-type fields
    // are read + defaulted against the top-level when a spec is built. L2-enable is not
    // a type knob -- it lives in the type's core_config file (l2cache.params.enabled).
    std::map<std::string, YAML::Node> types;
    for (const auto& kv : cfg.cpu_types) {
        const std::string name = kv.first.as<std::string>();
        checkKeys(kv.second, {"core_config", "cpu_freq"}, "cpu_types." + name + ".");
        types[name] = kv.second;
    }

    auto make_spec = [&](const std::string& type_name, uint32_t hartid) -> CoreSpec {
        auto it = types.find(type_name);
        if (it == types.end()) {
            std::string valid;
            for (const auto& kv : types) valid += (valid.empty() ? "" : ", ") + kv.first;
            throw std::runtime_error("cores: unknown cpu type '" + type_name + "' (valid: " + valid + ")");
        }
        const YAML::Node& type = it->second;
        CoreSpec s;
        s.hartid = hartid;
        s.type = type_name;
        s.overrides = flatten(!cfg.core_config_override.empty() ? cfg.core_config_override
                              : type["core_config"]             ? resolvePath(cfg.config_dir, type["core_config"].as<std::string>())
                                                                : cfg.core_config);
        s.cpu_freq = type["cpu_freq"] ? getFreq(type, "cpu_freq", "cpu_types." + type_name + ".") : cfg.cpu_freq;
        return s;
    };

    for (const auto& entry : cfg.cores) checkKeys(entry, {"type", "count", "hartids"}, "cores: ");

    std::set<uint32_t> used;
    // Pass 1: entries with explicit hartids reserve their ids first.
    for (const auto& entry : cfg.cores) {
        if (!entry["hartids"]) continue;
        const std::string type_name = entry["type"].as<std::string>();
        for (uint32_t hartid : parseHartids(entry["hartids"])) {
            if (!used.insert(hartid).second) throw std::runtime_error("cores: duplicate hartid " + std::to_string(hartid));
            specs.push_back(make_spec(type_name, hartid));
        }
    }
    // Pass 2: entries with count take the lowest unused hartids.
    uint32_t next = 0;
    for (const auto& entry : cfg.cores) {
        if (entry["hartids"]) continue;
        if (!entry["count"]) throw std::runtime_error("cores: each entry needs 'count' or 'hartids'");
        const std::string type_name = entry["type"].as<std::string>();
        const int count = entry["count"].as<int>();
        if (count <= 0) throw std::runtime_error("cores: 'count' must be > 0");
        for (int c = 0; c < count; ++c) {
            while (used.count(next)) ++next;
            used.insert(next);
            specs.push_back(make_spec(type_name, next));
        }
    }

    if (specs.empty()) throw std::runtime_error("cores: no cores defined");
    std::sort(specs.begin(), specs.end(), [](const CoreSpec& a, const CoreSpec& b) { return a.hartid < b.hartid; });
    for (uint32_t i = 0; shared && i < specs.size(); ++i) {
        if (specs[i].hartid != i) {
            throw std::runtime_error("cores: on a shared Whisper system the " + std::to_string(specs.size()) + " cores must take hartids 0 to " +
                                     std::to_string(specs.size() - 1) + "; hartid " + std::to_string(i) + " is unassigned");
        }
    }
    return specs;
}

SimConfig loadConfig(const std::string& path) {
    SimConfig cfg;
    cfg.cluster.num_cores = 0;  // set from the resolved cores list
    cfg.config_dir = std::filesystem::path(path).parent_path().string();
    std::ifstream f(path);
    if (!f.good()) {
        std::cerr << "[cluster] cannot read cluster config: " << path << "\n";
        std::exit(1);
    }
    try {
        YAML::Node node = YAML::Load(f);
        checkKeys(node,
                  {"cache_hit_latency", "cache_miss_latency", "cluster_cache_size_kb", "cluster_cache_associativity", "line_size", "coherence_enabled",
                   "coherence_latency", "data_carrying", "program", "snapshot", "instruction_limit", "cpu_freq", "cache_freq", "core_config", "verbose",
                   "cpu_types", "cores", "accelerators"},
                  "");
        if (node["cache_hit_latency"]) cfg.cluster.cache_hit_latency = get<uint32_t>(node, "cache_hit_latency");
        if (node["cache_miss_latency"]) cfg.cluster.cache_miss_latency = get<uint32_t>(node, "cache_miss_latency");
        if (node["cluster_cache_size_kb"]) cfg.cluster.cache_size_kb = get<uint64_t>(node, "cluster_cache_size_kb");
        if (node["cluster_cache_associativity"]) cfg.cluster.cache_associativity = get<uint32_t>(node, "cluster_cache_associativity");
        if (node["line_size"]) {
            cfg.line_size = get<uint64_t>(node, "line_size");
            cfg.cluster.cache_line_size = cfg.line_size;
        }
        if (node["coherence_enabled"]) cfg.cluster.coherence_enabled = get<bool>(node, "coherence_enabled");
        if (node["coherence_latency"]) cfg.cluster.coherence_latency = get<uint32_t>(node, "coherence_latency");
        if (node["data_carrying"]) cfg.cluster.data_carrying = get<bool>(node, "data_carrying");
        if (node["program"]) cfg.program = get<std::string>(node, "program");
        if (node["snapshot"]) cfg.snapshot = get<std::string>(node, "snapshot");
        if (node["instruction_limit"]) cfg.instruction_limit = get<uint64_t>(node, "instruction_limit");
        if (node["cpu_freq"]) cfg.cpu_freq = getFreq(node, "cpu_freq");
        if (node["cache_freq"]) cfg.cache_freq = getFreq(node, "cache_freq");
        if (node["core_config"]) cfg.core_config = resolvePath(cfg.config_dir, get<std::string>(node, "core_config"));
        if (node["verbose"]) cfg.verbose = get<bool>(node, "verbose");
        // Heterogeneous schema (optional): stored raw, resolved after CLI overrides.
        // Both keys must be present; presence is recorded explicitly (a default
        // YAML::Node reads as "defined", so the stored nodes can't report absence).
        const bool has_types = static_cast<bool>(node["cpu_types"]);
        const bool has_cores = static_cast<bool>(node["cores"]);
        if (has_types) cfg.cpu_types = node["cpu_types"];
        if (has_cores) cfg.cores = node["cores"];
        cfg.has_schema = has_types && has_cores;
        // Optional accelerator fabric peers. Each core's dcache uncacheable range must cover the
        // BAR so doorbell stores reach the fabric (checked in configureTree_).
        if (node["accelerators"]) {
            for (const auto& e : node["accelerators"]) {
                checkKeys(e, {"type", "bar_base", "bar_size", "ring_base", "ring_slots", "max_outstanding", "queue_depth", "clock_ghz", "link_latency"},
                          "accelerators: ");
                for (const char* key : {"bar_base", "ring_base"}) {
                    if (!e[key]) throw std::runtime_error(std::string("accelerators: each entry needs '") + key + "'");
                }
                AccelSpec a;
                if (e["type"]) a.type = e["type"].as<std::string>();
                if (e["bar_base"]) a.bar_base = e["bar_base"].as<uint64_t>();
                if (e["bar_size"]) a.bar_size = e["bar_size"].as<uint64_t>();
                if (e["ring_base"]) a.ring_base = e["ring_base"].as<uint64_t>();
                if (e["ring_slots"]) a.ring_slots = e["ring_slots"].as<uint32_t>();
                if (e["max_outstanding"]) a.max_outstanding = e["max_outstanding"].as<uint32_t>();
                if (e["queue_depth"]) a.queue_depth = e["queue_depth"].as<uint32_t>();
                if (e["clock_ghz"]) a.clock_ghz = e["clock_ghz"].as<double>();
                if (e["link_latency"]) a.link_latency = e["link_latency"].as<uint32_t>();
                cfg.accelerators.push_back(a);
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "[cluster] config error in " << path << ": " << e.what() << "\n";
        std::exit(1);
    }
    return cfg;
}
}  // namespace

ClusterSim::ClusterSim(sparta::Scheduler* scheduler)
    : sparta::app::Simulation("cluster", scheduler) {
    // Register the OOO core unit factories so cores can be built as
    // ResourceTreeNodes (shared with the single-core ChipSim).
    core::registerCoreResources(getResourceSet());
}

// The cluster and the cores free their nodes and units, which Sparta allows only in teardown.
ClusterSim::~ClusterSim() { getRoot()->enterTeardown(); }

// Parameters are final here (config files, -p and the core_config overrides) and not yet read.
void ClusterSim::configureTree_() {
    for (uint32_t id : mCoreHartids) {
        const std::string dcache = "core" + std::to_string(id) + ".dcache.params.";
        const uint64_t ub = getRoot()->getChildAs<sparta::Parameter<uint64_t>>(dcache + "uncacheable_base")->getValue();
        const uint64_t us = getRoot()->getChildAs<sparta::Parameter<uint64_t>>(dcache + "uncacheable_size")->getValue();
        for (const auto& [base, size] : mAccelBars) {
            if (base < ub || base + size > ub + us) {
                std::cerr << "[cluster] top." << dcache << "uncacheable_base/uncacheable_size [0x" << std::hex << ub << ", 0x" << ub + us
                          << ") must cover the accelerator BAR [0x" << base << ", 0x" << base + size << std::dec << "), or doorbell stores stay in the L1\n";
                std::exit(1);
            }
        }
    }
    // Cores share their output-file params; give each its own file or the last core's dump overwrites the rest.
    if (mCoreHartids.size() < 2) return;
    for (uint32_t id : mCoreHartids) {
        for (const char* name : {"visualizer_output_file", "visualizer_debug_file", "cache_viewer_output_file"}) {
            auto* p = getRoot()->getChildAs<sparta::ParameterBase>("core" + std::to_string(id) + ".params." + name);
            const std::filesystem::path path = p->getValueAsString();
            if (path.empty() || path == "stderr" || path == "stdout") continue;
            p->setValueFromString((path.parent_path() / (path.stem().string() + ".core" + std::to_string(id) + path.extension().string())).string());
        }
    }
}

void ClusterSim::buildTree_() {
    SimConfig cfg = loadConfig(mClusterConfigPath);

    // CLI overrides win over the config file (mirrors the single-core flag handling).
    if (!mProgramOverride.empty()) cfg.program = mProgramOverride;
    if (!mSnapshotOverride.empty()) cfg.snapshot = mSnapshotOverride;
    cfg.core_config_override = mCoreConfigOverride;
    if (mInstructionLimitOverride > 0) cfg.instruction_limit = mInstructionLimitOverride;

    // --cpu-freq replaces only the default clock; a cpu_type with its own cpu_freq keeps it.
    if (mCpuFreqOverride != 0.0) cfg.cpu_freq = mCpuFreqOverride;

    mVerbose = cfg.verbose;

    // Drop in the out-of-order core from models/cpu/src/core by supplying its
    // factory. The cluster template is unchanged regardless of the core: it only
    // ever sees cluster::CoreIf. The OOO core uses an external last-level cache so
    // its private L1s route fills/writebacks to the shared cluster cache.
    auto* sim = this;
    const std::string program = cfg.program;
    const bool snapshot_mode = !cfg.snapshot.empty();

    // Resolve the snapshot folder up front so we can inspect the restored harts.
    std::string snapshot_folder;
    if (snapshot_mode) {
        snapshot_folder = cpu::generateSnapshotFolderName(cfg.snapshot);
        if (snapshot_folder.empty()) {
            std::cerr << "[cluster] could not resolve snapshot folder from: " << cfg.snapshot << "\n";
            std::exit(1);
        }
    }

    // A single-hart snapshot (or one with no register files) gives every core a private copy of the system;
    // an ELF or a multi-hart snapshot runs on one system shared by all cores.
    const uint32_t restored_harts = snapshot_mode ? snapshotHartCount(snapshot_folder) : 0;
    const bool private_systems = snapshot_mode && restored_harts <= 1;
    // Accelerator BARs and descriptors are untagged PAs in one shared memory.
    if (private_systems && !cfg.accelerators.empty()) {
        std::cerr << "[cluster] config error: accelerators require a shared Whisper system; " << snapshot_folder << " is not a multi-hart snapshot\n";
        std::exit(1);
    }

    // Full-system snapshots ship their own whisper config (linux.json / whisper.json).
    std::string snapshot_config;
    if (snapshot_mode) {
        for (const char* name : {"linux.json", "whisper.json"}) {
            std::string p = snapshot_folder + "/" + name;
            if (std::filesystem::exists(p)) {
                snapshot_config = p;
                break;
            }
        }
    }
    const uint64_t instruction_limit = cfg.instruction_limit;
    const uint64_t line_size = cfg.line_size;
    const bool data_carrying = cfg.cluster.data_carrying;

    std::vector<CoreSpec> core_specs;
    try {
        core_specs = buildCoreSpecs(cfg, !private_systems);
    } catch (const std::exception& e) {
        std::cerr << "[cluster] config error: " << e.what() << "\n";
        std::exit(1);
    }
    const uint32_t num_cores = static_cast<uint32_t>(core_specs.size());
    for (const auto& s : core_specs) mCoreHartids.push_back(s.hartid);
    cfg.cluster.num_cores = num_cores;

    if (snapshot_mode && !private_systems && restored_harts != num_cores) {
        std::cerr << "[cluster] config error: " << num_cores << " cores configured, but " << snapshot_folder << " holds state for " << restored_harts
                  << " harts\n";
        std::exit(1);
    }

    // Per-type breakdown for the banner (e.g. "big=[0,1], little=[2,3,4,5]").
    {
        std::map<std::string, std::vector<uint32_t>> by_type;
        for (const auto& s : core_specs) by_type[s.type].push_back(s.hartid);
        std::ostringstream os;
        bool first = true;
        for (const auto& kv : by_type) {
            os << (first ? "" : ", ") << kv.first << "=[";
            for (size_t i = 0; i < kv.second.size(); ++i) os << (i ? "," : "") << kv.second[i];
            os << "]";
            first = false;
        }
        std::cout << "[cluster] cores: " << os.str() << "\n";
    }

    cluster::CoreFactory factory = [sim, program, snapshot_folder, snapshot_mode, snapshot_config, instruction_limit, line_size, data_carrying, num_cores,
                                    private_systems, core_specs](sparta::TreeNode* parent, uint32_t index) -> std::unique_ptr<cluster::CoreIf> {
        // `index` is the build position; the real core/hart id (and per-core config)
        // comes from the resolved spec at that position.
        const CoreSpec& s = core_specs[index];
        // s.hartid is the core/hart id; `index` is only the build position.
        core::CoreImpl::Config cc;
        cc.sim = sim;
        cc.index = s.hartid;           // drives node name core<id>, clock, driver->setId(hart)
        cc.external_cache = true;      // every cluster core sits behind the shared cache
        cc.cpu_freq_ghz = s.cpu_freq;  // 0 = inherit root clock
        cc.private_system = private_systems;
        cc.shared_harts = private_systems ? 1 : num_cores;
        if (snapshot_mode) {
            // Execution-driven from a snapshot; Whisper runs forward from the restored state.
            cc.snapshot_folder = snapshot_folder;
            if (!snapshot_config.empty()) cc.whisper_flags = {"--configfile", snapshot_config};
        } else {
            cc.program = program;
        }
        // Everything about the core -- geometry, caches, and L2-enable
        // (l2cache.params.enabled) -- comes from its own config file.
        cc.param_overrides = s.overrides;
        cc.instruction_limit = instruction_limit;
        cc.line_size = line_size;
        cc.data_carrying = data_carrying;
        return std::make_unique<core::CoreImpl>(parent, cc);
    };
    std::cout << "[cluster] core = out-of-order (models/cpu/src/core)\n";
    if (!snapshot_mode) {
        std::cout << "[cluster] whisper: ELF on one shared " << num_cores << "-hart system\n";
    } else if (private_systems) {
        std::cout << "[cluster] whisper: single-hart snapshot, a private system per core\n";
    } else {
        std::cout << "[cluster] whisper: multi-hart snapshot on one shared system\n";
    }
    if (snapshot_mode) {
        std::cout << "[cluster] snapshot: " << snapshot_folder << "  harts: " << restored_harts
                  << "  config: " << (snapshot_config.empty() ? "<none>" : snapshot_config) << "\n";
    }

    // The shared cache runs in its own fabric clock domain, reached across sync ports. It
    // may clock below the cores (access latency lives in the cache domain, the crossing is
    // a fixed 1-cycle delay). When no core is clocked, keep the cache on the root clock too
    // (a 0 MHz root has no posedge to synchronize against, so the crossing must be uniform).
    bool any_core_clocked = false;
    for (const auto& s : core_specs) any_core_clocked |= (s.cpu_freq > 0.0);
    // An unclocked core sits on the 0 MHz root, which has no edge to cross into a clocked domain.
    for (const auto& s : core_specs) {
        if ((any_core_clocked || cfg.cache_freq > 0.0) && s.cpu_freq <= 0.0) {
            std::cerr << "[cluster] config error: cpu_types." << s.type << ".cpu_freq (or cpu_freq) must be set when another core or cache_freq is clocked\n";
            std::exit(1);
        }
    }
    constexpr double kDefaultCacheFreqGhz = 2.0;
    const double cache_freq = cfg.cache_freq > 0.0 ? cfg.cache_freq : (any_core_clocked ? kDefaultCacheFreqGhz : 0.0);
    const sparta::Clock* cache_clk = getClockManager().getRoot().get();
    if (cache_freq > 0.0) {
        auto cache_clock = getClockManager().makeClock("cluster_cache_clock", getClockManager().getRoot(), cache_freq * 1000.0);
        getClockManager().normalize();
        cache_clk = cache_clock.get();  // ClockManager retains ownership
        std::cout << "[cluster] shared-cache clock: " << cache_freq << " GHz\n";
    }

    mCluster = std::make_unique<cluster::Cluster>(getRoot(), std::move(factory), cfg.cluster, cache_clk);

    // Accelerators: fabric peers attached beside the cores. Oracle hooks and the MMIO
    // doorbell route are wired after build() — the drivers exist only once cores are up.
    for (uint32_t ai = 0; ai < cfg.accelerators.size(); ++ai) {
        const auto& a = cfg.accelerators[ai];
        if (a.type != "dma") {
            std::cerr << "[cluster] unknown accelerator type: " << a.type << "\n";
            std::exit(1);
        }
        accel::AttachSpec attach;
        attach.link_latency_cycles = a.link_latency;
        if (a.clock_ghz > 0.0) {
            // Same rule as the shared-cache clock above: a 0 MHz root has no posedge to
            // synchronize against, so every domain must be clocked or none of them. Giving
            // the device its own clock over an unclocked fabric wedges the crossing.
            if (cache_freq <= 0.0) {
                std::cerr << "[cluster] accel" << ai << ": clock_ghz requires a clocked fabric; set cache_freq (and cpu_freq)\n";
                std::exit(1);
            }
            auto accel_clock = getClockManager().makeClock("accel" + std::to_string(ai) + "_clock", getClockManager().getRoot(), a.clock_ghz * 1000.0);
            getClockManager().normalize();
            attach.clock = accel_clock.get();  // ClockManager retains ownership
        }
        const uint32_t max_out = a.max_outstanding;
        const uint32_t depth = a.queue_depth;
        mCluster->addAccelerator(
            [line_size, max_out, depth](sparta::TreeNode* node, uint32_t) -> std::unique_ptr<accel::AcceleratorIf> {
                accel::DmaEngine::Params p;
                p.line_size = static_cast<uint32_t>(line_size);
                p.max_outstanding = max_out;
                p.cmd_queue_depth = depth;
                p.self_tick = true;
                return std::make_unique<accel::DmaEngine>(node, p);
            },
            attach);
    }

    mCluster->build();

    // Oracle + doorbell wiring. The device's data path is the shared Whisper memory,
    // reached through core 0's driver with physical addresses (loose coupling).
    if (!cfg.accelerators.empty()) {
        cpu::ExecutionDriver* drv = mCluster->numCores() > 0 ? mCluster->core(0)->executionDriver() : nullptr;
        if (!drv) {
            std::cerr << "[cluster] accelerators require an oracle: core 0 has no ExecutionDriver\n";
            std::exit(1);
        }
        const accel::DmaEngine::PeekFn peek = [drv](uint64_t addr, uint8_t* buf, uint32_t len) { drv->peekPhysBytes(addr, buf, len, /*translate=*/false); };
        const accel::DmaEngine::PokeFn poke = [drv](uint64_t addr, const uint8_t* buf, uint32_t len) {
            return drv->pokePhysBytes(addr, buf, len, /*translate=*/false);
        };
        for (uint32_t i = 0; i < cfg.accelerators.size(); ++i) {
            const auto& a = cfg.accelerators[i];
            auto* dma = dynamic_cast<accel::DmaEngine*>(mCluster->accelerator(i));
            sparta_assert(dma, "accelerator " << i << " is not a DmaEngine");
            dma->setOracleHooks(peek, poke);
            auto ring = std::make_unique<accel::RingFrontEnd>(dma, a.ring_base, a.ring_slots, a.bar_base, peek);
            mCluster->cache()->mmio().addRegion(a.bar_base, a.bar_size, [r = ring.get()](const mem::Request& req) { r->onDoorbell(req); });
            dma->setCommandSource(ring.get());  // the device ticks it, so a credit stall resumes
            mAccelRings.push_back(std::move(ring));
            mAccelBars.emplace_back(a.bar_base, a.bar_size);
            mAccelSinks.push_back(std::make_unique<accel::FunctionSink>(
                [i](const accel::AccelCompletion& c) { std::cout << "[accel" << i << "] completion tag=" << c.tag << "\n"; }));
            dma->setCompletionSink(mAccelSinks.back().get());
            std::cout << "[cluster] accel" << i << " = dma  bar=0x" << std::hex << a.bar_base << " ring=0x" << a.ring_base << std::dec
                      << " slots=" << a.ring_slots << " queue_depth=" << a.queue_depth
                      << " clock=" << (a.clock_ghz > 0.0 ? std::to_string(a.clock_ghz) + "GHz" : std::string("fabric")) << " link_latency=" << a.link_latency
                      << "\n";
        }
    }
}

void ClusterSim::bindTree_() {
    mCluster->bind();

    if (mVerbose) {
        mTaps.push_back(std::make_unique<sparta::log::Tap>(getRoot(), "info", std::cout));
        std::cout << "[cluster] Verbose logging enabled (info -> stdout)\n";
    }
}

void ClusterSim::run(uint64_t run_time) {
    mCluster->start();
    sparta::app::Simulation::run(run_time);
    // The factory builds only CoreImpl cores.
    for (uint32_t i = 0; i < mCluster->numCores(); ++i) static_cast<core::CoreImpl*>(mCluster->core(i))->finish_();
    if (mCluster->cache()) mCluster->cache()->reportStats();
}
