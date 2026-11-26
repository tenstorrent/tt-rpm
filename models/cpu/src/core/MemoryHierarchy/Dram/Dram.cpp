// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Tenstorrent USA, Inc.

#include "MemoryHierarchy/Dram/Dram.hpp"

#include <cmath>
#include <sstream>

#include "sparta/utils/SpartaAssert.hpp"

namespace memory {

namespace {

dram::Organization organization(const DramParams* p) {
    dram::Organization o;
    o.channels = p->channels;
    o.ranks = p->ranks;
    o.bank_groups = p->bank_groups;
    o.banks_per_group = p->banks_per_group;
    o.row_bytes = p->row_bytes;
    return o;
}

dram::Timing timing(const DramParams* p) {
    dram::Timing t;
    t.tCL = p->tCL;
    t.tCWL = p->tCWL;
    t.tRCD = p->tRCD;
    t.tRP = p->tRP;
    t.tRAS = p->tRAS;
    t.tRTP = p->tRTP;
    t.tWR = p->tWR;
    t.tCCD_S = p->tCCD_S;
    t.tCCD_L = p->tCCD_L;
    t.tRRD_S = p->tRRD_S;
    t.tRRD_L = p->tRRD_L;
    t.tFAW = p->tFAW;
    t.tWTR_S = p->tWTR_S;
    t.tWTR_L = p->tWTR_L;
    t.tBURST = p->tBURST;
    t.tRTRS = p->tRTRS;
    t.tREFI = p->tREFI;
    t.tRFC = p->tRFC;
    return t;
}

dram::SchedConfig schedConfig(const DramParams* p) {
    dram::SchedConfig s;
    s.read_queue_capacity = p->read_queue_capacity;
    s.write_queue_capacity = p->write_queue_capacity;
    s.write_high_watermark = p->write_high_watermark;
    s.write_low_watermark = p->write_low_watermark;
    return s;
}

}  // namespace

Dram::Dram(sparta::TreeNode* node, const DramParams* params)
    : sparta::Unit(node),
      mEnabled(params->enabled),
      mCtrl(organization(params), timing(params), schedConfig(params)),
      mCoreMhz(static_cast<uint64_t>(std::llround(getClock()->getFrequencyMhz()))),
      mDramMhz(params->data_rate_mts / 2) {
    sparta_assert(mDramMhz > 0, "dram.params.data_rate_mts must be non-zero");
    sparta_assert(params->static_latency_ns >= 0.0, "dram.params.static_latency_ns must not be negative");
    mStaticCycles = static_cast<uint64_t>(std::llround(params->static_latency_ns * static_cast<double>(mCoreMhz) / 1000.0));

    mCtrl.setReadDoneCallback([this](uint64_t line_addr, uint64_t arrive_cycle, uint64_t done) {
        const uint64_t ready = (done * mCoreMhz + mDramMhz - 1) / mDramMhz + mStaticCycles;
        mReadLatencyTotal += ready - arrive_cycle;
        ++mReadsReturned;
        mPendingReads.emplace(ready, line_addr);
    });

    const dram::Stats& s = mCtrl.stats();
    auto counter = [this](const char* n, const char* desc, const uint64_t* ref) {
        mCounters.emplace_back(std::make_unique<sparta::ReadOnlyCounter>(&unit_stat_set_, n, desc, sparta::CounterBase::COUNT_NORMAL, ref,
                                                                         sparta::InstrumentationNode::DEFAULT_VISIBILITY));
    };
    counter("num_reads", "Line reads issued to DRAM", &s.reads);
    counter("num_writes", "Line writes issued to DRAM", &s.writes);
    counter("num_write_merges", "Writes merged into a queued write to the same line", &s.write_merges);
    counter("num_wq_forwards", "Reads served from the write queue", &s.wq_forwards);
    counter("num_row_hits", "Requests that found their row open", &s.row_hits);
    counter("num_row_misses", "Requests that found their bank precharged", &s.row_misses);
    counter("num_row_conflicts", "Requests that found another row open", &s.row_conflicts);
    counter("num_activates", "ACT commands", &s.activates);
    counter("num_precharges", "PRE commands", &s.precharges);
    counter("num_refreshes", "REF commands", &s.refreshes);
    counter("num_write_drains", "Switches into write-drain mode", &s.write_drains);
    counter("data_bus_busy_dram_cycles", "DRAM cycles the data buses carried bursts, summed over channels", &s.data_bus_busy);
    counter("num_cycles", "Core cycles simulated", &mCycle);
    counter("read_latency_total", "Core cycles from request to data at the L2, summed over reads", &mReadLatencyTotal);
    counter("num_reads_returned", "Reads returned to the L2, including write-queue forwards", &mReadsReturned);
    counter("read_queue_occupancy_total", "Read-queue entries summed over core cycles (all channels)", &mRqOccupancyTotal);
    counter("write_queue_occupancy_total", "Write-queue entries summed over core cycles (all channels)", &mWqOccupancyTotal);

    auto stat = [this](const char* n, const char* desc, const std::string& expr) {
        mStatDefs.emplace_back(std::make_unique<sparta::StatisticDef>(&unit_stat_set_, n, desc, &unit_stat_set_, expr));
    };
    std::ostringstream bw;
    bw << "(num_reads + num_writes) * 64 * " << mCoreMhz << " / 1000 / num_cycles";
    stat("avg_read_latency", "Average core cycles from request to data at the L2", "read_latency_total / num_reads_returned");
    stat("row_hit_rate", "Row-buffer hits per request", "num_row_hits / (num_row_hits + num_row_misses + num_row_conflicts)");
    stat("bandwidth_gbps", "Achieved read plus write bandwidth in GB/s", bw.str());
    stat("avg_read_queue_occupancy", "Average read-queue entries (all channels)", "read_queue_occupancy_total / num_cycles");
    stat("avg_write_queue_occupancy", "Average write-queue entries (all channels)", "write_queue_occupancy_total / num_cycles");
}

void Dram::access(uint64_t line_addr, bool is_write) { mCtrl.enqueue(line_addr, is_write, mCycle, mDramCycle); }

// The cluster builds this unit on a frequency-less clock but never ticks it.
void Dram::tick() {
    sparta_assert(mCoreMhz > 0, "dram: the core clock has no frequency; cannot convert DRAM timing to core cycles");
    ++mCycle;
    const uint64_t target = mCycle * mDramMhz / mCoreMhz;
    while (mDramCycle < target) mCtrl.tick(++mDramCycle);
    mRqOccupancyTotal += mCtrl.readQueueOccupancy();
    mWqOccupancyTotal += mCtrl.writeQueueOccupancy();

    while (!mPendingReads.empty() && mPendingReads.begin()->first <= mCycle) {
        const uint64_t line = mPendingReads.begin()->second;
        mPendingReads.erase(mPendingReads.begin());
        if (mReadDone) mReadDone(line);
    }
}

double Dram::avgReadLatency() const { return mReadsReturned ? static_cast<double>(mReadLatencyTotal) / mReadsReturned : 0.0; }

double Dram::bandwidthGBps() const {
    const auto& s = mCtrl.stats();
    return mCycle ? static_cast<double>(s.reads + s.writes) * 64.0 * static_cast<double>(mCoreMhz) / 1000.0 / static_cast<double>(mCycle) : 0.0;
}

}  // namespace memory
