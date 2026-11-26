// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Tenstorrent USA, Inc.

#pragma once

#include <memory>

namespace predictors {

class BPCheckpointBase {
   public:
    virtual ~BPCheckpointBase() = default;
};

// A structure's speculative state. History structures (direction, indirect) snapshot before their
// speculativeUpdate, target structures (RAS) after their push/pop; a restore never undoes training.
class BPCheckpoint {
   public:
    BPCheckpoint() = default;
    explicit BPCheckpoint(std::unique_ptr<BPCheckpointBase> state)
        : mState(std::move(state)) {}

    BPCheckpoint(const BPCheckpoint&) = default;
    BPCheckpoint& operator=(const BPCheckpoint&) = default;
    BPCheckpoint(BPCheckpoint&&) = default;
    BPCheckpoint& operator=(BPCheckpoint&&) = default;

    bool valid() const { return mState != nullptr; }
    const BPCheckpointBase* get() const { return mState.get(); }

    template <typename T>
    const T* as() const {
        return dynamic_cast<const T*>(mState.get());
    }

   private:
    std::shared_ptr<const BPCheckpointBase> mState;
};

}  // namespace predictors
