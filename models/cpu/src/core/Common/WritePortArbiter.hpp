// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Tenstorrent USA, Inc.

#pragma once

#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "Common/PipelinePacket.hpp"

namespace core {
class PipelineVisualizer;
}

namespace midcore {

static constexpr uint8_t kLsqSourceId = 254;

struct WriteCandidate {
    uint8_t source_id{0};
    uint64_t tag{0};
    uint64_t fetch_seq{0};  // Monotonic fetch-order id (never reused)
    std::vector<core::PhysRegRef> phys_dsts;
    core::ROBToken rob_token;
};

class WritePortArbiter {
   public:
    struct Port {
        std::vector<uint8_t> sources;
        uint32_t rr_index{0};
    };

    void buildUnified(uint32_t num_ports, uint32_t num_exe_groups);
    void buildMapped(const std::vector<std::vector<uint8_t>>& mapping);

    void submit(const WriteCandidate& cand);
    void arbitrate();
    void clear();

    bool enabled() const { return !mPorts.empty(); }

    const std::vector<WriteCandidate>& granted() const { return mGranted; }

    bool isGranted(uint64_t tag) const { return mGrantedTags.count(tag); }

    void setVisualizer(core::PipelineVisualizer* v) { mVis = v; }

   private:
    std::vector<Port> mPorts;

    // per-source pending candidates (oldest first)
    std::unordered_map<uint8_t, std::vector<WriteCandidate>> mPending;

    std::vector<WriteCandidate> mGranted;
    std::unordered_set<uint64_t> mGrantedTags;

    core::PipelineVisualizer* mVis{nullptr};
};

}  // namespace midcore
