// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Tenstorrent USA, Inc.

#include <cstdlib>
#include <iostream>
#include <string>

#include "sparta/app/Simulation.hpp"
#include "sparta/app/SimulationConfiguration.hpp"
#include "sparta/kernel/Scheduler.hpp"

#include "TopSim.hpp"

int main(int argc, char** argv) {
    // Usage: simple [TICKS [ELF]]. ELF overrides config.yaml's target_command.
    uint64_t run_ticks = 20;
    std::string elf;
    char* end = nullptr;
    if (argc > 1) run_ticks = std::strtoull(argv[1], &end, 10);
    if (argc > 3 || (argc > 1 && (*argv[1] == '\0' || *end != '\0'))) {
        std::cerr << "usage: " << argv[0] << " [TICKS [ELF]]\n";
        return 1;
    }
    if (argc > 2) elf = argv[2];

    try {
        sparta::Scheduler scheduler;
        TopSim sim(&scheduler, elf);

        // Configure before buildTree so report_config_ is created (required by finalizeFramework)
        sparta::app::DefaultValues defaults;
        sparta::app::SimulationConfiguration config(defaults);
        sim.configure(argc, argv, &config, false);

        sim.buildTree();
        sim.configureTree();
        sim.finalizeTree();
        sim.finalizeFramework();
        // Ends early once an ELF exits and the pipeline drains (no events left).
        sim.run(run_ticks);
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
