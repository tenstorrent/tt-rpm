// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Tenstorrent USA, Inc.

#pragma once

#include <memory>
#include <string>
#include <vector>

#include "sparta/app/Simulation.hpp"
#include "sparta/kernel/Scheduler.hpp"
#include "sparta/simulation/Unit.hpp"

class FetchUnit;

namespace cpu {
class ExecutionDriver;
}

class TopSim : public sparta::app::Simulation {
   public:
    TopSim(sparta::Scheduler* sched, const std::string& target_elf = "");
    ~TopSim() override;

    void run(uint64_t run_time) override;

   protected:
    void configureTree_() override;
    void bindTree_() override;
    void buildTree_() override;

   private:
    FetchUnit* mFetch = nullptr;
    cpu::ExecutionDriver* mExecutionDriver = nullptr;
    std::string mTargetElf;  // overrides config.yaml target_command when set
    std::vector<std::unique_ptr<sparta::Unit>> mUnits;
};
