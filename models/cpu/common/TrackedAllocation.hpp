// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Tenstorrent USA, Inc.

#pragma once

#include "BaseTypes.hpp"

namespace cpu {

// No-op base for Instruction: no allocation tracking in rpm standalone build.
template <typename T>
struct TrackedAllocationPerCore {
    explicit TrackedAllocationPerCore(coreid_t) {}
};

}  // namespace cpu
