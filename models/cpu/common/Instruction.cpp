// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Tenstorrent USA, Inc.

#include "Instruction.hpp"

#include <string_view>

namespace cpu {

static InstClass classifyAlu(const WhisperUtil::TraceRecord& rec) {
    std::string_view name = rec.instructionName();
    // Strip any width suffix (mulw, divuw, remw, etc.) for prefix matching
    if (name.starts_with("mul") || name.starts_with("mulh")) return InstClass::Multiply;
    if (name.starts_with("div") || name.starts_with("rem")) return InstClass::Divide;
    return InstClass::ALU;
}

static uint8_t latencyForClass(InstClass c) {
    switch (c) {
        case InstClass::ALU:
            return 1;
        case InstClass::Multiply:
            return 3;
        case InstClass::Divide:
            return 20;
        case InstClass::Load:
            return 1;
        case InstClass::Store:
            return 1;
        case InstClass::Branch:
            return 1;
        case InstClass::Jump:
            return 1;
        case InstClass::Float:
            return 4;
        case InstClass::Vector:
            return 6;
        case InstClass::Atomic:
            return 4;
        case InstClass::Custom:
            return 1;
        default:
            return 1;
    }
}

static InstClass classifyFromTraceRecord(const WhisperUtil::TraceRecord& rec) {
    switch (rec.instType) {
        case 'l':
            return InstClass::Load;
        case 's':
            return InstClass::Store;
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

Instruction::Instruction(const WhisperUtil::TraceRecord& rec, instid_t id, coreid_t coreId, std::shared_ptr<TT_PERF::InstrPac> pacPtr)
    : TrackedAllocationPerCore<Instruction>(coreId),
      mRecord(rec),
      mId(id),
      mPacPtr(std::move(pacPtr)),
      mInstClass(classifyFromTraceRecord(rec)),
      mLatency(latencyForClass(mInstClass)) {}

void Instruction::refreshInstClass() {
    mInstClass = classifyFromTraceRecord(mRecord);
    mLatency = latencyForClass(mInstClass);
}

}  // namespace cpu
