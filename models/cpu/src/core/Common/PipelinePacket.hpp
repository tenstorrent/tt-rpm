#pragma once

#include <cstdint>
#include <optional>
#include <ostream>
#include <vector>

#include "models/cpu/common/BaseTypes.hpp"
#include "models/cpu/common/Instruction.hpp"

namespace core {

// Values match WhisperUtil::OperandType so static_cast is safe.
enum class RegType : uint8_t { Int = 0, Fp = 1, Csr = 2, Vec = 3, Imm = 4 };

inline bool isRegFileType(RegType t) { return t == RegType::Int || t == RegType::Fp || t == RegType::Vec; }

// Shared index for Int/Fp/Vec register type arrays (sized kNumRegFileTypes=3).
// Note: RegType::Vec=3 does not match array index 2, so a switch is required.
inline size_t regTypeIndex(RegType t) {
    switch (t) {
        case RegType::Int:
            return 0;
        case RegType::Fp:
            return 1;
        case RegType::Vec:
            return 2;
        default:
            __builtin_unreachable();
    }
}

enum class UopType : uint8_t {
    ALU = 0,
    Mul = 1,
    Div = 2,
    Branch = 3,
    Load = 4,
    Store = 5,
    FpOp = 6,
    VecOp = 7,
    Fence = 8,
};

// Number of distinct UopType values — single source of truth for array sizing.
static constexpr size_t kNumUopTypes = static_cast<size_t>(UopType::Fence) + 1;

struct PipelinePacket {
    cpu::address_t pc{0};
    cpu::InstPtr inst{nullptr};
    uint64_t tag{0};      // Whisper tag (may be reused after flush)
    uint64_t fetch_seq{0};  // Monotonic fetch-order id (never reused, globally unique)
    cpu::InstClass inst_class{cpu::InstClass::ALU};
    uint8_t size{4};              // instruction size in bytes (2 or 4 for RISC-V)
    uint8_t wrong_path_depth{0};  // speculation nesting depth (0 = correct path)
};

struct RegOperand {
    RegType type{RegType::Int};
    uint8_t number{0};
};

// Physical register reference -- carries both the physical reg number and its type.
// tag is used by the in-order arch scoreboard to guard against WAW aliasing; ignored in OOO.
struct PhysRegRef {
    uint16_t phys_reg{0};
    core::RegType type{core::RegType::Int};
    uint64_t tag{0};
};

struct DecodePacket {
    PipelinePacket pkt;
    UopType uop_type{UopType::ALU};
    uint8_t latency{1};
    bool predicted_taken{false};
    bool was_mispredicted{false};
};

// Token returned by the ROB on allocate — carries both slot index and epoch
// so that stale completions for recycled slots are safely ignored.
struct ROBToken {
    uint32_t idx{0};
    uint32_t epoch{0};
};

// ROB entry — allocated at Rename, committed in-order by Writeback
struct ROBEntry {
    uint64_t tag{0};
    uint64_t fetch_seq{0};  // Monotonic fetch-order id (never reused)
    cpu::address_t pc{0};
    UopType uop_type{UopType::ALU};
    uint8_t wrong_path_depth{0};            // For depth-aware squash
    std::vector<PhysRegRef> old_phys_dsts;  // Released to free list on commit
    std::vector<PhysRegRef> new_phys_dsts;  // Released to free list on squash
    bool completed{false};
    uint32_t epoch{0};
    std::optional<uint32_t> checkpoint_slot;  // Only branches have checkpoints
};

// Flows Rename → Issue (carries physical register mappings for scoreboard)
struct RenamedPacket {
    PipelinePacket pkt;
    UopType uop_type{UopType::ALU};
    uint8_t latency{1};
    ROBToken rob_token;
    std::vector<PhysRegRef> phys_srcs;
    std::vector<PhysRegRef> phys_dsts;

    // Routing hint: scheduler containing the primary producer (set by Rename)
    // Used for dependency-aware load balancing. 255 = no hint / unknown.
    uint8_t producer_scheduler{255};

    bool predicted_taken{false};
    bool was_mispredicted{false};

    // Checkpoint slot for branches (used for misprediction recovery)
    std::optional<uint32_t> checkpoint_slot;
};

// Flows Issue → Execute → LSQ
struct IssuePacket {
    PipelinePacket pkt;
    UopType uop_type{UopType::ALU};
    uint8_t latency{1};
    ROBToken rob_token;
    std::vector<PhysRegRef> phys_srcs;
    std::vector<PhysRegRef> phys_dsts;

    bool predicted_taken{false};
    bool was_mispredicted{false};

