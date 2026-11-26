// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Tenstorrent USA, Inc.

#pragma once

#include <cstdint>

namespace predictors {

using addr_t = uint64_t;

enum class BranchType : uint8_t {
    NotABranch,    // PC has no branch (BTB miss)
    Conditional,   // direction-predicted
    DirectJump,    // unconditional, target in BTB
    Call,          // pushes RAS
    Return,        // consumes RAS
    IndirectJump,  // target from indirect predictor
    IndirectCall,  // indirect + pushes RAS
    CoroutineSwap  // indirect; pops then pushes RAS
};

// Decode cannot compute these targets.
constexpr bool indirectTarget(BranchType t) {
    return t == BranchType::Return || t == BranchType::IndirectJump || t == BranchType::IndirectCall || t == BranchType::CoroutineSwap;
}

struct BranchInfo {
    addr_t pc{0};
    BranchType type{BranchType::NotABranch};
    uint8_t inst_size{4};
};

}  // namespace predictors
