// SPDX-FileCopyrightText: © 2026 Tenstorrent AI ULC
//
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>
#include <list>
#include <unordered_map>

#include "Common/PipelinePacket.hpp"
#include "MemoryHierarchy/Cache.hpp"

namespace midcore {

// ─────────────────────────────────────────────────────────────────────────────
// LoadQueue — non-Sparta helper class for the LSU.
//
// Holds in-flight load instructions waiting for D-cache responses.
// Supports store-to-load forwarding queries and write-port arbitration.
// ─────────────────────────────────────────────────────────────────────────────

class LoadQueue {
   public:
    struct Entry {
        core::IssuePacket ipkt;
        cpu::address_t address{0};
        uint8_t access_size{4};
        bool address_known{false};      // EA computed
        bool request_sent{false};       // sent to D-cache
        bool response_received{false};  // D-cache responded
        bool forwarded{false};          // satisfied by store buffer/SQ
        bool written_back{false};       // won write port
        uint32_t forward_cycles{0};     // countdown for forwarding latency
    };

    explicit LoadQueue(uint32_t capacity)
        : mCapacity(capacity) {}

    bool isFull() const { return mEntries.size() >= mCapacity; }
    bool empty() const { return mEntries.empty(); }
    size_t size() const { return mEntries.size(); }
    uint32_t capacity() const { return mCapacity; }

    // Enqueue a new load
    void enqueue(const core::IssuePacket& ipkt, cpu::address_t addr, uint8_t size) {
        mEntries.emplace_back(Entry{.ipkt = ipkt, .address = addr, .access_size = size, .address_known = true});
        mTagIndex[ipkt.pkt.tag] = std::prev(mEntries.end());
    }

    // Find entry by tag (O(1) via tag index)
    Entry* find(uint64_t tag) {
        auto it = mTagIndex.find(tag);
        if (it != mTagIndex.end()) return &(*it->second);
        return nullptr;
    }

    const Entry* find(uint64_t tag) const {
        auto it = mTagIndex.find(tag);
        if (it != mTagIndex.end()) return &(*it->second);
        return nullptr;
    }

    // Mark a load as having received its D-cache response
    void markResponseReceived(uint64_t tag) {
        if (auto* e = find(tag)) {
            e->response_received = true;
        }
    }

    // Mark a load as forwarded (satisfied from SQ/store buffer)
    void markForwarded(uint64_t tag, uint32_t forward_latency) {
        if (auto* e = find(tag)) {
            e->forwarded = true;
            e->forward_cycles = forward_latency;
        }
    }

    // Mark a load as having won the write port
    void markWrittenBack(uint64_t tag) {
        if (auto* e = find(tag)) {
            e->written_back = true;
        }
    }

    // Decrement forwarding countdown
    void tickForwardCountdowns() {
        for (auto& e : mEntries) {
            if (e.forwarded && e.forward_cycles > 0) {
                --e.forward_cycles;
            }
        }
    }

    // Check if a forwarded load is ready (countdown done)
    bool isForwardReady(uint64_t tag) const {
        if (const auto* e = find(tag)) {
            return e->forwarded && e->forward_cycles == 0;
        }
        return false;
    }

    // Access front for in-order drain
    Entry& front() { return mEntries.front(); }
    const Entry& front() const { return mEntries.front(); }

    // Pop front entry
    void popFront() {
        if (!mEntries.empty()) {
            mTagIndex.erase(mEntries.front().ipkt.pkt.tag);
            mEntries.pop_front();
        }
    }

    // Check if entry with given tag is ready to remove (response received and written back)
    bool canRemove(uint64_t tag) const {
        if (const auto* e = find(tag)) {
            return (e->response_received || e->forwarded) && e->written_back;
        }
        return false;
    }

    // Remove ALL entries with given tag (handles duplicate tags from tag reuse)
    // Returns true if any entries were removed
    bool removeByTag(uint64_t tag) {
        bool removed_any = false;
        // Remove from index first
        mTagIndex.erase(tag);
        // Remove ALL matching entries from the list (there might be duplicates due to tag reuse)
        for (auto it = mEntries.begin(); it != mEntries.end();) {
            if (it->ipkt.pkt.tag == tag) {
                it = mEntries.erase(it);
                removed_any = true;
            } else {
                ++it;
            }
        }
        return removed_any;
    }

    // Iterators for scanning
    auto begin() { return mEntries.begin(); }
    auto end() { return mEntries.end(); }
    auto begin() const { return mEntries.begin(); }
    auto end() const { return mEntries.end(); }

   private:
    std::list<Entry> mEntries;
    std::unordered_map<uint64_t, typename std::list<Entry>::iterator> mTagIndex;
    uint32_t mCapacity;
};

}  // namespace midcore
