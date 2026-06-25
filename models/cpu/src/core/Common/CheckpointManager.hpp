#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <vector>

namespace midcore {

// Manages RAT checkpoints for branch misprediction recovery.
// Each branch instruction gets a checkpoint at rename time, enabling
// recovery from ANY misprediction regardless of speculation depth.
class CheckpointManager {
   public:
    static constexpr size_t kNumRegTypes = 3;  // Int, Fp, Vec

    struct BranchCheckpoint {
        uint64_t branch_tag{0};
        bool valid{false};
        std::array<std::array<uint32_t, 32>, kNumRegTypes> rat_state;
    };

    explicit CheckpointManager(uint32_t capacity)
        : mCapacity(capacity) {
        mSlots.resize(capacity);
        mAllocationOrder.reserve(capacity);
    }

    bool canAllocate() const { return mNumActive < mCapacity; }

    // Allocate a checkpoint slot for the given branch tag.
    // Returns the slot index, or nullopt if no slots available.
    std::optional<uint32_t> allocate(uint64_t branch_tag) {
        if (!canAllocate()) return std::nullopt;

        // Find first free slot
        for (uint32_t i = 0; i < mCapacity; ++i) {
            if (!mSlots[i].valid) {
                mSlots[i].valid = true;
                mSlots[i].branch_tag = branch_tag;
                mTagToSlot[branch_tag] = i;
                mAllocationOrder.push_back(branch_tag);
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

    // Restore RAT state from the checkpoint for the given branch tag.
    // RATAccessor must provide: void setMapping(size_t type_idx, size_t arch_reg, uint32_t phys_reg)
    // Returns true if successful, false if no checkpoint found.
    template <typename RATAccessor>
    bool restore(uint64_t branch_tag, RATAccessor& rat) {
        auto it = mTagToSlot.find(branch_tag);
        if (it == mTagToSlot.end()) return false;

        uint32_t slot = it->second;
        if (!mSlots[slot].valid) return false;

        for (size_t t = 0; t < kNumRegTypes; ++t) {
            for (size_t r = 0; r < 32; ++r) {
                rat.setMapping(t, r, mSlots[slot].rat_state[t][r]);
            }
        }
        return true;
    }

    // Release the checkpoint for the given branch tag (branch retired successfully).
    void release(uint64_t branch_tag) {
        auto it = mTagToSlot.find(branch_tag);
        if (it == mTagToSlot.end()) return;

        uint32_t slot = it->second;
        mSlots[slot].valid = false;
        mTagToSlot.erase(it);

        // Remove from allocation order
        auto order_it = std::find(mAllocationOrder.begin(), mAllocationOrder.end(), branch_tag);
        if (order_it != mAllocationOrder.end()) {
            mAllocationOrder.erase(order_it);
        }

        --mNumActive;
    }

    // Release all checkpoints for branches younger than the given tag.
    // (Used after misprediction to clean up wrong-path branch checkpoints.)
    void releaseYoungerThan(uint64_t branch_tag) {
        std::vector<uint64_t> to_release;
        for (const auto& tag : mAllocationOrder) {
            if (tag > branch_tag) to_release.push_back(tag);
        }

        // Release them
        for (uint64_t tag : to_release) {
            release(tag);
        }
    }

    // Check if a checkpoint exists for the given branch tag.
    bool hasCheckpoint(uint64_t branch_tag) const { return mTagToSlot.find(branch_tag) != mTagToSlot.end(); }

    uint32_t numActive() const { return mNumActive; }
    uint32_t capacity() const { return mCapacity; }

   private:
    uint32_t mCapacity;
    uint32_t mNumActive{0};

    std::vector<BranchCheckpoint> mSlots;
    std::unordered_map<uint64_t, uint32_t> mTagToSlot;  // branch_tag -> slot index

    // Ordered by allocation for efficient trimming
    std::vector<uint64_t> mAllocationOrder;
};

}  // namespace midcore
