// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Tenstorrent USA, Inc.

#pragma once

#include <cstdint>
#include <functional>
#include <utility>

#include "models/cpu/accel/common/CommandPort.hpp"
#include "models/cpu/accel/common/CommandSourceIf.hpp"
#include "models/cpu/common/MemInterface.hpp"

// Loose-coupling command front-end: MMIO doorbell + in-memory descriptor ring. The core
// writes descriptors into a guest-memory ring, then stores a new tail index to the device's
// BAR doorbell register (an uncacheable MMIO store, peeled to this front-end by the
// cluster's MmioRouter). The mem::Request carries no data payload, so the tail value is
// peeked out of the oracle (Whisper) memory where the store retired. Descriptors are read
// the same way — ring-fetch traffic is not modeled; the engine's copy requests are the
// interesting traffic.
//
// The doorbell only advances the tail; descriptors are handed over as the device has credit
// for them, so a saturated device leaves the remainder in the ring and the walk resumes on a
// later tick. That is the ring's own backpressure, and it is why this is a CommandSourceIf
// with a tick rather than a pure callback.
//
// Descriptor layout (32 B, little-endian):
//   [0]  u64 opcode
//   [8]  u64 src  -> AccelCommand::arg0
//   [16] u64 dst  -> AccelCommand::arg1
//   [24] u64 len  -> AccelCommand::arg2
namespace accel {

class RingFrontEnd : public CommandSourceIf {
   public:
    using PeekFn = std::function<void(uint64_t addr, uint8_t* buf, uint32_t len)>;

    RingFrontEnd(CommandPort* port, uint64_t ring_base, uint32_t ring_slots, uint64_t doorbell_addr, PeekFn peek)
        : mPort(port),
          mRingBase(ring_base),
          mRingSlots(ring_slots),
          mDoorbell(doorbell_addr),
          mPeek(std::move(peek)) {}

    void attach(CommandPort* port) override { mPort = port; }

    // MmioRouter handler: publish the new tail, then hand over what the device will take.
    void onDoorbell(const mem::Request&) {
        uint64_t tail = 0;
        mPeek(mDoorbell, reinterpret_cast<uint8_t*>(&tail), sizeof(tail));
        mTail = tail;
        pump_();
    }

    void tick() override { pump_(); }

    uint64_t head() const { return mHead; }
    uint64_t pending() const { return mTail - mHead; }

   private:
    static constexpr uint32_t kDescSize = 32;

    void pump_() {
        while (mHead < mTail && mPort && mPort->canAccept()) {
            uint64_t d[4] = {0, 0, 0, 0};
            mPeek(mRingBase + (mHead % mRingSlots) * kDescSize, reinterpret_cast<uint8_t*>(d), kDescSize);
            AccelCommand cmd;
            cmd.opcode = static_cast<uint32_t>(d[0]);
            cmd.arg0 = d[1];
            cmd.arg1 = d[2];
            cmd.arg2 = d[3];
            cmd.context = 0;  // PA-mode descriptors (loose coupling)
            cmd.tag = mHead;  // descriptor sequence number (unwrapped ring head)
            mPort->enqueue(cmd);
            ++mHead;
        }
    }

    CommandPort* mPort;
    uint64_t mRingBase;
    uint32_t mRingSlots;
    uint64_t mDoorbell;
    PeekFn mPeek;
    uint64_t mHead{0};
    uint64_t mTail{0};
};

}  // namespace accel
