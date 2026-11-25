// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Tenstorrent USA, Inc.

#pragma once

#include <cstdint>
#include <functional>
#include <vector>

#include "models/cpu/common/MemInterface.hpp"

namespace cluster {

// MMIO decode in front of the shared cache's tag path. MMIO is register-granular and
// non-cacheable: a request whose line falls in a registered BAR range is forwarded to the
// device's handler and acked immediately — it must never allocate a line, enter the
// replacement path, or touch the coherence directory. The mem::Request carries no data
// payload, so the device reads the doorbell *value* out of the oracle (Whisper) memory: the
// core's store retired there normally before this request arrived.
class MmioRouter {
   public:
    using MmioHandler = std::function<void(const mem::Request&)>;

    void addRegion(uint64_t base, uint64_t size, MmioHandler handler) { mRegions.push_back({base, size, std::move(handler)}); }

    bool isMmio(uint64_t addr) const { return find(addr) != nullptr; }

    // Fire the region's handler. Precondition: isMmio(addr).
    void notify(const mem::Request& req) const {
        if (const Region* r = find(req.line_addr)) r->handler(req);
    }

   private:
    struct Region {
        uint64_t base;
        uint64_t size;
        MmioHandler handler;
    };

    const Region* find(uint64_t addr) const {
        for (const auto& r : mRegions)
            if (addr >= r.base && addr < r.base + r.size) return &r;
        return nullptr;
    }

    std::vector<Region> mRegions;
};

}  // namespace cluster
