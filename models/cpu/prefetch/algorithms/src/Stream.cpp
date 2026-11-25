// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Tenstorrent USA, Inc.

#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

#include "prefetch/common/PrefetcherRegistry.hpp"
#include "prefetch/prefetcher/src/DataPrefetcherInterface.hpp"

namespace prefetch {

namespace {

// Stream prefetcher in the style of stream buffers (Jouppi, ISCA 1990; Palacharla and Kessler,
// ISCA 1994) as cache-fill streams, with the tracking-entry design and feedback-directed
// throttling of Srinath, Mutlu, Kim and Patt, "Feedback Directed Prefetching", HPCA 2007.
class Stream : public DataPrefetcherInterface {
   public:
    explicit Stream(const PrefetcherConfig& cfg)
        : mLineSize(cfg.line_size),
          mLinesPerPage(cfg.page_size / cfg.line_size),
          mFeedback(cfg.feedback),
          mStaticDegree(cfg.degree),
          mStaticDistance(cfg.distance),
          mStreams(cfg.table_entries) {}

    void notifyAccess(const AccessEvent& e) override {
        const int64_t line = static_cast<int64_t>(e.addr / mLineSize);
        ++mClock;

        for (auto& s : mStreams) {
            if (s.mode != Mode::Monitor || page(line) != s.page) continue;
            const int64_t from_start = (line - s.start) * s.dir;
            const int64_t past_end = (line - s.end) * s.dir;
            if (from_start < 0 || past_end > kTrainWindow) continue;
            s.lru = mClock;
            s.lead = std::min(s.lead + degree(), distance());
            advance(s, line);
            return;
        }

        if (e.result != AccessResult::Miss) return;

        for (auto& s : mStreams) {
            if (s.page != page(line) || (s.mode != Mode::Allocated && s.mode != Mode::Training)) continue;
            const int64_t d = line - s.first;
            if (d == 0 || d > kTrainWindow || d < -kTrainWindow) continue;
            s.lru = mClock;
            const int dir = d > 0 ? 1 : -1;
            if (s.mode == Mode::Training && dir == s.dir) {
                s.mode = Mode::Monitor;
                s.start = s.first;
                s.end = line;
                s.lead = degree();
                advance(s, line);
            } else {
                s.mode = Mode::Training;
                s.dir = dir;
            }
            return;
        }

        Entry* victim = &mStreams[0];
        for (auto& s : mStreams) {
            if (s.mode == Mode::Invalid) {
                victim = &s;
                break;
            }
            if (s.lru < victim->lru) victim = &s;
        }
        *victim = Entry{};
        victim->mode = Mode::Allocated;
        victim->page = page(line);
        victim->first = line;
        victim->lru = mClock;
    }
    void notifyFill(addr_t, bool, uint64_t) override {}
    void notifyEviction(addr_t, uint64_t) override {}

    // Srinath et al. Section 3: counters are halved into a running average each interval, then
    // accuracy, lateness and pollution pick a Table 2 case that steps the Table 1 level.
    void notifyFeedback(const PrefetchFeedback& f) override {
        auto avg = [](double& c, uint64_t v) { c = 0.5 * c + 0.5 * static_cast<double>(v); };
        avg(mIssued, f.issued);
        avg(mUsed, f.useful + f.late);
        avg(mLate, f.late);
        avg(mPollution, f.pollution_misses);
        avg(mDemandMisses, f.demand_misses);
        if (mIssued <= 0.0) return;

        const double accuracy = mUsed / mIssued;
        const bool late = mUsed > 0.0 && mLate / mUsed > kLatenessThreshold;
        const bool polluting = mDemandMisses > 0.0 && mPollution / mDemandMisses > kPollutionThreshold;
        int step = 0;
        if (accuracy >= kAccuracyHigh) {
            step = late ? 1 : (polluting ? -1 : 0);
        } else if (accuracy >= kAccuracyLow) {
            step = late ? (polluting ? -1 : 1) : (polluting ? -1 : 0);
        } else {
            step = (late || polluting) ? -1 : 0;
        }
        mLevel = std::clamp(mLevel + step, 0, static_cast<int>(kLevels.size()) - 1);
    }

    std::vector<PrefetchRequest> generate() override {
        std::vector<PrefetchRequest> out;
        out.swap(mPending);
        return out;
    }

    void reset() override {
        for (auto& s : mStreams) s = Entry{};
        mPending.clear();
        mLevel = kInitialLevel;
    }
    std::string name() const override { return "stream"; }

   private:
    enum class Mode : uint8_t { Invalid, Allocated, Training, Monitor };
    struct Entry {
        Mode mode{Mode::Invalid};
        int64_t page{0};
        int64_t first{0};  // line of the allocating miss
        int dir{0};
        int64_t start{0};  // monitored region [start, end] in stream direction; end = last line requested
        int64_t end{0};
        uint32_t lead{0};  // lines kept ahead of the demand; grows by the degree per confirming access
        uint64_t lru{0};
    };

    struct Level {
        uint32_t distance;
        uint32_t degree;
    };
    // Table 1 of Srinath et al.: very conservative .. very aggressive.
    static constexpr std::array<Level, 5> kLevels{{{4, 1}, {8, 1}, {16, 2}, {32, 4}, {64, 4}}};
    static constexpr int kInitialLevel = 2;  // middle-of-the-road
    static constexpr int64_t kTrainWindow = 16;
    static constexpr double kAccuracyHigh = 0.75;
    static constexpr double kAccuracyLow = 0.40;
    static constexpr double kLatenessThreshold = 0.01;
    static constexpr double kPollutionThreshold = 0.005;

    uint32_t degree() const { return mFeedback ? kLevels[mLevel].degree : mStaticDegree; }
    uint32_t distance() const { return mFeedback ? kLevels[mLevel].distance : mStaticDistance; }
    int64_t page(int64_t line) const { return line / static_cast<int64_t>(mLinesPerPage); }

    // Request lines up to `lead` past the demand, staying in the page. Lines the demand already
    // passed are skipped. The start pointer trails the end by at most the prefetch distance.
    void advance(Entry& s, int64_t demand) {
        if ((demand - s.end) * s.dir > 0) s.end = demand;
        const int64_t target = demand + s.dir * static_cast<int64_t>(s.lead);
        while ((target - s.end) * s.dir > 0 && page(s.end + s.dir) == s.page) {
            s.end += s.dir;
            mPending.push_back({static_cast<addr_t>(s.end) * mLineSize});
        }
        const int64_t span = (s.end - s.start) * s.dir;
        if (span > static_cast<int64_t>(distance())) s.start = s.end - s.dir * static_cast<int64_t>(distance());
    }

    uint32_t mLineSize;
    uint32_t mLinesPerPage;
    bool mFeedback;
    uint32_t mStaticDegree;
    uint32_t mStaticDistance;
    int mLevel{kInitialLevel};
    double mIssued{0}, mUsed{0}, mLate{0}, mPollution{0}, mDemandMisses{0};
    uint64_t mClock{0};
    std::vector<Entry> mStreams;
    std::vector<PrefetchRequest> mPending;
};

}  // namespace

REGISTER_PREFETCHER(DataPrefetcherInterface, PrefetcherConfig, "stream", Stream)

}  // namespace prefetch
