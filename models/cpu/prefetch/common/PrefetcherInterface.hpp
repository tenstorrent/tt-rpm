// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Tenstorrent USA, Inc.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "prefetch/common/PrefetchTypes.hpp"

namespace prefetch {

class PrefetcherInterface {
   public:
    virtual ~PrefetcherInterface() = default;

    // Observation / training (trains on the cache-access stream).
    virtual void notifyAccess(const AccessEvent& e) = 0;  // demand access (hit/miss)
    virtual void notifyFill(addr_t line_addr, bool prefetched, uint64_t cycle) = 0;
    virtual void notifyEviction(addr_t line_addr, uint64_t cycle) = 0;

    // Generation
    virtual std::vector<PrefetchRequest> generate() = 0;

    // End of a host feedback interval (only when PrefetcherConfig::feedback is set).
    virtual void notifyFeedback(const PrefetchFeedback&) {}

    virtual void reset() = 0;
    virtual std::string name() const = 0;
};

}  // namespace prefetch
