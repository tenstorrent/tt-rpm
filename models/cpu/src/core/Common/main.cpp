// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Tenstorrent USA, Inc.

#include <cctype>
#include <iostream>
#include <string>

#include "sparta/app/CommandLineSimulator.hpp"

#include "ChipSim.hpp"

static const char* USAGE =
    "Usage: core [options]\n"
    "  -r TICKS        Run for TICKS scheduler ticks (omit to run indefinitely)\n"
    "  -i INSTRS       Stop after retiring INSTRS instructions\n"
    "  --cpu-freq GHZ  CPU clock frequency in GHz (default: 3.0)\n"
    "  -c CONFIG.yaml  Load Sparta config file\n"
    "  -p PATH VALUE   Override a parameter\n"
    "  --show-tree     Print the device tree\n"
    "  --trace-file F  Trace file (csv.zst) for trace-driven mode\n"
    "  --target-elf F  ELF binary path\n"
    "\n"
    "Legacy positional syntax also supported:\n"
    "  core [num_ticks] [trace_file | elf_path]\n";

namespace {
bool looksLikeTrace(const std::string& s) {
    if (s.empty()) return false;
    if (s.find("-trace") != std::string::npos) return true;
    if (s.find("-snap") != std::string::npos) return true;
    if (s.size() >= 8 && s.substr(s.size() - 8) == ".csv.zst") return true;
    if (s.size() >= 4 && s.substr(s.size() - 4) == ".csv") return true;
    return false;
}
bool looksLikeNumber(const std::string& s) {
    if (s.empty()) return false;
    for (char c : s)
        if (!std::isdigit(static_cast<unsigned char>(c))) return false;
    return true;
}
}  // namespace

int main(int argc, char** argv) {
    // Detect legacy positional args and convert to Sparta flags.
    // Legacy: ./core [num_cycles] [trace_file | elf_path]
    // Converted: ./core -r num_cycles --trace-file trace_file  (etc.)
    std::vector<std::string> new_args;
    new_args.push_back(argv[0]);

    bool has_dash_args = false;
    for (int i = 1; i < argc; ++i) {
        if (argv[i][0] == '-') {
            has_dash_args = true;
            break;
        }
    }

    if (!has_dash_args && argc >= 2) {
        // Legacy positional mode
        int pos = 1;
        if (pos < argc && looksLikeNumber(argv[pos])) {
            new_args.push_back("-r");
            new_args.push_back(argv[pos]);
            ++pos;
        }
        if (pos < argc) {
            std::string arg(argv[pos]);
            if (looksLikeTrace(arg)) {
                new_args.push_back("--trace-file");
            } else {
                new_args.push_back("--target-elf");
            }
            new_args.push_back(arg);
            ++pos;
        }
    } else {
        for (int i = 1; i < argc; ++i) {
            new_args.push_back(argv[i]);
        }
    }

    // Build argc/argv for CommandLineSimulator
    std::vector<char*> c_args;
    for (auto& s : new_args) c_args.push_back(s.data());
    int new_argc = static_cast<int>(c_args.size());
    char** new_argv = c_args.data();

    try {
        sparta::app::DefaultValues DEFAULTS;
        sparta::app::CommandLineSimulator cls(USAGE, DEFAULTS);

        std::string trace_file;
        std::string target_elf;
        uint64_t instruction_limit = 0;
        double cpu_freq = 3.0;

        auto& app_opts = cls.getApplicationOptions();
        app_opts.add_options()("trace-file", sparta::app::named_value<std::string>("FILE", &trace_file), "Trace file for trace-driven mode")(
            "target-elf", sparta::app::named_value<std::string>("FILE", &target_elf), "ELF binary path")(
            "instruction-limit,i", sparta::app::named_value<uint64_t>("INSTRS", &instruction_limit)->default_value(0),
            "Stop after retiring this many instructions. 0 (default) means no limit.")(
            "cpu-freq", sparta::app::named_value<double>("GHZ", &cpu_freq)->default_value(3.0), "CPU clock frequency in GHz (default: 3.0)");

        int err_code = 0;
        if (!cls.parse(new_argc, new_argv, err_code)) {
            return err_code;
        }

        sparta::Scheduler scheduler;
        ChipSim sim(&scheduler);

        if (!trace_file.empty())
            sim.setTraceFilenameOverride(trace_file);
        else if (!target_elf.empty())
            sim.setTargetCommandOverride(target_elf);
        if (instruction_limit > 0) sim.setInstructionLimit(instruction_limit);
        if (cpu_freq > 0) sim.setCpuFreqGHz(cpu_freq);

        cls.populateSimulation(&sim);
        cls.runSimulator(&sim);
        cls.postProcess(&sim);
        return 0;

    } catch (const std::exception& e) {
        std::cerr << "[main] ERROR: " << e.what() << "\n";
        return 1;
    }
}
