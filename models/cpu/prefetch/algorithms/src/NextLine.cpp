// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Tenstorrent USA, Inc.

#include <vector>

#include "prefetch/common/PrefetcherRegistry.hpp"
#include "prefetch/prefetcher/src/DataPrefetcherInterface.hpp"
#include "prefetch/prefetcher/src/InstructionPrefetcherInterface.hpp"

namespace prefetch {

namespace {

// Tagged next-N-line prefetch (Smith, "Cache Memories", ACM Computing Surveys 1982): a miss or
// the first demand use of a prefetched line requests the next `degree` lines in the same page.
template <typename Base>
class NextLine : public Base {
   public:
    explicit NextLine(const PrefetcherConfig& cfg)
        : mLineSize(cfg.line_size),
          mPageSize(cfg.page_size),
          mDegree(cfg.degree) {}

    void notifyAccess(const AccessEvent& e) override {
        if (e.result == AccessResult::Hit && !e.prefetched) return;
        if (e.result == AccessResult::MshrHit && !e.prefetched) return;
        const addr_t line = e.addr & ~addr_t(mLineSize - 1);
        if (line == mLastTrigger) return;
        mLastTrigger = line;
        const addr_t page = line & ~addr_t(mPageSize - 1);
        for (uint32_t k = 1; k <= mDegree; ++k) {
            const addr_t t = line + addr_t(k) * mLineSize;
            if ((t & ~addr_t(mPageSize - 1)) != page) break;
            mPending.push_back({t});
        }
    }
    void notifyFill(addr_t, bool, uint64_t) override {}
    void notifyEviction(addr_t, uint64_t) override {}

    std::vector<PrefetchRequest> generate() override {
        std::vector<PrefetchRequest> out;
        out.swap(mPending);
        return out;
    }

    void reset() override {
        mPending.clear();
        mLastTrigger = ~addr_t(0);
    }
    std::string name() const override { return "next_line"; }

   private:
    uint32_t mLineSize;
    uint32_t mPageSize;
    uint32_t mDegree;
    addr_t mLastTrigger{~addr_t(0)};
    std::vector<PrefetchRequest> mPending;
};

using DataNextLine = NextLine<DataPrefetcherInterface>;
using InstNextLine = NextLine<InstructionPrefetcherInterface>;

}  // namespace

REGISTER_PREFETCHER(DataPrefetcherInterface, PrefetcherConfig, "next_line", DataNextLine)
REGISTER_PREFETCHER(InstructionPrefetcherInterface, PrefetcherConfig, "next_line", InstNextLine)

}  // namespace prefetch
