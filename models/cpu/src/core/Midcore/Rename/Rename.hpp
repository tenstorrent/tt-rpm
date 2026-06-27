// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Tenstorrent USA, Inc.

#pragma once

#include <algorithm>
#include <array>
#include <cassert>
#include <deque>
#include <vector>

#include "sparta/ports/DataPort.hpp"
#include "sparta/simulation/ParameterSet.hpp"
#include "sparta/simulation/Unit.hpp"
#include "sparta/statistics/Counter.hpp"

#include "Common/CheckpointManager.hpp"
#include "Common/PipelinePacket.hpp"
#include "Common/PipelineVisualizer.hpp"

namespace midcore {

class PhysicalRegisterFile {
    static constexpr size_t kNumRegTypes = 3;  // Int, Fp, Vec
    static constexpr size_t kMaxBanks = 16;

    static size_t idx(core::RegType t) { return core::regTypeIndex(t); }

    struct PhysicalMapping {
        uint32_t architecturalRegister{0};
        uint32_t speculativeRegister{0};
    };

    std::array<std::deque<uint16_t>, kNumRegTypes> mFreeLists;
    std::array<std::array<PhysicalMapping, 32>, kNumRegTypes> mMappings;
    std::array<uint16_t, kNumRegTypes> mFreeCounts{};

    uint16_t mHeadroom;

   public:
    // ============== Banking Support ==============
    struct BankConfig {
        uint32_t num_banks{1};
        uint32_t reads_per_bank{8};
        uint32_t writes_per_bank{4};
        bool enabled{false};
    };

    struct Bank {
        uint32_t reads_used{0};
        uint32_t writes_used{0};
        uint32_t reads_per_cycle{8};
        uint32_t writes_per_cycle{4};

        bool canRead() const { return reads_used < reads_per_cycle; }
        bool canWrite() const { return writes_used < writes_per_cycle; }
        void consumeRead() { ++reads_used; }
        void consumeWrite() { ++writes_used; }
        void reset() { reads_used = writes_used = 0; }
    };

    PhysicalRegisterFile(uint32_t num_physical_registers, uint16_t headroom = 4)
        : mHeadroom(headroom) {
        for (size_t t = 0; t < kNumRegTypes; ++t) {
            mMappings[t].fill({0, 0});
            for (uint32_t i = 0; i < 32; ++i) {
                mMappings[t][i] = {i, i};
            }
            for (uint32_t i = 32; i < num_physical_registers; ++i) {
                mFreeLists[t].push_back(static_cast<uint16_t>(i));
            }
            mFreeCounts[t] = static_cast<uint16_t>(num_physical_registers - 32);
        }
    }

    void configureBanking(const BankConfig& cfg) {
        mBankConfig = cfg;
        if (cfg.enabled && cfg.num_banks > 0) {
            mBanks.resize(cfg.num_banks);
            for (auto& b : mBanks) {
                b.reads_per_cycle = cfg.reads_per_bank;
                b.writes_per_cycle = cfg.writes_per_bank;
            }
        }
    }

    uint32_t bankFor(uint16_t phys_reg) const {
        if (mBanks.empty()) return 0;
        return phys_reg % static_cast<uint32_t>(mBanks.size());
    }

    bool canReadRegs(const std::vector<core::PhysRegRef>& regs) const {
        if (!mBankConfig.enabled || mBanks.empty()) return true;

        // Count reads per bank (stack-allocated)
        std::array<uint32_t, kMaxBanks> bank_reads{};
        for (const auto& r : regs) {
            uint32_t bank = bankFor(r.phys_reg);
            ++bank_reads[bank];
        }

        // Check if all banks can handle the reads
        for (size_t i = 0; i < mBanks.size(); ++i) {
            if (mBanks[i].reads_used + bank_reads[i] > mBanks[i].reads_per_cycle) {
                return false;
            }
        }
        return true;
    }

    void consumeReads(const std::vector<core::PhysRegRef>& regs) {
        if (!mBankConfig.enabled || mBanks.empty()) return;
        for (const auto& r : regs) {
            mBanks[bankFor(r.phys_reg)].consumeRead();
        }
    }

    bool canWriteRegs(const std::vector<core::PhysRegRef>& regs) const {
        if (!mBankConfig.enabled || mBanks.empty()) return true;

        std::array<uint32_t, kMaxBanks> bank_writes{};
        for (const auto& r : regs) {
            uint32_t bank = bankFor(r.phys_reg);
            ++bank_writes[bank];
        }

        for (size_t i = 0; i < mBanks.size(); ++i) {
            if (mBanks[i].writes_used + bank_writes[i] > mBanks[i].writes_per_cycle) {
                return false;
            }
        }
        return true;
    }

    void consumeWrites(const std::vector<core::PhysRegRef>& regs) {
        if (!mBankConfig.enabled || mBanks.empty()) return;
        for (const auto& r : regs) {
            mBanks[bankFor(r.phys_reg)].consumeWrite();
        }
    }

    void resetBanks() {
        for (auto& b : mBanks) b.reset();
    }

