// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Tenstorrent USA, Inc.

#pragma once

#include "prefetch/common/PrefetcherInterface.hpp"

namespace prefetch {

// Fed by the I-cache / fetch-block stream; trains on fetch PCs and I-misses.
class InstructionPrefetcherInterface : public PrefetcherInterface {};

}  // namespace prefetch
