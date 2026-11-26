// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Tenstorrent USA, Inc.

#pragma once

#include <deque>
#include <functional>
#include <iosfwd>
#include <memory>
#include <string>
#include <vector>

#include "sparta/statistics/Counter.hpp"
#include "sparta/statistics/ReadOnlyCounter.hpp"
#include "sparta/statistics/StatisticDef.hpp"
#include "sparta/statistics/StatisticSet.hpp"

#include "MemoryHierarchy/CacheInterface.hpp"
#include "prefetch/common/PrefetcherInterface.hpp"

namespace core {

// Runs one prefetcher beside a cache: trains it on the demand stream, holds its requests in a
// bounded queue (new requests are dropped when it is full) and issues them behind demand.
// The algorithm may be null when the host only carries prefetches queued from another level.
class PrefetchHost {
   public:
    struct Config {
        uint32_t line_size{64};
        uint32_t queue_capacity{16};
        uint64_t feedback_interval{0};  // cache evictions per feedback interval; 0 = no feedback
    };
    // Sends one prefetch toward its fill level. NoMshr leaves it at the queue head for a later cycle.
    using IssueFn = std::function<cpu::CacheInterface::PrefetchOutcome(cpu::address_t)>;

    PrefetchHost(sparta::StatisticSet* stats, std::unique_ptr<prefetch::PrefetcherInterface> pf, const Config& cfg);

    // Cache whose prefetch bits and miss counts feed the usefulness stats.
    void bind(const cpu::CacheInterface* cache) { mCache = cache; }

    // Train on a demand access the bound cache accepted; `p` is its lookup taken before the access.
    void train(const cpu::CacheInterface::Probe& p, cpu::address_t addr, cpu::address_t pc, bool is_store, prefetch::CacheLevel level, uint64_t cycle);
    void onFill(cpu::address_t line_addr, bool prefetched, uint64_t cycle) {
        if (mPf) mPf->notifyFill(line_addr, prefetched, cycle);
    }
    void onEviction(cpu::address_t line_addr, uint64_t cycle);

    // Queue a prefetch generated outside this host (an upper level's fill-into-this-level request).
    void enqueue(cpu::address_t addr);

    // Collect new requests and, if the cache has a free port this cycle, issue at most one.
    void tick(const IssueFn& issue, bool port_free = true);

    // End-of-run summary line; prints nothing if no prefetch was ever generated.
    void report(std::ostream& os, const char* label) const;

   private:
    class CacheCounter : public sparta::ReadOnlyCounter {
       public:
        CacheCounter(sparta::StatisticSet* s, const std::string& name, const std::string& desc, std::function<uint64_t()> fn)
            : sparta::ReadOnlyCounter(s, name, desc, sparta::CounterBase::COUNT_NORMAL),
              mFn(std::move(fn)) {}
        counter_type get() const override { return mFn(); }

       private:
        std::function<uint64_t()> mFn;
    };

    prefetch::PrefetchFeedback snapshot() const;

    std::unique_ptr<prefetch::PrefetcherInterface> mPf;
    Config mCfg;
    const cpu::CacheInterface* mCache{nullptr};
    std::deque<cpu::address_t> mQueue;

    uint64_t mEvictions{0};
    prefetch::PrefetchFeedback mLastFeedback;

    sparta::Counter mIssued;
    sparta::Counter mDropped;
    sparta::Counter mFiltered;
    sparta::Counter mStalled;
    CacheCounter mUseful;
    CacheCounter mLate;
    CacheCounter mUseless;
    CacheCounter mPollution;
    CacheCounter mNonDemand;
    CacheCounter mDemandMisses;
    sparta::StatisticDef mAccuracy;
    sparta::StatisticDef mCoverage;
};

}  // namespace core
