// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Tenstorrent USA, Inc.

#pragma once

#include <cstdint>

namespace core {

// Latency class of a scalar FP instruction, from its RISC-V encoding (opcode and funct5).
enum class FpOpClass : uint8_t { Other, Add, Mul, Fma, DivSqrt, Move, Compare, Convert };

inline FpOpClass fpOpClass(uint32_t inst) {
    switch (inst & 0x7f) {
        case 0x43:  // fmadd
        case 0x47:  // fmsub
        case 0x4b:  // fnmsub
        case 0x4f:  // fnmadd
            return FpOpClass::Fma;
        case 0x53:  // OP-FP
            break;
        default:
            return FpOpClass::Other;
    }
    switch (inst >> 27) {
        case 0x00:  // fadd
        case 0x01:  // fsub
            return FpOpClass::Add;
        case 0x02:
            return FpOpClass::Mul;
        case 0x03:  // fdiv
        case 0x0b:  // fsqrt
            return FpOpClass::DivSqrt;
        case 0x04:  // fsgnj, fsgnjn, fsgnjx
        case 0x1c:  // fmv.x.fmt, fclass
        case 0x1e:  // fmv.fmt.x, fli
            return FpOpClass::Move;
        case 0x05:  // fmin, fmax
        case 0x14:  // feq, flt, fle
            return FpOpClass::Compare;
        case 0x08:  // fcvt.fmt.fmt, fround
        case 0x18:  // fcvt.int.fmt
        case 0x1a:  // fcvt.fmt.int
            return FpOpClass::Convert;
        default:
            return FpOpClass::Other;
    }
}

// vfdiv, vfrdiv or vfsqrt.v: OP-V with funct6 0x20 or 0x21 (OPFVV/OPFVF), or VFUNARY1 with vs1 = 0 (RVV 1.0).
inline bool vecFpIsDivSqrt(uint32_t inst) {
    if ((inst & 0x7f) != 0x57) return false;
    const uint32_t funct3 = (inst >> 12) & 0x7;
    const uint32_t funct6 = inst >> 26;
    if (funct3 == 1 && funct6 == 0x13) return ((inst >> 15) & 0x1f) == 0;
    return (funct3 == 1 || funct3 == 5) && (funct6 == 0x20 || funct6 == 0x21);
}

}  // namespace core
