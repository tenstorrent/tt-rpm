// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Tenstorrent USA, Inc.

#include "Common/Core.hpp"

namespace core {

Core::Core(sparta::TreeNode* node, const CoreParams* params)
    : sparta::Unit(node),
      mOooEnabled(params->ooo_enabled) {
    // Read all params (Sparta requires consumption)
    (void)static_cast<bool>(params->wrong_path_enabled);
    (void)static_cast<uint32_t>(params->wrong_path_max_depth);
    (void)static_cast<uint32_t>(params->misprediction_penalty_cycles);
    (void)static_cast<bool>(params->bypass_queues);
    (void)static_cast<bool>(params->visualizer_enabled);
    (void)static_cast<uint32_t>(params->visualizer_max_instructions);
    (void)static_cast<std::string>(params->visualizer_format);
    (void)static_cast<std::string>(params->visualizer_output_file);
    (void)static_cast<bool>(params->visualizer_color);
    (void)static_cast<bool>(params->visualizer_streaming);
    (void)static_cast<bool>(params->visualizer_debug_enabled);
    (void)static_cast<std::string>(params->visualizer_debug_file);
    (void)static_cast<uint64_t>(params->visualizer_debug_start_cycle);
    (void)static_cast<uint64_t>(params->visualizer_debug_end_cycle);
    (void)static_cast<bool>(params->cache_viewer_enabled);
    (void)static_cast<std::string>(params->cache_viewer_format);
    (void)static_cast<std::string>(params->cache_viewer_output_file);
}

}  // namespace core
