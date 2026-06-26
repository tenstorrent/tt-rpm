// SPDX-FileCopyrightText: © 2026 Tenstorrent AI ULC
//
// SPDX-License-Identifier: Apache-2.0

#include "sparta/app/Simulation.hpp"
#include "sparta/app/SimulationConfiguration.hpp"
#include "sparta/kernel/Scheduler.hpp"

#include "TopSim.hpp"

int main(int argc, char** argv) {
    sparta::Scheduler scheduler;
    TopSim sim(&scheduler);

    // Configure before buildTree so report_config_ is created (required by finalizeFramework)
    sparta::app::DefaultValues defaults;
    sparta::app::SimulationConfiguration config(defaults);
    sim.configure(argc, argv, &config, false);

    sim.buildTree();
    sim.configureTree();
    sim.finalizeTree();
    sim.finalizeFramework();
    // Run enough cycles for request path (LS->L1->L2->DRAM) and response path (DRAM->L2/L1->LSU)
    sim.run(20);
    return 0;
}
