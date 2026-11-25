// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Tenstorrent USA, Inc.

#pragma once

#include <cstdint>
#include <functional>
#include <memory>

#include "models/cpu/accel/common/AcceleratorTypes.hpp"
#include "models/cpu/accel/common/CommandPort.hpp"
#include "models/cpu/common/MemInterface.hpp"

namespace sparta {
class TreeNode;
}

// The device body. Parallels cluster::CoreIf on
// purpose: an accelerator attaches to the fabric exactly the way a core does — one or more
// mem::MemMaster egress ports, each stamped with a globally-unique requester id assigned by
// the Cluster in bind(). Front-ends (tight: Execute intercepting a custom instruction; loose:
// an MMIO doorbell + command ring) push AccelCommands in; completions leave via
// tryDequeueCompletion() or a CompletionSink.
namespace accel {

class CommandSourceIf;
class CompletionSink;

class AcceleratorIf : public CommandPort {
   public:
    ~AcceleratorIf() override = default;

    // Lifecycle hook for post-construction setup (a device that builds fully in its
    // constructor implements this as a no-op).
    virtual void init(sparta::TreeNode* node) = 0;

    // Memory egress IS the existing mem interface -> plugs into ClusterCache unchanged.
    virtual uint32_t numMemMasters() const = 0;
    virtual mem::MemMaster& memMaster(uint32_t i) = 0;
    virtual void setRequesterId(uint32_t master, uint32_t id) = 0;

    // Command-source and completion-sink policy objects. Both are optional: a device with no source is driven
    // by direct enqueue() calls (harnesses), and one with no sink queues completions for
    // tryDequeueCompletion(). The device ticks its source, so a source stalled on credits
    // makes progress without separate scheduling.
    virtual void setCommandSource(CommandSourceIf*) {}
    virtual void setCompletionSink(CompletionSink*) {}

    // Completion source. The command ingress (canAccept/enqueue) is CommandPort.
    virtual bool tryDequeueCompletion(AccelCompletion&) = 0;

    // Per-cycle driver: either called by the owning front-end each cycle, or self-ticked by
    // the device for standalone use.
    virtual void tick() = 0;

    // Begin execution. Called once by the owning Cluster/CoreImpl, after the device tree has
    // been finalized (mirrors cluster::CoreIf::start).
    virtual void start() {}
};

// Factory mirroring cluster::CoreFactory.
using AcceleratorFactory = std::function<std::unique_ptr<AcceleratorIf>(sparta::TreeNode* parent, uint32_t index)>;

}  // namespace accel
