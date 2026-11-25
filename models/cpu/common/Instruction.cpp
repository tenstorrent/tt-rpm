// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Tenstorrent USA, Inc.

#include "Instruction.hpp"

#include <array>
#include <string>
#include <string_view>

namespace cpu {

static InstClass classifyAlu(const WhisperUtil::TraceRecord& rec) {
    const std::string name = rec.instructionName();  // returned by value
    // Prefix matching also covers the high-half and width variants (mulh, mulw, divuw, remw, etc.)
    if (name.starts_with("mul")) return InstClass::Multiply;
    if (name.starts_with("div") || name.starts_with("rem")) return InstClass::Divide;
    return InstClass::ALU;
}

static InstClass classifyFromTraceRecord(const WhisperUtil::TraceRecord& rec) {
    switch (rec.instType) {
        case 'l':
        case 's':
            // The trace types LR as a load and SC as a store; both are atomics.
            if (rec.assembly.starts_with("lr.") || rec.assembly.starts_with("sc.")) return InstClass::Atomic;
            return rec.instType == 'l' ? InstClass::Load : InstClass::Store;
        case 't':
        case 'n':
            return InstClass::Branch;
        case 'j':
        case 'c':
        case 'r':
            return InstClass::Jump;
        case 'f':
            return InstClass::Float;
        case 'v':
            return InstClass::Vector;
        case 'a':
            return InstClass::Atomic;
        default:
            return classifyAlu(rec);
    }
}

// vtype is an implicit source of every vector instruction; PerfApi fills its value only at execute.
static bool vectorSewAtMost32(const TT_PERF::InstrPac* pac) {
    if (!pac || !pac->executed()) return false;
    std::array<TT_PERF::Operand, 8> ops;
    const unsigned count = pac->getImplicitSrcOperands(ops);
    for (unsigned i = 0; i < count; i++) {
        if (ops[i].type == WdRiscv::OperandType::CsReg && ops[i].number == static_cast<unsigned>(WdRiscv::CsrNumber::VTYPE)) {
            return ((ops[i].value.scalar >> 3) & 7) <= 2;  // vsew 0-2 is SEW 8-32
        }
    }
    return false;
}

static FpDivPrecision classifyFpDiv(const WhisperUtil::TraceRecord& rec, const TT_PERF::InstrPac* pac) {
    if (rec.instType != 'f' && rec.instType != 'v') return FpDivPrecision::None;
    const std::string_view assembly = rec.assembly;
    const std::string_view name = assembly.substr(0, assembly.find(' '));
    if (name.starts_with("fdiv.") || name.starts_with("fsqrt.")) {
        return (name.ends_with(".s") || name.ends_with(".h")) ? FpDivPrecision::Single : FpDivPrecision::Double;
    }
    if (name.starts_with("vfdiv.") || name.starts_with("vfrdiv.") || name.starts_with("vfsqrt.")) {
        return vectorSewAtMost32(pac) ? FpDivPrecision::Single : FpDivPrecision::Double;
    }
    return FpDivPrecision::None;
}

Instruction::Instruction(const WhisperUtil::TraceRecord& rec, instid_t id, coreid_t coreId, std::shared_ptr<TT_PERF::InstrPac> pacPtr)
    : TrackedAllocationPerCore<Instruction>(coreId),
      mRecord(rec),
      mId(id),
      mPacPtr(std::move(pacPtr)),
      mInstClass(classifyFromTraceRecord(rec)),
      mFpDivPrecision(classifyFpDiv(rec, mPacPtr.get())) {}

void Instruction::refreshInstClass() {
    mInstClass = classifyFromTraceRecord(mRecord);
    mFpDivPrecision = classifyFpDiv(mRecord, mPacPtr.get());
}

}  // namespace cpu
