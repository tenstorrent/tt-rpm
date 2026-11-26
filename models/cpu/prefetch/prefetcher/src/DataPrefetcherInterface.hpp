// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Tenstorrent USA, Inc.

#pragma once

#include "prefetch/common/PrefetcherInterface.hpp"

namespace prefetch {

// Fed by the LSU / D-cache; trains on load/store addresses, PCs, and D-misses.
class DataPrefetcherInterface : public PrefetcherInterface {};

}  // namespace prefetch
