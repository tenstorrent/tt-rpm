// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Tenstorrent USA, Inc.


#include "SnapshotUtil.hpp"

#include <sys/stat.h>

#include <filesystem>
#include <regex>

#include "sparta/utils/SpartaAssert.hpp"

namespace cpu {

std::string generateSnapshotFolderName(const std::string& traceFileName) {
    if (traceFileName.empty()) return "";

    // Check if the path is already a snapshot folder (contains memory or registers file)
    std::filesystem::path input_path(traceFileName);
    if (std::filesystem::is_directory(input_path)) {
        auto memory_path = input_path / "memory";
        auto registers_path = input_path / "registers";
        if (std::filesystem::exists(memory_path) || std::filesystem::exists(registers_path)) {
            std::cout << "[Snapshot Util] Input is already a snapshot folder: " << traceFileName << '\n';
            return traceFileName;
        }
    }

    std::string snapshotFolder;
    bool is_first_simpoint{false};

    static const std::regex intervl("i[0-9]+(M|K)?");  // i, then NUMERIC, then M or K or nothing.
    static const std::regex snapsht("-s[0-9]+-");      // Match simpoint ID like -s975- (surrounded by hyphens)

    std::filesystem::path file_path(traceFileName);
    auto trace_name = file_path.filename().string();
    std::smatch i, s;
    std::regex_search(trace_name, s, snapsht);
    if (s.size() != 1) return "";
    std::string snpShotId = s[0];
    if (snpShotId.empty()) return "";

    snpShotId = snpShotId.substr(2, snpShotId.length() - 3);
    uint64_t snpShotIndx = std::stoul(snpShotId);
    if (snpShotIndx == 0) is_first_simpoint = true;

    std::regex_search(trace_name, i, intervl);
    if (i.empty()) return "";
    std::string intervlSubStr = i[0];
    if (intervlSubStr.empty()) return "";
    intervlSubStr = intervlSubStr.substr(1);
    uint64_t intervlSize = 0;
    uint64_t multiplier = 1;
    if (intervlSubStr.back() == 'M' || intervlSubStr.back() == 'K') {
        multiplier = (intervlSubStr.back() == 'M') ? 1000000 : 1000;
        intervlSubStr = intervlSubStr.substr(0, intervlSubStr.length() - 1);
    }
    intervlSize = std::stoul(intervlSubStr) * multiplier;
    uint64_t snpShotInstr = snpShotIndx * intervlSize;

    std::stringstream snapshotFolderSS;
    // Use position to extract prefix before "-s975-", then add "snap" + instruction count
    snapshotFolderSS << trace_name.substr(0, s.position(0) + 1) << "snap" << snpShotInstr;

    std::cout << "\n\t[Snapshot Util] token " << s[0] << " match at position " << s.position(0) << "\n\t\tSnapshot Index: " << snpShotIndx;
    std::cout << "\n\t[Snapshot Util] token " << i[0] << " match at position " << i.position(0) << "\n\t\tIntervl : " << intervlSubStr << " * " << multiplier
              << " * " << snpShotIndx << " = " << snpShotInstr << "\n\t\tsnap loc: " << snapshotFolderSS.str();
    std::cout << "\n";

    snapshotFolder = file_path.parent_path().string() + "/" + snapshotFolderSS.str();

    // FIXME: eventually should converge the naming for snap folders
    auto roi_snap_name = std::regex_replace(snapshotFolder, std::regex("snap"), "snap-roi0-");
    if (is_first_simpoint && !std::filesystem::exists(snapshotFolder) && !std::filesystem::exists(roi_snap_name)) return "";

    struct stat snapshotFolderStat = {};
    if (stat(snapshotFolder.c_str(), &snapshotFolderStat) != 0) {
        // FIXME:try again on roi naming convention
        if (stat(roi_snap_name.c_str(), &snapshotFolderStat) != 0) {
            std::cerr << "[Snapshot Util] Warning: Could not open snapshot folder (path tried: " << snapshotFolder << ")" << '\n';
            std::cerr << "[Snapshot Util] Also tried ROI variant: " << roi_snap_name << '\n';
            snapshotFolder = "";
        } else
            snapshotFolder = roi_snap_name;
    }

    std::cout << "[Snapshot Util] Found snapshot folder path: " << snapshotFolder << '\n';

    return snapshotFolder;
}

//! \brief Generate whisper path from snapshot folder
std::string generateWhisperPath(const std::string& snapshotFolderName) {
    if (snapshotFolderName.empty()) return "";
    std::filesystem::path file_path(snapshotFolderName);

    // check if parent, whisper_traces, directory exists
    if (file_path.has_parent_path()) {
        file_path = file_path.parent_path();
    }

    // check if root traces directory exists
    if (file_path.has_parent_path()) {
        file_path = file_path.parent_path();
    }

    file_path /= "bin";
    std::string whisperFolderPath = file_path.string();
    struct stat whisperFolderStat = {};
    if (stat(whisperFolderPath.c_str(), &whisperFolderStat) != 0) {
        std::cerr << "[Snapshot Util] Warning: Could not open whisper configuration folder: " << whisperFolderPath << '\n';
        whisperFolderPath = "";
    } else {
        std::cout << "[Snapshot Util] Found whisper configuration path: " << whisperFolderPath << '\n';
    }

    return whisperFolderPath;
}

}  // namespace cpu
