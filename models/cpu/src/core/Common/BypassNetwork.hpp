#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "PipelinePacket.hpp"

namespace midcore {

// Forward declaration
class Execute;

// ============================================================================
// Bypass Path Configuration
// ============================================================================
struct BypassPathConfig {
    std::string producer;  // FU group name or "load"
    std::string consumer;  // FU group name or "any"
    uint8_t latency;       // 0 = same cycle, 1 = next cycle, etc.
};

// ============================================================================
// Bypass Network
//
// Models the timing of result forwarding between functional units.
// When an instruction issues, it registers its destination with the bypass
// network. Consumers can query whether a result will be available via bypass
// (faster than reading from the register file).
// ============================================================================
class BypassNetwork {
   public:
    static constexpr uint8_t kNoBypass = 255;
    static constexpr uint8_t kAnyConsumer = 254;

    struct Config {
        bool enabled{false};
        uint8_t regfile_read_latency{1};  // Latency to read from PRF (no bypass)
        std::vector<BypassPathConfig> paths;
    };

    void configure(const Config& cfg, uint32_t num_fu_groups);

    // Called when an instruction issues to a FU group
    // Returns the cycle when result will be available for bypass
    void registerProducer(uint64_t tag, uint8_t fu_group, uint8_t exec_latency, const std::vector<core::PhysRegRef>& dsts, uint64_t issue_cycle);

    // Called when instruction completes (result now in PRF)
    void completeProducer(uint64_t tag);

    // Query: what's the effective latency for a consumer in fu_group to get
    // the result from producer_tag?
    // Returns: cycles until result available (0 = this cycle)
    //          kNoBypass if no bypass path exists (must wait for PRF)
    uint8_t getBypassLatency(uint64_t producer_tag, uint8_t consumer_fu) const;

    // Query: given a physical register, is there an in-flight producer that
    // can bypass to consumer_fu? If so, return the bypass latency.
    // Returns kNoBypass if no bypass available.
    uint8_t checkBypassForReg(const core::PhysRegRef& reg, uint8_t consumer_fu, uint64_t current_cycle) const;

    // Advance one cycle (decrement in-flight countdowns)
    void tick(uint64_t current_cycle);

    // Stats
    uint64_t numBypassHits() const { return mNumBypassHits; }
    uint64_t numBypassMisses() const { return mNumBypassMisses; }

    bool enabled() const { return mEnabled; }
    uint8_t regfileReadLatency() const { return mRegfileReadLatency; }

    // Get the minimum bypass latency from a producer FU to any consumer.
    // Used by speculative wakeup to determine earliest possible consumer scheduling.
    // Returns kNoBypass if no bypass paths exist from this producer.
    uint8_t getMinBypassLatency(uint8_t producer_fu) const;

   private:
    struct InFlightResult {
        uint64_t tag;
        uint8_t producer_fu;
        uint64_t available_cycle;  // Cycle when result is ready at bypass
        std::vector<core::PhysRegRef> dsts;
    };

    // Lookup: producer_fu -> consumer_fu -> latency
    // latency = cycles AFTER issue that result is available to consumer
    // kNoBypass means no bypass path exists
    std::vector<std::vector<uint8_t>> mBypassLatency;

    // In-flight results that can be bypassed, keyed by instruction tag
    std::unordered_map<uint64_t, InFlightResult> mInFlight;

    // Map: physical reg -> tag of in-flight result (for fast lookup)
    // Key = (type << 16) | phys_reg
    std::unordered_map<uint32_t, uint64_t> mRegToTag;

    bool mEnabled{false};
    uint8_t mRegfileReadLatency{1};
    uint32_t mNumFuGroups{0};

    // FU name -> index mapping (set during configure)
    std::unordered_map<std::string, uint8_t> mFuNameToIndex;

    // Precomputed minimum bypass latency per producer FU (computed in configure)
    std::vector<uint8_t> mMinBypassLatency;

    // Stats (mutable: updated from const query methods)
    mutable uint64_t mNumBypassHits{0};
    mutable uint64_t mNumBypassMisses{0};

    static uint32_t regKey(const core::PhysRegRef& r) { return (static_cast<uint32_t>(r.type) << 16) | r.phys_reg; }
};

}  // namespace midcore