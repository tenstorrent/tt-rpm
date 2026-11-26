// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Tenstorrent USA, Inc.

#include <algorithm>
#include <cstdint>
#include <vector>

#include "prefetch/common/PrefetcherRegistry.hpp"
#include "prefetch/prefetcher/src/DataPrefetcherInterface.hpp"

namespace prefetch {

namespace {

// Reference prediction table: Chen and Baer, "Effective hardware-based data prefetching for
// high-performance processors", IEEE Trans. Computers 44(5), 1995. Per-PC last address and
// stride with the paper's four-state machine; only the steady state prefetches.
class IpStride : public DataPrefetcherInterface {
   public:
    explicit IpStride(const PrefetcherConfig& cfg)
        : mLineSize(cfg.line_size),
          mPageSize(cfg.page_size),
          mDegree(cfg.degree),
          mDistance(cfg.distance),
          mTable(cfg.table_entries) {}

    void notifyAccess(const AccessEvent& e) override {
        Entry& en = mTable[(e.pc >> 1) % mTable.size()];
        if (!en.valid || en.tag != e.pc) {
            en = Entry{};
            en.valid = true;
            en.tag = e.pc;
            en.prev = e.addr;
            return;
        }
        const int64_t delta = static_cast<int64_t>(e.addr - en.prev);
        const bool correct = delta == en.stride;
        switch (en.state) {
            case State::Initial:
                en.state = correct ? State::Steady : State::Transient;
                if (!correct) en.stride = delta;
                break;
            case State::Transient:
                en.state = correct ? State::Steady : State::NoPred;
                if (!correct) en.stride = delta;
                break;
            case State::Steady:
                if (!correct) en.state = State::Initial;  // stride kept: one irregular access is tolerated
                break;
            case State::NoPred:
                if (correct)
                    en.state = State::Transient;
                else
                    en.stride = delta;
                break;
        }
        en.prev = e.addr;
        if (!correct) en.has_last = false;  // a stride break (e.g. a loop restarting the array) re-arms the run
        if (en.state == State::Steady && en.stride != 0) issue(en, e.addr);
    }
    void notifyFill(addr_t, bool, uint64_t) override {}
    void notifyEviction(addr_t, uint64_t) override {}

    std::vector<PrefetchRequest> generate() override {
        std::vector<PrefetchRequest> out;
        out.swap(mPending);
        return out;
    }

    void reset() override {
        for (auto& en : mTable) en = Entry{};
        mPending.clear();
    }
    std::string name() const override { return "ip_stride"; }

   private:
    enum class State : uint8_t { Initial, Transient, Steady, NoPred };
    struct Entry {
        bool valid{false};
        addr_t tag{0};
        addr_t prev{0};
        int64_t stride{0};
        State state{State::Initial};
        addr_t last_line{0};  // furthest line already requested, so steady-state triggers add only new lines
        bool has_last{false};
    };

    void issue(Entry& en, addr_t addr) {
        const int64_t line = static_cast<int64_t>(mLineSize);
        // A sub-line stride walks lines in order, so step by whole lines.
        const int64_t step = (en.stride >= line || en.stride <= -line) ? en.stride : (en.stride > 0 ? line : -line);
        // `distance` steps ahead, pulled back so a large stride still prefetches inside the page.
        const addr_t page = addr & ~addr_t(mPageSize - 1);
        const int64_t room = step > 0 ? static_cast<int64_t>(page + mPageSize - 1 - addr) / step : static_cast<int64_t>(addr - page) / -step;
        const int64_t first = std::min<int64_t>(mDistance, room);
        if (first < 1) return;
        for (int64_t k = first; k < first + mDegree; ++k) {
            const addr_t t = addr + static_cast<addr_t>(step * k);
            if ((t & ~addr_t(mPageSize - 1)) != page) break;
            const addr_t tl = t & ~addr_t(mLineSize - 1);
            if (en.has_last && (step > 0 ? tl <= en.last_line : tl >= en.last_line)) continue;
            en.last_line = tl;
            en.has_last = true;
            mPending.push_back({tl});
        }
    }

    uint32_t mLineSize;
    uint32_t mPageSize;
    uint32_t mDegree;
    uint32_t mDistance;
    std::vector<Entry> mTable;
    std::vector<PrefetchRequest> mPending;
};

}  // namespace

REGISTER_PREFETCHER(DataPrefetcherInterface, PrefetcherConfig, "ip_stride", IpStride)

}  // namespace prefetch
