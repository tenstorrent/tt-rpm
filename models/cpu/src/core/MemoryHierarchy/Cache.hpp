// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Tenstorrent USA, Inc.

#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <list>
#include <memory>
#include <string>
#include <vector>

#include "sparta/utils/MathUtils.hpp"
#include "sparta/utils/SpartaAssert.hpp"

#include "MemoryHierarchy/ReplacementRegistry.hpp"

namespace cpu {

using address_t = uint64_t;

// ── Replacement policy interface ──

class ReplacementIF {
   public:
    explicit ReplacementIF(uint32_t num_ways)
        : mNumWays(num_ways) {}
    virtual ~ReplacementIF() = default;
    virtual void reset() = 0;
    virtual void onAccess(uint32_t way) = 0;
    virtual void onAllocate(uint32_t way) = 0;
    virtual uint32_t getVictimWay() const = 0;

   protected:
    uint32_t mNumWays;
};

// ── True LRU ──

class LRUReplacement : public ReplacementIF {
   public:
    explicit LRUReplacement(uint32_t num_ways)
        : ReplacementIF(num_ways) {
        for (uint32_t i = 0; i < num_ways; ++i) mMap.emplace_back(mStack.emplace(mStack.end(), i));
    }

    void reset() override {
        mStack.clear();
        mMap.clear();
        for (uint32_t i = 0; i < mNumWays; ++i) mMap.emplace_back(mStack.emplace(mStack.end(), i));
    }

    void onAccess(uint32_t way) override { touchMRU(way); }
    void onAllocate(uint32_t way) override { touchMRU(way); }

    uint32_t getVictimWay() const override { return mStack.front(); }

   private:
    void touchMRU(uint32_t way) { mStack.splice(mStack.end(), mStack, mMap[way]); }
    std::list<uint32_t> mStack;
    std::vector<std::list<uint32_t>::iterator> mMap;
};

// ── Tree PLRU ──

class TreePLRUReplacement : public ReplacementIF {
   public:
    explicit TreePLRUReplacement(uint32_t num_ways)
        : ReplacementIF(num_ways),
          mLevels(sparta::utils::floor_log2(num_ways)),
          mBits(2 * num_ways, 0) {
        sparta_assert(sparta::utils::is_power_of_2(num_ways));
    }

    void reset() override { std::fill(mBits.begin(), mBits.end(), uint8_t{0}); }

    void onAccess(uint32_t way) override { touchMRU(way); }
    void onAllocate(uint32_t way) override { touchMRU(way); }

    uint32_t getVictimWay() const override {
        uint32_t idx = 1;
        for (uint32_t i = 0; i < mLevels; ++i) idx = 2 * idx + (mBits[idx] ? 1 : 0);
        return idx - mNumWays;
    }

   private:
    void touchMRU(uint32_t way) {
        uint32_t idx = way + mNumWays;
        for (uint32_t i = 0; i < mLevels; ++i) {
            bool right = idx & 1;
            idx >>= 1;
            mBits[idx] = right ? 0 : 1;
        }
    }
    uint32_t mLevels;
    std::vector<uint8_t> mBits;
};

// Registered in Replacement.cpp.

// ── CacheLine ──

// Max line size the inline data-carrying shadow buffer supports (bytes). Lines are 64 B in
// this model; the buffer is only touched when data-carrying mode is enabled on the owning cache.
inline constexpr uint32_t kMaxLineBytes = 64;

class CacheLine {
   public:
    bool isValid() const noexcept { return mValid; }
    bool isModified() const noexcept { return mModified; }
    bool isPrefetched() const noexcept { return mPrefetched; }
    bool isUnique() const noexcept { return mUnique; }
    address_t getTag() const noexcept { return mTag; }
    address_t getAddress() const noexcept { return mAddress; }

    void setModified(bool m) noexcept { mModified = m; }
    void setPrefetched(bool p) noexcept { mPrefetched = p; }
    void setUnique(bool u) noexcept { mUnique = u; }

    void initialize(address_t addr, address_t tag) noexcept {
        mValid = true;
        mModified = false;
        mPrefetched = false;
        mUnique = false;
        mTag = tag;
        mAddress = addr;
    }
    void invalidate() noexcept {
        mValid = false;
        mModified = false;
        mPrefetched = false;
        mUnique = false;
        mTag = 0;
        mAddress = 0;
    }

    // ── Data-carrying shadow ──────────────────────────────────────────
    // The 64 B line contents, a timing-model shadow seeded from Whisper on fill. NEVER the
    // architectural source of a load result (Whisper stays authoritative); used only for
    // coherence/traffic realism and the shadow-vs-oracle self-check.
    uint8_t* data() noexcept { return mData.data(); }
    const uint8_t* data() const noexcept { return mData.data(); }
    // Seed the whole line from a 64 B source (fill).
    void seedData(const uint8_t* src) noexcept { std::copy(src, src + kMaxLineBytes, mData.begin()); }

   private:
    address_t mTag{0};
    address_t mAddress{0};
    bool mValid{false};
    bool mModified{false};
    std::array<uint8_t, kMaxLineBytes> mData{};
    bool mPrefetched{false};  // installed by a prefetch, not yet demand-hit
    bool mUnique{false};      // held exclusive (E or M): writable without asking the next level
};

// ── CacheSet ──

class CacheSet {
   public:
    CacheSet(uint32_t numWays, const std::string& policy)
        : mNumWays(numWays) {
        mLines.resize(numWays);
        mReplacement = ReplacementRegistry::instance().create(policy, numWays);
    }

