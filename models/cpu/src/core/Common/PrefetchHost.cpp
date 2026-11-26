// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Tenstorrent USA, Inc.

#include "Common/PrefetchHost.hpp"

#include <algorithm>
#include <ostream>

namespace core {

PrefetchHost::PrefetchHost(sparta::StatisticSet* stats, std::unique_ptr<prefetch::PrefetcherInterface> pf, const Config& cfg)
    : mPf(std::move(pf)),
      mCfg(cfg),
      mIssued(stats, "pf_issued", "Prefetches sent toward their fill level", sparta::Counter::COUNT_NORMAL),
      mDropped(stats, "pf_dropped", "Prefetches dropped because the prefetch queue was full", sparta::Counter::COUNT_NORMAL),
      mFiltered(stats, "pf_filtered", "Queued prefetches discarded: line present or already in flight", sparta::Counter::COUNT_NORMAL),
      mStalled(stats, "pf_mshr_stall_cycles", "Cycles the prefetch queue head waited for a spare MSHR", sparta::Counter::COUNT_NORMAL),
      mUseful(stats, "pf_useful", "Prefetched lines whose first demand access hit", [this] { return mCache ? mCache->numUsefulPrefetches() : 0; }),
      mLate(stats, "pf_late", "Demand misses merged into an in-flight prefetch", [this] { return mCache ? mCache->numLatePrefetches() : 0; }),
      mUseless(stats, "pf_useless", "Prefetched lines evicted before any demand use", [this] { return mCache ? mCache->numUselessPrefetches() : 0; }),
      mPollution(stats, "pf_pollution_misses", "Demand misses to lines a prefetch evicted (approximate filter)",
                 [this] { return mCache ? mCache->numPollutionMisses() : 0; }),
      mNonDemand(stats, "pf_nondemand_hits", "Prefetched lines first used by a page walk or an upper-level prefetch, not a demand",
                 [this] { return mCache ? mCache->numNonDemandPrefetchHits() : 0; }),
      mDemandMisses(stats, "pf_demand_line_misses", "Demand misses that allocated an MSHR", [this] { return mCache ? mCache->numMisses() : 0; }),
      mAccuracy(stats, "pf_accuracy", "Fraction of issued prefetches a demand used (on time or late)", stats, "(pf_useful + pf_late) / pf_issued",
                sparta::StatisticDef::VS_FRACTIONAL, sparta::InstrumentationNode::DEFAULT_VISIBILITY),
      mCoverage(stats, "pf_coverage", "Fraction of would-be demand misses a prefetch covered (on time or late)", stats,
                "(pf_useful + pf_late) / (pf_useful + pf_late + pf_demand_line_misses)", sparta::StatisticDef::VS_FRACTIONAL,
                sparta::InstrumentationNode::DEFAULT_VISIBILITY) {}

void PrefetchHost::train(const cpu::CacheInterface::Probe& p, cpu::address_t addr, cpu::address_t pc, bool is_store, prefetch::CacheLevel level,
                         uint64_t cycle) {
    if (!mPf) return;
    prefetch::AccessEvent ev;
    ev.addr = addr;
    ev.pc = pc;
    ev.result = p.hit ? prefetch::AccessResult::Hit : (p.in_flight ? prefetch::AccessResult::MshrHit : prefetch::AccessResult::Miss);
    ev.level = level;
    ev.is_store = is_store;
    ev.prefetched = p.prefetched;
    ev.cycle = cycle;
    mPf->notifyAccess(ev);
}

prefetch::PrefetchFeedback PrefetchHost::snapshot() const {
    prefetch::PrefetchFeedback f;
    f.issued = mIssued.get();
    f.useful = mUseful.get();
    f.late = mLate.get();
    f.pollution_misses = mPollution.get();
    f.demand_misses = mDemandMisses.get();
    return f;
}

void PrefetchHost::onEviction(cpu::address_t line_addr, uint64_t cycle) {
    if (!mPf) return;
    mPf->notifyEviction(line_addr, cycle);
    if (mCfg.feedback_interval == 0 || ++mEvictions < mCfg.feedback_interval) return;
    mEvictions = 0;
    const prefetch::PrefetchFeedback now = snapshot();
    prefetch::PrefetchFeedback d;
    d.issued = now.issued - mLastFeedback.issued;
    d.useful = now.useful - mLastFeedback.useful;
    d.late = now.late - mLastFeedback.late;
    d.pollution_misses = now.pollution_misses - mLastFeedback.pollution_misses;
    d.demand_misses = now.demand_misses - mLastFeedback.demand_misses;
    mLastFeedback = now;
    mPf->notifyFeedback(d);
}

void PrefetchHost::enqueue(cpu::address_t addr) {
    const cpu::address_t line = addr & ~cpu::address_t(mCfg.line_size - 1);
    if (std::find(mQueue.begin(), mQueue.end(), line) != mQueue.end()) return;
    if (mQueue.size() >= mCfg.queue_capacity) {
        ++mDropped;
        return;
    }
    mQueue.push_back(line);
}

void PrefetchHost::report(std::ostream& os, const char* label) const {
    const uint64_t issued = mIssued.get(), useful = mUseful.get(), late = mLate.get();
    if (issued + mDropped.get() + mFiltered.get() == 0) return;
    const uint64_t used = useful + late;
    const uint64_t base = used + mDemandMisses.get();
    os << "  " << label << "issued=" << issued << "  useful=" << useful << "  late=" << late << "  useless=" << mUseless.get() << "  dropped=" << mDropped.get()
       << "  (accuracy=" << (issued ? 100.0 * used / issued : 0.0) << "%  coverage=" << (base ? 100.0 * used / base : 0.0) << "%)\n";
}

void PrefetchHost::tick(const IssueFn& issue, bool port_free) {
    if (mPf) {
        for (const auto& req : mPf->generate()) enqueue(req.addr);
    }
    while (port_free && !mQueue.empty()) {
        switch (issue(mQueue.front())) {
            case cpu::CacheInterface::PrefetchOutcome::Filtered:
                ++mFiltered;
                mQueue.pop_front();
                continue;
            case cpu::CacheInterface::PrefetchOutcome::NoMshr:
                ++mStalled;
                return;
            case cpu::CacheInterface::PrefetchOutcome::Issued:
                ++mIssued;
                mQueue.pop_front();
                return;
        }
    }
}

}  // namespace core
