// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Tenstorrent USA, Inc.

#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <vector>

namespace memory::dram {

// JEDEC command-timing constraints, in DRAM clock cycles (tCK).
struct Timing {
    uint32_t tCL{22};
    uint32_t tCWL{16};
    uint32_t tRCD{22};
    uint32_t tRP{22};
    uint32_t tRAS{52};
    uint32_t tRTP{12};
    uint32_t tWR{24};
    uint32_t tCCD_S{4};
    uint32_t tCCD_L{8};
    uint32_t tRRD_S{4};
    uint32_t tRRD_L{8};
    uint32_t tFAW{34};
    uint32_t tWTR_S{4};
    uint32_t tWTR_L{12};
    uint32_t tBURST{4};
    uint32_t tRTRS{2};
    uint32_t tREFI{12480};
    uint32_t tRFC{560};  // 0 disables refresh
};

// Every count must be a power of two.
struct Organization {
    uint32_t channels{2};
    uint32_t ranks{1};
    uint32_t bank_groups{4};
    uint32_t banks_per_group{4};
    uint32_t row_bytes{8192};  // per rank: one row across all devices
    uint32_t line_bytes{64};
};

struct SchedConfig {
    uint32_t read_queue_capacity{32};   // per channel
    uint32_t write_queue_capacity{64};  // per channel
    uint32_t write_high_watermark{54};  // enter write drain at this occupancy
    uint32_t write_low_watermark{32};   // leave write drain at this occupancy
};

struct Stats {
    uint64_t reads{0};
    uint64_t writes{0};
    uint64_t write_merges{0};  // write to a line already in the write queue
    uint64_t wq_forwards{0};   // read served from the write queue
    uint64_t row_hits{0};
    uint64_t row_misses{0};     // bank was precharged
    uint64_t row_conflicts{0};  // bank had another row open
    uint64_t activates{0};
    uint64_t precharges{0};
    uint64_t refreshes{0};
    uint64_t write_drains{0};   // switches into write-drain mode
    uint64_t data_bus_busy{0};  // tCK the data buses carried bursts, summed over channels
};

// Open-page DDR controller: channels -> ranks -> bank groups -> banks, FR-FCFS scheduling
// (Rixner et al., ISCA 2000) over separate read and write queues with write-drain watermarks.
// Ticks in DRAM clock cycles; each channel issues at most one command per cycle.
class Controller {
   public:
    // `done` is the DRAM cycle the last data beat arrives; `tag` is the value given to enqueue().
    using ReadDoneCb = std::function<void(uint64_t line_addr, uint64_t tag, uint64_t done)>;

    Controller(const Organization& org, const Timing& t, const SchedConfig& sched);

    void setReadDoneCallback(ReadDoneCb cb) { mReadDone = std::move(cb); }

    bool canAccept(uint64_t addr, bool is_write) const;
    void enqueue(uint64_t addr, bool is_write, uint64_t tag, uint64_t now);
    void tick(uint64_t now);

    uint32_t readQueueOccupancy() const;
    uint32_t writeQueueOccupancy() const;
    const Stats& stats() const { return mStats; }

   private:
    struct Coord {
        uint32_t channel{0};
        uint32_t rank{0};
        uint32_t bank_group{0};
        uint32_t bank{0};  // flat index within the rank: bank_group * banks_per_group + bank
        uint64_t row{0};
    };

    struct Request {
        uint64_t addr{0};
        uint64_t tag{0};
        Coord c;
        bool classified{false};  // row hit/miss/conflict already counted
    };

    struct Bank {
        bool open{false};
        uint64_t row{0};
        uint64_t next_act{0};
        uint64_t next_pre{0};
        uint64_t next_rd{0};
        uint64_t next_wr{0};
    };

    struct Rank {
        std::vector<Bank> banks;
        std::deque<uint64_t> act_window;  // the last four ACT cycles (tFAW)
        uint64_t next_refresh{0};
        bool refresh_pending{false};
    };

    struct Channel {
        std::deque<Request> rq;
        std::deque<Request> wq;
        std::vector<Rank> ranks;
        bool draining{false};
    };

    Coord decode(uint64_t addr) const;
    bool tryRefresh(Channel& ch, uint64_t now);
    bool trySchedule(Channel& ch, std::deque<Request>& q, bool is_write, uint64_t now);
    bool rowHasPendingHit(const std::deque<Request>& q, const Coord& c) const;
    bool canActivate(const Rank& r, const Bank& b, uint64_t now) const;
    void classify(Request& req, uint64_t* counter);
    void issueAct(Channel& ch, const Coord& c, uint64_t now);
    void issuePre(Channel& ch, uint32_t rank, uint32_t bank, uint64_t now);
    void issueColumn(Channel& ch, const Coord& c, bool is_write, uint64_t now);

    Organization mOrg;
    Timing mT;
    SchedConfig mSched;
    uint32_t mLineShift;
    uint32_t mLinesPerRow;
    std::vector<Channel> mChannels;
    ReadDoneCb mReadDone;
    Stats mStats;
};

}  // namespace memory::dram
