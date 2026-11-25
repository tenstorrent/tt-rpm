// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Tenstorrent USA, Inc.

#pragma once

#include "models/cpu/common/Coherence.hpp"

// Home-Node MSI protocol engine (tag-only; Whisper is the data oracle).
//
//   * ReadShared (load/ifetch fill): record the requester as a sharer, grant S. A line dirty at
//     a remote owner is invalidated first (MSI has no clean-shared downgrade without data, so the
//     owner drops it and refills); from the owner itself the line stays M.
//   * ReadUnique / CleanUnique (store-miss fill / store-to-clean upgrade): invalidate every OTHER
//     sharer, grant the requester M. CleanUnique completes with CompUpgrade (no data); the
//     requester's write waits for it.
//   * ReadOnce / WriteUnique (non-caching requester): as ReadShared / ReadUnique, but the requester
//     is never recorded, so no later access snoops it.
//   * Writeback: the RN dropped the line -- clear its sharer bit and ownership, unless keep_sharer
//     is set (one of its caches may still hold the line, or a snoop already updated the entry).
//
// The engine only decides transitions and which snoops to emit; the Home Node (ClusterCache)
// owns the DirEntry storage, sends the snoops, and returns the response.
namespace mem {

class MsiProtocolEngine : public ProtocolEngine {
   public:
    Reaction onRequest(RnId id, const Request& req, DirEntry& e) override {
        Reaction r;
        fillResponseHeader(r.response, id, req);
        const uint64_t self = bit(id);

        switch (req.opcode) {
            case ReqOpcode::ReadShared:
                if (e.state == LineState::M && e.owner != id && e.owner != kInvalidRequesterId) {
                    snoopInvalidate(r, req, e.owner);
                    e.sharers &= ~bit(e.owner);
                }
                // From the owner itself (its I-cache, or its L2 after a non-inclusive eviction): its
                // D-cache may still hold the line dirty, so it stays the owner for later readers to snoop.
                if (e.owner != id) {
                    e.owner = kInvalidRequesterId;
                    e.state = LineState::S;
                }
                e.sharers |= self;
                r.response.resp_opcode = RespOpcode::CompData;
                r.response.granted_state = LineState::S;
                break;

            case ReqOpcode::ReadUnique:
                snoopOthers(r, req, e, id);
                e.sharers = self;
                e.owner = id;
                e.state = LineState::M;
                r.response.resp_opcode = RespOpcode::CompData;
                r.response.granted_state = LineState::M;
                break;

            case ReqOpcode::CleanUnique:
                snoopOthers(r, req, e, id);
                e.sharers = self;
                e.owner = id;
                e.state = LineState::M;
                r.response.resp_opcode = RespOpcode::CompUpgrade;
                r.response.granted_state = LineState::M;
                break;

            case ReqOpcode::ReadOnce:
                if (e.state == LineState::M && e.owner != kInvalidRequesterId) {
                    snoopInvalidate(r, req, e.owner);
                    e.sharers &= ~bit(e.owner);
                    e.owner = kInvalidRequesterId;
                    e.state = e.sharers ? LineState::S : LineState::I;
                }
                r.response.resp_opcode = RespOpcode::CompData;
                r.response.granted_state = LineState::I;
                break;

            case ReqOpcode::WriteUnique:
                snoopOthers(r, req, e, id);
                e.sharers = 0;
                e.owner = kInvalidRequesterId;
                e.state = LineState::I;
                r.response.resp_opcode = RespOpcode::CompAck;
                r.response.granted_state = LineState::I;
                break;
        }
        return r;
    }

    // ClusterCache tracks snoop acks itself (its held-grant table), so this is never driven.
    Reaction onSnoopResponse(RnId, const SnoopResponse&, DirEntry&) override {
        Reaction r;
        r.send_response = false;
        return r;
    }

    void onWriteback(RnId id, const Writeback& wb, DirEntry& e) override {
        if (wb.keep_sharer) return;
        e.sharers &= ~bit(id);
        if (e.owner == id) e.owner = kInvalidRequesterId;
        if (e.sharers == 0) e.state = LineState::I;
    }

   private:
    static uint64_t bit(RnId id) { return uint64_t(1) << id; }

    static void fillResponseHeader(Response& resp, RnId id, const Request& req) {
        resp.line_addr = req.line_addr;
        resp.requester_id = id;
        resp.source = req.source;
        resp.txn_id = req.txn_id;
    }

    // Queue a SnpInvalidate to one RN.
    static void snoopInvalidate(Reaction& r, const Request& req, RnId target) {
        Snoop s;
        s.line_addr = req.line_addr;
        s.requester_id = target;  // the RN to snoop
        s.source = req.source;
        s.txn_id = req.txn_id;
        s.opcode = SnoopOpcode::SnpInvalidate;
        r.snoops.push_back(s);
    }

    // Invalidate every current sharer/owner except the requester.
    static void snoopOthers(Reaction& r, const Request& req, const DirEntry& e, RnId id) {
        uint64_t others = e.sharers & ~bit(id);
        for (RnId t = 0; others != 0; ++t, others >>= 1)
            if (others & 1) snoopInvalidate(r, req, t);
    }
};

}  // namespace mem