    bool bankingEnabled() const { return mBankConfig.enabled; }
    uint32_t numBanks() const { return static_cast<uint32_t>(mBanks.size()); }

    bool canAllocate(const std::vector<core::RegOperand>& dst_regs) const {
        std::array<uint32_t, kNumRegTypes> needed{};
        for (const auto& dst : dst_regs) {
            if (dst.type == core::RegType::Int && dst.number == 0) continue;
            ++needed[idx(dst.type)];
        }
        for (size_t t = 0; t < kNumRegTypes; ++t) {
            if (mFreeCounts[t] < needed[t] + mHeadroom) return false;
        }
        return true;
    }

    uint32_t getFreeRegister(core::RegType type) {
        auto& fl = mFreeLists[idx(type)];
        auto& cnt = mFreeCounts[idx(type)];
        assert(!fl.empty() && "getFreeRegister: free list exhausted");
        uint16_t preg = fl.front();
        fl.pop_front();
        --cnt;
        return preg;
    }

    void releaseRegister(uint32_t register_index, core::RegType type) {
        mFreeLists[idx(type)].push_back(static_cast<uint16_t>(register_index));
        ++mFreeCounts[idx(type)];
    }

    uint16_t freeCount(core::RegType type) const { return mFreeCounts[idx(type)]; }

    PhysicalMapping& mapping(core::RegType type, uint32_t arch_reg) {
        assert(arch_reg < 32 && "mapping: arch_reg out of bounds");
        return mMappings[idx(type)][arch_reg];
    }

    const PhysicalMapping& mapping(core::RegType type, uint32_t arch_reg) const {
        assert(arch_reg < 32 && "mapping: arch_reg out of bounds");
        return mMappings[idx(type)][arch_reg];
    }

    // RAT accessors for CheckpointManager (template interface)
    uint32_t getMapping(size_t type_idx, size_t arch_reg) const { return mMappings[type_idx][arch_reg].speculativeRegister; }

    void setMapping(size_t type_idx, size_t arch_reg, uint32_t phys_reg) { mMappings[type_idx][arch_reg].speculativeRegister = phys_reg; }

    // Release register with duplicate check (needed for squash recovery)
    void releaseRegisterSafe(uint32_t register_index, core::RegType type) {
        auto& fl = mFreeLists[idx(type)];
        uint16_t preg = static_cast<uint16_t>(register_index);
        // Prevent duplicate entries in the free list
        if (std::find(fl.begin(), fl.end(), preg) != fl.end()) {
            return;  // Already in free list, skip
        }
        fl.push_back(preg);
        ++mFreeCounts[idx(type)];
    }

   private:
    BankConfig mBankConfig;
    std::vector<Bank> mBanks;
};

// Tracks in-flight destination registers for the in-order pipeline.
// Each entry holds the tag of the most recent writer (0 = not busy).
// clearReg only clears the busy state when the completing instruction's tag
// matches the stored writer tag, preventing WAW aliasing from stale completions.
class ArchitecturalScoreboard {
    static constexpr size_t kNumRegTypes = 3;

    static size_t idx(core::RegType t) { return core::regTypeIndex(t); }

    std::array<std::array<uint64_t, 32>, kNumRegTypes> mWriterTag{};

   public:
    void markBusy(uint8_t arch_reg, core::RegType type, uint64_t tag) {
        if (type == core::RegType::Int && arch_reg == 0) return;
        assert(arch_reg < 32 && "markBusy: register index out of bounds");
        mWriterTag[idx(type)][arch_reg] = tag + 1;
    }

    void clearReg(uint8_t arch_reg, core::RegType type, uint64_t tag) {
        assert(arch_reg < 32 && "clearReg: register index out of bounds");
        if (mWriterTag[idx(type)][arch_reg] == tag + 1) mWriterTag[idx(type)][arch_reg] = 0;
    }

    bool sourcesReady(const std::vector<core::RegOperand>& srcs) const {
        for (const auto& src : srcs) {
            if (!core::isRegFileType(src.type)) continue;
            assert(src.number < 32 && "sourcesReady: register index out of bounds");
            if (mWriterTag[idx(src.type)][src.number] != 0) return false;
        }
        return true;
    }
};

class Issue;
class Execute;
class ReorderBuffer;
class LSQ;

}  // namespace midcore

namespace frontend {
class DecodeQueue;
}

namespace midcore {

class RenameParams : public sparta::ParameterSet {
   public:
    RenameParams(sparta::TreeNode* n)
        : sparta::ParameterSet(n) {}
    PARAMETER(uint32_t, dispatch_width, 4, "Max instructions renamed/dispatched per cycle")
    PARAMETER(uint32_t, num_phys_regs, 128, "Number of physical registers per type")
    PARAMETER(uint16_t, freelist_headroom, 8, "Min free regs before stalling dispatch")
    PARAMETER(bool, ooo_enabled, true, "Out-of-order rename/dispatch enabled")
    PARAMETER(bool, regfile_banking_enabled, false, "Enable register file banking")
    PARAMETER(uint32_t, regfile_num_banks, 4, "Number of register file banks")
    PARAMETER(uint32_t, regfile_reads_per_bank, 2, "Read ports per bank per cycle")
    PARAMETER(uint32_t, regfile_writes_per_bank, 1, "Write ports per bank per cycle")
    PARAMETER(uint32_t, max_branch_checkpoints, 32, "Max in-flight branch checkpoints")
    PARAMETER(bool, log_enabled, false, "Enable debug logging for this unit")
};

class Rename : public sparta::Unit {
   public:
    static constexpr char name[] = "rename";

