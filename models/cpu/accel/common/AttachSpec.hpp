// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Tenstorrent USA, Inc.

#pragma once

#include <cstdint>

namespace sparta {
class Clock;
}

// How a device attaches to the shared cluster cache as its own requester: its clock domain and
// its distance from the fabric. Resolved by the Cluster at build/bind time.
namespace accel {

struct AttachSpec {
    // Clock domain for the device and its bridge. Null inherits the fabric clock. The owner
    // (ClusterSim) makes the clock so the ClockManager keeps ownership.
    const sparta::Clock* clock{nullptr};

    // Extra delay each way between the device and the fabric -- the distance term that
    // separates an on-die peer from a PCIe/CXL device.
    uint32_t link_latency_cycles{0};
};

}  // namespace accel
