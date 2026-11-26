// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Tenstorrent USA, Inc.

#pragma once

#include <initializer_list>
#include <string>

#include "sparta/simulation/Parameter.hpp"
#include "sparta/utils/SpartaAssert.hpp"

namespace core {

// Returns a string parameter's value, asserting it is one of `allowed`: a typo in a config must
// fail rather than quietly select a different machine.
inline std::string checkChoice(const sparta::Parameter<std::string>& param, std::initializer_list<const char*> allowed) {
    const std::string& value = param.getValue();
    std::string list;
    for (const char* a : allowed) {
        if (value == a) return value;
        list += (list.empty() ? "" : ", ") + std::string(a);
    }
    sparta_assert(false, param.getLocation() << " = '" << value << "'; expected one of: " << list);
    return value;
}

}  // namespace core
