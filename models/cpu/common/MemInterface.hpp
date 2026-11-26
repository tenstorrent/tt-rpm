// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Tenstorrent USA, Inc.

#pragma once

#include <cstdint>
#include <ios>
#include <memory>
#include <ostream>
#include <string>

#include "sparta/ports/DataPort.hpp"
#include "sparta/ports/SyncPort.hpp"
#include "sparta/simulation/Clock.hpp"
#include "sparta/simulation/TreeNode.hpp"

// The one memory interface used at every cache boundary (L1 <-> private L2, L1/L2 <-> shared
// cluster cache): the packet family plus the master/slave port bundles. Identity is
// (requester_id, source, txn_id); the address is payload, never identity. A cache in the
// middle of the hierarchy is a MemSlave to the level above and a MemMaster to the level
// below, so levels compose (LSU -> L1 -> [L2 ->] ClusterCache) and toggling a level is pure
// wiring. Nothing here is core- or cluster-specific.
namespace mem {

using addr_t = uint64_t;

// Sentinel for "not associated with a particular requester".
inline constexpr uint32_t kInvalidRequesterId = 0xFFFFFFFFu;

enum class CacheSource : uint8_t { ICACHE = 0, DCACHE = 1 };

// ---- Coherence vocabulary (protocol-agnostic) -------------------------------
// These opcodes/states are the wire vocabulary the coherence framework moves; they encode no
// protocol transitions (those live in the pluggable engine -- see Coherence.hpp). A richer
// protocol adds values without changing any port.

// REQ channel (RN -> HN): what the requester wants for a line.
enum class ReqOpcode : uint8_t {
    ReadShared,   // load miss: want a readable copy
    ReadUnique,   // store miss / RFO: want a writable copy
    CleanUnique,  // store hit on a clean(S) line: upgrade to writable, no fill
    ReadOnce,     // non-caching read (e.g. DMA): the requester keeps no copy, so no sharer is recorded
    WriteUnique,  // non-caching write: invalidate every cached copy, the requester keeps none
};

// RSP channel (HN -> RN): how a request was completed.
enum class RespOpcode : uint8_t { CompData, CompUpgrade, CompAck };

// SNP channel (HN -> RN): a coherence action asked of a sharer/owner.
enum class SnoopOpcode : uint8_t {
    SnpInvalidate,  // -> I   (invalidating snoop for a writer)   [MSI]
};

// Per-line coherence state (granted_state on RSP, final_state on SNPRSP). MSI uses {I,S,M}.
enum class LineState : uint8_t { I, S, E, M };

// ---- Packets ----------------------------------------------------------------

// REQ, RN -> HN: fetch/allocate a line, carrying the coherence intent (opcode).
struct Request {
    uint64_t line_addr{0};
    uint32_t requester_id{0};  // originating private hierarchy (cluster routing)
    CacheSource source{CacheSource::ICACHE};
    uint64_t txn_id{0};
    ReqOpcode opcode{ReqOpcode::ReadShared};
    bool prefetch{false};       // prefetch hint to fill the receiving level only; no response
    bool from_prefetch{false};  // an upper level's prefetch miss: answered, but not a demand access here
    uint64_t rn_seq{0};         // send order at the RN's cluster bridge, shared with its writebacks
    // Convenience for tag-only sites that only care read-vs-write intent.
    bool isWrite() const { return opcode == ReqOpcode::ReadUnique || opcode == ReqOpcode::CleanUnique || opcode == ReqOpcode::WriteUnique; }
};

// RSP, HN -> RN: a requested line is ready (no data: Whisper is the oracle).
struct Response {
    uint64_t line_addr{0};
    uint32_t requester_id{0};
    CacheSource source{CacheSource::ICACHE};
    uint64_t txn_id{0};
    RespOpcode resp_opcode{RespOpcode::CompData};  // completion kind
    LineState granted_state{LineState::S};         // coherence state granted to the requester
};

// WB, RN -> HN: a dirty line was evicted.
struct Writeback {
    uint64_t line_addr{0};
    uint32_t requester_id{0};
    CacheSource source{CacheSource::DCACHE};
    uint64_t txn_id{0};
    bool reserved{false};  // holds the victim-buffer entry the L1's writeback gate reserved
    // The home leaves its directory entry alone: a cache nearer the core may still hold the line, or
    // this is a snooped line's data and the snooping request already updated the entry.
    bool keep_sharer{false};
    uint64_t rn_seq{0};  // send order at the RN's cluster bridge, shared with its requests
};

// SNP, HN -> RN: act on this line (inclusive back-invalidation / coherence downgrade).
struct Snoop {
    uint64_t line_addr{0};
    uint32_t requester_id{0};
    CacheSource source{CacheSource::ICACHE};
    uint64_t txn_id{0};
    SnoopOpcode opcode{SnoopOpcode::SnpInvalidate};
};
using Invalidate = Snoop;  // name used by the private caches, which only act on SnpInvalidate

// SNPRSP, RN -> HN: acknowledges a snoop once applied (lets the HN serialize a write against
// outstanding snoops).
struct SnoopResponse {
    uint64_t line_addr{0};
    uint32_t requester_id{0};
    uint64_t txn_id{0};
    LineState final_state{LineState::I};  // state the RN's line ended in
    bool was_dirty{false};                // the snooped copy was dirty (owner had M)
};

inline std::ostream& operator<<(std::ostream& os, const Request& r) {
    return os << "mem::Request{line=0x" << std::hex << r.line_addr << std::dec << ", req=" << r.requester_id << (r.isWrite() ? ", wr" : ", rd") << "}";
}
inline std::ostream& operator<<(std::ostream& os, const Response& r) {
    return os << "mem::Response{line=0x" << std::hex << r.line_addr << std::dec << ", req=" << r.requester_id << "}";
}
inline std::ostream& operator<<(std::ostream& os, const Writeback& r) {
    return os << "mem::Writeback{line=0x" << std::hex << r.line_addr << std::dec << ", req=" << r.requester_id << "}";
}
inline std::ostream& operator<<(std::ostream& os, const Snoop& r) {
    return os << "mem::Snoop{line=0x" << std::hex << r.line_addr << std::dec << ", req=" << r.requester_id << "}";
}
inline std::ostream& operator<<(std::ostream& os, const SnoopResponse& r) {
    return os << "mem::SnoopResponse{line=0x" << std::hex << r.line_addr << std::dec << ", req=" << r.requester_id << "}";
}

// ---- Port bundles -----------------------------------------------------------

// The requesting side (Request Node / faces the next level toward memory): drives REQ +
// writebacks + snoop-responses out, receives RSP + snoops (SNP) in.
struct MemMaster {
    sparta::DataOutPort<Request> req_out;           // REQ
    sparta::DataInPort<Response> resp_in;           // RSP
    sparta::DataOutPort<Writeback> wb_out;          // WB
    sparta::DataInPort<Snoop> inval_in;             // SNP  (in: HN -> RN)
    sparta::DataOutPort<SnoopResponse> snprsp_out;  // SNPRSP (out: RN -> HN)

