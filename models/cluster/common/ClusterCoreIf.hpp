// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Tenstorrent USA, Inc.

#pragma once

#include <cstdint>
#include <functional>
#include <memory>

#include "models/cpu/common/MemInterface.hpp"

namespace sparta {
class TreeNode;
class Clock;
}  // namespace sparta

namespace cpu {
class ExecutionDriver;
}

namespace cluster {

// ============================================================================
// Droppable-core contract.
//
// This is the *only* thing the cluster knows about a CPU core.  A user attaches a
// core of their choice by implementing this interface: build the core's units
// (pipeline + private L1/L2 + any protocol adapter) under the provided tree node,
// and expose the core's memory egress as one or more mem::MemMaster ports.
//
// The cluster owns no part of the core's private hierarchy.  Whether the core is
// in-order or OOO, L1-only or L1+L2, the cluster connects it identically.
// ============================================================================
class CoreIf {
   public:
    virtual ~CoreIf() = default;

    // High-level lifecycle entry point. The cluster calls this once to bring the
    // core up; the core internally sequences its own initialization steps (build
    // its units, set up any ISS/driver, wire its internal pipeline, switch its
    // private caches to external-fill, create its boundary adapters, etc.).
    // After init() returns, the core's memory master ports are valid.
    virtual void init() = 0;

    // Number of memory master ports this core exposes to the shared cache.
    // Examples: 1 for a unified memory egress, 2 for separate I-side / D-side.
    virtual uint32_t numMemMasters() const = 0;

    // The i-th memory master: this core's request/response egress to the cluster.
    virtual mem::MemMaster& memMaster(uint32_t i) = 0;

    // The core's clock domain (its cpu_freq clock, or the root clock if unset). Valid
    // after init(); reads the node clock directly (no resource lookup), so it is safe
    // to query at build time when wiring the clock-crossing bridge.
    virtual const sparta::Clock* clock() const = 0;

    // The cluster assigns a globally-unique requester id to each master and calls
    // this so the core stamps that id on the requests it issues through that
    // master.  The shared cache uses it to route responses / back-invalidations.
    virtual void setRequesterId(uint32_t master_idx, uint32_t requester_id) = 0;

    // Begin execution. Called once, after the device tree has been finalized.
    virtual void start() {}

    // The core's Whisper driver, if it has one — the oracle handle for fabric peers
    // (accelerators) that need to peek/poke architectural memory. Default: no driver.
    virtual cpu::ExecutionDriver* executionDriver() { return nullptr; }
};

// Factory used to drop a core into the cluster.  It creates one core (the core
// owns and names its own subtree under `parent`) and returns it.  `core_index`
// is the cluster-local core index in [0, num_cores).
using CoreFactory = std::function<std::unique_ptr<CoreIf>(sparta::TreeNode* parent, uint32_t core_index)>;

}  // namespace cluster
