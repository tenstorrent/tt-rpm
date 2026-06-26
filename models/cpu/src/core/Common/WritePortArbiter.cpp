// SPDX-FileCopyrightText: © 2026 Tenstorrent AI ULC
//
// SPDX-License-Identifier: Apache-2.0

#include "Common/WritePortArbiter.hpp"

#include "Common/PipelineVisualizer.hpp"

namespace midcore {

void WritePortArbiter::buildUnified(uint32_t num_ports, uint32_t num_exe_groups) {
    mPorts.resize(num_ports);
    std::vector<uint8_t> all_sources;
    for (uint32_t g = 0; g < num_exe_groups; ++g) all_sources.push_back(static_cast<uint8_t>(g));
    all_sources.push_back(kLsqSourceId);
    for (auto& port : mPorts) port.sources = all_sources;
}

void WritePortArbiter::buildMapped(const std::vector<std::vector<uint8_t>>& mapping) {
    mPorts.resize(mapping.size());
    for (size_t i = 0; i < mapping.size(); ++i) mPorts[i].sources = mapping[i];
}

void WritePortArbiter::submit(const WriteCandidate& cand) { mPending[cand.source_id].push_back(cand); }

void WritePortArbiter::arbitrate() {
    mGranted.clear();
    mGrantedTags.clear();

    for (auto& port : mPorts) {
        const uint32_t n = static_cast<uint32_t>(port.sources.size());
        if (n == 0) continue;

        for (uint32_t attempt = 0; attempt < n; ++attempt) {
            uint32_t idx = (port.rr_index + attempt) % n;
            uint8_t src = port.sources[idx];
            auto it = mPending.find(src);
            if (it == mPending.end() || it->second.empty()) continue;

            // skip candidates already granted on another port this cycle
            auto& vec = it->second;
            bool found = false;
            for (auto cit = vec.begin(); cit != vec.end(); ++cit) {
                if (!mGrantedTags.count(cit->tag)) {
                    mGranted.push_back(std::move(*cit));
                    mGrantedTags.insert(mGranted.back().tag);
                    vec.erase(cit);
                    found = true;
                    break;
                }
            }

            if (found) {
                port.rr_index = (idx + 1) % n;
                break;
            }
        }
    }

    // Mark denied candidates for write-port stall visualization
    if (mVis) {
        for (auto& [src, vec] : mPending) {
            for (auto& cand : vec) {
                if (!mGrantedTags.count(cand.tag)) mVis->onWritePortStall(cand.fetch_seq, 0);
            }
        }
    }
}

void WritePortArbiter::clear() {
    mPending.clear();
    mGranted.clear();
    mGrantedTags.clear();
}

}  // namespace midcore
