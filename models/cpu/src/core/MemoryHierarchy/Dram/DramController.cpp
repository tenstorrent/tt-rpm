// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Tenstorrent USA, Inc.

#include "MemoryHierarchy/Dram/DramController.hpp"

#include <algorithm>

#include "sparta/utils/MathUtils.hpp"
#include "sparta/utils/SpartaAssert.hpp"

namespace memory::dram {

namespace {

// `now + delta`, but never earlier than the next cycle (cross-rank turnarounds can go negative).
uint64_t after(uint64_t now, int64_t delta) { return now + static_cast<uint64_t>(std::max<int64_t>(1, delta)); }

void atLeast(uint64_t& field, uint64_t t) { field = std::max(field, t); }

}  // namespace

Controller::Controller(const Organization& org, const Timing& t, const SchedConfig& sched)
    : mOrg(org),
      mT(t),
      mSched(sched) {
    for (uint32_t n : {org.channels, org.ranks, org.bank_groups, org.banks_per_group, org.row_bytes, org.line_bytes}) {
        sparta_assert(n > 0 && sparta::utils::is_power_of_2(n), "DRAM channels, ranks, bank_groups, banks_per_group and row_bytes must be powers of two");
    }
    sparta_assert(org.row_bytes >= org.line_bytes, "DRAM row_bytes must be at least one line");
    sparta_assert(sched.read_queue_capacity > 0 && sched.write_queue_capacity > 0, "DRAM queue capacities must be non-zero");
    sparta_assert(sched.write_low_watermark < sched.write_high_watermark && sched.write_high_watermark <= sched.write_queue_capacity,
                  "DRAM write watermarks must satisfy low < high <= write_queue_capacity");
    sparta_assert(t.tBURST > 0 && t.tCCD_S >= t.tBURST && t.tCCD_L >= t.tCCD_S, "DRAM timing must satisfy tBURST <= tCCD_S <= tCCD_L");
    sparta_assert(t.tRFC == 0 || t.tREFI > t.tRFC, "DRAM tREFI must exceed tRFC (or set tRFC = 0 to disable refresh)");

    mLineShift = sparta::utils::floor_log2(org.line_bytes);
    mLinesPerRow = org.row_bytes / org.line_bytes;
    mChannels.resize(org.channels);
    const uint32_t num_ranks = org.channels * org.ranks;
    uint32_t i = 0;
    for (auto& ch : mChannels) {
        ch.ranks.resize(org.ranks);
        for (auto& r : ch.ranks) {
            r.banks.resize(org.bank_groups * org.banks_per_group);
            r.next_refresh = static_cast<uint64_t>(t.tREFI) * ++i / num_ranks;  // stagger ranks
        }
    }
}

// From the line offset up: channel, bank group, column, bank, rank, row. Channel and bank group
// sit lowest so a sequential stream alternates them (tCCD_S, not tCCD_L); bank and bank group are
// XORed with low row bits (permutation interleaving, Zhang, Zhu and Zhang, MICRO 2000).
Controller::Coord Controller::decode(uint64_t addr) const {
    uint64_t la = addr >> mLineShift;
    auto take = [&la](uint32_t n) {
        const uint64_t v = la & (n - 1);
        la >>= sparta::utils::floor_log2(n);
        return static_cast<uint32_t>(v);
    };
    Coord c;
    c.channel = take(mOrg.channels);
    uint32_t bg = take(mOrg.bank_groups);
    take(mLinesPerRow);
    uint32_t bank = take(mOrg.banks_per_group);
    c.rank = take(mOrg.ranks);
    c.row = la;
    bank ^= static_cast<uint32_t>(c.row & (mOrg.banks_per_group - 1));
    bg ^= static_cast<uint32_t>((c.row >> sparta::utils::floor_log2(mOrg.banks_per_group)) & (mOrg.bank_groups - 1));
    c.bank_group = bg;
    c.bank = bg * mOrg.banks_per_group + bank;
    return c;
}

bool Controller::canAccept(uint64_t addr, bool is_write) const {
    const Channel& ch = mChannels[decode(addr).channel];
    return is_write ? ch.wq.size() < mSched.write_queue_capacity : ch.rq.size() < mSched.read_queue_capacity;
}

void Controller::enqueue(uint64_t addr, bool is_write, uint64_t tag, uint64_t now) {
    const Coord c = decode(addr);
    Channel& ch = mChannels[c.channel];
    const bool in_wq = std::any_of(ch.wq.begin(), ch.wq.end(), [addr](const Request& r) { return r.addr == addr; });
    if (is_write) {
        if (in_wq) {
            ++mStats.write_merges;
            return;
        }
        sparta_assert(ch.wq.size() < mSched.write_queue_capacity, "DRAM write queue overflow; caller must check canAccept()");
        ch.wq.push_back({addr, tag, c});
        return;
    }
    if (in_wq) {  // the newest data is still in the controller
        ++mStats.wq_forwards;
        if (mReadDone) mReadDone(addr, tag, now);
        return;
    }
    sparta_assert(ch.rq.size() < mSched.read_queue_capacity, "DRAM read queue overflow; caller must check canAccept()");
    ch.rq.push_back({addr, tag, c});
}

uint32_t Controller::readQueueOccupancy() const {
    uint32_t n = 0;
    for (const auto& ch : mChannels) n += static_cast<uint32_t>(ch.rq.size());
    return n;
}

uint32_t Controller::writeQueueOccupancy() const {
    uint32_t n = 0;
    for (const auto& ch : mChannels) n += static_cast<uint32_t>(ch.wq.size());
    return n;
}

void Controller::tick(uint64_t now) {
    for (auto& ch : mChannels) {
        if (tryRefresh(ch, now)) continue;
        if (!ch.draining && ch.wq.size() >= mSched.write_high_watermark) {
            ch.draining = true;
            ++mStats.write_drains;
        } else if (ch.draining && ch.wq.size() <= mSched.write_low_watermark) {
            ch.draining = false;
        }
        // Writes go out in drains, or when no read is waiting; never interleaved with reads.
        if (ch.draining || ch.rq.empty())
            trySchedule(ch, ch.wq, true, now);
        else
            trySchedule(ch, ch.rq, false, now);
    }
}

bool Controller::tryRefresh(Channel& ch, uint64_t now) {
    if (mT.tRFC == 0) return false;
    for (uint32_t ri = 0; ri < ch.ranks.size(); ++ri) {
        Rank& r = ch.ranks[ri];
        if (!r.refresh_pending && now >= r.next_refresh) r.refresh_pending = true;
        if (!r.refresh_pending) continue;
        bool all_closed = true;
        bool ready = true;
        for (uint32_t bi = 0; bi < r.banks.size(); ++bi) {
            const Bank& b = r.banks[bi];
            if (b.open) {
                all_closed = false;
                if (now >= b.next_pre) {
                    issuePre(ch, ri, bi, now);
                    return true;
                }
            }
            ready = ready && now >= b.next_act;
        }
        if (all_closed && ready) {
            for (auto& b : r.banks) b.next_act = now + mT.tRFC;
            r.next_refresh += mT.tREFI;
            r.refresh_pending = false;
            ++mStats.refreshes;
            return true;
        }
    }
    return false;
}

bool Controller::rowHasPendingHit(const std::deque<Request>& q, const Coord& c) const {
    const uint64_t open_row = mChannels[c.channel].ranks[c.rank].banks[c.bank].row;
    return std::any_of(q.begin(), q.end(), [&](const Request& r) { return r.c.rank == c.rank && r.c.bank == c.bank && r.c.row == open_row; });
}

bool Controller::canActivate(const Rank& r, const Bank& b, uint64_t now) const {
    return now >= b.next_act && (r.act_window.size() < 4 || now >= r.act_window.front() + mT.tFAW);
}

void Controller::classify(Request& req, uint64_t* counter) {
    if (req.classified) return;
    req.classified = true;
    ++*counter;
}

// FR-FCFS: the oldest request whose column command is ready (a row hit) goes first; otherwise
// the oldest request whose ACT or PRE is ready. A row with pending hits is not closed.
bool Controller::trySchedule(Channel& ch, std::deque<Request>& q, bool is_write, uint64_t now) {
    for (auto it = q.begin(); it != q.end(); ++it) {
        const Coord& c = it->c;
        if (ch.ranks[c.rank].refresh_pending) continue;
        const Bank& b = ch.ranks[c.rank].banks[c.bank];
        if (b.open && b.row == c.row && now >= (is_write ? b.next_wr : b.next_rd)) {
            classify(*it, &mStats.row_hits);
            issueColumn(ch, c, is_write, now);
            if (is_write) {
                ++mStats.writes;
            } else {
                ++mStats.reads;
                if (mReadDone) mReadDone(it->addr, it->tag, now + mT.tCL + mT.tBURST);
            }
            q.erase(it);
            return true;
        }
    }
    for (auto& req : q) {
        const Coord& c = req.c;
        Rank& r = ch.ranks[c.rank];
        if (r.refresh_pending) continue;
        const Bank& b = r.banks[c.bank];
        if (b.open && b.row == c.row) continue;  // row hit waiting on column timing
        if (!b.open) {
            if (canActivate(r, b, now)) {
                classify(req, &mStats.row_misses);
                issueAct(ch, c, now);
                return true;
            }
        } else if (now >= b.next_pre && !rowHasPendingHit(q, c)) {
            classify(req, &mStats.row_conflicts);
            issuePre(ch, c.rank, c.bank, now);
            return true;
        }
    }
    return false;
}

void Controller::issueAct(Channel& ch, const Coord& c, uint64_t now) {
    Rank& r = ch.ranks[c.rank];
    Bank& b = r.banks[c.bank];
    b.open = true;
    b.row = c.row;
    atLeast(b.next_rd, now + mT.tRCD);
    atLeast(b.next_wr, now + mT.tRCD);
    atLeast(b.next_pre, now + mT.tRAS);
    for (uint32_t i = 0; i < r.banks.size(); ++i) {
        const bool same_bg = i / mOrg.banks_per_group == c.bank_group;
        atLeast(r.banks[i].next_act, now + (i == c.bank ? mT.tRAS + mT.tRP : same_bg ? mT.tRRD_L : mT.tRRD_S));
    }
    r.act_window.push_back(now);
    if (r.act_window.size() > 4) r.act_window.pop_front();
    ++mStats.activates;
}

void Controller::issuePre(Channel& ch, uint32_t rank, uint32_t bank, uint64_t now) {
    Bank& b = ch.ranks[rank].banks[bank];
    b.open = false;
    atLeast(b.next_act, now + mT.tRP);
    ++mStats.precharges;
}

void Controller::issueColumn(Channel& ch, const Coord& c, bool is_write, uint64_t now) {
    const int64_t cl = mT.tCL, cwl = mT.tCWL, burst = mT.tBURST, rtrs = mT.tRTRS;
    for (uint32_t ri = 0; ri < ch.ranks.size(); ++ri) {
        for (uint32_t i = 0; i < ch.ranks[ri].banks.size(); ++i) {
            Bank& o = ch.ranks[ri].banks[i];
            if (ri == c.rank) {
                const bool same_bg = i / mOrg.banks_per_group == c.bank_group;
                const uint32_t ccd = same_bg ? mT.tCCD_L : mT.tCCD_S;
                if (is_write) {
                    atLeast(o.next_wr, now + ccd);
                    atLeast(o.next_rd, now + mT.tCWL + mT.tBURST + (same_bg ? mT.tWTR_L : mT.tWTR_S));
                } else {
                    atLeast(o.next_rd, now + ccd);
                    atLeast(o.next_wr, after(now, cl + burst + 2 - cwl));
                }
            } else if (is_write) {
                atLeast(o.next_wr, after(now, burst + rtrs));
                atLeast(o.next_rd, after(now, cwl + burst + rtrs - cl));
            } else {
                atLeast(o.next_rd, after(now, burst + rtrs));
                atLeast(o.next_wr, after(now, cl + burst + rtrs - cwl));
            }
        }
    }
    Bank& b = ch.ranks[c.rank].banks[c.bank];
    atLeast(b.next_pre, now + (is_write ? mT.tCWL + mT.tBURST + mT.tWR : mT.tRTP));
    mStats.data_bus_busy += mT.tBURST;
}

}  // namespace memory::dram
