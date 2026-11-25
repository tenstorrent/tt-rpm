// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Tenstorrent USA, Inc.

#pragma once

#include <array>
#include <vector>

#include "common/TLB.hpp"

namespace tlb {

// Set-associative TLB holding every leaf page size, LRU-replaced. A lookup probes the set each
// resident page size indexes, so ways == entries gives a fully associative TLB.
class SetAssocTlb : public TlbInterface {
   public:
    explicit SetAssocTlb(const TlbConfig& cfg);

    TranslationResult lookup(const TranslationRequest& req) override;
    void insert(const TlbEntry& entry) override;
    void invalidateAll() override;
    // Per the RISC-V privileged spec, ASID-scoped invalidations spare global entries.
    void invalidateAsid(asid_t asid) override;
    void invalidatePage(addr_t vaddr, asid_t asid) override;
    void invalidatePage(addr_t vaddr) override;
    void reset() override { invalidateAll(); }
    std::string name() const override { return "setassoc"; }

    bool supportsSuperpages() const override { return true; }
    bool isGlobalAcrossAsid() const override { return true; }

   private:
    struct Way {
        TlbEntry e;  // e.vpn is the VPN at e.page_size granularity
        uint64_t last_use{0};
    };

    static constexpr size_t kNumSizes = 4;
    Way* set_(addr_t vpn) { return &mWays[(vpn & (mNumSets - 1)) * mAssoc]; }
    template <typename Pred>
    void invalidateIf_(Pred pred);

    uint32_t mNumSets;
    uint32_t mAssoc;
    uint64_t mUseClock{0};
    std::array<uint32_t, kNumSizes> mResident{};  // valid entries per page size, to skip empty probes
    std::vector<Way> mWays;                       // mNumSets x mAssoc
};

}  // namespace tlb
