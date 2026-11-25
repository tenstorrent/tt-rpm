// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Tenstorrent USA, Inc.

#pragma once

#include <cstdint>

namespace prefetch {

using addr_t = uint64_t;

enum class AccessResult : uint8_t { Hit, Miss, MshrHit };
enum class CacheLevel : uint8_t { L1I, L1D, L2 };

// Speculative demand-stream event (may be on the wrong path).
struct AccessEvent {
    addr_t addr{0};
    addr_t pc{0};
    AccessResult result{AccessResult::Miss};
    CacheLevel level{CacheLevel::L1D};
    bool is_store{false};    // D-side: store vs load
    bool prefetched{false};  // demand hit a previously-prefetched line
    uint64_t cycle{0};
};

struct PrefetchRequest {
    addr_t addr{0};
};

// Sizing handed to an algorithm by its host cache.
struct PrefetcherConfig {
    uint32_t line_size{64};
    uint32_t page_size{4096};  // prefetches never cross a page: the next physical page is unknown
    uint32_t degree{1};        // lines requested per trigger
    uint32_t distance{1};      // how far ahead of the trigger the first request lands
    uint32_t table_entries{64};
    bool feedback{false};  // host reports PrefetchFeedback; the algorithm may adapt to it
};

// Counts over one feedback interval, for feedback-directed throttling.
struct PrefetchFeedback {
    uint64_t issued{0};
    uint64_t useful{0};  // first demand hit on a prefetched line
    uint64_t late{0};    // demand merged into the prefetch's MSHR
    uint64_t pollution_misses{0};
    uint64_t demand_misses{0};
};

}  // namespace prefetch
