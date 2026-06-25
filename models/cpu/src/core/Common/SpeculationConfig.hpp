#pragma once

#include <cstdint>

namespace core {

// Centralized configuration for speculation behavior.
// Set during simulation initialization from config.yaml.
struct SpeculationConfig {
    bool enabled{false};           // Master switch for wrong-path speculation
    uint8_t max_depth{8};          // Max nested speculation levels
    uint32_t max_checkpoints{32};  // Max in-flight branch checkpoints

    // Derived helpers
    bool allowSpeculativeFetch() const { return enabled; }
};

// Global speculation configuration - set once at startup, read by all stages.
// Thread-safe for read access after initialization.
inline SpeculationConfig& getMutableSpeculationConfig() {
    static SpeculationConfig sConfig;
    return sConfig;
}

inline const SpeculationConfig& getSpeculationConfig() { return getMutableSpeculationConfig(); }

inline void setSpeculationConfig(const SpeculationConfig& cfg) { getMutableSpeculationConfig() = cfg; }

}  // namespace core