    // Construct from RenamedPacket (drops producer_scheduler hint)
    static IssuePacket from(const RenamedPacket& rpkt) {
        return {rpkt.pkt, rpkt.uop_type, rpkt.latency, rpkt.rob_token, rpkt.phys_srcs, rpkt.phys_dsts, rpkt.predicted_taken, rpkt.was_mispredicted};
    }
};

inline std::ostream& operator<<(std::ostream& os, const RegOperand& op) {
    os << "RegOperand{type=" << static_cast<int>(op.type) << ", num=" << static_cast<int>(op.number) << "}";
    return os;
}

inline std::ostream& operator<<(std::ostream& os, const PipelinePacket& pkt) {
    os << "PipelinePacket{pc=0x" << std::hex << pkt.pc << std::dec << ", tag=" << pkt.tag << ", fetch_seq=" << pkt.fetch_seq
       << ", depth=" << static_cast<int>(pkt.wrong_path_depth) << "}";
    return os;
}

inline std::ostream& operator<<(std::ostream& os, const DecodePacket& dp) {
    os << "DecodePacket{" << dp.pkt << ", uop=" << static_cast<int>(dp.uop_type) << ", lat=" << static_cast<int>(dp.latency) << "}";
    return os;
}

inline std::ostream& operator<<(std::ostream& os, const PhysRegRef& ref) {
    os << "PhysRegRef{p" << ref.phys_reg << ", type=" << static_cast<int>(ref.type) << "}";
    return os;
}

inline std::ostream& operator<<(std::ostream& os, const ROBToken& tok) {
    os << "ROBToken{idx=" << tok.idx << ", epoch=" << tok.epoch << "}";
    return os;
}

inline std::ostream& operator<<(std::ostream& os, const RenamedPacket& rpkt) {
    os << "RenamedPacket{" << rpkt.pkt << ", uop=" << static_cast<int>(rpkt.uop_type) << ", lat=" << static_cast<int>(rpkt.latency)
       << ", rob=" << rpkt.rob_token << ", phys_srcs=" << rpkt.phys_srcs.size() << ", phys_dsts=" << rpkt.phys_dsts.size() << "}";
    return os;
}

inline std::ostream& operator<<(std::ostream& os, const IssuePacket& ipkt) {
    os << "IssuePacket{" << ipkt.pkt << ", uop=" << static_cast<int>(ipkt.uop_type) << ", lat=" << static_cast<int>(ipkt.latency) << ", rob=" << ipkt.rob_token
       << ", phys_srcs=" << ipkt.phys_srcs.size() << ", phys_dsts=" << ipkt.phys_dsts.size() << "}";
    return os;
}

struct FetchRequest {
    cpu::address_t base_pc{0};
    bool hit{true};
    uint32_t size_bytes{0};
    std::vector<PipelinePacket> packets;
};

inline std::ostream& operator<<(std::ostream& os, const FetchRequest& req) {
    os << "FetchRequest{base_pc=0x" << std::hex << req.base_pc << std::dec << ", size=" << req.size_bytes << ", pkts=" << req.packets.size() << "}";
    return os;
}

struct FetchResponse {
    cpu::address_t base_pc{0};
    bool miss{false};
};

inline std::ostream& operator<<(std::ostream& os, const FetchResponse& response) {
    os << "FetchResponse{base_pc=0x" << std::hex << response.base_pc << "}";
    return os;
}

struct MemRequest {
    uint64_t tag{0};
    cpu::address_t pc{0};
    cpu::address_t address{0};  // effective address for structural cache lookup
    uint8_t access_size{4};     // access width in bytes (1, 2, 4, 8)
    bool is_store{false};
};

inline std::ostream& operator<<(std::ostream& os, const MemRequest& req) {
    os << "MemRequest{tag=" << req.tag << ", pc=0x" << std::hex << req.pc << ", addr=0x" << req.address << std::dec
       << ", size=" << static_cast<int>(req.access_size) << ", store=" << req.is_store << "}";
    return os;
}

struct MemResponse {
    uint64_t tag{0};
    bool miss{false};
};

inline std::ostream& operator<<(std::ostream& os, const MemResponse& resp) {
    os << "MemResponse{tag=" << resp.tag << "}";
    return os;
}

// L1 → L2 fill request (sent when L1 structural-mode CacheInterface misses).
struct FillRequest {
    cpu::address_t line_addr{0};
};

inline std::ostream& operator<<(std::ostream& os, const FillRequest& r) {
    os << "FillRequest{line=0x" << std::hex << r.line_addr << std::dec << "}";
    return os;
}

// L2 → L1 fill response (sent when the L2 has finished fetching the line).
struct FillResponse {
    cpu::address_t line_addr{0};
};

inline std::ostream& operator<<(std::ostream& os, const FillResponse& r) {
    os << "FillResponse{line=0x" << std::hex << r.line_addr << std::dec << "}";
    return os;
}

// L1 → L2 writeback request (sent when dirty L1 line is evicted).
struct WritebackRequest {
    cpu::address_t line_addr{0};
};

inline std::ostream& operator<<(std::ostream& os, const WritebackRequest& r) {
    os << "WritebackRequest{line=0x" << std::hex << r.line_addr << std::dec << "}";
    return os;
}

// L2 → L1 invalidate request (sent for inclusive L2 back-invalidation).
struct InvalidateRequest {
    cpu::address_t line_addr{0};
};

inline std::ostream& operator<<(std::ostream& os, const InvalidateRequest& r) {
    os << "InvalidateRequest{line=0x" << std::hex << r.line_addr << std::dec << "}";
    return os;
}

struct BranchPrediction {
    uint64_t tag{0};
    uint64_t fetch_seq{0};  // Monotonic fetch-order id (never reused)
    uint64_t pc{0};
    uint64_t target_pc{0};        // Actual branch target from Whisper (if taken)
    uint8_t inst_size{4};         // Instruction size in bytes (2 or 4 for RISC-V)
    uint8_t wrong_path_depth{0};  // Speculation depth of the branch instruction
    bool predicted_taken{false};
    bool mispredicted{false};
};

inline std::ostream& operator<<(std::ostream& os, const BranchPrediction& bp) {
    os << "BranchPrediction{pc=0x" << std::hex << bp.pc << ", target=0x" << bp.target_pc << std::dec << ", taken=" << bp.predicted_taken
       << ", mispred=" << bp.mispredicted << "}";
    return os;
}

// === Speculation / Flush Support ===

// Branch redirect sent from Execute on confirmed misprediction
struct BranchRedirect {
    uint64_t branch_tag{0};
    uint64_t branch_fetch_seq{0};  // Monotonic fetch-order id for stale redirect filtering
    uint64_t branch_pc{0};
    uint64_t correct_target_pc{0};
    uint8_t wrong_path_depth{0};
    bool was_taken{false};
};

inline std::ostream& operator<<(std::ostream& os, const BranchRedirect& br) {
    os << "BranchRedirect{tag=" << br.branch_tag << ", fetch_seq=" << br.branch_fetch_seq << ", pc=0x" << std::hex << br.branch_pc << ", target=0x"
       << br.correct_target_pc << std::dec << ", taken=" << br.was_taken << "}";
    return os;
}

// Signal sent when a branch resolves correctly (prediction matched actual outcome).
// This allows the pipeline to decrement wrong_path_depth for younger instructions.
struct BranchResolved {
    uint64_t branch_tag{0};
    uint64_t branch_fetch_seq{0};
    uint8_t resolved_depth{0};  // The depth of the resolved branch
};

inline std::ostream& operator<<(std::ostream& os, const BranchResolved& br) {
    os << "BranchResolved{tag=" << br.branch_tag << ", fetch_seq=" << br.branch_fetch_seq << ", depth=" << static_cast<int>(br.resolved_depth) << "}";
    return os;
}

// Lightweight redirect from BranchPredictor (speculative, no flush)
struct PredictedRedirect {
    uint64_t branch_tag{0};
    uint64_t branch_fetch_seq{0};  // Monotonic fetch-order id of the branch (for stale redirect detection)
    uint64_t branch_pc{0};
    uint64_t target_pc{0};
    uint8_t wrong_path_depth{0};  // Depth of the branch that triggered this redirect
};

inline std::ostream& operator<<(std::ostream& os, const PredictedRedirect& pr) {
    os << "PredictedRedirect{tag=" << pr.branch_tag << ", fetch_seq=" << pr.branch_fetch_seq << ", pc=0x" << std::hex << pr.branch_pc << ", target=0x"
       << pr.target_pc << std::dec << ", depth=" << (int)pr.wrong_path_depth << "}";
    return os;
}

// Source of a flush request - determines priority and handling
enum class FlushSource : uint8_t {
    BranchPredictor,  // Speculative redirect (low priority)
    Execute           // Confirmed misprediction (high priority)
};

inline std::ostream& operator<<(std::ostream& os, FlushSource src) {
    switch (src) {
        case FlushSource::BranchPredictor:
            os << "BranchPredictor";
            break;
        case FlushSource::Execute:
            os << "Execute";
            break;
    }
    return os;
}

// Flush request sent to FlushArbiter
struct FlushRequest {
    FlushSource source{FlushSource::BranchPredictor};
    uint64_t branch_tag{0};
    uint64_t branch_fetch_seq{0};  // Monotonic fetch-order id (prediction clearing / stale-redirect filtering)
    uint64_t branch_pc{0};
    uint64_t target_pc{0};
    uint8_t branch_depth{0};
    bool predicted_taken{false};
    bool was_taken{false};  // Actual branch outcome (for Execute misprediction recovery)
};

inline std::ostream& operator<<(std::ostream& os, const FlushRequest& fr) {
    os << "FlushRequest{source=" << fr.source << ", tag=" << fr.branch_tag << ", fetch_seq=" << fr.branch_fetch_seq << ", pc=0x" << std::hex << fr.branch_pc
       << ", target=0x" << fr.target_pc << std::dec << ", depth=" << static_cast<int>(fr.branch_depth) << ", taken=" << fr.predicted_taken << "}";
    return os;
}

}  // namespace core