    MemMaster(sparta::TreeNode* ps, const std::string& prefix)
        : req_out(ps, prefix + "_req_out"),
          resp_in(ps, prefix + "_resp_in"),
          wb_out(ps, prefix + "_wb_out"),
          inval_in(ps, prefix + "_inval_in"),
          snprsp_out(ps, prefix + "_snprsp_out") {}
};

// The serving side (Home Node / faces the previous level toward the core): receives REQ +
// writebacks + snoop-responses, drives RSP + snoops (SNP).
struct MemSlave {
    sparta::DataInPort<Request> req_in;           // REQ
    sparta::DataOutPort<Response> resp_out;       // RSP
    sparta::DataInPort<Writeback> wb_in;          // WB
    sparta::DataOutPort<Snoop> inval_out;         // SNP  (out: HN -> RN)
    sparta::DataInPort<SnoopResponse> snprsp_in;  // SNPRSP (in: RN -> HN)

    MemSlave(sparta::TreeNode* ps, const std::string& prefix)
        : req_in(ps, prefix + "_req_in"),
          resp_out(ps, prefix + "_resp_out"),
          wb_in(ps, prefix + "_wb_in"),
          inval_out(ps, prefix + "_inval_out"),
          snprsp_in(ps, prefix + "_snprsp_in") {}
};

// Standardized binding: connect a master (requesting level) to a slave (serving level).
inline void connect(MemMaster& m, MemSlave& s) {
    m.req_out.bind(s.req_in);
    s.resp_out.bind(m.resp_in);
    m.wb_out.bind(s.wb_in);
    s.inval_out.bind(m.inval_in);
    m.snprsp_out.bind(s.snprsp_in);
}

// ---- Clock-domain-crossing variants ----------------------------------------
// Same channels as MemMaster/MemSlave but on SyncInPort/SyncOutPort, so the two ends may
// sit on clocks of different frequency (a core's cpu_freq vs the shared-cache clock).
// Ports are heap-built so the owner can construct them once its clock is known; each
// SyncInPort carries a fixed crossing delay.
struct SyncMemMaster {
    std::unique_ptr<sparta::SyncOutPort<Request>> req_out;
    std::unique_ptr<sparta::SyncInPort<Response>> resp_in;
    std::unique_ptr<sparta::SyncOutPort<Writeback>> wb_out;
    std::unique_ptr<sparta::SyncInPort<Snoop>> inval_in;
    std::unique_ptr<sparta::SyncOutPort<SnoopResponse>> snprsp_out;

