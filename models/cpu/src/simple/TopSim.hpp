// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Tenstorrent USA, Inc.

#pragma once

#include "sparta/app/Simulation.hpp"
#include "sparta/kernel/Scheduler.hpp"

class FetchUnit;

namespace cpu {
class ExecutionDriver;
}

class TopSim : public sparta::app::Simulation {
   public:
    TopSim(sparta::Scheduler* sched);

    void run(uint64_t run_time) override;

   protected:
    void configureTree_() override;
    void bindTree_() override;
    void buildTree_() override;

   private:
    FetchUnit* mFetch = nullptr;
    cpu::ExecutionDriver* mExecutionDriver = nullptr;
};
