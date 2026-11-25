// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Tenstorrent USA, Inc.

#include "tlb/src/SetAssocTlb.hpp"

#include <bit>
#include <stdexcept>

namespace tlb {

namespace {
constexpr unsigned pageShift(PageSize s) { return 12 + 9 * static_cast<unsigned>(s); }
}  // namespace

SetAssocTlb::SetAssocTlb(const TlbConfig& cfg)
    : mAssoc(cfg.ways) {
    if (mAssoc == 0 || cfg.entries % mAssoc != 0) {
        throw std::invalid_argument(cfg.name + ": entries (" + std::to_string(cfg.entries) + ") must be a nonzero multiple of ways (" +
                                    std::to_string(cfg.ways) + ")");
    }
    mNumSets = cfg.entries / mAssoc;
    if (!std::has_single_bit(mNumSets)) {
        throw std::invalid_argument(cfg.name + ": entries / ways = " + std::to_string(mNumSets) + " sets; must be a power of two");
    }
    mWays.resize(static_cast<size_t>(mNumSets) * mAssoc);
}

TranslationResult SetAssocTlb::lookup(const TranslationRequest& req) {
    for (size_t s = 0; s < kNumSizes; ++s) {
        if (mResident[s] == 0) continue;
        const auto size = static_cast<PageSize>(s);
        const addr_t vpn = req.vaddr >> pageShift(size);
        Way* set = set_(vpn);
        for (uint32_t w = 0; w < mAssoc; ++w) {
            const TlbEntry& e = set[w].e;
            if (!e.valid || e.page_size != size || e.vpn != vpn || (!e.perms.global && e.asid != req.asid)) continue;
            set[w].last_use = ++mUseClock;
            const addr_t offset_mask = (addr_t{1} << pageShift(size)) - 1;
            return {TlbResult::Hit, (e.ppn << 12) | (req.vaddr & offset_mask), size, e.perms};
        }
    }
    return {};
}

void SetAssocTlb::insert(const TlbEntry& entry) {
    Way* set = set_(entry.vpn);
    Way* victim = &set[0];
    for (uint32_t w = 0; w < mAssoc; ++w) {
        const TlbEntry& e = set[w].e;
        if (e.valid && e.page_size == entry.page_size && e.vpn == entry.vpn && e.asid == entry.asid) {
            victim = &set[w];
            break;
        }
        if (!e.valid || (victim->e.valid && set[w].last_use < victim->last_use)) victim = &set[w];
    }
    if (victim->e.valid) --mResident[static_cast<size_t>(victim->e.page_size)];
    victim->e = entry;
    victim->e.valid = true;
    victim->last_use = ++mUseClock;
    ++mResident[static_cast<size_t>(entry.page_size)];
}

template <typename Pred>
void SetAssocTlb::invalidateIf_(Pred pred) {
    for (auto& w : mWays) {
        if (w.e.valid && pred(w.e)) {
            w.e.valid = false;
            --mResident[static_cast<size_t>(w.e.page_size)];
        }
    }
}

void SetAssocTlb::invalidateAll() {
    for (auto& w : mWays) w = Way{};
    mResident = {};
    mUseClock = 0;
}

void SetAssocTlb::invalidateAsid(asid_t asid) {
    invalidateIf_([asid](const TlbEntry& e) { return !e.perms.global && e.asid == asid; });
}

void SetAssocTlb::invalidatePage(addr_t vaddr, asid_t asid) {
    invalidateIf_([=](const TlbEntry& e) { return !e.perms.global && e.asid == asid && e.vpn == vaddr >> pageShift(e.page_size); });
}

void SetAssocTlb::invalidatePage(addr_t vaddr) {
    invalidateIf_([=](const TlbEntry& e) { return e.vpn == vaddr >> pageShift(e.page_size); });
}

}  // namespace tlb

REGISTER_TLB(tlb::TlbInterface, tlb::TlbConfig, "setassoc", tlb::SetAssocTlb)