    Rename(sparta::TreeNode* node, const RenameParams* params);

    // Direct input port for packets from Decode (bypass DecodeQueue)
    sparta::DataInPort<std::vector<core::DecodePacket>> packets_in{&unit_port_set_, "packets_in"};

    // OOO path: RenamedPacket → Issue
    sparta::DataOutPort<std::vector<core::RenamedPacket>> out_port{&unit_port_set_, "packets_out"};

    // In-order path: IssuePacket → Execute directly
    sparta::DataOutPort<std::vector<core::IssuePacket>> inorder_out{&unit_port_set_, "inorder_out"};

    sparta::DataInPort<std::vector<core::PhysRegRef>> commit_in{&unit_port_set_, "commit_in"};

    // In-order wakeup: completions from Execute and LSQ
    sparta::DataInPort<std::vector<core::PhysRegRef>> completion_exe_in{&unit_port_set_, "completion_exe_in"};
    sparta::DataInPort<std::vector<core::PhysRegRef>> completion_lsq_in{&unit_port_set_, "completion_lsq_in"};

    // Flush support for misprediction recovery
    sparta::DataInPort<core::FlushRequest> flush_in{&unit_port_set_, "flush_in"};
    sparta::DataInPort<std::vector<core::PhysRegRef>> freed_regs_in{&unit_port_set_, "freed_regs_in"};

    // Checkpoint release port (from Writeback when branches retire successfully)
    sparta::DataInPort<uint64_t> checkpoint_release_in{&unit_port_set_, "checkpoint_release_in", 0};

    void tick();
    bool isReady() const;  // Check if can dispatch
    size_t available() const { return mDispatchWidth - mPendingPackets.size(); }
    void setDecodeQueue(frontend::DecodeQueue* dq) { mDecodeQueue = dq; }
    void setDownstream(midcore::Issue* issue) { mDownstream = issue; }
    void setDownstreamExecute(midcore::Execute* exe) { mDownstreamExecute = exe; }
    void setROB(midcore::ReorderBuffer* rob) { mRob = rob; }
    void setLSQ(midcore::LSQ* lsq) { mLsq = lsq; }
    void setVisualizer(core::PipelineVisualizer* v) { mVis = v; }

    // Enable direct mode (bypass DecodeQueue)
    void setDirectMode(bool enabled) { mDirectMode = enabled; }

    uint64_t numDispatched() const { return mNumDispatched.get(); }
    uint64_t numPrfStallCycles() const { return mNumPrfStallCycles.get(); }
    uint64_t numCheckpointStallCycles() const { return mNumCheckpointStallCycles.get(); }

    PhysicalRegisterFile& getPrf() { return mPrf; }
    const PhysicalRegisterFile& getPrf() const { return mPrf; }
    CheckpointManager& getCheckpointManager() { return mCheckpointMgr; }

   private:
    void receiveCommits_(const std::vector<core::PhysRegRef>& old_dsts);
    void receiveCompletions_(const std::vector<core::PhysRegRef>& completions);
    void receiveFlush_(const core::FlushRequest& req);
    void receiveFreedRegs_(const std::vector<core::PhysRegRef>& regs);
    void receiveCheckpointRelease_(const uint64_t& branch_tag);
    void receivePackets_(const std::vector<core::DecodePacket>& pkts);

    uint32_t tickOoo_();
    uint32_t tickInorder_();

    struct RenameEntry {
        core::DecodePacket dp;
        std::vector<core::RegOperand> src_regs;
        std::vector<core::RegOperand> dst_regs;
    };

    frontend::DecodeQueue* mDecodeQueue{nullptr};
    midcore::Issue* mDownstream = nullptr;
    midcore::Execute* mDownstreamExecute = nullptr;
    midcore::ReorderBuffer* mRob = nullptr;
    midcore::LSQ* mLsq = nullptr;

    PhysicalRegisterFile mPrf;
    ArchitecturalScoreboard mArchScoreboard;
    CheckpointManager mCheckpointMgr;
    bool mOooEnabled;
    uint32_t mDispatchWidth;
    bool mLogEnabled{false};
    bool mDirectMode{false};

    // Speculation depth tracking (for visualization/debugging)
    uint8_t mCurrentWrongPathDepth{0};

    core::PipelineVisualizer* mVis{nullptr};

    std::vector<core::RenamedPacket> mRenamedBuf;
    std::vector<core::IssuePacket> mIssuedBuf;
    std::vector<core::DecodePacket> mPendingPackets;  // Buffer for direct mode

    sparta::Counter mNumDispatched;
    sparta::Counter mNumPrfStallCycles;
    sparta::Counter mNumCheckpointStallCycles;
};

}  // namespace midcore
