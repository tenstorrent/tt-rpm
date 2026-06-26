// SPDX-FileCopyrightText: © 2026 Tenstorrent AI ULC
//
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <string>

namespace cpu {

std::string generateSnapshotFolderName(const std::string& traceFileName);
std::string generateWhisperPath(const std::string& snapshotFolderName);

}  // namespace cpu