    CacheLine* findLine(address_t tag) {
        for (uint32_t w = 0; w < mNumWays; ++w) {
            if (mLines[w].isValid() && mLines[w].getTag() == tag) return &mLines[w];
        }
        return nullptr;
    }

    const CacheLine* findLine(address_t tag) const {
        for (uint32_t w = 0; w < mNumWays; ++w) {
            if (mLines[w].isValid() && mLines[w].getTag() == tag) return &mLines[w];
        }
        return nullptr;
    }

    uint32_t getVictimWay() const {
        for (uint32_t w = 0; w < mNumWays; ++w)
            if (!mLines[w].isValid()) return w;
        return mReplacement->getVictimWay();
    }

    void insertLine(uint32_t way, address_t addr, address_t tag) {
        mLines[way].initialize(addr, tag);
        mReplacement->onAllocate(way);
    }

    void updateOnHit(uint32_t way) { mReplacement->onAccess(way); }

    CacheLine& getLine(uint32_t way) { return mLines[way]; }

    uint32_t getWayNumber(const CacheLine* line) const noexcept {
        auto off = line - mLines.data();
        return (off >= 0 && off < static_cast<ptrdiff_t>(mNumWays)) ? static_cast<uint32_t>(off) : mNumWays;
    }

    void invalidateAll() {
        for (auto& l : mLines) l.invalidate();
        mReplacement->reset();
    }

   private:
    uint32_t mNumWays;
    std::vector<CacheLine> mLines;
    std::unique_ptr<ReplacementIF> mReplacement;
};

// ── SimpleCache ──

class SimpleCache {
   public:
    struct Config {
        uint64_t cacheSizeKb{64};
        uint64_t lineSize{64};
        uint64_t associativity{8};
        std::string replacementPolicy{"lru"};
        // Parameter names for config errors.
        std::string sizeParam{"cache_size_kb"}, lineSizeParam{"cache_line_size"}, associativityParam{"cache_associativity"};
    };

    explicit SimpleCache(const Config& cfg)
        : mConfig(cfg) {
        uint64_t bytes = cfg.cacheSizeKb * 1024;
        sparta_assert(cfg.associativity > 0, cfg.associativityParam << " must be nonzero");
        sparta_assert(sparta::utils::is_power_of_2(cfg.lineSize), cfg.lineSizeParam << " = " << cfg.lineSize << "; must be a power of two");
        const uint64_t set_bytes = cfg.lineSize * cfg.associativity;
        sparta_assert(bytes > 0 && bytes % set_bytes == 0, cfg.sizeParam << " = " << cfg.cacheSizeKb << " KiB; must be a nonzero multiple of "
                                                                         << cfg.lineSizeParam << " x " << cfg.associativityParam << " = " << set_bytes
                                                                         << " bytes");
        mNumSets = bytes / set_bytes;

        mLineShift = sparta::utils::floor_log2(cfg.lineSize);
        mPow2Sets = sparta::utils::is_power_of_2(mNumSets);
        mSetMask = mPow2Sets ? static_cast<uint32_t>(mNumSets - 1) : 0;
        mSetShift = mPow2Sets ? (mLineShift + sparta::utils::floor_log2(mNumSets)) : mLineShift;

        mSets.reserve(mNumSets);
        for (uint32_t i = 0; i < mNumSets; ++i) mSets.emplace_back(cfg.associativity, cfg.replacementPolicy);
    }

    bool isHit(address_t addr) const { return peekLine(addr) != nullptr; }

    CacheLine* getLine(address_t addr) { return mSets[setIndex(addr)].findLine(extractTag(addr)); }

    const CacheLine* peekLine(address_t addr) const { return mSets[setIndex(addr)].findLine(extractTag(addr)); }

    CacheLine& getLineForReplacement(address_t addr) {
        auto& set = mSets[setIndex(addr)];
        return set.getLine(set.getVictimWay());
    }

    void allocate(CacheLine& line, address_t addr) {
        auto& set = mSets[setIndex(addr)];
        uint32_t way = set.getWayNumber(&line);
        set.insertLine(way, addr, extractTag(addr));
    }

    void touch(CacheLine& line) {
        auto& set = mSets[setIndex(line.getAddress())];
        set.updateOnHit(set.getWayNumber(&line));
    }

    const Config& getConfig() const noexcept { return mConfig; }

    // ── Read-only accessors for tracing / cache-viewer ──────────────────

    // Index of the set that addr maps to.
    uint32_t setIndex(address_t addr) const noexcept {
        uint64_t la = addr >> mLineShift;
        return mPow2Sets ? (static_cast<uint32_t>(la) & mSetMask) : static_cast<uint32_t>(la % mNumSets);
    }

    // Way number of the victim line that would be evicted for addr's set.
    // (Does NOT update replacement state.)
    uint32_t getVictimWay(address_t addr) const noexcept { return mSets[setIndex(addr)].getVictimWay(); }

    // Way number of a specific line pointer (must belong to addr's set).
    uint32_t getLineWay(address_t addr, const CacheLine* line) const noexcept { return mSets[setIndex(addr)].getWayNumber(line); }

    // Read-only view of all sets (for tag-array snapshot dumps).
    const std::vector<CacheSet>& getSets() const noexcept { return mSets; }

    uint64_t numSets() const noexcept { return mNumSets; }

   private:
    address_t extractTag(address_t addr) const noexcept { return mPow2Sets ? (addr >> mSetShift) : static_cast<address_t>((addr >> mLineShift) / mNumSets); }

    Config mConfig;
    uint32_t mNumSets;
    uint32_t mLineShift;
    uint32_t mSetShift;
    uint32_t mSetMask;
    bool mPow2Sets;
    std::vector<CacheSet> mSets;
};

}  // namespace cpu
