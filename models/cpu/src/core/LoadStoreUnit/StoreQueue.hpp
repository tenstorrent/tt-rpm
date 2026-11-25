// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Tenstorrent USA, Inc.

#pragma once

#include <cstdint>
#include <list>
#include <unordered_map>
#include <vector>

#include "Common/PipelinePacket.hpp"
#include "MemoryHierarchy/Cache.hpp"

namespace midcore {

// Bytes [addr, addr + size) touched by one element of a memory access.
struct MemRange {
    cpu::address_t addr{0};
    uint32_t size{0};
};

inline bool overlaps(const std::vector<MemRange>& a, const std::vector<MemRange>& b) {
    for (const auto& x : a)
        for (const auto& y : b)
            if (x.addr < y.addr + y.size && y.addr < x.addr + x.size) return true;
    return false;
}

// ─────────────────────────────────────────────────────────────────────────────
// StoreQueue — non-Sparta helper class for the LSU.
//
// Holds in-flight store instructions. Stores are committed in-order when the
// ROB retires them, then drained to the D-cache store buffer.
//
// Fences and atomics also take an SQ entry, so they are ordered with the stores.
// ─────────────────────────────────────────────────────────────────────────────

class StoreQueue {
   public:
    struct Entry {
        core::IssuePacket ipkt;
        cpu::address_t address{0};
        uint8_t access_size{4};
        std::vector<cpu::address_t> lines;  // cache lines touched, requested in order
        std::vector<MemRange> ranges{};     // bytes written; empty for a fence or LR
        uint32_t next_line{0};
        bool address_known{false};
        bool committed{false};      // ROB has retired this store
        bool sent_to_cache{false};  // Drained to D-cache store buffer (fence/atomic: done)
        uint64_t data_ready_cycle{0};
        uint64_t pred_seq{UINT64_MAX};  // store-set predecessor that must resolve first
        bool is_fence{false};
        bool is_atomic{false};
        bool acquire{false};            // atomic with aq set: orders younger loads
        uint32_t responses_pending{0};  // atomic D-cache access in flight
        uint64_t req_id{0};
        bool request_sent{false};
    };

    explicit StoreQueue(uint32_t capacity)
        : mCapacity(capacity) {}

    bool isFull() const { return mEntries.size() >= mCapacity; }
    bool empty() const { return mEntries.empty(); }
    size_t size() const { return mEntries.size(); }
    uint32_t capacity() const { return mCapacity; }

    // Enqueue a new store
    Entry& enqueue(const core::IssuePacket& ipkt, cpu::address_t addr, uint8_t size, std::vector<cpu::address_t> lines) {
        mEntries.emplace_back(Entry{.ipkt = ipkt, .address = addr, .access_size = size, .lines = std::move(lines), .address_known = true});
        mTagIndex[ipkt.pkt.tag] = std::prev(mEntries.end());
        return mEntries.back();
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

    // Youngest store older than load_tag that writes any of the given bytes, or nullptr.
    const Entry* youngestOverlappingOlderThan(const std::vector<MemRange>& ranges, uint64_t load_tag) const {
        const Entry* best = nullptr;
        for (const auto& e : mEntries) {
            if (e.ipkt.pkt.tag >= load_tag || (best && e.ipkt.pkt.tag < best->ipkt.pkt.tag)) continue;
            if (overlaps(e.ranges, ranges)) best = &e;
        }
        return best;
    }

    // ── Commit / Drain ───────────────────────────────────────────────────────

    // Mark a store as committed (called when ROB retires it)
    void markCommitted(uint64_t tag) {
        if (auto* e = find(tag)) {
            e->committed = true;
        }
    }

    // Mark a store as sent to cache
    void markSentToCache(uint64_t tag) {
        if (auto* e = find(tag)) {
            e->sent_to_cache = true;
        }
    }

    // Get the next committed-but-not-sent store for draining to D-cache
    Entry* nextDrainable() {
        for (auto& e : mEntries) {
            if (e.committed && !e.sent_to_cache) return &e;
        }
        return nullptr;
    }

    // Access front for in-order drain
    Entry& front() { return mEntries.front(); }
    const Entry& front() const { return mEntries.front(); }

    // Pop front entry (only when fully drained)
    void popFront() {
        if (!mEntries.empty()) {
            mTagIndex.erase(mEntries.front().ipkt.pkt.tag);
            mEntries.pop_front();
        }
    }

    // Check if front entry can be removed (committed and sent to cache)
    bool canPopFront() const {
        if (mEntries.empty()) return false;
        const auto& e = mEntries.front();
        return e.committed && e.sent_to_cache;
    }

    // Check if entry with given tag is fully drained (committed and sent to cache)
    bool canRemove(uint64_t tag) const {
        if (const auto* e = find(tag)) {
            return e->committed && e->sent_to_cache;
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
