#include "Common/WritebackBuffer.hpp"

#include <algorithm>

namespace midcore {

void WritebackBuffer::configure(const Config& cfg) {
    mCapacity = cfg.capacity;
    mDrainWidth = cfg.drain_width;
    mWritebackLatency = cfg.writeback_latency;
}

bool WritebackBuffer::accept(uint64_t tag, uint64_t fetch_seq, const std::vector<core::PhysRegRef>& phys_dsts, const core::ROBToken& rob_token,
                             uint8_t source_id) {
    if (mEntries.size() >= mCapacity) {
        ++mNumStalls;
        return false;
    }

    Entry e;
    e.tag = tag;
    e.fetch_seq = fetch_seq;
    e.phys_dsts = phys_dsts;
    e.rob_token = rob_token;
    e.source_id = source_id;
    e.cycles_remaining = mWritebackLatency;
    e.ready = (mWritebackLatency == 0);

    mEntries.push_back(std::move(e));
    ++mNumAccepted;
    return true;
}

void WritebackBuffer::tick() {
    for (auto& e : mEntries) {
        if (!e.ready && e.cycles_remaining > 0) {
            --e.cycles_remaining;
            if (e.cycles_remaining == 0) {
                e.ready = true;
            }
        }
    }
}

std::vector<WritebackBuffer::Entry*> WritebackBuffer::getReadyEntries() {
    std::vector<Entry*> ready;
    ready.reserve(mDrainWidth);

    for (auto& e : mEntries) {
        if (e.ready && ready.size() < mDrainWidth) {
            ready.push_back(&e);
        }
    }

    return ready;
}

void WritebackBuffer::remove(uint64_t tag) {
    auto it = std::find_if(mEntries.begin(), mEntries.end(), [tag](const Entry& e) { return e.tag == tag; });
    if (it != mEntries.end()) {
        mEntries.erase(it);
        ++mNumDrained;
    }
}

}  // namespace midcore
