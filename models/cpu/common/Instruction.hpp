// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Tenstorrent USA, Inc.

#pragma once

#include <memory>
#include <string>

#include "sparta/utils/SpartaAssert.hpp"
#include "sparta/utils/SpartaSharedPointer.hpp"

#include "BaseTypes.hpp"
#include "PerfApi.hpp"
#include "TraceReader.hpp"
#include "TrackedAllocation.hpp"

namespace cpu {

enum class InstClass { ALU, Multiply, Divide, Load, Store, Branch, Jump, Float, Vector, Atomic, Custom };
enum class FpDivPrecision : uint8_t { None, Single, Double };  // FP or vector divide/sqrt, by element width

class Instruction : public TrackedAllocationPerCore<Instruction> {
   public:
    Instruction(const WhisperUtil::TraceRecord& rec, instid_t id, coreid_t coreId, std::shared_ptr<TT_PERF::InstrPac> pacPtr);

    const WhisperUtil::TraceRecord& getTraceRecord() const { return mRecord; }
    WhisperUtil::TraceRecord& getMutableTraceRecord() { return mRecord; }
    const std::shared_ptr<TT_PERF::InstrPac>& getPerfPtr() const { return mPacPtr; }
    uint64_t getFetchVA() const { return mRecord.virtPc; }
    instid_t getId() const { return mId; }

    void setPerfApiSerializing() { mIsPerfApiSerializing = true; }
    bool isPerfApiSerializing() const { return mIsPerfApiSerializing; }
    void setLast() { mFlags |= InstFlags::flgIsLast; }

    friend std::ostream& operator<<(std::ostream& os, const Instruction& inst) { return os << inst.mRecord.assembly; }

    InstClass getInstClass() const { return mInstClass; }
    void refreshInstClass();  // Call after TraceRecord is updated
    FpDivPrecision getFpDivPrecision() const { return mFpDivPrecision; }

   private:
    WhisperUtil::TraceRecord mRecord;
    instid_t mId;
    std::shared_ptr<TT_PERF::InstrPac> mPacPtr;
    InstFlagType mFlags = 0;
    bool mIsPerfApiSerializing = false;
    InstClass mInstClass = InstClass::ALU;
    FpDivPrecision mFpDivPrecision = FpDivPrecision::None;
};

using InstPtr = sparta::SpartaSharedPointer<Instruction>;

}  // namespace cpu
