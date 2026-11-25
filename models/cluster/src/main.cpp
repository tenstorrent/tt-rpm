// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Tenstorrent USA, Inc.

#include <iostream>
#include <string>

#include "sparta/app/CommandLineSimulator.hpp"
#include "sparta/kernel/Scheduler.hpp"

#include "ClusterSim.hpp"

// Cluster entry point. Mirrors the single-core model (models/cpu/src/core/Common/
// main.cpp): drive the simulation through Sparta's CommandLineSimulator so we get
// -r (run ticks, omit to run indefinitely), -i (instruction limit), -c/-p, report
// post-processing, --show-tree, etc. Cluster topology (cpu_types + cores, cache
// latencies, line_size) and the dropped-in cores' CPU configs come from the cluster
// config file; CLI flags override the workload/limit knobs.
static const char* USAGE =
    "Usage: cluster [options]\n"
    "  -r TICKS              Run for TICKS scheduler ticks (omit to run indefinitely)\n"
    "  -i INSTRS             Stop each core after retiring INSTRS instructions\n"
    "  --cluster-config F    Cluster config file (default: config.yaml)\n"
    "  --core-config F       CPU config for every core (replaces each cpu_type's core_config)\n"
    "  --target-elf F        ELF to execute on every core (shared multi-hart Whisper)\n"
    "  --snapshot PATH       Whisper snapshot directory; runs execution-driven.\n"
    "                        Topology is inferred from the snapshot: multi-hart (registers0..N-1)\n"
    "                        runs one shared Whisper; single-hart runs an independent Whisper per core\n"
    "                        The run ends when any core's program exits or commits the ROI-exit HINT\n"
    "                        (addi x0, x12, imm); bound snapshots without one with -i or -r\n"
    "  --cpu-freq GHZ        CPU clock frequency in GHz\n"
    "  -c CONFIG.yaml        Load a Sparta param config\n"
    "  -p PATH VALUE         Override a parameter\n"
    "  --show-tree           Print the device tree\n";

int main(int argc, char** argv) {
    try {
        sparta::app::DefaultValues DEFAULTS;
        sparta::app::CommandLineSimulator cls(USAGE, DEFAULTS);

        std::string cluster_config = "config.yaml";
        std::string core_config;
        std::string target_elf;
        std::string snapshot;
        uint64_t instruction_limit = 0;
        double cpu_freq = 0.0;

        auto& app_opts = cls.getApplicationOptions();
        app_opts.add_options()("cluster-config", sparta::app::named_value<std::string>("FILE", &cluster_config)->default_value("config.yaml"),
                               "Cluster config file (topology + workload defaults)")("core-config", sparta::app::named_value<std::string>("FILE", &core_config),
                                                                                     "CPU config for every core (replaces each cpu_type's core_config)")(
            "target-elf", sparta::app::named_value<std::string>("FILE", &target_elf), "ELF to execute on every core")(
            "snapshot", sparta::app::named_value<std::string>("PATH", &snapshot),
            "Whisper snapshot directory; execution-driven. Topology inferred from hart count")(
            "instruction-limit,i", sparta::app::named_value<uint64_t>("INSTRS", &instruction_limit)->default_value(0),
            "Stop each core after retiring this many instructions (0 = no limit)")(
            "cpu-freq", sparta::app::named_value<double>("GHZ", &cpu_freq)->default_value(0.0), "CPU clock frequency in GHz");

        int err_code = 0;
        if (!cls.parse(argc, argv, err_code)) {
            return err_code;
        }

        sparta::Scheduler scheduler;
        ClusterSim sim(&scheduler);

        sim.setClusterConfigPath(cluster_config);
        if (!core_config.empty()) sim.setCoreConfigOverride(core_config);
        if (!target_elf.empty()) sim.setTargetCommandOverride(target_elf);
        if (!snapshot.empty()) sim.setSnapshotOverride(snapshot);
        if (instruction_limit > 0) sim.setInstructionLimit(instruction_limit);
        if (cpu_freq > 0) sim.setCpuFreqGHz(cpu_freq);

        cls.populateSimulation(&sim);
        cls.runSimulator(&sim);
        cls.postProcess(&sim);
        return 0;

    } catch (const std::exception& e) {
        std::cerr << "[cluster] ERROR: " << e.what() << "\n";
        return 1;
    }
}
