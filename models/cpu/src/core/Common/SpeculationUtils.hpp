// SPDX-FileCopyrightText: © 2026 Tenstorrent AI ULC
//
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>

#include "Common/PipelinePacket.hpp"

namespace core {

// Unified squash predicate used by all pipeline stages.
// Returns true if instruction should be squashed based on flush request.
//
// Squash logic depends on flush source:
// - Execute flushes (misprediction recovery):
//   - COMPLETELY protect depth=0 instructions (correct-path from after redirect)
//   - Squash deeper speculation OR same-depth-and-younger (for speculative instructions)
// - BP flushes (speculative redirect):
//   - DO NOT protect depth=0 (fallthrough needs to be flushed, Whisper is also flushed)
//   - Squash same-depth-and-younger (including depth 0)
inline bool shouldSquash(uint64_t inst_tag, uint8_t inst_depth, uint64_t branch_tag, uint8_t branch_depth, FlushSource source) {
    // Squash logic:
    // - Always squash instructions younger than the mispredicted branch at the same depth
    // - Always squash instructions at deeper speculation levels
    // - For Execute flushes following a BP flush, the correct-path instructions
    //   fetched after BP flush have depth 0, so they're protected.

    // BP flush: Whisper already flushed, speculative instructions have depth > 0
    // Execute flush: confirming misprediction, depth 0 are correct-path after BP redirect

    (void)source;  // Use depth-based logic for both sources

    // Squash deeper speculation
    if (inst_depth > branch_depth) {
        return true;
    }

    // Same depth: squash younger instructions
    if (inst_depth == branch_depth && inst_tag > branch_tag) {
        return true;
    }

    return false;
}

// Convenience overload taking a FlushRequest
inline bool shouldSquash(uint64_t inst_tag, uint8_t inst_depth, const FlushRequest& req) {
    return shouldSquash(inst_tag, inst_depth, req.branch_tag, req.branch_depth, req.source);
}

// Convenience overload taking a PipelinePacket
inline bool shouldSquash(const PipelinePacket& pkt, const FlushRequest& req) { return shouldSquash(pkt.tag, pkt.wrong_path_depth, req); }

}  // namespace core
