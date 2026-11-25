// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Tenstorrent USA, Inc.

#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

#include "models/cpu/common/BaseTypes.hpp"

namespace midcore {

// ─────────────────────────────────────────────────────────────────────────────
// StoreSetPredictor — memory-dependence predictor of Chrysos and Emer, "Memory
// dependence prediction using store sets", ISCA 1998.
//
// The SSIT maps a load or store PC to a store-set ID; the LFST holds, per set,
// the last fetched store not yet issued. Instructions are named by fetch_seq.
// ─────────────────────────────────────────────────────────────────────────────

class StoreSetPredictor {
   public:
    static constexpr uint64_t kNone = UINT64_MAX;

    StoreSetPredictor(uint32_t ssit_entries, uint32_t lfst_entries)
        : mSsit(ssit_entries, kInvalid),
          mLfst(lfst_entries, kNone) {}

    // Dispatch of a load: the store it must wait for, or kNone.
    uint64_t dispatchLoad(cpu::address_t pc) const {
        const uint32_t id = mSsit[ssitIndex(pc)];
        return id == kInvalid ? kNone : mLfst[id];
    }

    // Dispatch of a store: becomes its set's last fetched store. Returns the previous one,
    // which it must not overtake, or kNone.
    uint64_t dispatchStore(cpu::address_t pc, uint64_t seq) {
        const uint32_t id = mSsit[ssitIndex(pc)];
        if (id == kInvalid) return kNone;
        const uint64_t prev = mLfst[id];
        mLfst[id] = seq;
        return prev;
    }

    // Store issue (address known): later loads of the set no longer wait for it.
    void issueStore(cpu::address_t pc, uint64_t seq) {
        const uint32_t id = mSsit[ssitIndex(pc)];
        if (id != kInvalid && mLfst[id] == seq) mLfst[id] = kNone;
    }

    // Ordering violation between a load and an older store: put both in one set.
    void train(cpu::address_t load_pc, cpu::address_t store_pc) {
        uint32_t& ld = mSsit[ssitIndex(load_pc)];
        uint32_t& st = mSsit[ssitIndex(store_pc)];
        if (ld == kInvalid && st == kInvalid) {
            ld = st = static_cast<uint32_t>((load_pc >> 1) % mLfst.size());
        } else if (st == kInvalid) {
            st = ld;
        } else if (ld == kInvalid) {
            ld = st;
        } else {
            ld = st = std::min(ld, st);  // merge: the smaller set ID wins
        }
    }

    // Cyclic clearing, which the paper uses to drop false dependences that accumulate.
    void clear() { std::fill(mSsit.begin(), mSsit.end(), kInvalid); }

   private:
    static constexpr uint32_t kInvalid = UINT32_MAX;

    size_t ssitIndex(cpu::address_t pc) const { return (pc >> 1) % mSsit.size(); }

    std::vector<uint32_t> mSsit;  // PC -> store-set ID
    std::vector<uint64_t> mLfst;  // store-set ID -> fetch_seq of its last fetched, unissued store
};

}  // namespace midcore
