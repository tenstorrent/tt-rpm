// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Tenstorrent USA, Inc.

#pragma once

#include <cstdint>
#include <functional>

namespace memory {

// Main memory as seen by the last private cache: whole-line reads and writes, with
// back-pressure. A different memory model replaces the DRAM by implementing this.
class MemoryBackend {
   public:
    using ReadDoneCb = std::function<void(uint64_t line_addr)>;

    virtual ~MemoryBackend() = default;

    // False while the queue this line maps to is full; the caller retries later.
    virtual bool canAccept(uint64_t line_addr, bool is_write) const = 0;
    // Reads complete through the read-done callback; writes need no reply.
    virtual void access(uint64_t line_addr, bool is_write) = 0;
    virtual void setReadDoneCallback(ReadDoneCb cb) = 0;
    // Advance one core cycle.
    virtual void tick() = 0;
};

}  // namespace memory
