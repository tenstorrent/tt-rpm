// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Tenstorrent USA, Inc.

#pragma once

#include <cstdint>
#include <list>
#include <unordered_map>

#include "Common/PipelinePacket.hpp"
#include "MemoryHierarchy/Cache.hpp"

namespace midcore {

// ─────────────────────────────────────────────────────────────────────────────
// StoreQueue — non-Sparta helper class for the LSU.
//
// Holds in-flight store instructions. Stores are committed in-order when the
// ROB retires them, then drained to the D-cache store buffer.
//
// Supports store-to-load forwarding: loads can query the SQ for recent stores
// that fully cover the load's address range.
// ─────────────────────────────────────────────────────────────────────────────

class StoreQueue {
   public:
    struct Entry {
        core::IssuePacket ipkt;
        cpu::address_t address{0};
        uint8_t access_size{4};
        bool address_known{false};
        bool committed{false};      // ROB has retired this store
        bool sent_to_cache{false};  // Drained to D-cache store buffer
    };

    explicit StoreQueue(uint32_t capacity)
        : mCapacity(capacity) {}

    bool isFull() const { return mEntries.size() >= mCapacity; }
    bool empty() const { return mEntries.empty(); }
    size_t size() const { return mEntries.size(); }
    uint32_t capacity() const { return mCapacity; }

    // Enqueue a new store
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

    // ── Store-to-Load Forwarding ─────────────────────────────────────────────
    //
    // Search backwards (most recent store first) for a store that fully covers
    // the load address range [addr, addr + size).
    //
    // Returns pointer to the forwarding store entry, or nullptr if no match.
    //
    // Note: Only searches stores *older than* the load (by tag) would require
    // the load's tag as a parameter. For simplicity, we search all entries —
    // the LSU ensures loads only check stores that are older.

    const Entry* findForwarding(cpu::address_t addr, uint8_t size) const {
        // Walk backwards (most recent store first)
        for (auto it = mEntries.rbegin(); it != mEntries.rend(); ++it) {
            if (!it->address_known) continue;
            // Full coverage check: store range must contain load range
            cpu::address_t st_end = it->address + it->access_size;
            cpu::address_t ld_end = addr + size;
            if (it->address <= addr && st_end >= ld_end) {
                return &(*it);
            }
        }
        return nullptr;
    }

    // Find forwarding store that is older than a given tag
    const Entry* findForwardingOlderThan(cpu::address_t addr, uint8_t size, uint64_t load_tag) const {
        for (auto it = mEntries.rbegin(); it != mEntries.rend(); ++it) {
            // Only consider stores older than the load (lower tag = older)
            if (it->ipkt.pkt.tag >= load_tag) continue;
            if (!it->address_known) continue;
            cpu::address_t st_end = it->address + it->access_size;
            cpu::address_t ld_end = addr + size;
            if (it->address <= addr && st_end >= ld_end) {
                return &(*it);
            }
        }
        return nullptr;
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
