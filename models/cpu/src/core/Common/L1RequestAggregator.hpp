// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Tenstorrent USA, Inc.

#pragma once

#include "sparta/ports/DataPort.hpp"
#include "sparta/simulation/Unit.hpp"

#include "Logging.hpp"
#include "MemoryHierarchy/L2Cache.hpp"
#include "models/cpu/common/MemInterface.hpp"

namespace core {

// Per-core merge point below both the private L2 and the shared cluster cache. It takes
// the two L1 streams (I-side, D-side) on separate slave faces and presents one tagged
// upstream master, so the next level is reached over a single port regardless of which L1
// missed. The source rides in the packet; responses/invalidates are demuxed back by it.
// Whether the upstream binds to the L2 or straight to the cluster cache is pure wiring.
class L1RequestAggregator : public sparta::Unit {
   public:
    explicit L1RequestAggregator(sparta::TreeNode* node)
        : sparta::Unit(node),
          mL1I(&unit_port_set_, "l1i"),
          mL1D(&unit_port_set_, "l1d"),
          mUp(&unit_port_set_, "up") {
        mL1I.req_in.registerConsumerHandler(CREATE_SPARTA_HANDLER_WITH_DATA(L1RequestAggregator, handleIReq, mem::Request));
        mL1D.req_in.registerConsumerHandler(CREATE_SPARTA_HANDLER_WITH_DATA(L1RequestAggregator, handleDReq, mem::Request));
        mL1D.wb_in.registerConsumerHandler(CREATE_SPARTA_HANDLER_WITH_DATA(L1RequestAggregator, handleWb, mem::Writeback));
        mUp.resp_in.registerConsumerHandler(CREATE_SPARTA_HANDLER_WITH_DATA(L1RequestAggregator, handleResp, mem::Response));
        mUp.inval_in.registerConsumerHandler(CREATE_SPARTA_HANDLER_WITH_DATA(L1RequestAggregator, handleInval, mem::Invalidate));
    }

    // Slave face for the I-cache / D-cache (each L1 binds its fill ports here).
    mem::MemSlave& l1Slave(mem::CacheSource src) { return src == mem::CacheSource::ICACHE ? mL1I : mL1D; }
    // Upstream master toward the next level (L2 slave, or cluster-cache slave).
    mem::MemMaster& up() { return mUp; }

    // Backpressure: an L1 may only issue a fill the next level can accept. With a private
    // L2 below, defer to its per-source queue; with no L2, the cluster cache always accepts.
    void setL2(memory::L2Cache* l2) { mL2 = l2; }
    bool canAccept(mem::CacheSource src) const { return mL2 ? mL2->canAccept(src) : true; }
    bool reserveWriteback() { return mL2 ? mL2->reserveWriteback() : true; }
    bool hasL2() const { return mL2 != nullptr; }

    void setRequesterId(uint32_t id) { mRequesterId = id; }

   private:
    void forward(const mem::Request& r, mem::CacheSource src) {
        mem::Request f = r;
        f.source = src;
        f.requester_id = mRequesterId;
        mUp.req_out.send(f, 1);
    }
    void handleIReq(const mem::Request& r) { forward(r, mem::CacheSource::ICACHE); }
    void handleDReq(const mem::Request& r) { forward(r, mem::CacheSource::DCACHE); }

    void handleWb(const mem::Writeback& r) {
        mem::Writeback w = r;
        w.source = mem::CacheSource::DCACHE;
        w.requester_id = mRequesterId;
        mUp.wb_out.send(w, 1);
    }

    void handleResp(const mem::Response& r) { l1Slave(r.source).resp_out.send(r, 1); }

    void handleInval(const mem::Invalidate& r) {
        mL1I.inval_out.send(r, 1);
        mL1D.inval_out.send(r, 1);
        // Acknowledge the snoop to the home node. Invalidation is applied synchronously in the
        // L1 handlers above, so the ack is exact; it lets the HN serialize a write against its
        // outstanding snoops (the SNPRSP the directory waits on). txn_id echoes the snoop.
        mem::SnoopResponse ack;
        ack.line_addr = r.line_addr;
        ack.requester_id = mRequesterId;
        ack.txn_id = r.txn_id;
        ack.final_state = mem::LineState::I;
        mUp.snprsp_out.send(ack, 1);
    }

    mem::MemSlave mL1I;
    mem::MemSlave mL1D;
    mem::MemMaster mUp;
    memory::L2Cache* mL2{nullptr};
    uint32_t mRequesterId{0};
};

}  // namespace core
