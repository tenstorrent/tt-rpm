// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Tenstorrent USA, Inc.

#pragma once

#include <cstdint>
#include <ostream>

namespace cpu {

using address_t = uint64_t;
using coreid_t = uint64_t;

// Instruction sequence tag (model instruction id).
struct InstIdTag {
    uint64_t val_ = 0;
    InstIdTag() = default;
    /* implicit */ InstIdTag(uint64_t v)
        : val_(v) {}
    uint64_t getInstNum() const { return val_; }
    bool operator==(uint64_t v) const { return val_ == v; }
    bool operator==(const InstIdTag& o) const { return val_ == o.val_; }
};
using instid_t = InstIdTag;

inline std::ostream& operator<<(std::ostream& os, const InstIdTag& id) { return os << id.getInstNum(); }

// Instruction flags bitmask.
using InstFlagType = uint32_t;
namespace InstFlags {
constexpr InstFlagType flgIsLast = 1u << 1;
}  // namespace InstFlags

}  // namespace cpu
