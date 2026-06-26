// SPDX-FileCopyrightText: © 2026 Tenstorrent AI ULC
//
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <deque>

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
            if (op.is_mem) {
                mPendingRetireTags.push_back(op.driver_tag);
            } else {
                ILOG("[ls] cycle " << getClock()->currentCycle() << " retiring non-mem tag " << op.driver_tag << " pc=0x" << std::hex << op.pc << std::dec);
                mExecutionDriver->retireInstruction(op.driver_tag);
            }
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
        if (mExecutionDriver && !mPendingRetireTags.empty()) {
            uint64_t tag = mPendingRetireTags.front();
            mPendingRetireTags.pop_front();
            ILOG("[ls] cycle " << getClock()->currentCycle() << " retiring mem tag " << tag << " (response received)");
            mExecutionDriver->retireInstruction(tag);
        }
    }

    cpu::ExecutionDriver* mExecutionDriver = nullptr;
    std::deque<uint64_t> mPendingRetireTags;
};
