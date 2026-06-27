// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Tenstorrent USA, Inc.

#pragma once

#include "sparta/ports/DataPort.hpp"
#include "sparta/simulation/Unit.hpp"

#include "Logging.hpp"
#include "Types.hpp"

class MergedRegisterFile {
    std::vector<bool> free_list;
    std::vector<uint64_t> architectural_registers;
    std::vector<uint64_t> speculative_registers;

   public:
    int getFreeRegister() {  // this is emulating a priority encoder i guess
        for (size_t i = 0; i < free_list.size(); i++) {
            if (free_list[i]) {
                free_list[i] = false;
                return i;
            }
        }
        return -1;
    }
    void releaseRegister(uint64_t register_index) { free_list[register_index] = true; }
    void renameRegister(uint64_t register_index, uint64_t physical_id) { architectural_registers[register_index] = physical_id; }
    void commitRegister(uint64_t register_index) { architectural_registers[register_index] = speculative_registers[register_index]; }
};

class IssueQueue {
    std::vector<Instruction> instructions;
};

class MidCore : public sparta::Unit {
   public:
    sparta::DataInPort<Instruction> inst_in{&unit_port_set_, "inst_in"};

    sparta::DataOutPort<Operation> op_out{&unit_port_set_, "op_out"};

    MidCore(sparta::TreeNode* node)
        : sparta::Unit(node) {
        inst_in.registerConsumerHandler(CREATE_SPARTA_HANDLER_WITH_DATA(MidCore, handleInst, Instruction));
    }

   private:
    void handleInst(const Instruction& inst) {
        ILOG("[midcore] cycle " << getClock()->currentCycle() << " received instruction pc=0x" << std::hex << inst.pc << std::dec
                                << (inst.is_mem ? (inst.is_load ? " load" : " store") : " alu"));
        Operation op;
        op.pc = inst.pc;
        op.is_mem = inst.is_mem;
        op.is_load = inst.is_load;
        op.is_store = inst.is_store;
        op.mem_addr = inst.mem_addr;
        op.driver_tag = inst.driver_tag;
        ILOG("[midcore] cycle " << getClock()->currentCycle() << " sending operation pc=0x" << std::hex << op.pc << std::dec << " is_mem=" << op.is_mem
                                << " addr=0x" << std::hex << op.mem_addr << std::dec);
        op_out.send(op, 1);
    }
};
