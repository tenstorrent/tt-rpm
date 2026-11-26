// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Tenstorrent USA, Inc.

#include "Mmu/Mmu.hpp"

#include <bit>

#include "Common/SpeculationUtils.hpp"
#include "LoadStoreUnit/BackendMemoryStructures/BackendMemoryStructures.hpp"
#include "Logging.hpp"

namespace core {

namespace {

constexpr uint32_t kMaxLevels = 5;  // Sv57

tlb::PageSize pageSizeOf(uint64_t bytes) { return static_cast<tlb::PageSize>((std::countr_zero(bytes) - 12) / 9); }
unsigned pageShift(tlb::PageSize s) { return 12 + 9 * static_cast<unsigned>(s); }

tlb::TlbEntry makeEntry(cpu::address_t va, cpu::address_t pa, tlb::asid_t asid, tlb::PageSize size, bool global) {
    tlb::TlbEntry e;
    e.vpn = va >> pageShift(size);
    e.ppn = (pa >> pageShift(size)) << (pageShift(size) - 12);
    e.asid = asid;
    e.page_size = size;
    e.perms.global = global;
    e.valid = true;
    return e;
}

}  // namespace

Mmu::Mmu(sparta::TreeNode* node, const MmuParams* params)
    : sparta::Unit(node),
      mEnabled(params->enabled),
      mL2Latency(params->l2tlb_latency),
      mNumWalkers(params->num_walkers),
      mItlbHits(&unit_stat_set_, "itlb_hits", "L1 ITLB hits", sparta::Counter::COUNT_NORMAL),
      mItlbMisses(&unit_stat_set_, "itlb_misses", "L1 ITLB misses", sparta::Counter::COUNT_NORMAL),
      mDtlbHits(&unit_stat_set_, "dtlb_hits", "L1 DTLB hits", sparta::Counter::COUNT_NORMAL),
      mDtlbMisses(&unit_stat_set_, "dtlb_misses", "L1 DTLB misses", sparta::Counter::COUNT_NORMAL),
      mL2TlbHits(&unit_stat_set_, "l2tlb_hits", "L2 TLB hits", sparta::Counter::COUNT_NORMAL),
      mL2TlbMisses(&unit_stat_set_, "l2tlb_misses", "L2 TLB misses", sparta::Counter::COUNT_NORMAL),
      mWalksStarted(&unit_stat_set_, "walks", "Page-table walks started", sparta::Counter::COUNT_NORMAL),
      mWalksCoalesced(&unit_stat_set_, "walks_coalesced", "L2 TLB misses that joined a walk already in flight", sparta::Counter::COUNT_NORMAL),
      mWalksFaulted(&unit_stat_set_, "walks_faulted", "Walks ending in a page fault (no fill)", sparta::Counter::COUNT_NORMAL),
      mWalksSuperpage(&unit_stat_set_, "walks_superpage", "Walks whose leaf maps a 2 MiB or larger page", sparta::Counter::COUNT_NORMAL),
      mWalksCancelled(&unit_stat_set_, "walks_cancelled", "Queued walks dropped because every requester was squashed", sparta::Counter::COUNT_NORMAL),
      mWalksOrphaned(&unit_stat_set_, "walks_orphaned", "Walks that finished after every requester was squashed", sparta::Counter::COUNT_NORMAL),
      mWalkCycles(&unit_stat_set_, "walk_cycles", "Cycles from walk start to its last PTE", sparta::Counter::COUNT_NORMAL),
      mWalkQueueCycles(&unit_stat_set_, "walk_queue_cycles", "Cycles walks waited for a free walker", sparta::Counter::COUNT_NORMAL),
      mPteLoads(&unit_stat_set_, "walk_pte_loads", "PTE loads issued to the L1D", sparta::Counter::COUNT_NORMAL),
      mPwcHits(&unit_stat_set_, "pwc_hits", "Walks that skipped levels through the page-walk cache", sparta::Counter::COUNT_NORMAL),
      mPwcMisses(&unit_stat_set_, "pwc_misses", "Walks that started at the root", sparta::Counter::COUNT_NORMAL),
      mItlbStallCycles(&unit_stat_set_, "itlb_stall_cycles", "Fetch cycles stalled on a translation", sparta::Counter::COUNT_NORMAL),
      mDtlbWaitCycles(&unit_stat_set_, "dtlb_wait_cycles", "Load/store polls that found a translation pending", sparta::Counter::COUNT_NORMAL),
      mSfences(&unit_stat_set_, "sfences", "sfence.vma instructions applied", sparta::Counter::COUNT_NORMAL) {
    sparta_assert(mNumWalkers > 0, params->num_walkers.getLocation() << " must be nonzero");
    const std::string type = params->tlb_type;
    auto make = [&](uint32_t entries, uint32_t ways, const sparta::Parameter<uint32_t>& p) {
        return tlb::Registry<tlb::TlbInterface, tlb::TlbConfig>::instance().create(type, tlb::TlbConfig{entries, ways, p.getLocation()});
    };
    mL1[kInst] = make(params->itlb_entries, params->itlb_ways, params->itlb_entries);
    mL1[kData] = make(params->dtlb_entries, params->dtlb_ways, params->dtlb_entries);
    const uint32_t l2_ways = params->l2tlb_ways;
    if (params->l2tlb_entries > 0) mL2 = make(params->l2tlb_entries, l2_ways, params->l2tlb_entries);
    mPwc.assign(kMaxLevels - 1, std::vector<PwcEntry>(params->pwc_entries));

    pte_response_in.registerConsumerHandler(CREATE_SPARTA_HANDLER_WITH_DATA(Mmu, receivePteResponse_, MemResponse));
    flush_in.registerConsumerHandler(CREATE_SPARTA_HANDLER_WITH_DATA(Mmu, receiveFlush_, FlushRequest));
}

void Mmu::setExecutionDriver(cpu::ExecutionDriver* driver) {
    mDriver = driver;
    if (mDriver && mEnabled) mDriver->setSfenceHook([this](const cpu::ExecutionDriver::SfenceVma& f) { sfence_(f); });
}

std::optional<Mmu::Ticket> Mmu::start_(Side side, cpu::address_t va, bool is_store, uint64_t tag) {
    if (!mDriver->translationOn()) return std::nullopt;
    const auto asid = static_cast<tlb::asid_t>(mDriver->currentAsid());
    const tlb::TranslationRequest req{.vaddr = va, .asid = asid, .is_store = is_store, .is_fetch = side == kInst};

    if (mL1[side]->lookup(req).result == tlb::TlbResult::Hit) {
        ++(side == kInst ? mItlbHits : mDtlbHits);
        return std::nullopt;
    }
    ++(side == kInst ? mItlbMisses : mDtlbMisses);

    for (const auto& f : mL1Fills) {
        if (f.side == side && f.e.asid == asid && f.e.vpn == va >> pageShift(f.e.page_size)) return Ticket{.ready_cycle = f.ready_cycle};
    }

    if (mL2) {
        const auto r = mL2->lookup(req);
        if (r.result == tlb::TlbResult::Hit) {
            ++mL2TlbHits;
            const uint64_t ready = now_() + mL2Latency;
            mL1Fills.push_back({ready, side, makeEntry(va, r.paddr, asid, r.page_size, r.perms.global)});
            return Ticket{.ready_cycle = ready};
        }
        ++mL2TlbMisses;
    }

    for (auto& [id, w] : mWalks) {
        if (w.asid == asid && (w.va >> 12) == (va >> 12)) {
            w.sides |= 1u << side;
            ++w.refs;
            ++mWalksCoalesced;
            return Ticket{.walk_id = id};
        }
    }

    auto pw = mDriver->walkPageTable(va, side == kInst, is_store, tag);
    if (pw.pte_addrs.empty() && !pw.fault) return std::nullopt;
    const uint64_t id = ++mNextWalkId;
    // The walk starts once the L2 TLB lookup has reported the miss.
    const uint64_t queued = now_() + (mL2 ? mL2Latency : 0);
    mWalks.emplace(id, Walk{.va = va, .asid = asid, .sides = static_cast<uint8_t>(1u << side), .pw = std::move(pw), .refs = 1, .queued_cycle = queued});
    ILOG("[mmu] queue walk " << id << " side=" << int(side) << " va=0x" << std::hex << va << std::dec);
    return Ticket{.walk_id = id};
}

bool Mmu::ready_(const Ticket& t) const { return t.walk_id ? !mWalks.contains(t.walk_id) : now_() >= t.ready_cycle; }

void Mmu::release_(const Ticket& t) {
    auto it = mWalks.find(t.walk_id);
    if (it == mWalks.end() || --it->second.refs > 0 || it->second.started) return;
    mWalks.erase(it);
    ++mWalksCancelled;
}

bool Mmu::fetchReady(cpu::address_t va) {
    if (!mEnabled || !mDriver) return true;
    if (mFetchTicket && (mFetchVa >> 12) == (va >> 12)) {
        if (!ready_(*mFetchTicket)) {
            ++mItlbStallCycles;
            return false;
        }
        mFetchTicket.reset();
        return true;
    }
    // Fetch was redirected away from the page it was waiting on.
    if (mFetchTicket) release_(*mFetchTicket);
    mFetchTicket = start_(kInst, va, false, 0);
    if (!mFetchTicket || ready_(*mFetchTicket)) {
        mFetchTicket.reset();
        return true;
    }
    mFetchVa = va;
    ++mItlbStallCycles;
    return false;
}

bool Mmu::dataReady(const PipelinePacket& pkt, bool is_store) {
    if (!mEnabled || !mDriver || !pkt.inst) return true;
    if (auto it = mDataTickets.find(pkt.fetch_seq); it != mDataTickets.end()) {
        if (!ready_(it->second.t)) {
            ++mDtlbWaitCycles;
            return false;
        }
        mDataTickets.erase(it);
        return true;
    }
    const auto& rec = pkt.inst->getTraceRecord();
    if (rec.virtAddrs.empty()) return true;
    const auto t = start_(kData, rec.virtAddrs[0], is_store, pkt.tag);
    if (!t || ready_(*t)) return true;
    mDataTickets.emplace(pkt.fetch_seq, DataTicket{*t, pkt.tag, pkt.wrong_path_depth});
    ++mDtlbWaitCycles;
    return false;
}

// Wrong-path walks are allowed. A started walk runs to completion and fills the TLBs; a queued
// walk whose requesters were all squashed is dropped.
void Mmu::receiveFlush_(const FlushRequest& req) {
    for (auto it = mDataTickets.begin(); it != mDataTickets.end();) {
        if (shouldSquash(it->second.tag, it->second.depth, req) || (req.branch_fetch_seq != 0 && it->first > req.branch_fetch_seq)) {
            release_(it->second.t);
            it = mDataTickets.erase(it);
        } else {
            ++it;
        }
    }
}

void Mmu::tick() {
    const uint64_t now = now_();
    std::erase_if(mL1Fills, [&](const L1Fill& f) {
        if (f.ready_cycle > now) return false;
        mL1[f.side]->insert(f.e);
        return true;
    });
    if (mWalks.empty()) return;
    for (auto it = mWalks.begin(); it != mWalks.end();) {
        auto cur = it++;
        Walk& w = cur->second;
        if (!w.started) {
            if (mActiveWalks == mNumWalkers || now < w.queued_cycle) break;
            ++mActiveWalks;
            ++mWalksStarted;
            w.started = true;
            w.start_cycle = now;
            mWalkQueueCycles += now - w.queued_cycle;
            w.next = pwcSkip_(w);
        }
        if (w.outstanding) continue;
        if (w.next == w.pw.pte_addrs.size()) {
            complete_(cur);
            continue;
        }
        const cpu::address_t addr = w.pw.pte_addrs[w.next];
        if (!mDcache->canAcceptRequest(addr)) continue;
        pte_request_out.send(MemRequest{.tag = kPteTagBit | cur->first, .address = addr, .access_size = 8, .page_walk = true}, 0);
        w.outstanding = true;
        ++mPteLoads;
    }
}

void Mmu::receivePteResponse_(const MemResponse& resp) {
    if (!(resp.tag & kPteTagBit)) return;
    auto it = mWalks.find(resp.tag & ~kPteTagBit);
    if (it == mWalks.end()) return;
    Walk& w = it->second;
    w.outstanding = false;
    if (!w.stale) pwcFill_(w, w.next);
    if (++w.next == w.pw.pte_addrs.size()) complete_(it);
}

void Mmu::complete_(std::map<uint64_t, Walk>::iterator it) {
    const Walk& w = it->second;
    if (w.pw.fault) {
        ++mWalksFaulted;
    } else if (w.pw.levels > 0 && !w.stale) {
        if (w.pw.page_size > 4096) ++mWalksSuperpage;
        const auto e = makeEntry(w.va, w.pw.pa, w.asid, pageSizeOf(w.pw.page_size), w.pw.global);
        if (mL2) mL2->insert(e);
        for (Side s : {kInst, kData}) {
            if (w.sides & (1u << s)) mL1[s]->insert(e);
        }
    }
    if (w.refs == 0) ++mWalksOrphaned;
    mWalkCycles += now_() - w.start_cycle;
    --mActiveWalks;
    ILOG("[mmu] walk " << it->first << " done after " << now_() - w.start_cycle << " cycles");
    mWalks.erase(it);
}

// Split translation caches tagged by VA prefix: the entry for level k (0 = root) holds the
// pointer that level's PTE yields, so a hit skips loads 0..k (Barr, Cox and Rixner, ISCA 2010).
uint32_t Mmu::pwcSkip_(const Walk& w) {
    const uint32_t n = w.pw.pte_addrs.size();
    if (w.pw.levels == 0 || mPwc[0].empty() || n < 2) return 0;
    for (uint32_t k = n - 1; k-- > 0;) {
        const uint64_t tag = w.va >> (12 + 9 * (w.pw.levels - 1 - k));
        for (auto& e : mPwc[k]) {
            if (e.valid && e.tag == tag && e.asid == w.asid && e.levels == w.pw.levels) {
                e.last_use = ++mPwcClock;
                ++mPwcHits;
                return k + 1;
            }
        }
    }
    ++mPwcMisses;
    return 0;
}

void Mmu::pwcFill_(const Walk& w, uint32_t level) {
    // Only non-leaf PTEs the walk went past are known valid pointers.
    if (w.pw.levels == 0 || mPwc[0].empty() || level + 1 >= w.pw.pte_addrs.size()) return;
    const uint64_t tag = w.va >> (12 + 9 * (w.pw.levels - 1 - level));
    auto& set = mPwc[level];
    PwcEntry* victim = &set[0];
    for (auto& e : set) {
        if (e.valid && e.tag == tag && e.asid == w.asid && e.levels == w.pw.levels) return;
        if (!e.valid || (victim->valid && e.last_use < victim->last_use)) victim = &e;
    }
    *victim = PwcEntry{true, tag, w.asid, w.pw.levels, ++mPwcClock};
}

// RISC-V privileged spec: rs1 != x0 only orders leaf PTEs, so the page-walk cache survives it.
void Mmu::sfence_(const cpu::ExecutionDriver::SfenceVma& f) {
    ++mSfences;
    const auto asid = static_cast<tlb::asid_t>(f.asid);
    std::vector<tlb::TlbInterface*> tlbs{mL1[kInst].get(), mL1[kData].get()};
    if (mL2) tlbs.push_back(mL2.get());
    for (auto* t : tlbs) {
        if (f.all_va && f.all_asid)
            t->invalidateAll();
        else if (f.all_va)
            t->invalidateAsid(asid);
        else if (f.all_asid)
            t->invalidatePage(f.va);
        else
            t->invalidatePage(f.va, asid);
    }
    if (f.all_va) {
        for (auto& level : mPwc) {
            for (auto& e : level) {
                if (f.all_asid || e.asid == asid) e.valid = false;
            }
        }
    }
    for (auto& [id, w] : mWalks) w.stale = true;
    mL1Fills.clear();
}

}  // namespace core
