// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Tenstorrent USA, Inc.

#pragma once

#include <cstdint>
#include <list>
#include <unordered_map>
#include <vector>

#include "Common/PipelinePacket.hpp"
#include "LoadStoreUnit/StoreQueue.hpp"
#include "MemoryHierarchy/Cache.hpp"

namespace midcore {

// ─────────────────────────────────────────────────────────────────────────────
// LoadQueue — non-Sparta helper class for the LSU.
//
// Holds load instructions from execute to retire, so a store whose address
// resolves late can find younger loads that already read stale data.
// ─────────────────────────────────────────────────────────────────────────────

class LoadQueue {
   public:
    struct Entry {
        core::IssuePacket ipkt;
        cpu::address_t address{0};
        uint8_t access_size{4};
        std::vector<cpu::address_t> lines;  // cache lines touched, requested in order
        std::vector<MemRange> ranges{};     // bytes read
        uint32_t next_line{0};
        uint32_t responses_pending{0};
        uint64_t req_id{0};             // shared by this load's D-cache accesses
        bool address_known{false};      // EA computed
        bool request_sent{false};       // sent to D-cache
        bool response_received{false};  // D-cache responded
        bool forwarded{false};          // satisfied by store buffer/SQ
        bool written_back{false};       // won write port
        uint32_t forward_cycles{0};     // countdown for forwarding latency
        uint64_t fwd_store_tag{0};      // store-queue entry forwarded from
        bool fwd_from_sq{false};
        uint64_t dep_store_seq{UINT64_MAX};  // store the store-set predictor makes it wait for
        bool committed{false};               // ROB has retired this load
        bool violation{false};               // read before an older overlapping store's address was known
        uint8_t waits_counted{0};            // LSQ::WaitReason bits already counted for this load
    };

    explicit LoadQueue(uint32_t capacity)
        : mCapacity(capacity) {}

    bool isFull() const { return mEntries.size() >= mCapacity; }
    bool empty() const { return mEntries.empty(); }
    size_t size() const { return mEntries.size(); }
    uint32_t capacity() const { return mCapacity; }

    // Enqueue a new load
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

    // Check if entry with given tag is ready to remove (written back and retired)
    bool canRemove(uint64_t tag) const {
        if (const auto* e = find(tag)) {
            return (e->response_received || e->forwarded) && e->written_back && e->committed;
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
