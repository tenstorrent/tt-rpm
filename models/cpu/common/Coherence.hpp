// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Tenstorrent USA, Inc.

#pragma once

#include <cstdint>
#include <vector>

#include "models/cpu/common/MemInterface.hpp"

// Home-Node coherence interface for the cluster. Transport (ports/packets/opcodes) lives in
// MemInterface.hpp; protocol transitions live in a pluggable ProtocolEngine, so another protocol
// is a new engine behind the same ports. NullProtocolEngine is the tag-only default (no
// directory, no snoops).
namespace mem {

using RnId = uint32_t;  // request-node (core) index; indexes the sharer bitvector

// Per-line coherence metadata held at the Home Node, inclusive of all L1s. Tag-only: Whisper
// is the data oracle, so this tracks residency/ownership, never bytes.
struct DirEntry {
    LineState state{LineState::I};
    uint64_t sharers{0};              // bitvector[num_RN]; ample for small clusters (<=64 cores)
    RnId owner{kInvalidRequesterId};  // valid iff state == M (or E/O in richer protocols)
};

// What a protocol engine asks the Home Node to do for one event: emit some snoops, then send
// one response to the requester once those snoops are acknowledged on SNPRSP. Tag-only, so no
// data ever moves; "dirty forwarding" is a latency, not a transfer.
struct Reaction {
    std::vector<Snoop> snoops;  // SNP messages to emit to other RNs
    Response response;          // RSP to send to the requester
    bool send_response{true};   // false => defer until outstanding snoops resolve
};

// Home-Node coherence protocol engine (pluggable). All protocol-specific transitions live here.
class ProtocolEngine {
   public:
    virtual ~ProtocolEngine() = default;

    // A request arrived on REQ from RN `id`; may mutate `entry`; returns the reaction.
    virtual Reaction onRequest(RnId id, const Request& req, DirEntry& entry) = 0;

    // A snoop response arrived on SNPRSP from RN `id`; advance the transaction for its line.
    virtual Reaction onSnoopResponse(RnId id, const SnoopResponse& rsp, DirEntry& entry) = 0;

    // An RN wrote back / evicted a line (WB channel).
    virtual void onWriteback(RnId id, const Writeback& wb, DirEntry& entry) = 0;
};

// Selected when coherence is disabled: no directory effects, no snoops.
class NullProtocolEngine : public ProtocolEngine {
   public:
    Reaction onRequest(RnId, const Request& req, DirEntry&) override {
        Reaction r;
        r.response.line_addr = req.line_addr;
        r.response.requester_id = req.requester_id;
        r.response.source = req.source;
        r.response.txn_id = req.txn_id;
        return r;  // plain grant, no snoops
    }
    Reaction onSnoopResponse(RnId, const SnoopResponse&, DirEntry&) override {
        Reaction r;
        r.send_response = false;
        return r;
    }
    void onWriteback(RnId, const Writeback&, DirEntry&) override {}
};

}  // namespace mem
