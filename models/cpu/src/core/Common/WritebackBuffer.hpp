// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Tenstorrent USA, Inc.

#pragma once

#include <cstdint>
#include <deque>
#include <vector>

#include "Common/PipelinePacket.hpp"

namespace core {
class PipelineVisualizer;
}

namespace midcore {

// ============================================================================
// Writeback Buffer
//
// Sits between Execute/LSQ and the Write Port Arbiter.
// Buffers completed instructions for a configurable number of cycles before
// they become eligible for writeback. This models:
// - PRF write latency
// - Decoupling between FU completion and scoreboard wakeup
// ============================================================================
class WritebackBuffer {
   public:
    struct Config {
        uint32_t capacity{16};
        uint32_t drain_width{4};       // Max entries that can drain per cycle
        uint8_t writeback_latency{1};  // Cycles in buffer before eligible
    };

    struct Entry {
        uint64_t tag{0};
        uint64_t fetch_seq{0};  // Monotonic fetch-order id (never reused)
        std::vector<core::PhysRegRef> phys_dsts;
        core::ROBToken rob_token;
        uint8_t source_id{0};  // Execute group or LSQ
        uint8_t cycles_remaining{0};
        bool ready{false};  // Countdown complete
    };

    void configure(const Config& cfg);

    // Accept a completion from Execute or LSQ
    // Returns true if accepted, false if buffer full
    bool accept(uint64_t tag, uint64_t fetch_seq, const std::vector<core::PhysRegRef>& phys_dsts, const core::ROBToken& rob_token, uint8_t source_id);

    // Advance countdowns, mark entries as ready
    void tick();

    // Get ready entries (up to drain_width) for submission to arbiter
    std::vector<Entry*> getReadyEntries();

    // Remove an entry after it's been granted a write port
    void remove(uint64_t tag);

    // Query
    bool isFull() const { return mEntries.size() >= mCapacity; }
    bool canAccept() const { return mEntries.size() < mCapacity; }
    uint32_t size() const { return static_cast<uint32_t>(mEntries.size()); }

    void setVisualizer(core::PipelineVisualizer* v) { mVis = v; }

    // Stats
    uint64_t numAccepted() const { return mNumAccepted; }
    uint64_t numDrained() const { return mNumDrained; }
    uint64_t numStalls() const { return mNumStalls; }

   private:
    std::deque<Entry> mEntries;
    uint32_t mCapacity{16};
    uint32_t mDrainWidth{4};
    uint8_t mWritebackLatency{1};

    core::PipelineVisualizer* mVis{nullptr};

    // Stats
    uint64_t mNumAccepted{0};
    uint64_t mNumDrained{0};
    uint64_t mNumStalls{0};
};

}  // namespace midcore
