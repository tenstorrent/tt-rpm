// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Tenstorrent USA, Inc.

#include "MemoryHierarchy/Cache.hpp"

namespace cpu {

REGISTER_REPLACEMENT("lru", LRUReplacement)
REGISTER_REPLACEMENT("truelru", LRUReplacement)
REGISTER_REPLACEMENT("plru", TreePLRUReplacement)
REGISTER_REPLACEMENT("treeplru", TreePLRUReplacement)

}  // namespace cpu
