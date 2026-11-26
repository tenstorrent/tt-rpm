// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Tenstorrent USA, Inc.

#pragma once

#include <cstdint>

#include "sparta/utils/SpartaAssert.hpp"

namespace core {

// Speculation behavior of one core. CoreImpl owns one per core, so cores in a cluster can differ.
struct SpeculationConfig {
    bool enabled{false};   // Master switch for wrong-path speculation
    uint8_t max_depth{8};  // Max nested speculation levels
};

// A unit's pointer to its core's SpeculationConfig. Reading it before CoreImpl sets it asserts,
// so a unit left unwired cannot quietly run with speculation off.
class SpeculationConfigRef {
   public:
    void set(const SpeculationConfig* cfg) { mCfg = cfg; }
    const SpeculationConfig* operator->() const {
        sparta_assert(mCfg != nullptr, "SpeculationConfig read before CoreImpl::wireInternal set it");
        return mCfg;
    }

   private:
    const SpeculationConfig* mCfg{nullptr};
};

}  // namespace core
