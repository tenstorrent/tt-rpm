// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Tenstorrent USA, Inc.

#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <vector>

#include "sparta/simulation/ParameterSet.hpp"
#include "sparta/simulation/Unit.hpp"
#include "sparta/statistics/ReadOnlyCounter.hpp"
#include "sparta/statistics/StatisticDef.hpp"

#include "MemoryHierarchy/Dram/DramController.hpp"
#include "MemoryHierarchy/Dram/MemoryBackend.hpp"

namespace memory {

// Defaults: DDR4-3200AA (22-22-22), x8 devices, 8 Gb (JESD79-4); two 64-bit channels of one rank.
// Queue depths and drain watermarks are gem5 defaults (MemInterface 32/64 entries; MemCtrl 85%/50% of 64).
class DramParams : public sparta::ParameterSet {
   public:
    DramParams(sparta::TreeNode* n)
        : sparta::ParameterSet(n) {}

    PARAMETER(bool, enabled, true, "Model DRAM below the L2 (false: flat l2cache.miss_latency)")
    PARAMETER(uint32_t, data_rate_mts, 3200, "DRAM data rate in MT/s; the DRAM clock is half of it")
    PARAMETER(double, static_latency_ns, 40.0, "Fixed controller, interconnect and LLC-miss latency added to every read")
    PARAMETER(uint32_t, channels, 2, "Independent channels, each with its own data bus and queues")
    PARAMETER(uint32_t, ranks, 1, "Ranks per channel")
    PARAMETER(uint32_t, bank_groups, 4, "Bank groups per rank")
    PARAMETER(uint32_t, banks_per_group, 4, "Banks per bank group")
    PARAMETER(uint32_t, row_bytes, 8192, "Row (page) size of a rank in bytes")
    PARAMETER(uint32_t, read_queue_capacity, 32, "Read queue entries per channel")
    PARAMETER(uint32_t, write_queue_capacity, 64, "Write queue entries per channel")
    PARAMETER(uint32_t, write_high_watermark, 54, "Write-queue occupancy that starts a write drain")
    PARAMETER(uint32_t, write_low_watermark, 32, "Write-queue occupancy that ends a write drain")
    PARAMETER(uint32_t, tCL, 22, "READ to first data (tCK)")
    PARAMETER(uint32_t, tCWL, 16, "WRITE to first data (tCK)")
    PARAMETER(uint32_t, tRCD, 22, "ACT to READ/WRITE (tCK)")
    PARAMETER(uint32_t, tRP, 22, "PRE to ACT (tCK)")
    PARAMETER(uint32_t, tRAS, 52, "ACT to PRE (tCK)")
    PARAMETER(uint32_t, tRTP, 12, "READ to PRE (tCK)")
    PARAMETER(uint32_t, tWR, 24, "End of write data to PRE (tCK)")
    PARAMETER(uint32_t, tCCD_S, 4, "Column to column, different bank group (tCK)")
    PARAMETER(uint32_t, tCCD_L, 8, "Column to column, same bank group (tCK)")
    PARAMETER(uint32_t, tRRD_S, 4, "ACT to ACT, different bank group (tCK)")
    PARAMETER(uint32_t, tRRD_L, 8, "ACT to ACT, same bank group (tCK)")
    PARAMETER(uint32_t, tFAW, 34, "Window holding at most four ACTs per rank (tCK)")
    PARAMETER(uint32_t, tWTR_S, 4, "End of write data to READ, different bank group (tCK)")
    PARAMETER(uint32_t, tWTR_L, 12, "End of write data to READ, same bank group (tCK)")
    PARAMETER(uint32_t, tBURST, 4, "Data-bus cycles per 64 B line (BL8 on a 64-bit bus)")
    // Not a JEDEC timing; 2 is Ramulator's DDR4-3200 nRTRS (src/DDR4.h), which Ramulator2 calls an estimate.
    PARAMETER(uint32_t, tRTRS, 2, "Rank-to-rank data-bus switch (tCK)")
    PARAMETER(uint32_t, tREFI, 12480, "Refresh interval (tCK)")
    PARAMETER(uint32_t, tRFC, 560, "Refresh cycle time (tCK); 0 disables refresh")
};

// Native DRAM timing model behind the MemoryBackend interface; converts between the core
// clock (this unit's clock) and the DRAM clock.
class Dram : public sparta::Unit, public MemoryBackend {
   public:
    static constexpr char name[] = "dram";

    Dram(sparta::TreeNode* node, const DramParams* params);

    bool isEnabled() const { return mEnabled; }

    bool canAccept(uint64_t line_addr, bool is_write) const override { return mCtrl.canAccept(line_addr, is_write); }
    void access(uint64_t line_addr, bool is_write) override;
    void setReadDoneCallback(ReadDoneCb cb) override { mReadDone = std::move(cb); }
    void tick() override;

    const dram::Stats& stats() const { return mCtrl.stats(); }
    double avgReadLatency() const;  // core cycles, request to data back at the L2
    double bandwidthGBps() const;

   private:
    bool mEnabled;
    dram::Controller mCtrl;
    uint64_t mCoreMhz;
    uint64_t mDramMhz;
    uint64_t mStaticCycles;
    uint64_t mCycle{0};                               // core cycles
    uint64_t mDramCycle{0};                           // DRAM cycles
    std::multimap<uint64_t, uint64_t> mPendingReads;  // ready core cycle -> line
    ReadDoneCb mReadDone;

    uint64_t mReadLatencyTotal{0};
    uint64_t mReadsReturned{0};
    uint64_t mRqOccupancyTotal{0};
    uint64_t mWqOccupancyTotal{0};

    std::vector<std::unique_ptr<sparta::ReadOnlyCounter>> mCounters;
    std::vector<std::unique_ptr<sparta::StatisticDef>> mStatDefs;
};

}  // namespace memory
