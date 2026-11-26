// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Tenstorrent USA, Inc.

#pragma once

#include <algorithm>
#include <deque>
#include <utility>

#include "sparta/ports/DataPort.hpp"
#include "sparta/simulation/Unit.hpp"

#include "Logging.hpp"
#include "Types.hpp"
#include "models/cpu/common/ExecutionDriver.hpp"
#include "models/cpu/common/whisper_include_fix.hpp"

class LSUnit : public sparta::Unit {
   public:
    sparta::DataInPort<Operation> op_in{&unit_port_set_, "op_in"};

    sparta::DataOutPort<MemRequest> mem_req_out{&unit_port_set_, "mem_req_out"};

    sparta::DataInPort<MemResponse> resp_in{&unit_port_set_, "resp_in"};

    void setExecutionDriver(cpu::ExecutionDriver* driver) { mExecutionDriver = driver; }

    LSUnit(sparta::TreeNode* node)
        : sparta::Unit(node) {
        op_in.registerConsumerHandler(CREATE_SPARTA_HANDLER_WITH_DATA(LSUnit, handleOp, Operation));
        resp_in.registerConsumerHandler(CREATE_SPARTA_HANDLER_WITH_DATA(LSUnit, handleResp, MemResponse));
    }

   private:
    void handleOp(const Operation& op) {
        ILOG("[ls] cycle " << getClock()->currentCycle() << " received operation pc=0x" << std::hex << op.pc << std::dec << " is_mem=" << op.is_mem
                           << " addr=0x" << op.mem_addr);
        if (mExecutionDriver) {
            mInFlight.push_back({op.driver_tag, !op.is_mem});
            retireCompleted();
        }
        if (op.is_mem) {
            MemRequest req;
            req.addr = op.mem_addr;
            req.is_load = op.is_load;
            ILOG("[ls] cycle " << getClock()->currentCycle() << " sending mem request " << (req.is_load ? "load" : "store") << " addr=0x" << std::hex
                               << req.addr << std::dec);
            mem_req_out.send(req, 1);
        }
    }

    void handleResp(const MemResponse& resp) {
        ILOG("[ls] cycle " << getClock()->currentCycle() << " received response data 0x" << std::hex << resp.data << std::dec
                           << " -- execution complete (simple pipeline)");
        if (mExecutionDriver) {
            // Responses return in request order, so this completes the oldest outstanding mem op.
            auto it = std::find_if(mInFlight.begin(), mInFlight.end(), [](const auto& e) { return !e.second; });
            if (it != mInFlight.end()) it->second = true;
            retireCompleted();
        }
    }

    // ExecutionDriver requires program-order retirement: a non-mem op waits behind older mem ops.
    void retireCompleted() {
        while (!mInFlight.empty() && mInFlight.front().second) {
            ILOG("[ls] cycle " << getClock()->currentCycle() << " retiring tag " << mInFlight.front().first);
            mExecutionDriver->retireInstruction(mInFlight.front().first);
            mInFlight.pop_front();
        }
    }

    cpu::ExecutionDriver* mExecutionDriver = nullptr;
    std::deque<std::pair<uint64_t, bool>> mInFlight;  // (driver tag, completed), program order
};
