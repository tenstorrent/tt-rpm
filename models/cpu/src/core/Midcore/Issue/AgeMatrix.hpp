// SPDX-FileCopyrightText: © 2026 Tenstorrent AI ULC
//
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <bitset>
#include <cstdint>
#include <vector>

namespace midcore {

// Maximum supported issue queue size (compile-time constant for bitset)
static constexpr uint32_t kMaxIQSlots = 64;
static constexpr uint32_t kInvalidSlot = UINT32_MAX;

// ============================================================================
// AgeMatrix — Hardware-accurate age tracking for issue queue selection
// ============================================================================
// An NxN bit matrix where age[i][j] = 1 means slot i is OLDER than slot j.
// This enables O(1) age comparison and parallel oldest-ready selection.
//
// Operations:
//   allocate(slot)   - Mark slot as youngest (newer than all existing)
//   deallocate(slot) - Remove slot from tracking
//   selectOldest(ready_mask) - Find oldest slot that's ready
//   isOlder(a, b)    - Query: is slot a older than slot b?
//
class AgeMatrix {
   public:
    explicit AgeMatrix(uint32_t capacity)
        : mCapacity(capacity),
          mMatrix(capacity) {
        // Initialize all bits to 0
        for (auto& row : mMatrix) {
            row.reset();
        }
        mValid.reset();
    }

    // Allocate a new entry at the given slot (marks it as youngest)
    void allocate(uint32_t slot) {
        if (slot >= mCapacity) return;

        // New entry is younger than all existing valid entries:
        // - Set row[slot] to all 0s (slot is not older than anyone)
        // - Set column[slot] to 1 for all valid entries (everyone else is older)
        mMatrix[slot].reset();

        for (uint32_t i = 0; i < mCapacity; ++i) {
            if (mValid.test(i)) {
                mMatrix[i].set(slot);  // Entry i is older than new slot
            }
        }

        mValid.set(slot);
    }

    // Deallocate an entry (remove from age tracking)
    void deallocate(uint32_t slot) {
        if (slot >= mCapacity) return;

        mValid.reset(slot);
        mMatrix[slot].reset();

        // Clear column: no one is older/younger than a deallocated slot
        for (uint32_t i = 0; i < mCapacity; ++i) {
            mMatrix[i].reset(slot);
        }
    }

    // Find the oldest slot among those set in ready_mask
    // Returns kInvalidSlot if no ready slots exist
    uint32_t selectOldest(const std::bitset<kMaxIQSlots>& ready_mask) const {
        // A slot is "oldest among ready" if:
        // For all other ready slots j: age[slot][j] = 1 (slot is older than j)
        // Equivalently: (age[slot] & ready_mask) == (ready_mask - {slot})

        std::bitset<kMaxIQSlots> candidates = ready_mask & mValid;
        if (candidates.none()) {
            return kInvalidSlot;
        }

        for (uint32_t slot = 0; slot < mCapacity; ++slot) {
            if (!candidates.test(slot)) continue;

            // Check if this slot is older than all other candidates
            std::bitset<kMaxIQSlots> others = candidates;
            others.reset(slot);

            // slot is oldest if mMatrix[slot] has 1s for all other candidates
            if ((mMatrix[slot] & others) == others) {
                return slot;
            }
        }

        // Fallback: return first ready slot (shouldn't happen with correct matrix)
        for (uint32_t slot = 0; slot < mCapacity; ++slot) {
            if (candidates.test(slot)) return slot;
        }

        return kInvalidSlot;
    }

    // Query: is slot a older than slot b?
    bool isOlder(uint32_t a, uint32_t b) const {
        if (a >= mCapacity || b >= mCapacity) return false;
        return mMatrix[a].test(b);
    }

    // Check if a slot is currently allocated
    bool isValid(uint32_t slot) const {
        if (slot >= mCapacity) return false;
        return mValid.test(slot);
    }

    // Get count of valid entries
    uint32_t count() const { return static_cast<uint32_t>(mValid.count()); }

    // Get the valid mask (for building ready masks)
    const std::bitset<kMaxIQSlots>& validMask() const { return mValid; }

    // Find a free slot, returns kInvalidSlot if full
    uint32_t findFreeSlot() const {
        for (uint32_t i = 0; i < mCapacity; ++i) {
            if (!mValid.test(i)) return i;
        }
        return kInvalidSlot;
    }

    uint32_t capacity() const { return mCapacity; }

   private:
    uint32_t mCapacity;
    std::vector<std::bitset<kMaxIQSlots>> mMatrix;  // mMatrix[i][j] = 1 → i older than j
    std::bitset<kMaxIQSlots> mValid;                // Which slots are occupied
};

}  // namespace midcore
