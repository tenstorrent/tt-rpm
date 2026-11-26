// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Tenstorrent USA, Inc.

#pragma once

#include <cstdint>

// Common accelerator vocabulary. The device body only ever sees these two structs; it never
// knows whether a command arrived as a custom instruction (tight coupling) or a ring
// descriptor (loose coupling).
namespace accel {

// What to compute — produced identically by a custom instruction OR a ring descriptor.
struct AccelCommand {
    uint32_t opcode{0};   // RoCC funct, or ring-packet opcode
    uint64_t arg0{0};     // rs1, or src_ptr descriptor field
    uint64_t arg1{0};     // rs2, or dst_ptr descriptor field
    uint64_t arg2{0};     // aux operand, or len descriptor field (real RoCC carries a
                          // descriptor pointer in rs1; the model keeps operands flat)
    uint64_t context{0};  // satp/ASID + mstatus snapshot (tight); 0 for PA-mode (loose).
                          // Informational only: translation uses live hart state, not this.
    uint64_t tag{0};      // correlates completion: dest preg (result op) / ROB token (xd=0) / CQ slot
};

struct AccelCompletion {
    uint64_t tag{0};
    uint64_t result{0};
    bool writes_result{false};
};

// Opcode for the reference DmaEngine: copy arg2 bytes from arg0 (src) to arg1 (dst).
inline constexpr uint32_t kOpcodeMemcpy = 1;

}  // namespace accel
