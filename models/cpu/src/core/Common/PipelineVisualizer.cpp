// SPDX-FileCopyrightText: © 2026 Tenstorrent AI ULC
//
// SPDX-License-Identifier: Apache-2.0

#include "Common/PipelineVisualizer.hpp"

#include <algorithm>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <ostream>
#include <sstream>
#include <string_view>

#include "Common/PipelinePacket.hpp"

namespace core {

// ANSI color codes
namespace ansi {
static constexpr const char* RESET = "\033[0m";
static constexpr const char* BLUE = "\033[34m";      // Fetch
static constexpr const char* CYAN = "\033[36m";      // Decode
static constexpr const char* MAGENTA = "\033[35m";   // Rename
static constexpr const char* YELLOW = "\033[33m";    // Issue queue wait
static constexpr const char* BLYELLOW = "\033[93m";  // Issue dispatch
static constexpr const char* GREEN = "\033[32m";     // Execute
static constexpr const char* BGREEN = "\033[92m";    // Writeback
static constexpr const char* RED = "\033[31m";       // miss / mispred
static constexpr const char* BRED = "\033[91m";      // I-cache miss fetch
}  // namespace ansi

// Format a tag for display - speculative tags shown as *N with indentation by depth
// With real tags, we use wrong_path_depth from the trace to determine speculation
static std::string formatTag(uint64_t tag, uint8_t wrong_path_depth = 0) {
    // Cap indentation at 4 spaces max to prevent formatting issues
    uint8_t capped_depth = std::min(wrong_path_depth, static_cast<uint8_t>(4));
    std::string indent(capped_depth, ' ');
    if (wrong_path_depth > 0) {
        return indent + std::string(wrong_path_depth, '*') + std::to_string(tag);
    }
    return indent + std::to_string(tag);
}

PipelineVisualizer::PipelineVisualizer(uint32_t max_instructions, std::string output_file, std::string format, bool color)
    : mMax(max_instructions),
      mOutputFile(std::move(output_file)),
      mFormat(std::move(format)),
      mColor(color) {}

void PipelineVisualizer::enableDebugMode(const std::string& debug_file, uint64_t start_cycle, uint64_t end_cycle) {
    mDebugEnabled = true;
    mDebugFile = debug_file;
    mDebugStartCycle = start_cycle;
    mDebugEndCycle = end_cycle;
    mLastDebugCycle = start_cycle > 0 ? start_cycle - 1 : 0;
    mLastDumpedCycle = start_cycle > 0 ? start_cycle - 1 : 0;
    mDebugStream.open(debug_file);
    if (!mDebugStream.is_open()) {
        std::cerr << "[vis] Could not open debug file " << debug_file << "\n";
        mDebugEnabled = false;
    } else {
        mDebugStream << "# Pipeline Debug Trace (Waterfall) - Cycles " << start_cycle << " to " << (end_cycle ? std::to_string(end_cycle) : "end") << "\n";
        mDebugStream << "# Each row shows instruction progression across cycles\n";
        mDebugStream << "# Stages: F=Fetch Ic=ICache D=Decode Rn=Rename Iq=IssueQueue Is=Issue Ex=Execute Lq=LSQ WB=Writeback\n";
        mDebugStream << std::string(100, '=') << "\n";
    }
}

void PipelineVisualizer::debugTick(uint64_t current_cycle) {
    if (!mDebugEnabled) return;
    if (current_cycle < mDebugStartCycle) return;
    if (mDebugEndCycle > 0 && current_cycle > mDebugEndCycle) return;

    // Output waterfall page every kDebugPageWidth cycles
    constexpr uint64_t kDebugPageWidth = 20;  // Smaller pages for easier debugging

    mLastDebugCycle = current_cycle;

    // Dump a page when we complete a page boundary
    uint64_t cycles_since_start = current_cycle - mDebugStartCycle;
    if (cycles_since_start > 0 && cycles_since_start % kDebugPageWidth == 0) {
        uint64_t page_start = current_cycle - kDebugPageWidth;
        dumpDebugWaterfall_(mDebugStream, page_start, current_cycle - 1);
        mLastDumpedCycle = current_cycle - 1;
    }
}

void PipelineVisualizer::flushDebug() {
    if (!mDebugEnabled) return;

    // Dump any remaining cycles since last dump
    if (mLastDebugCycle > mLastDumpedCycle) {
        uint64_t page_start = mLastDumpedCycle + 1;
        if (page_start < mDebugStartCycle) page_start = mDebugStartCycle;
        dumpDebugWaterfall_(mDebugStream, page_start, mLastDebugCycle);
    }

    mDebugStream.flush();
}

InstrTrace* PipelineVisualizer::get_(uint64_t tag) {
    if (mMax > 0 && mOrder.size() >= mMax && !mTraces.count(tag)) return nullptr;
    auto [it, inserted] = mTraces.emplace(tag, InstrTrace{});
    if (inserted) {
        it->second.tag = tag;
        mOrder.push_back(tag);
    }
    return &it->second;
}

void PipelineVisualizer::onFetch(uint64_t tag, uint64_t pc, uint64_t c, cpu::InstClass inst_class, std::string_view disasm, uint8_t wrong_path_depth,
                                 uint64_t whisper_tag) {
    if (auto* t = get_(tag)) {
        t->pc = pc;
        t->fetch = c;
        t->inst_class = inst_class;
        t->disasm = disasm;
        t->wrong_path_depth = wrong_path_depth;
        t->whisper_tag = whisper_tag;
    }
}
void PipelineVisualizer::onIcacheStart(uint64_t tag, uint64_t c) {
    if (auto* t = get_(tag)) {
        if (!t->icache_start) t->icache_start = c;
    }
}
void PipelineVisualizer::onIcacheEnd(uint64_t tag, uint64_t c) {
    if (auto* t = get_(tag)) {
        if (!t->icache_end) t->icache_end = c;
    }
}
void PipelineVisualizer::onFetchQueueEnter(uint64_t tag, uint64_t c) {
    if (auto* t = get_(tag)) {
        if (!t->fetch_queue_enter) t->fetch_queue_enter = c;
    }
}
void PipelineVisualizer::onFetchQueueExit(uint64_t tag, uint64_t c) {
    if (auto* t = get_(tag)) {
        if (!t->fetch_queue_exit) t->fetch_queue_exit = c;
    }
}
void PipelineVisualizer::onDecode(uint64_t tag, uint64_t c) {
    if (auto* t = get_(tag)) t->decode = c;
}
void PipelineVisualizer::onDecodeQueueEnter(uint64_t tag, uint64_t c) {
    if (auto* t = get_(tag)) {
        if (!t->decode_queue_enter) t->decode_queue_enter = c;
    }
}
void PipelineVisualizer::onDecodeQueueExit(uint64_t tag, uint64_t c) {
    if (auto* t = get_(tag)) {
        if (!t->decode_queue_exit) t->decode_queue_exit = c;
    }
}
void PipelineVisualizer::onRename(uint64_t tag, UopType u, uint64_t c) {
    if (auto* t = get_(tag)) {
        t->uop_type = u;
        t->rename = c;
    }
}
void PipelineVisualizer::onIqEnter(uint64_t tag, uint64_t c) {
    if (auto* t = get_(tag)) t->iq_enter = c;
}
void PipelineVisualizer::onIssue(uint64_t tag, uint64_t c) {
    if (auto* t = get_(tag)) t->issue = c;
}
void PipelineVisualizer::onExeEnter(uint64_t tag, uint64_t c) {
    if (auto* t = get_(tag)) t->exe_enter = c;
}
void PipelineVisualizer::onExeDone(uint64_t tag, uint64_t c) {
    if (auto* t = get_(tag)) {
        if (!t->exe_done) t->exe_done = c;
    }
}
void PipelineVisualizer::onExeExit(uint64_t tag, uint64_t c) {
    if (auto* t = get_(tag)) t->exe_exit = c;
}
void PipelineVisualizer::onLsqEnter(uint64_t tag, uint64_t c) {
    if (auto* t = get_(tag)) t->lsq_enter = c;
}
void PipelineVisualizer::onLsqExit(uint64_t tag, uint64_t c) {
    if (auto* t = get_(tag)) t->lsq_exit = c;
}
void PipelineVisualizer::onWriteback(uint64_t tag, uint64_t c) {
    if (auto* t = get_(tag)) t->wb = c;
}

void PipelineVisualizer::onIcacheMiss(uint64_t tag, uint64_t c) {
    if (auto* t = get_(tag)) t->icache_miss_cycle = c;
}
void PipelineVisualizer::onDcacheMiss(uint64_t tag, uint64_t c) {
    if (auto* t = get_(tag)) t->dcache_miss_cycle = c;
}
void PipelineVisualizer::onBranchMispredict(uint64_t tag, uint64_t c) {
    if (auto* t = get_(tag)) t->branch_mispred_cycle = c;
}
void PipelineVisualizer::onWritePortStall(uint64_t tag, uint64_t) {
    if (auto* t = get_(tag)) t->wp_stall = true;
}
void PipelineVisualizer::onSquashFallthrough(uint64_t tag) {
    if (auto* t = get_(tag)) t->is_fallthrough_squash = true;
}
void PipelineVisualizer::onSquash(uint64_t tag, uint64_t cycle) {
    if (auto* t = get_(tag)) t->squash_cycle = cycle;
}

// ──────────────────────────────────────────────────────────
// dump
// ──────────────────────────────────────────────────────────

void PipelineVisualizer::dump() {
    // Flush any pending debug output first
    flushDebug();

    std::ostream* out = &std::cerr;
    std::ofstream file;
    if (!mOutputFile.empty() && mOutputFile != "stderr") {
        file.open(mOutputFile);
        if (file.is_open())
            out = &file;
        else
            std::cerr << "[vis] Could not open " << mOutputFile << ", falling back to stderr\n";
    }

    if (mFormat == "waterfall")
        dumpWaterfall_(*out);
    else if (mFormat == "log")
        dumpLog_(*out);
    else if (mFormat == "kanata")
        dumpKanata_(*out);
    else
        dumpTable_(*out);
}

// ── helpers ──────────────────────────────────────────────

// Convert InstClass to UopType for display (approximation for instructions that didn't reach decode)
static UopType instClassToUopType(cpu::InstClass ic) {
    switch (ic) {
        case cpu::InstClass::ALU:
            return UopType::ALU;
        case cpu::InstClass::Multiply:
            return UopType::Mul;
        case cpu::InstClass::Divide:
            return UopType::Div;
        case cpu::InstClass::Branch:
        case cpu::InstClass::Jump:
            return UopType::Branch;
        case cpu::InstClass::Load:
            return UopType::Load;
        case cpu::InstClass::Store:
        case cpu::InstClass::Atomic:
            return UopType::Store;
        case cpu::InstClass::Float:
            return UopType::FpOp;
        case cpu::InstClass::Vector:
            return UopType::VecOp;
        case cpu::InstClass::Custom:
            return UopType::ALU;
    }
    return UopType::ALU;
}

const char* PipelineVisualizer::uopStr_(UopType t) {
    switch (t) {
        case UopType::ALU:
            return "ALU  ";
        case UopType::Mul:
            return "MUL  ";
        case UopType::Div:
            return "DIV  ";
        case UopType::Branch:
            return "BR   ";
        case UopType::Load:
            return "LOAD ";
        case UopType::Store:
            return "ST   ";
        case UopType::FpOp:
            return "FP   ";
        case UopType::VecOp:
            return "VEC  ";
        case UopType::Fence:
            return "FENCE";
    }
    return "?    ";
}

void PipelineVisualizer::dumpDebugWaterfall_(std::ostream& os, uint64_t page_start, uint64_t page_end) const {
    // Find instructions active in this cycle range
    std::vector<std::pair<uint64_t, const InstrTrace*>> active;

    for (uint64_t tag : mOrder) {
        const auto& t = mTraces.at(tag);
        // Check if instruction is active in any cycle of this range
        bool is_active = false;
        for (uint64_t c = page_start; c <= page_end && !is_active; ++c) {
            const char* stage = stageName_(t, c);
            if (stage[0] != ' ' && stage[0] != '.') {
                is_active = true;
            }
        }
        if (is_active) {
            active.emplace_back(tag, &t);
        }
    }

    if (active.empty()) {
        os << "\n=== Cycles " << page_start << " - " << page_end << " === (no active instructions)\n";
        return;
    }

    // Sort by tag
    std::sort(active.begin(), active.end(), [](const auto& a, const auto& b) { return a.first < b.first; });

    os << "\n=== Cycles " << page_start << " - " << page_end << " ===\n";

    // Header row with cycle numbers
    os << std::left << std::setw(14) << "VIS_TAG" << std::setw(6) << "WTAG" << std::setw(6) << "DEP" << std::setw(12) << "PC" << std::setw(6) << "TYPE"
       << "|";
    for (uint64_t c = page_start; c <= page_end; ++c) {
        os << std::setw(3) << (c % 100);  // Show last 2 digits of cycle
    }
    os << "| DISASM\n";
    os << std::string(44, '-') << "+" << std::string((page_end - page_start + 1) * 3, '-') << "+---\n";

    // Output each instruction row
    for (const auto& [tag, t] : active) {
        std::ostringstream pc_str;
        pc_str << std::hex << t->pc;

        UopType display_type = t->rename ? t->uop_type : instClassToUopType(t->inst_class);

        os << std::left << std::setw(14) << formatTag(tag, t->wrong_path_depth) << std::setw(6) << t->whisper_tag << std::setw(6)
           << static_cast<int>(t->wrong_path_depth) << std::setw(12) << pc_str.str() << std::setw(6) << uopStr_(display_type) << "|";

        // Stage progression across cycles
        for (uint64_t c = page_start; c <= page_end; ++c) {
            const char* s = stageName_(*t, c);
            os << s[0] << s[1] << " ";
        }

        // Disassembly at end (truncated)
        os << "| " << t->disasm.substr(0, 20) << "\n";
    }

    os.flush();
}

void PipelineVisualizer::dumpDebugCycle_(std::ostream& os, uint64_t cycle) const {
    os << "\n=== Cycle " << cycle << " ===\n";

    // Group instructions by their current stage
    std::vector<std::tuple<uint64_t, const InstrTrace*, const char*>> active;

    for (uint64_t tag : mOrder) {
        const auto& t = mTraces.at(tag);
        const char* stage = stageName_(t, cycle);
        if (stage[0] != ' ' && stage[0] != '.') {  // Active in some stage
            active.emplace_back(tag, &t, stage);
        }
    }

    if (active.empty()) {
        os << "  (no active instructions)\n";
        return;
    }

    // Sort by tag for consistent output
    std::sort(active.begin(), active.end(), [](const auto& a, const auto& b) { return std::get<0>(a) < std::get<0>(b); });

    // Output format: VIS_TAG (depth) | WHISPER_TAG | PC | TYPE | STAGE | DISASM
    os << std::left << std::setw(20) << "VIS_TAG" << std::setw(8) << "WTAG" << std::setw(6) << "DEPTH" << std::setw(14) << "PC" << std::setw(7) << "TYPE"
       << std::setw(6) << "STAGE"
       << "DISASM\n";
    os << std::string(80, '-') << "\n";

    for (const auto& [tag, t, stage] : active) {
        std::ostringstream pc_str;
        pc_str << "0x" << std::hex << t->pc;

        UopType display_type = t->rename ? t->uop_type : instClassToUopType(t->inst_class);

        os << std::left << std::setw(20) << formatTag(tag, t->wrong_path_depth) << std::setw(8) << t->whisper_tag << std::setw(6)
           << static_cast<int>(t->wrong_path_depth) << std::setw(14) << pc_str.str() << std::setw(7) << uopStr_(display_type) << std::setw(6) << stage
           << t->disasm << "\n";
    }

    // Summary stats for the cycle
    std::map<std::string, int> stage_counts;
    for (const auto& [tag, t, stage] : active) {
        (void)tag;
        (void)t;
        stage_counts[stage]++;
    }

    os << "\nStage Summary: ";
    for (const auto& [stage, count] : stage_counts) {
        os << stage << "=" << count << " ";
    }
    os << "\n";
}

// Returns true if instruction should be filtered from visualization
// We want to SHOW wrong-path instructions (depth > 0) even if squashed
// We want to HIDE correct-path "leftovers" (depth == 0) that didn't complete
// We want to HIDE fallthrough instructions after a taken branch (not on predicted path)
bool PipelineVisualizer::wasSquashedEarly_(uint64_t tag, const InstrTrace& t) const {
    (void)tag;  // With real tags, we use wrong_path_depth instead
    // Hide fallthrough instructions after a taken branch - these are not on the predicted path

    return false;
    if (t.is_fallthrough_squash) {
        return true;
    }
    // Always show wrong-path instructions (depth > 0) - these are the actual predicted path
    if (t.wrong_path_depth > 0) {
        return false;  // Show all wrong-path instructions
    }
    // For correct-path (depth == 0): hide if it didn't complete (leftover collateral)
    bool completed = t.wb || t.exe_exit || t.lsq_exit;
    return !completed;
}

// Returns 2-char stage label for a given cycle.
//   F  = Fetch          Ic = ICache         Fq = FetchQueue
//   D  = Decode         Dq = DecodeQueue    Rn = Rename
//   Iq = Issue-queue    Is = Issue          Sc = Scoreboard
//   Ex = Executing      Wp = Write-port     Lb = LSQ backpressure
//   Lq = In LSQ         Rb = ROB wait       WB = Writeback/retire
const char* PipelineVisualizer::stageName_(const InstrTrace& t, uint64_t c) const {
    if (c < t.fetch || t.fetch == 0) return "  ";

    // Check for squash - show "Sq" at squash cycle, nothing after
    if (t.squash_cycle) {
        if (c == t.squash_cycle) return "Sq";
        if (c > t.squash_cycle) return "  ";  // Nothing after squash
    }

    if (c == t.fetch) return "F ";

    // I-Cache processing
    if (t.icache_start && c >= t.icache_start && c <= t.icache_end) return "Ic";

    // FetchQueue: only show if instruction actually queued (not bypassed)
    // Bypassed instructions have enter == exit or no entry at all
    if (t.fetch_queue_enter && t.fetch_queue_exit && t.fetch_queue_enter < t.fetch_queue_exit) {
        if (c >= t.fetch_queue_enter && c <= t.fetch_queue_exit) return "Fq";
    } else if (t.fetch_queue_enter && !t.fetch_queue_exit) {
        // Still in queue (exit not recorded yet)
        if (c >= t.fetch_queue_enter) return "Fq";
    }

    if (c == t.decode) return "D ";

    // DecodeQueue: only show if instruction actually queued (not bypassed)
    if (t.decode_queue_enter && t.decode_queue_exit && t.decode_queue_enter < t.decode_queue_exit) {
        if (c >= t.decode_queue_enter && c <= t.decode_queue_exit) return "Dq";
    } else if (t.decode_queue_enter && !t.decode_queue_exit) {
        // Still in queue (exit not recorded yet)
        if (c >= t.decode_queue_enter) return "Dq";
    }

    if (c == t.rename) return "Rn";

    // OOO issue queue
    if (t.iq_enter && c >= t.iq_enter && (!t.issue || c < t.issue)) return "Iq";
    if (t.issue && c == t.issue) return "Is";

    // Scoreboard stall: after dispatch but before entering Execute
    uint64_t dispatch = t.issue ? t.issue : t.rename;
    if (t.exe_enter && c > dispatch && c < t.exe_enter) return "Sc";

    // Executing: exe_enter .. exe_done (actual computation)
    uint64_t done = t.exe_done ? t.exe_done : t.exe_exit;
    if (t.exe_enter && done && c >= t.exe_enter && c <= done) return "Ex";

    // Write-port stall (non-memory) or LSQ backpressure (memory):
    // exe_done+1 .. exe_exit
    if (t.exe_done && t.exe_exit && c > t.exe_done && c <= t.exe_exit) {
        bool is_mem = (t.uop_type == UopType::Load || t.uop_type == UopType::Store);
        return is_mem ? "Lb" : "Wp";
    }

    // In LSQ
    if (t.lsq_enter && c >= t.lsq_enter && c <= t.lsq_exit) return "Lq";

    // ROB retirement wait: after execution/LSQ completes, before WB
    uint64_t pipeline_done = t.lsq_exit ? t.lsq_exit : t.exe_exit;
    if (pipeline_done && t.wb && c > pipeline_done && c < t.wb) return "Rb";

    if (t.wb && c == t.wb) return "WB";
    if (t.wb && c > t.wb) return "  ";
    return ". ";
}

// Returns ANSI color prefix for a stage label, incorporating event flags.
const char* PipelineVisualizer::stageAnsiColor_(const char* s, const InstrTrace& t) const {
    if (s[0] == 'F' && s[1] == ' ') return t.icache_miss_cycle ? ansi::BRED : ansi::BLUE;         // Fetch
    if (s[0] == 'I' && s[1] == 'c') return t.icache_miss_cycle ? ansi::RED : ansi::BLUE;          // I-Cache
    if (s[0] == 'F' && s[1] == 'q') return ansi::BLUE;                                            // FetchQueue
    if (s[0] == 'D' && s[1] == ' ') return ansi::CYAN;                                            // Decode
    if (s[0] == 'D' && s[1] == 'q') return ansi::CYAN;                                            // DecodeQueue
    if (s[0] == 'R' && s[1] == 'n') return ansi::MAGENTA;                                         // Rename
    if (s[0] == 'R' && s[1] == 'b') return ansi::YELLOW;                                          // ROB wait
    if (s[0] == 'I' && s[1] == 'q') return ansi::YELLOW;                                          // Issue queue
    if (s[0] == 'I' && s[1] == 's') return t.branch_mispred_cycle ? ansi::BRED : ansi::BLYELLOW;  // Issue dispatch
    if (s[0] == 'S') return ansi::RED;                                                            // Sc (scoreboard stall)
    if (s[0] == 'E') return ansi::GREEN;                                                          // Ex
    if (s[0] == 'W' && s[1] == 'p') return ansi::RED;                                             // Write-port stall
    if (s[0] == 'L' && s[1] == 'b') return ansi::RED;                                             // LSQ backpressure
    if (s[0] == 'L') return t.dcache_miss_cycle ? ansi::RED : ansi::GREEN;                        // Lq
    if (s[0] == 'W') return ansi::BGREEN;                                                         // WB
    return nullptr;
}

// ── table format ─────────────────────────────────────────

void PipelineVisualizer::dumpTable_(std::ostream& os) const {
    auto cy = [](uint64_t c) -> std::string { return c ? std::to_string(c) : "-"; };
    auto range = [](uint64_t a, uint64_t b) -> std::string {
        if (!a) return "-";
        if (!b) return std::to_string(a);  // Entered but never exited - just show entry cycle
        return (a == b) ? std::to_string(a) : (std::to_string(a) + "-" + std::to_string(b));
    };

    os << "\n=== Pipeline Trace (table) ===\n";
    os << std::left << std::setw(20) << "TAG" << std::setw(8) << "WTAG" << std::setw(14) << "PC" << std::setw(7) << "TYPE" << std::setw(7) << "FETCH"
       << std::setw(10) << "ICACHE" << std::setw(10) << "FQUEUE" << std::setw(7) << "DEC" << std::setw(10) << "DQUEUE" << std::setw(7) << "REN" << std::setw(10)
       << "IQ" << std::setw(7) << "ISS" << std::setw(10) << "EXE" << std::setw(10) << "LSQ" << std::setw(7) << "WB"
       << "EVENTS"
       << "\n";
    os << std::string(145, '-') << "\n";

    for (uint64_t tag : mOrder) {
        const auto& t = mTraces.at(tag);

        // Skip correct-path leftovers (depth 0 that didn't complete)
        // but show wrong-path instructions even if squashed
        if (wasSquashedEarly_(tag, t)) continue;

        std::ostringstream pc_str;
        pc_str << "0x" << std::hex << t.pc;

        // Build events string
        std::string events;
        if (t.icache_miss_cycle) events += "iM ";
        if (t.dcache_miss_cycle) events += "dM ";
        if (t.branch_mispred_cycle) events += "Bp ";
        if (t.wp_stall) events += "Ws ";
        if (events.empty()) events = "-";

        // For IQ, show the range of cycles instruction was waiting in queue (enter to issue-1)
        // If bypassed (issue == iq_enter), this shows just the entry cycle
        uint64_t iq_end = (t.iq_enter && t.issue && t.issue > t.iq_enter) ? t.issue - 1 : t.iq_enter;

        // Use uop_type if rename occurred (it gets set there), otherwise derive from inst_class
        UopType display_type = t.rename ? t.uop_type : instClassToUopType(t.inst_class);

        os << std::left << std::setw(20) << formatTag(tag, t.wrong_path_depth) << std::setw(8) << t.whisper_tag << std::setw(14) << pc_str.str() << std::setw(7)
           << uopStr_(display_type) << std::setw(7) << cy(t.fetch) << std::setw(10) << range(t.icache_start, t.icache_end) << std::setw(10)
           << range(t.fetch_queue_enter, t.fetch_queue_exit) << std::setw(7) << cy(t.decode) << std::setw(10)
           << range(t.decode_queue_enter, t.decode_queue_exit) << std::setw(7) << cy(t.rename) << std::setw(10) << range(t.iq_enter, iq_end) << std::setw(7)
           << cy(t.issue) << std::setw(10) << range(t.exe_enter, t.exe_done ? t.exe_done : t.exe_exit) << std::setw(10) << range(t.lsq_enter, t.lsq_exit)
           << std::setw(7) << cy(t.wb) << events << "\n";
    }
}

// ── waterfall format ──────────────────────────────────────

void PipelineVisualizer::dumpWaterfall_(std::ostream& os) const {
    if (mOrder.empty()) return;

    uint64_t c_min = UINT64_MAX, c_max = 0;
    for (uint64_t tag : mOrder) {
        const auto& t = mTraces.at(tag);
        // Skip correct-path leftovers for cycle range calculation
        if (wasSquashedEarly_(tag, t)) continue;
        if (t.fetch) {
            c_min = std::min(c_min, t.fetch);
        }
        uint64_t end = t.wb ? t.wb : std::max({t.exe_exit, t.lsq_exit, t.rename, t.icache_end});
        if (end) {
            c_max = std::max(c_max, end);
        }
    }
    if (c_min == UINT64_MAX || c_max == 0) return;

    constexpr uint32_t kPageWidth = 32;

    for (uint64_t page_start = c_min; page_start <= c_max; page_start += kPageWidth) {
        uint64_t page_end = std::min(page_start + kPageWidth - 1, c_max);

        os << "\n=== Pipeline Trace (waterfall) cycles " << page_start << " - " << page_end << " ===\n";

        // Header: cycle numbers
        os << std::left << std::setw(20) << "TAG" << std::setw(7) << "TYPE"
           << "| ";
        for (uint64_t c = page_start; c <= page_end; ++c) os << std::setw(3) << c;
        os << "|\n";
        os << std::string(25 + (page_end - page_start + 1) * 3 + 1, '-') << "\n";

        for (uint64_t tag : mOrder) {
            const auto& t = mTraces.at(tag);

            // Skip correct-path leftovers but show wrong-path instructions
            if (wasSquashedEarly_(tag, t)) continue;

            uint64_t end = t.wb ? t.wb : std::max({t.exe_exit, t.lsq_exit, t.rename, t.icache_end});
            if (t.fetch > page_end || (end > 0 && end < page_start)) continue;

            os << std::left << std::setw(20) << formatTag(tag, t.wrong_path_depth) << std::setw(7) << uopStr_(t.uop_type) << "| ";

            for (uint64_t c = page_start; c <= page_end; ++c) {
                const char* s = stageName_(t, c);
                if (mColor) {
                    const char* color = stageAnsiColor_(s, t);
                    if (color)
                        os << color << s[0] << s[1] << ansi::RESET;
                    else
                        os << s[0] << s[1];
                } else {
                    os << s[0] << s[1];
                }
                os << " ";
            }

            // Event markers at end of row
            if (t.icache_miss_cycle || t.dcache_miss_cycle || t.branch_mispred_cycle || t.wp_stall) {
                os << " [";
                if (t.icache_miss_cycle) os << (mColor ? ansi::BRED : "") << "iM" << (mColor ? ansi::RESET : "") << " ";
                if (t.dcache_miss_cycle) os << (mColor ? ansi::RED : "") << "dM" << (mColor ? ansi::RESET : "") << " ";
                if (t.branch_mispred_cycle) os << (mColor ? ansi::BRED : "") << "Bp" << (mColor ? ansi::RESET : "") << " ";
                if (t.wp_stall) os << (mColor ? ansi::RED : "") << "Ws" << (mColor ? ansi::RESET : "") << " ";
                os << "]";
            }
            os << "|\n";
        }
    }
}

// ── pipetrace-viewer log format ───────────────────────────
//
// Format: <cycle>;<RecordID>;<EventName>;<data>
// RecordID R{tag} is the unique, monotonically increasing instruction key.
// Event mapping:
//   Fetch         → Fetch  (carries 0x<pc> <disasm>)
//   IcacheStart   → IcacheStart
//   IcacheEnd     → IcacheEnd
//   FetchQueueIn  → FetchQueueIn
//   FetchQueueOut → FetchQueueOut
//   Decode        → Decode
//   DecodeQueueIn → DecodeQueueIn
//   DecodeQueueOut→ DecodeQueueOut
//   Rename        → Allocate
//   Issue         → Pick   (dispatched from issue queue; OOO only)
//   ExeEnter      → Execute
//   LsqEnter      → LsArb  (memory ops only)
//   DCache        → DCache  (data cache miss)
//   Mispred       → Restart (pipeline restart from branch misprediction)
//   Writeback     → Retire

void PipelineVisualizer::dumpLog_(std::ostream& os) const {
    os << "# Pipeline trace — pipetrace-viewer log format\n";

    for (uint64_t tag : mOrder) {
        const auto& t = mTraces.at(tag);

        // Skip correct-path leftovers but show wrong-path instructions
        if (wasSquashedEarly_(tag, t)) continue;

        const std::string rid = "R" + std::to_string(tag);

        if (t.fetch) {
            os << t.fetch << ";" << rid << ";Fetch;0x" << std::hex << t.pc << std::dec;
            if (!t.disasm.empty()) os << " " << t.disasm;
            os << "\n";
        }

        if (t.icache_start) os << t.icache_start << ";" << rid << ";IcacheStart;\n";
        if (t.icache_end) os << t.icache_end << ";" << rid << ";IcacheEnd;\n";
        if (t.icache_miss_cycle) os << t.icache_miss_cycle << ";" << rid << ";ICache;\n";

        if (t.fetch_queue_enter) os << t.fetch_queue_enter << ";" << rid << ";FetchQueueIn;\n";
        if (t.fetch_queue_exit) os << t.fetch_queue_exit << ";" << rid << ";FetchQueueOut;\n";

        if (t.decode) os << t.decode << ";" << rid << ";Decode;\n";

        if (t.decode_queue_enter) os << t.decode_queue_enter << ";" << rid << ";DecodeQueueIn;\n";
        if (t.decode_queue_exit) os << t.decode_queue_exit << ";" << rid << ";DecodeQueueOut;\n";

        if (t.rename) os << t.rename << ";" << rid << ";Allocate;\n";

        // OOO only: instruction dispatched from issue queue to execute
        if (t.issue) os << t.issue << ";" << rid << ";Pick;\n";

        if (t.exe_enter) os << t.exe_enter << ";" << rid << ";Execute;\n";

        // Memory ops: LSQ entry
        if (t.lsq_enter) os << t.lsq_enter << ";" << rid << ";LsArb;\n";

        if (t.dcache_miss_cycle) os << t.dcache_miss_cycle << ";" << rid << ";DCache;\n";

        if (t.branch_mispred_cycle) os << t.branch_mispred_cycle << ";" << rid << ";Restart;\n";

        if (t.wb) os << t.wb << ";" << rid << ";Retire;\n";
    }
}

// Onikiri2-Kanata format (https://github.com/shioyadan/Konata). Tab-separated,
// cycle-ordered: header, then C=<start>, then per-cycle C<delta> with I/L (start
// + label), S/E (stage begin/end), R (retire/flush). Stages are emitted as a
// begin/end chain so each occupies the gap until the next stage starts. Keyed by
// the monotonic fetch_seq (InstrTrace::tag), so wrong-path instructions (reused
// model tags) never collide; they retire with type=1 (flush) at their squash
// cycle. INSN_ID_IN_FILE is assigned densely in fetch order, as Konata expects.
void PipelineVisualizer::dumpKanata_(std::ostream& os) const {
    os << "Kanata\t0004\n";

    struct Ev {
        uint64_t cycle;
        int prio;  // ordering within a cycle: I(0) < L(1) < E(2) < S(3) < R(4)
        std::string line;
    };
    std::vector<Ev> evs;

    // Assign file ids in fetch-cycle order (Konata wants sequential ids).
    std::vector<uint64_t> order = mOrder;
    std::stable_sort(order.begin(), order.end(), [&](uint64_t a, uint64_t b) { return mTraces.at(a).fetch < mTraces.at(b).fetch; });

    uint64_t fileId = 0;
    uint64_t retireId = 0;
    for (uint64_t key : order) {
        const auto& t = mTraces.at(key);
        if (!t.fetch) continue;  // never entered the pipeline

        std::vector<std::pair<uint64_t, const char*>> ms;
        auto add = [&](uint64_t c, const char* s) {
            if (c) ms.push_back({c, s});
        };
        add(t.fetch, "F");
        add(t.icache_start, "Ic");
        add(t.fetch_queue_enter, "Fq");
        add(t.decode, "De");
        add(t.decode_queue_enter, "Dq");
        add(t.rename, "Rn");
        add(t.iq_enter, "Iq");
        add(t.issue, "Is");
        add(t.exe_enter, "Ex");
        add(t.lsq_enter, "Ls");
        add(t.wb, "Wb");
        if (ms.empty()) continue;
        std::stable_sort(ms.begin(), ms.end(), [](const auto& a, const auto& b) { return a.first < b.first; });

        const uint64_t id = fileId++;
        const uint64_t fc = ms.front().first;

        {
            std::ostringstream o;
            o << "I\t" << id << "\t" << t.whisper_tag << "\t0";
            evs.push_back({fc, 0, o.str()});
        }
        {
            std::ostringstream o;
            o << "L\t" << id << "\t0\t0x" << std::hex << t.pc << std::dec;
            if (!t.disasm.empty()) o << " " << t.disasm;
            if (t.wrong_path_depth) o << "  [wrong-path d" << static_cast<int>(t.wrong_path_depth) << "]";
            evs.push_back({fc, 1, o.str()});
        }
        {
            std::ostringstream o;
            o << "L\t" << id << "\t1\tfetch_seq=" << t.tag << " model_tag=" << t.whisper_tag;
            if (t.icache_miss_cycle) o << " icache_miss";
            if (t.dcache_miss_cycle) o << " dcache_miss";
            if (t.branch_mispred_cycle) o << " branch_mispredict";
            evs.push_back({fc, 1, o.str()});
        }

        // Begin/end chain for the inner stages (each ends where the next begins).
        for (size_t i = 0; i + 1 < ms.size(); ++i) {
            std::ostringstream so;
            so << "S\t" << id << "\t0\t" << ms[i].second;
            evs.push_back({ms[i].first, 3, so.str()});
            std::ostringstream eo;
            eo << "E\t" << id << "\t0\t" << ms[i].second;
            evs.push_back({ms[i + 1].first, 2, eo.str()});
        }

        // Last stage runs until retire/flush.
        const bool retired = (t.wb != 0);
        const uint64_t endCycle = retired ? t.wb : (t.squash_cycle ? t.squash_cycle : ms.back().first);
        const uint64_t lastStart = ms.back().first;
        {
            std::ostringstream so;
            so << "S\t" << id << "\t0\t" << ms.back().second;
            evs.push_back({lastStart, 3, so.str()});
        }
        {
            std::ostringstream eo;
            eo << "E\t" << id << "\t0\t" << ms.back().second;
            evs.push_back({std::max(endCycle, lastStart), 2, eo.str()});
        }
        {
            const int type = retired ? 0 : 1;  // 0 = retire, 1 = flush
            const uint64_t rid = retired ? retireId++ : 0;
            std::ostringstream ro;
            ro << "R\t" << id << "\t" << rid << "\t" << type;
            evs.push_back({std::max(endCycle, lastStart), 4, ro.str()});
        }
    }

    std::stable_sort(evs.begin(), evs.end(), [](const Ev& a, const Ev& b) {
        if (a.cycle != b.cycle) return a.cycle < b.cycle;
        return a.prio < b.prio;
    });

    if (evs.empty()) return;
    uint64_t cur = evs.front().cycle;
    os << "C=\t" << cur << "\n";
    for (const auto& e : evs) {
        if (e.cycle > cur) {
            os << "C\t" << (e.cycle - cur) << "\n";
            cur = e.cycle;
        }
        os << e.line << "\n";
    }
}

}  // namespace core
