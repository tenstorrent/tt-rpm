// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Tenstorrent USA, Inc.

#pragma once

// How a command reaches the device. The device body
// never learns which front-end drove it, so swapping one for another is a configuration
// change rather than a device rewrite.
//
// Implementations: RingFrontEnd (MMIO doorbell + in-memory descriptor ring, loose); an
// instruction front-end intercepting a custom opcode in Execute (tight) is the planned peer.
namespace accel {

class CommandPort;

class CommandSourceIf {
   public:
    virtual ~CommandSourceIf() = default;

    // Bind to the device's ingress. Called once, before start().
    virtual void attach(CommandPort* port) = 0;

    // Per-cycle driver, called by the owning device. A source with nothing pending is a
    // no-op; a source that stalled on credits retries here.
    virtual void tick() {}
};

}  // namespace accel