    SyncMemMaster(sparta::TreeNode* ps, const std::string& prefix, const sparta::Clock* clk, sparta::Clock::Cycle cross_delay = 1)
        : req_out(std::make_unique<sparta::SyncOutPort<Request>>(ps, prefix + "_req_out", clk)),
          resp_in(std::make_unique<sparta::SyncInPort<Response>>(ps, prefix + "_resp_in", clk)),
          wb_out(std::make_unique<sparta::SyncOutPort<Writeback>>(ps, prefix + "_wb_out", clk)),
          inval_in(std::make_unique<sparta::SyncInPort<Snoop>>(ps, prefix + "_inval_in", clk)),
          snprsp_out(std::make_unique<sparta::SyncOutPort<SnoopResponse>>(ps, prefix + "_snprsp_out", clk)) {
        resp_in->setPortDelay(cross_delay);
        inval_in->setPortDelay(cross_delay);
    }
};

struct SyncMemSlave {
    std::unique_ptr<sparta::SyncInPort<Request>> req_in;
    std::unique_ptr<sparta::SyncOutPort<Response>> resp_out;
    std::unique_ptr<sparta::SyncInPort<Writeback>> wb_in;
    std::unique_ptr<sparta::SyncOutPort<Snoop>> inval_out;
    std::unique_ptr<sparta::SyncInPort<SnoopResponse>> snprsp_in;

    SyncMemSlave(sparta::TreeNode* ps, const std::string& prefix, const sparta::Clock* clk, sparta::Clock::Cycle cross_delay = 1)
        : req_in(std::make_unique<sparta::SyncInPort<Request>>(ps, prefix + "_req_in", clk)),
          resp_out(std::make_unique<sparta::SyncOutPort<Response>>(ps, prefix + "_resp_out", clk)),
          wb_in(std::make_unique<sparta::SyncInPort<Writeback>>(ps, prefix + "_wb_in", clk)),
          inval_out(std::make_unique<sparta::SyncOutPort<Snoop>>(ps, prefix + "_inval_out", clk)),
          snprsp_in(std::make_unique<sparta::SyncInPort<SnoopResponse>>(ps, prefix + "_snprsp_in", clk)) {
        req_in->setPortDelay(cross_delay);
        wb_in->setPortDelay(cross_delay);
        snprsp_in->setPortDelay(cross_delay);
    }
};

// Bind a sync master to a sync slave (may be on different-frequency clocks).
inline void connectSync(SyncMemMaster& m, SyncMemSlave& s) {
    m.req_out->bind(s.req_in.get());
    s.resp_out->bind(m.resp_in.get());
    m.wb_out->bind(s.wb_in.get());
    s.inval_out->bind(m.inval_in.get());
    m.snprsp_out->bind(s.snprsp_in.get());
}

// Line-align an address for a given (power-of-two) line size.
inline addr_t lineAddr(addr_t a, addr_t line_size) { return a & ~(line_size - 1); }

}  // namespace mem
