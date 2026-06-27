// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Tenstorrent USA, Inc.

#include "MemoryHierarchy/CacheTracer.hpp"

#include <algorithm>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <unordered_map>

namespace cpu {

// ─── construction ─────────────────────────────────────────────────────────

CacheTracer::CacheTracer(std::string name, uint32_t max_events)
    : mName(std::move(name)),
      mMax(max_events) {}

// ─── record helpers ───────────────────────────────────────────────────────

uint32_t CacheTracer::newRequest(uint64_t cycle, uint64_t token, uint64_t addr) {
    uint32_t id = mNextReqId++;
    CacheEvent ev;
    ev.cycle = cycle;
    ev.req_id = id;
    ev.type = CacheEventType::Request;
    ev.token = token;
    ev.addr = addr;
    append(ev);
    return id;
}

void CacheTracer::recordHit(uint32_t req_id, uint64_t cycle, uint32_t set, uint32_t way) {
    CacheEvent ev;
    ev.cycle = cycle;
    ev.req_id = req_id;
    ev.type = CacheEventType::Hit;
    ev.set = set;
    ev.way = way;
    append(ev);
}

void CacheTracer::recordMiss(uint32_t req_id, uint64_t cycle, uint32_t set) {
    CacheEvent ev;
    ev.cycle = cycle;
    ev.req_id = req_id;
    ev.type = CacheEventType::Miss;
    ev.set = set;
    append(ev);
}

void CacheTracer::recordCoalesce(uint32_t req_id, uint64_t cycle, uint32_t mshr_id) {
    CacheEvent ev;
    ev.cycle = cycle;
    ev.req_id = req_id;
    ev.type = CacheEventType::Coalesce;
    ev.mshr_id = mshr_id;
    append(ev);
}

void CacheTracer::recordMshrAlloc(uint32_t req_id, uint64_t cycle, uint32_t mshr_id, uint64_t line_addr) {
    CacheEvent ev;
    ev.cycle = cycle;
    ev.req_id = req_id;
    ev.type = CacheEventType::MshrAlloc;
    ev.mshr_id = mshr_id;
    ev.addr = line_addr;
    append(ev);
}

void CacheTracer::recordFillStart(uint64_t cycle, uint32_t mshr_id, uint64_t line_addr) {
    CacheEvent ev;
    ev.cycle = cycle;
    ev.type = CacheEventType::FillStart;
    ev.mshr_id = mshr_id;
    ev.addr = line_addr;
    append(ev);
}

void CacheTracer::recordFillDone(uint64_t cycle, uint32_t mshr_id, uint64_t line_addr, uint32_t set, uint32_t way, bool evict_valid, uint64_t evict_addr) {
    CacheEvent ev;
    ev.cycle = cycle;
    ev.type = CacheEventType::FillDone;
    ev.mshr_id = mshr_id;
    ev.addr = line_addr;
    ev.set = set;
    ev.way = way;
    ev.evict_valid = evict_valid;
    ev.evict_addr = evict_addr;
    append(ev);
}

void CacheTracer::recordResponse(uint32_t req_id, uint64_t cycle, bool miss) {
    CacheEvent ev;
    ev.cycle = cycle;
    ev.req_id = req_id;
    ev.type = CacheEventType::Response;
    ev.miss = miss;
    append(ev);
}

void CacheTracer::recordL2ArbWin(uint64_t cycle, uint8_t source, uint64_t line_addr) {
    CacheEvent ev;
    ev.cycle = cycle;
    ev.type = CacheEventType::L2ArbWin;
    ev.source = source;
    ev.addr = line_addr;
    append(ev);
}

void CacheTracer::recordL2ArbLose(uint64_t cycle, uint8_t source, uint64_t line_addr, uint32_t wait_cycles) {
    CacheEvent ev;
    ev.cycle = cycle;
    ev.type = CacheEventType::L2ArbLose;
    ev.source = source;
    ev.addr = line_addr;
    ev.wait_cycles = wait_cycles;
    append(ev);
}

// ─── static helpers ───────────────────────────────────────────────────────

const char* CacheTracer::eventName(CacheEventType t) {
    switch (t) {
        case CacheEventType::Request:
            return "REQUEST   ";
        case CacheEventType::Hit:
            return "HIT       ";
        case CacheEventType::Miss:
            return "MISS      ";
        case CacheEventType::Coalesce:
            return "COALESCE  ";
        case CacheEventType::MshrAlloc:
            return "MSHR_ALLOC";
        case CacheEventType::FillStart:
            return "FILL_START";
        case CacheEventType::FillDone:
            return "FILL_DONE ";
        case CacheEventType::Response:
            return "RESPONSE  ";
        case CacheEventType::L2ArbWin:
            return "ARB_WIN   ";
        case CacheEventType::L2ArbLose:
            return "ARB_LOSE  ";
    }
    return "?         ";
}

const char* CacheTracer::sourceName(uint8_t src) { return (src == 0) ? "icache" : "dcache"; }

// ─── dump entry point ─────────────────────────────────────────────────────

void CacheTracer::dump(std::ostream& os, const std::string& format) const {
    if (mEvents.empty()) {
        os << "\n[" << mName << "] No cache events recorded.\n";
        return;
    }

    if (format == "waterfall") {
        dumpWaterfall(os);
        dumpSummary(os);
    } else if (format == "summary") {
        dumpSummary(os);
    } else {
        dumpLog(os);
        dumpSummary(os);
    }
}

// ─── log format ───────────────────────────────────────────────────────────

void CacheTracer::dumpLog(std::ostream& os) const {
    os << "\n╔══════════════════════════════════════════════════════════════╗\n"
       << "║  Cache Event Log: " << std::left << std::setw(43) << mName << "║\n"
       << "╚══════════════════════════════════════════════════════════════╝\n";

    os << std::left << std::setw(8) << "CYCLE" << std::setw(8) << "REQ_ID" << std::setw(12) << "EVENT"
       << "DETAILS\n";
    os << std::string(80, '-') << "\n";

    for (const auto& ev : mEvents) {
        os << std::left << std::dec << std::setw(8) << ev.cycle << std::setw(8) << ev.req_id << std::setw(12) << eventName(ev.type);

        std::ostringstream det;
        switch (ev.type) {
            case CacheEventType::Request:
                det << "token=" << ev.token << "  addr=0x" << std::hex << ev.addr;
                break;
            case CacheEventType::Hit:
                det << "set=" << std::dec << ev.set << "  way=" << ev.way;
                break;
            case CacheEventType::Miss:
                det << "set=" << std::dec << ev.set;
                break;
            case CacheEventType::Coalesce:
                det << "→ mshr=" << std::dec << ev.mshr_id;
                break;
            case CacheEventType::MshrAlloc:
                det << "mshr=" << std::dec << ev.mshr_id << "  line=0x" << std::hex << ev.addr;
                break;
            case CacheEventType::FillStart:
                det << "mshr=" << std::dec << ev.mshr_id << "  line=0x" << std::hex << ev.addr;
                break;
            case CacheEventType::FillDone:
                det << "mshr=" << std::dec << ev.mshr_id << "  line=0x" << std::hex << ev.addr << "  set=" << std::dec << ev.set << "  way=" << ev.way;
                if (ev.evict_valid)
                    det << "  (evicted 0x" << std::hex << ev.evict_addr << ")";
                else
                    det << "  (evicted INVALID)";
                break;
            case CacheEventType::Response:
                det << (ev.miss ? "MISS" : "HIT");
                break;
            case CacheEventType::L2ArbWin:
                det << sourceName(ev.source) << "  line=0x" << std::hex << ev.addr;
                break;
            case CacheEventType::L2ArbLose:
                det << sourceName(ev.source) << "  line=0x" << std::hex << ev.addr << "  waited=" << std::dec << ev.wait_cycles << " cycles";
                break;
        }
        os << det.str() << "\n";
    }
}

// ─── waterfall format ─────────────────────────────────────────────────────

void CacheTracer::dumpWaterfall(std::ostream& os) const {
    // ── Reconstruct per-request records ────────────────────────────────
    struct ReqRec {
        uint32_t req_id;
        uint64_t token;
        uint64_t addr;
        uint64_t arrive_cycle{0};
        uint64_t response_cycle{0};
        uint64_t fill_done_cycle{0};
        uint32_t mshr_id{UINT32_MAX};
        char outcome{'?'};  // 'H'=hit 'M'=miss 'C'=coalesce
    };

    std::unordered_map<uint32_t, ReqRec> recs;

    for (const auto& ev : mEvents) {
        switch (ev.type) {
            case CacheEventType::Request: {
                ReqRec r;
                r.req_id = ev.req_id;
                r.token = ev.token;
                r.addr = ev.addr;
                r.arrive_cycle = ev.cycle;
                recs[ev.req_id] = r;
                break;
            }
            case CacheEventType::Hit:
                if (auto it = recs.find(ev.req_id); it != recs.end()) it->second.outcome = 'H';
                break;
            case CacheEventType::Miss:
                if (auto it = recs.find(ev.req_id); it != recs.end()) it->second.outcome = 'M';
                break;
            case CacheEventType::Coalesce:
                if (auto it = recs.find(ev.req_id); it != recs.end()) {
                    it->second.outcome = 'C';
                    it->second.mshr_id = ev.mshr_id;
                }
                break;
            case CacheEventType::MshrAlloc:
                if (auto it = recs.find(ev.req_id); it != recs.end()) it->second.mshr_id = ev.mshr_id;
                break;
            case CacheEventType::FillDone: {
                // Attribute to any request whose mshr_id matches.
                for (auto& [id, r] : recs)
                    if (r.mshr_id == ev.mshr_id && r.fill_done_cycle == 0) r.fill_done_cycle = ev.cycle;
                break;
            }
            case CacheEventType::Response:
                if (auto it = recs.find(ev.req_id); it != recs.end()) it->second.response_cycle = ev.cycle;
                break;
            default:
                break;
        }
    }

    // Build ordered list by arrive_cycle.
    std::vector<const ReqRec*> order;
    order.reserve(recs.size());
    for (const auto& [id, r] : recs) order.push_back(&r);
    std::sort(order.begin(), order.end(), [](const ReqRec* a, const ReqRec* b) {
        return a->arrive_cycle < b->arrive_cycle || (a->arrive_cycle == b->arrive_cycle && a->req_id < b->req_id);
    });

    if (order.empty()) return;

    uint64_t c_min = order.front()->arrive_cycle;
    uint64_t c_max = 0;
    for (const auto* r : order) {
        uint64_t end = r->response_cycle ? r->response_cycle : (r->fill_done_cycle ? r->fill_done_cycle : r->arrive_cycle);
        c_max = std::max(c_max, end);
    }

    // Render in pages of 32 cycles.
    constexpr uint32_t kPage = 32;

    auto stateAt = [](const ReqRec& r, uint64_t c) -> const char* {
        if (c < r.arrive_cycle) return "  ";
        if (r.response_cycle && c == r.response_cycle) return "Rp";
        if (r.response_cycle && c > r.response_cycle) return "  ";
        if (c == r.arrive_cycle) {
            if (r.outcome == 'H') return "Ht";
            if (r.outcome == 'M') return "Ms";
            if (r.outcome == 'C') return "Co";
            return "Rq";
        }
        if (r.fill_done_cycle && c == r.fill_done_cycle) return "Fl";
        return " ·";
    };

    os << "\n╔══════════════════════════════════════════════════════════════╗\n"
       << "║  Cache Waterfall: " << std::left << std::setw(43) << mName << "║\n"
       << "╚══════════════════════════════════════════════════════════════╝\n"
       << "Legend: Rq=Request  Ht=Hit  Ms=Miss  Co=Coalesce  "
          "Fl=FillDone  Rp=Response  ·=Waiting\n";

    for (uint64_t page = c_min; page <= c_max; page += kPage) {
        uint64_t pend = std::min(page + kPage - 1, c_max);

        os << "\n  cycles " << page << " – " << pend << "\n";
        // Fixed-width hex for both token and addr avoids decimal overflow.
        constexpr int kIdW = 7;     // REQ_ID
        constexpr int kTokW = 19;   // "0x" + 16 hex digits + 1 space
        constexpr int kAddrW = 19;  // same
        constexpr int kOutW = 5;    // outcome char + spaces

        os << std::left << std::setw(kIdW) << "REQ_ID" << std::setw(kTokW) << "TOKEN" << std::setw(kAddrW) << "ADDR" << std::setw(kOutW) << "OUT"
           << "| ";
        for (uint64_t c = page; c <= pend; ++c) os << std::setw(3) << std::dec << c;
        os << "|\n";
        os << std::string(kIdW + kTokW + kAddrW + kOutW + 2 + (pend - page + 1) * 3 + 1, '-') << "\n";

        for (const auto* rp : order) {
            const ReqRec& r = *rp;
            uint64_t end = r.response_cycle ? r.response_cycle : (r.fill_done_cycle ? r.fill_done_cycle : r.arrive_cycle);
            if (r.arrive_cycle > pend || (end > 0 && end < page)) continue;

            std::ostringstream tok_str, addr_str;
            tok_str << "0x" << std::hex << r.token;
            addr_str << "0x" << std::hex << r.addr;

            char out_str[2] = {r.outcome, '\0'};

            os << std::left << std::dec << std::setw(kIdW) << r.req_id << std::setw(kTokW) << tok_str.str() << std::setw(kAddrW) << addr_str.str()
               << std::setw(kOutW) << out_str << "| ";

            for (uint64_t c = page; c <= pend; ++c) os << stateAt(r, c) << " ";
            os << "|\n";
        }
    }
}

// ─── summary format ───────────────────────────────────────────────────────

void CacheTracer::dumpSummary(std::ostream& os) const {
    uint64_t hits = 0, misses = 0, coalesced = 0, responses = 0;
    uint64_t fill_done = 0, l2_wins = 0, l2_losses = 0;
    uint64_t total_wait = 0;

    // MSHR utilization: max simultaneous active MSHRs.
    // Compute by tracking active mshr_ids over time.
    uint32_t max_mshr_concurrent = 0;
    uint32_t cur_mshr = 0;
    std::unordered_map<uint32_t, bool> active_mshr;

    for (const auto& ev : mEvents) {
        switch (ev.type) {
            case CacheEventType::Hit:
                ++hits;
                break;
            case CacheEventType::Miss:
                ++misses;
                break;
            case CacheEventType::Coalesce:
                ++coalesced;
                break;
            case CacheEventType::MshrAlloc:
                active_mshr[ev.mshr_id] = true;
                ++cur_mshr;
                max_mshr_concurrent = std::max(max_mshr_concurrent, cur_mshr);
                break;
            case CacheEventType::FillDone:
                ++fill_done;
                if (active_mshr.count(ev.mshr_id)) {
                    active_mshr.erase(ev.mshr_id);
                    --cur_mshr;
                }
                break;
            case CacheEventType::Response:
                ++responses;
                break;
            case CacheEventType::L2ArbWin:
                ++l2_wins;
                break;
            case CacheEventType::L2ArbLose:
                ++l2_losses;
                total_wait += ev.wait_cycles;
                break;
            default:
                break;
        }
    }

    uint64_t total = hits + misses;
    double hit_rate = total > 0 ? 100.0 * hits / total : 0.0;

    os << "\n── " << mName << " summary ──────────────────────────────────────\n";
    os << std::fixed << std::setprecision(1) << "  Requests: " << total << "  hits=" << hits << " (" << hit_rate << "%)"
       << "  misses=" << misses << "  coalesced=" << coalesced << "\n"
       << "  Fill completions: " << fill_done << "  Max concurrent MSHRs: " << max_mshr_concurrent << "\n";
    if (l2_wins + l2_losses > 0) {
        os << "  L2 arb wins=" << l2_wins << "  losses=" << l2_losses;
        if (l2_losses > 0) os << "  avg_wait=" << std::setprecision(1) << (static_cast<double>(total_wait) / l2_losses) << " cycles";
        os << "\n";
    }
    os << "  Total events recorded: " << mEvents.size();
    if (mMax > 0 && mEvents.size() >= mMax) os << "  (cap=" << mMax << " reached — output may be truncated)";
    os << "\n";
}

}  // namespace cpu
