#pragma once

#include <cstdint>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "Common/PipelinePacket.hpp"

namespace core {

struct InstrTrace {
    uint64_t tag{0};
    uint64_t whisper_tag{0};  // Whisper's tag (may be reused after flush)
    uint64_t pc{0};
    std::string disasm;
    cpu::InstClass inst_class{cpu::InstClass::ALU};
    UopType uop_type{UopType::ALU};
    uint8_t wrong_path_depth{0};  // speculation depth for visualization
    // pipeline stage timestamps
    uint64_t fetch{0};
    uint64_t icache_start{0}, icache_end{0};
    uint64_t fetch_queue_enter{0}, fetch_queue_exit{0};
    uint64_t decode{0};
    uint64_t decode_queue_enter{0}, decode_queue_exit{0};
    uint64_t rename{0};
    uint64_t iq_enter{0};  // cycle instruction entered issue queue (OOO only)
    uint64_t issue{0};     // cycle dispatched from issue queue to execute
    uint64_t exe_enter{0}, exe_done{0}, exe_exit{0};
    uint64_t lsq_enter{0}, lsq_exit{0};
    uint64_t wb{0};
    // notable event cycles (0 = event did not occur)
    uint64_t icache_miss_cycle{0};
    uint64_t dcache_miss_cycle{0};
    uint64_t branch_mispred_cycle{0};
    uint64_t squash_cycle{0};           // cycle instruction was squashed (0 = not squashed)
    bool wp_stall{false};               // stalled waiting for write port
    bool is_fallthrough_squash{false};  // squashed fallthrough after taken branch
};

class PipelineVisualizer {
   public:
    PipelineVisualizer(uint32_t max_instructions, std::string output_file, std::string format, bool color = false);
    ~PipelineVisualizer() = default;

    // Enable debug output for a specific cycle range (0 for end_cycle means unlimited)
    void enableDebugMode(const std::string& debug_file, uint64_t start_cycle, uint64_t end_cycle);

    // Called every cycle to output debug info if in range
    void debugTick(uint64_t current_cycle);

    // Flush any pending debug output (call before crash/exit)
    void flushDebug();

    // stage entry/exit hooks
    void onFetch(uint64_t tag, uint64_t pc, uint64_t cycle, cpu::InstClass inst_class, std::string_view disasm, uint8_t wrong_path_depth, uint64_t whisper_tag);
    void onIcacheStart(uint64_t tag, uint64_t cycle);
    void onIcacheEnd(uint64_t tag, uint64_t cycle);
    void onFetchQueueEnter(uint64_t tag, uint64_t cycle);
    void onFetchQueueExit(uint64_t tag, uint64_t cycle);
    void onDecode(uint64_t tag, uint64_t cycle);
    void onDecodeQueueEnter(uint64_t tag, uint64_t cycle);
    void onDecodeQueueExit(uint64_t tag, uint64_t cycle);
    void onRename(uint64_t tag, UopType t, uint64_t cycle);
    void onIqEnter(uint64_t tag, uint64_t cycle);  // entered issue queue
    void onIssue(uint64_t tag, uint64_t cycle);    // dispatched from issue queue
    void onExeEnter(uint64_t tag, uint64_t cycle);
    void onExeDone(uint64_t tag, uint64_t cycle);
    void onExeExit(uint64_t tag, uint64_t cycle);
    void onLsqEnter(uint64_t tag, uint64_t cycle);
    void onLsqExit(uint64_t tag, uint64_t cycle);
    void onWriteback(uint64_t tag, uint64_t cycle);
    // notable events
    void onIcacheMiss(uint64_t tag, uint64_t cycle);
    void onDcacheMiss(uint64_t tag, uint64_t cycle);
    void onBranchMispredict(uint64_t tag, uint64_t cycle);
    void onWritePortStall(uint64_t tag, uint64_t cycle);
    void onSquash(uint64_t tag, uint64_t cycle);  // instruction squashed
    void onSquashFallthrough(uint64_t tag);       // fallthrough after taken branch

    void dump();

   private:
    uint32_t mMax;
    std::string mOutputFile;
    std::string mFormat;
    bool mColor;

    std::unordered_map<uint64_t, InstrTrace> mTraces;
    std::vector<uint64_t> mOrder;

    // Debug mode - output per-cycle state for a specific cycle range
    bool mDebugEnabled{false};
    std::string mDebugFile;
    uint64_t mDebugStartCycle{0};
    uint64_t mDebugEndCycle{0};
    uint64_t mLastDebugCycle{0};
    uint64_t mLastDumpedCycle{0};
    std::ofstream mDebugStream;

    InstrTrace* get_(uint64_t tag);

    bool wasSquashedEarly_(uint64_t tag, const InstrTrace& t) const;

    void dumpTable_(std::ostream& os) const;
    void dumpWaterfall_(std::ostream& os) const;
    void dumpLog_(std::ostream& os) const;
    void dumpKanata_(std::ostream& os) const;  // Onikiri2-Kanata format (for ext/konata)
    void dumpDebugCycle_(std::ostream& os, uint64_t cycle) const;
    void dumpDebugWaterfall_(std::ostream& os, uint64_t page_start, uint64_t page_end) const;

    const char* stageName_(const InstrTrace& t, uint64_t cycle) const;
    const char* stageAnsiColor_(const char* stage, const InstrTrace& t) const;
    static const char* uopStr_(UopType t);
};

}  // namespace core
