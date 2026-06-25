#include "BypassNetwork.hpp"

#include <iostream>

namespace midcore {

void BypassNetwork::configure(const Config& cfg, uint32_t num_fu_groups) {
    mEnabled = cfg.enabled;
    mRegfileReadLatency = cfg.regfile_read_latency;
    mNumFuGroups = num_fu_groups;

    if (!mEnabled) return;

    // Initialize bypass latency matrix with kNoBypass (no path)
    mBypassLatency.assign(num_fu_groups + 1, std::vector<uint8_t>(num_fu_groups + 1, kNoBypass));

    // Build FU name -> index mapping
    // Index 0..num_fu_groups-1 = Execute FU groups
    // Index num_fu_groups = "load" (from LSQ)
    mFuNameToIndex["alu"] = 0;
    mFuNameToIndex["mul"] = 1;
    mFuNameToIndex["div"] = 2;
    mFuNameToIndex["branch"] = 3;
    mFuNameToIndex["ldst"] = 4;
    mFuNameToIndex["fp"] = 5;
    mFuNameToIndex["vec"] = 6;
    mFuNameToIndex["fence"] = 7;
    mFuNameToIndex["load"] = num_fu_groups;  // Special: load results from LSQ
    mFuNameToIndex["any"] = kAnyConsumer;

    // Parse configured bypass paths
    for (const auto& path : cfg.paths) {
        auto prod_it = mFuNameToIndex.find(path.producer);
        auto cons_it = mFuNameToIndex.find(path.consumer);

        if (prod_it == mFuNameToIndex.end()) {
            std::cerr << "[bypass] Warning: unknown producer '" << path.producer << "'\n";
            continue;
        }

        uint8_t prod_idx = prod_it->second;

        if (path.consumer == "any" || cons_it->second == kAnyConsumer) {
            // Bypass to all consumers
            for (uint32_t c = 0; c < num_fu_groups; ++c) {
                mBypassLatency[prod_idx][c] = path.latency;
            }
        } else if (cons_it != mFuNameToIndex.end()) {
            mBypassLatency[prod_idx][cons_it->second] = path.latency;
        } else {
            std::cerr << "[bypass] Warning: unknown consumer '" << path.consumer << "'\n";
        }
    }

    // Precompute minimum bypass latency from each producer to any consumer
    mMinBypassLatency.assign(num_fu_groups + 1, kNoBypass);
    for (uint32_t prod = 0; prod <= num_fu_groups; ++prod) {
        uint8_t min_lat = kNoBypass;
        for (uint32_t cons = 0; cons < num_fu_groups; ++cons) {
            if (cons < mBypassLatency[prod].size()) {
                uint8_t lat = mBypassLatency[prod][cons];
                if (lat != kNoBypass && lat < min_lat) {
                    min_lat = lat;
                }
            }
        }
        mMinBypassLatency[prod] = min_lat;
    }

    std::cerr << "[bypass] Configured with " << cfg.paths.size() << " paths, "
              << "regfile_read_latency=" << static_cast<int>(mRegfileReadLatency) << "\n";
}

void BypassNetwork::registerProducer(uint64_t tag, uint8_t fu_group, uint8_t exec_latency, const std::vector<core::PhysRegRef>& dsts, uint64_t issue_cycle) {
    if (!mEnabled || dsts.empty()) return;

    InFlightResult& result = mInFlight[tag];
    result.tag = tag;
    result.producer_fu = fu_group;
    result.available_cycle = issue_cycle + exec_latency;  // When result is computed
    result.dsts = dsts;

    // Register in reg lookup map
    for (const auto& dst : dsts) {
        mRegToTag[regKey(dst)] = tag;
    }
}

void BypassNetwork::completeProducer(uint64_t tag) {
    if (!mEnabled) return;

    auto it = mInFlight.find(tag);
    if (it == mInFlight.end()) return;

    // Remove from reg lookup
    for (const auto& dst : it->second.dsts) {
        mRegToTag.erase(regKey(dst));
    }

    // Remove from in-flight map
    mInFlight.erase(it);
}

uint8_t BypassNetwork::getBypassLatency(uint64_t producer_tag, uint8_t consumer_fu) const {
    if (!mEnabled) return kNoBypass;

    auto it = mInFlight.find(producer_tag);
    if (it == mInFlight.end()) return kNoBypass;

    const auto& result = it->second;
    if (result.producer_fu < mBypassLatency.size() && consumer_fu < mBypassLatency[result.producer_fu].size()) {
        return mBypassLatency[result.producer_fu][consumer_fu];
    }

    return kNoBypass;
}

uint8_t BypassNetwork::checkBypassForReg(const core::PhysRegRef& reg, uint8_t consumer_fu, uint64_t current_cycle) const {
    if (!mEnabled) return kNoBypass;

    auto reg_it = mRegToTag.find(regKey(reg));
    if (reg_it == mRegToTag.end()) return kNoBypass;

    auto result_it = mInFlight.find(reg_it->second);
    if (result_it == mInFlight.end()) return kNoBypass;

    const InFlightResult& result = result_it->second;

    // Check if bypass path exists
    if (result.producer_fu >= mBypassLatency.size() || consumer_fu >= mBypassLatency[result.producer_fu].size()) {
        return kNoBypass;
    }

    uint8_t path_latency = mBypassLatency[result.producer_fu][consumer_fu];
    if (path_latency == kNoBypass) {
        ++mNumBypassMisses;
        return kNoBypass;
    }

    // Calculate when result is available via this bypass path
    // available_cycle is when result is computed at producer
    // path_latency is additional cycles to forward to consumer
    uint64_t bypass_ready = result.available_cycle + path_latency;

    if (bypass_ready <= current_cycle) {
        ++mNumBypassHits;
        return 0;  // Available now
    }

    ++mNumBypassHits;
    return static_cast<uint8_t>(bypass_ready - current_cycle);
}

void BypassNetwork::tick(uint64_t /*current_cycle*/) {
    // Currently no per-cycle processing needed
    // Results are cleaned up via completeProducer()
}

uint8_t BypassNetwork::getMinBypassLatency(uint8_t producer_fu) const {
    if (!mEnabled) return kNoBypass;
    if (producer_fu >= mMinBypassLatency.size()) return kNoBypass;
    return mMinBypassLatency[producer_fu];
}

}  // namespace midcore