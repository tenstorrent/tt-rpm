#pragma once

#include <string>

namespace cpu {

std::string generateSnapshotFolderName(const std::string& traceFileName);
std::string generateWhisperPath(const std::string& snapshotFolderName);

}  // namespace cpu
