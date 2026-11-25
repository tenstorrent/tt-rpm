// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Tenstorrent USA, Inc.

#pragma once

#include "common/AlgorithmRegistry.hpp"

namespace prefetch {

template <typename Iface, typename Params>
using Registry = rpm::Registry<Iface, Params>;

}  // namespace prefetch

#define REGISTER_PREFETCHER(Iface, Params, KEY, Type) RPM_REGISTER_ALGORITHM(Iface, Params, KEY, Type)
