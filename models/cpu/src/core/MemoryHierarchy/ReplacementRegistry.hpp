// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Tenstorrent USA, Inc.

#pragma once

#include "common/AlgorithmRegistry.hpp"

namespace cpu {

class ReplacementIF;

// Replacement policies are constructed from the set associativity (num ways).
using ReplacementParams = uint32_t;
using ReplacementRegistry = rpm::Registry<ReplacementIF, ReplacementParams>;

}  // namespace cpu

#define REGISTER_REPLACEMENT(KEY, Type) RPM_REGISTER_ALGORITHM(cpu::ReplacementIF, cpu::ReplacementParams, KEY, Type)
