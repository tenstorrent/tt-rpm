// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Tenstorrent USA, Inc.

#pragma once

#include <cstdint>
#include <string>

#include "common/AlgorithmRegistry.hpp"

namespace tlb {

using addr_t = uint64_t;
using asid_t = uint16_t;

// RISC-V Sv leaf page sizes (4 KiB base plus superpages).
enum class PageSize : uint8_t { Size4K, Size2M, Size1G, Size512G };

enum class TlbResult : uint8_t { Hit, Miss };

// Permission / attribute bits carried by a translation (RISC-V PTE R/W/X/U + G).
struct Permissions {
    bool read{false};
    bool write{false};
    bool exec{false};
    bool user{false};
    bool global{false};
};

// A translation request from the fetch (I-side) or LSU (D-side).
struct TranslationRequest {
    addr_t vaddr{0};
    asid_t asid{0};
    bool is_store{false};  // D-side: write intent (sets dirty)
    bool is_fetch{false};  // I-side
    uint64_t seq{0};       // generating instruction's seq
    uint64_t cycle{0};
};

// A resident TLB entry / fill payload (produced by a page-table walk).
struct TlbEntry {
    addr_t vpn{0};  // virtual page number (tag)
    addr_t ppn{0};  // physical page number
    asid_t asid{0};
    PageSize page_size{PageSize::Size4K};
    Permissions perms{};
    bool valid{false};
};

struct TranslationResult {
    TlbResult result{TlbResult::Miss};
    addr_t paddr{0};
    PageSize page_size{PageSize::Size4K};
    Permissions perms{};
};

// One interface fronts both the fetch (I-side) and LSU (D-side) translation
// paths; the caller distinguishes them via TranslationRequest::is_fetch.
class TlbInterface {
   public:
    virtual ~TlbInterface() = default;

    // Lookup — blind to the page-table walker; returns Hit + translation or Miss.
    virtual TranslationResult lookup(const TranslationRequest& req) = 0;

    // Fill a resident entry after a successful walk (host drives replacement).
    virtual void insert(const TlbEntry& entry) = 0;

    // sfence.vma-style invalidation.
    virtual void invalidateAll() = 0;
    virtual void invalidateAsid(asid_t asid) = 0;
    virtual void invalidatePage(addr_t vaddr, asid_t asid) = 0;
    virtual void invalidatePage(addr_t vaddr) = 0;  // every ASID, global entries included

    virtual void reset() = 0;
    virtual std::string name() const = 0;

    // Capability flags (host provisions only what an algorithm uses; new flags
    // default to current behavior).
    virtual bool supportsSuperpages() const { return false; }
    virtual bool isGlobalAcrossAsid() const { return false; }
};

// Geometry of one TLB instance; the host builds several (ITLB, DTLB, L2 TLB) from its own params.
struct TlbConfig {
    uint32_t entries{32};
    uint32_t ways{32};
    std::string name;  // owning parameter, for error messages
};

template <typename Iface, typename Params>
using Registry = rpm::Registry<Iface, Params>;

}  // namespace tlb

#define REGISTER_TLB(Iface, Params, KEY, Type) RPM_REGISTER_ALGORITHM(Iface, Params, KEY, Type)
