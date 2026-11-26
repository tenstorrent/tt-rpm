// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Tenstorrent USA, Inc.

#pragma once

#include <cstdint>

#include "models/cpu/accel/common/AcceleratorTypes.hpp"

// A device's command ingress, as seen by whatever front-end drives it. The queue behind it
// is finite: a source must check canAccept() before enqueue(), so a saturated device applies
// backpressure to its source instead of growing an unbounded backlog. Real hardware delivers
// exactly this signal -- Intel's ENQCMD reports queue-full back to the issuing instruction --
// and an in-pipeline front-end needs it to stall issue.
namespace accel {

class CommandPort {
   public:
    virtual ~CommandPort() = default;

    // Room for at least one more command right now.
    virtual bool canAccept() const = 0;

    // Hand a command to the device. Precondition: canAccept().
    virtual void enqueue(const AccelCommand&) = 0;
};

}  // namespace accel
