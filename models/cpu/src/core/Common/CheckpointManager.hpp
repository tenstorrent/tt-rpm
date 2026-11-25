// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Tenstorrent USA, Inc.

#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <vector>

namespace midcore {

// Manages RAT checkpoints for branch misprediction recovery.
// Each conditional or indirect branch gets a checkpoint at rename time, enabling
// recovery from ANY misprediction regardless of speculation depth.
// Keyed by fetch_seq: model tags are reused after a frontend redirect while older
// packets with the same tag are still in flight, and a reused key leaked a slot.
class CheckpointManager {
   public:
    static constexpr size_t kNumRegTypes = 3;  // Int, Fp, Vec

    struct BranchCheckpoint {
        uint64_t branch_fetch_seq{0};
        bool valid{false};
        std::array<std::array<uint32_t, 32>, kNumRegTypes> rat_state;
    };

    explicit CheckpointManager(uint32_t capacity)
        : mCapacity(capacity) {
        mSlots.resize(capacity);
        mAllocationOrder.reserve(capacity);
    }

    bool canAllocate() const { return mNumActive < mCapacity; }

    // Allocate a checkpoint slot for the given branch fetch_seq.
    // Returns the slot index, or nullopt if no slots available.
    std::optional<uint32_t> allocate(uint64_t branch_fetch_seq) {
        if (!canAllocate()) return std::nullopt;

        // Find first free slot
        for (uint32_t i = 0; i < mCapacity; ++i) {
            if (!mSlots[i].valid) {
                mSlots[i].valid = true;
                mSlots[i].branch_fetch_seq = branch_fetch_seq;
                mSeqToSlot[branch_fetch_seq] = i;
                mAllocationOrder.push_back(branch_fetch_seq);
                ++mNumActive;
                return i;
            }
        }
        return std::nullopt;
    }

    // Save the current RAT state into the specified checkpoint slot.
    // RATAccessor must provide: uint32_t getMapping(size_t type_idx, size_t arch_reg)
    template <typename RATAccessor>
    void saveRAT(uint32_t slot, const RATAccessor& rat) {
        if (slot >= mCapacity || !mSlots[slot].valid) return;

        for (size_t t = 0; t < kNumRegTypes; ++t) {
            for (size_t r = 0; r < 32; ++r) {
                mSlots[slot].rat_state[t][r] = rat.getMapping(t, r);
            }
        }
    }

    // Restore RAT state from the checkpoint for the given branch fetch_seq.
    // RATAccessor must provide: void setMapping(size_t type_idx, size_t arch_reg, uint32_t phys_reg)
    // Returns true if successful, false if no checkpoint found.
    template <typename RATAccessor>
    bool restore(uint64_t branch_fetch_seq, RATAccessor& rat) {
        auto it = mSeqToSlot.find(branch_fetch_seq);
        if (it == mSeqToSlot.end()) return false;

        uint32_t slot = it->second;
        if (!mSlots[slot].valid) return false;

        for (size_t t = 0; t < kNumRegTypes; ++t) {
            for (size_t r = 0; r < 32; ++r) {
                rat.setMapping(t, r, mSlots[slot].rat_state[t][r]);
            }
        }
        return true;
    }

    // Release the checkpoint for the given branch fetch_seq (branch resolved correctly).
    void release(uint64_t branch_fetch_seq) {
        auto it = mSeqToSlot.find(branch_fetch_seq);
        if (it == mSeqToSlot.end()) return;

        uint32_t slot = it->second;
        mSlots[slot].valid = false;
        mSeqToSlot.erase(it);

        // Remove from allocation order
        auto order_it = std::find(mAllocationOrder.begin(), mAllocationOrder.end(), branch_fetch_seq);
        if (order_it != mAllocationOrder.end()) {
            mAllocationOrder.erase(order_it);
        }

        --mNumActive;
    }

    // Release all checkpoints for branches younger than the given fetch_seq.
    // (Used after misprediction to clean up wrong-path branch checkpoints.)
    void releaseYoungerThan(uint64_t branch_fetch_seq) {
        std::vector<uint64_t> to_release;
        for (const auto& seq : mAllocationOrder) {
            if (seq > branch_fetch_seq) to_release.push_back(seq);
        }

        // Release them
        for (uint64_t seq : to_release) {
            release(seq);
        }
    }

    // Check if a checkpoint exists for the given branch fetch_seq.
    bool hasCheckpoint(uint64_t branch_fetch_seq) const { return mSeqToSlot.find(branch_fetch_seq) != mSeqToSlot.end(); }

    uint32_t numActive() const { return mNumActive; }
    uint32_t capacity() const { return mCapacity; }

   private:
    uint32_t mCapacity;
    uint32_t mNumActive{0};

    std::vector<BranchCheckpoint> mSlots;
    std::unordered_map<uint64_t, uint32_t> mSeqToSlot;  // branch_fetch_seq -> slot index

    // Ordered by allocation for efficient trimming
    std::vector<uint64_t> mAllocationOrder;
};

}  // namespace midcore
