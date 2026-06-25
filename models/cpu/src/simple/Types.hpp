#pragma once
#include <cstdint>
#include <ios>
#include <ostream>

enum class OpClass {
    INVALID = -1,
    ALU = 0,
    MEM = 1,
    BRANCH = 2,
};

struct Instruction {
    uint64_t pc = 0;
    uint64_t driver_tag = 0;  // ExecutionDriver sequence tag (0 = none); used for retire/flush
    bool is_mem = false;      // load/store/amo/vector mem (from Whisper when ExecutionDriver set)
    bool is_load = false;     // load or load part of AMO
    bool is_store = false;    // store or store part of AMO
    uint64_t mem_addr = 0;    // effective data VA for mem ops (from Whisper dataVa())
    int8_t rs1 = -1;
    int8_t rs2 = -1;
    int8_t rd = -1;
    int8_t op_class = static_cast<int8_t>(OpClass::INVALID);
};
inline std::ostream& operator<<(std::ostream& os, const Instruction& i) { return os << "Instruction{pc=0x" << std::hex << i.pc << std::dec << "}"; }

struct Operation {
    uint64_t pc = 0;
    bool is_mem = false;
    bool is_load = false;
    bool is_store = false;
    uint64_t mem_addr = 0;
    uint64_t driver_tag = 0;  // Propagation of Instruction::driver_tag for retire
};
inline std::ostream& operator<<(std::ostream& os, const Operation& o) {
    return os << "Operation{pc=0x" << std::hex << o.pc << std::dec << ", is_mem=" << o.is_mem << ", addr=0x" << o.mem_addr << std::dec << "}";
}

struct MemRequest {
    uint64_t addr = 0;
    bool is_load = true;  // true = load, false = store
};
inline std::ostream& operator<<(std::ostream& os, const MemRequest& r) {
    return os << "MemRequest{addr=0x" << std::hex << r.addr << std::dec << ", load=" << r.is_load << "}";
}

struct MemResponse {
    uint64_t data = 0;
};
inline std::ostream& operator<<(std::ostream& os, const MemResponse& r) { return os << "MemResponse{data=0x" << std::hex << r.data << std::dec << "}"; }
