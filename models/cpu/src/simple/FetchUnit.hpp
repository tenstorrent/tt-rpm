// SPDX-FileCopyrightText: © 2026 Tenstorrent AI ULC
//
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "sparta/events/UniqueEvent.hpp"
#include "sparta/ports/DataPort.hpp"
#include "sparta/simulation/Unit.hpp"

#include "Logging.hpp"
#include "Types.hpp"

namespace cpu {
class ExecutionDriver;
}

class FetchUnit : public sparta::Unit {
   public:
    sparta::DataOutPort<Instruction> inst_out{&unit_port_set_, "inst_out"};

    // instr_size: bytes per instruction. fetch_width: instructions sent per cycle (min of all stage widths; no drops).
    FetchUnit(sparta::TreeNode* node, uint64_t instr_size = 4, uint64_t fetch_width = 2)
        : sparta::Unit(node),
          mInstrSize(instr_size),
          mFetchWidth(fetch_width),
          mEvFetch(&unit_event_set_, "ev_fetch", CREATE_SPARTA_HANDLER(FetchUnit, fetchNext), 0) {}

    void onBindTreeEarly_() override {}

    // Start sequential fetch: schedule the first fetch for next cycle.
    void sendInitial() { mEvFetch.schedule(1); }

    // When set, fetch is driven by Whisper via ExecutionDriver (real program execution).
    void setExecutionDriver(cpu::ExecutionDriver* driver) { mExecutionDriver = driver; }

   private:
    void fetchNext();

    uint64_t mInstrSize;
    uint64_t mFetchWidth;
    sparta::UniqueEvent<> mEvFetch;
    uint64_t mPc = 0x1000;
    cpu::ExecutionDriver* mExecutionDriver = nullptr;
    bool mFirstWhisperFetch = true;
};
