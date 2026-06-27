// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Tenstorrent USA, Inc.

#pragma once

#include "sparta/ports/DataPort.hpp"
#include "sparta/simulation/Unit.hpp"

#include "Logging.hpp"
#include "Types.hpp"

class DRAM : public sparta::Unit {
   public:
    sparta::DataInPort<MemRequest> req_in{&unit_port_set_, "req_in"};

    sparta::DataOutPort<MemResponse> resp_out{&unit_port_set_, "resp_out"};

    DRAM(sparta::TreeNode* node)
        : sparta::Unit(node) {
        req_in.registerConsumerHandler(CREATE_SPARTA_HANDLER_WITH_DATA(DRAM, handleReq, MemRequest));
    }

   private:
    void handleReq(const MemRequest& req) {
        ILOG("[" << getContainer()->getName() << "] cycle " << getClock()->currentCycle() << " received request for addr 0x" << std::hex << req.addr
                 << std::dec);
        MemResponse resp{0xDEADBEEF};
        if (resp_out.isBound()) {
            resp_out.send(resp, 1);
        }
    }
};