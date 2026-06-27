// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Tenstorrent USA, Inc.

#pragma once

#include "sparta/simulation/ParameterSet.hpp"
#include "sparta/simulation/Unit.hpp"

#include "models/cpu/common/BaseTypes.hpp"

namespace core {

class Core : public sparta::Unit {
   public:
    class CoreParams : public sparta::ParameterSet {
       public:
        CoreParams(sparta::TreeNode* n)
            : sparta::ParameterSet(n) {}
        PARAMETER(bool, ooo_enabled, true, "Out-of-order execution enabled")
        PARAMETER(bool, wrong_path_enabled, false, "Enable wrong-path speculative execution")
        PARAMETER(uint32_t, wrong_path_max_depth, 8, "Max nested speculation levels")
        PARAMETER(uint32_t, misprediction_penalty_cycles, 15, "Stall cycles on misprediction when wrong_path_enabled=false")
        PARAMETER(bool, bypass_queues, true, "Bypass FetchQueue/DecodeQueue for simpler wrong-path bring-up")
        PARAMETER(bool, visualizer_enabled, false, "Enable pipeline visualizer")
        PARAMETER(uint32_t, visualizer_max_instructions, 200, "Max instructions to visualize")
        PARAMETER(std::string, visualizer_format, "table", "Visualizer format: table | waterfall | log")
        PARAMETER(std::string, visualizer_output_file, "stderr", "Visualizer output file path")
        PARAMETER(bool, visualizer_color, false, "Enable ANSI color in visualizer output")
        PARAMETER(bool, visualizer_streaming, false, "Stream events to file immediately (for crash debugging)")
        PARAMETER(bool, visualizer_debug_enabled, false, "Enable per-cycle debug output")
        PARAMETER(std::string, visualizer_debug_file, "pipeline_debug.txt", "Debug output file path")
        PARAMETER(uint64_t, visualizer_debug_start_cycle, 0, "First cycle to output debug info")
        PARAMETER(uint64_t, visualizer_debug_end_cycle, 0, "Last cycle to output debug info (0 = unlimited)")
        PARAMETER(bool, cache_viewer_enabled, false, "Enable cache viewer/tracer")
        PARAMETER(std::string, cache_viewer_format, "log", "Cache viewer format: log | waterfall | summary")
        PARAMETER(std::string, cache_viewer_output_file, "stderr", "Cache viewer output file")
    };

    Core(sparta::TreeNode* node, const CoreParams* params);
    ~Core() override = default;

    static constexpr char name[] = "core";

    void setId(cpu::coreid_t id) { mId = id; }
    cpu::coreid_t getId() const { return mId; }
    bool oooEnabled() const { return mOooEnabled; }

   private:
    cpu::coreid_t mId{0};
    bool mOooEnabled{true};
};

}  // namespace core
