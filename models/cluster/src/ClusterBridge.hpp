// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Tenstorrent USA, Inc.

#pragma once

#include <deque>

#include "sparta/events/UniqueEvent.hpp"
#include "sparta/simulation/Unit.hpp"

#include "Logging.hpp"
#include "models/cpu/common/MemInterface.hpp"

namespace cluster {

// Per-core clock-domain bridge between a core (its cpu clock) and the shared cluster
// cache (fabric clock). It lives on the core clock: the core binds its DataPort master to
// the bridge's DataPort `down` (same clock), and the bridge's SyncMemMaster `up` binds to
// the cache's SyncMemSlave to cross the frequency boundary. It just forwards, so coherence
// packets pass through unchanged.
//
// A SyncOutPort must be driven on a posedge of its clock, but a DataInPort handler can
// fire off-edge, so core->cache packets are queued and drained by an event aligned to the
// core clock's next posedge. cache->core packets use plain DataOutPorts (no such rule).
class ClusterBridge : public sparta::Unit {
   public:
    // `link_latency` is extra delay applied in each direction -- the distance between the
    // requester and the fabric. Zero for an on-die core; nonzero models a link (PCIe/CXL).
    explicit ClusterBridge(sparta::TreeNode* node, uint32_t link_latency = 0)
        : sparta::Unit(node),
          mDown(&unit_port_set_, "down"),
          mUp(&unit_port_set_, "up", getClock()),
          mLinkLatency(link_latency),
          mDrain(&unit_event_set_, "drain", CREATE_SPARTA_HANDLER(ClusterBridge, drain)) {
        mDown.req_in.registerConsumerHandler(CREATE_SPARTA_HANDLER_WITH_DATA(ClusterBridge, fwdReq, mem::Request));
        mDown.wb_in.registerConsumerHandler(CREATE_SPARTA_HANDLER_WITH_DATA(ClusterBridge, fwdWb, mem::Writeback));
        mDown.snprsp_in.registerConsumerHandler(CREATE_SPARTA_HANDLER_WITH_DATA(ClusterBridge, fwdSnpRsp, mem::SnoopResponse));
        mUp.resp_in->registerConsumerHandler(CREATE_SPARTA_HANDLER_WITH_DATA(ClusterBridge, fwdResp, mem::Response));
        mUp.inval_in->registerConsumerHandler(CREATE_SPARTA_HANDLER_WITH_DATA(ClusterBridge, fwdInval, mem::Snoop));
        mDown.wb_in.precedes(mDown.req_in);  // same cycle: the writeback is older
    }

    mem::MemSlave& down() { return mDown; }   // faces the core (DataPort, core clock)
    mem::SyncMemMaster& up() { return mUp; }  // faces the cache (sync, crosses domains)

   private:
    // core -> cache: queue, then drain on a bridge-clock posedge.
    void fwdReq(const mem::Request& r) {
        mReqQ.push_back(r);
        mReqQ.back().rn_seq = ++mSeq;
        scheduleDrain();
    }
    void fwdWb(const mem::Writeback& w) {
        mWbQ.push_back(w);
        mWbQ.back().rn_seq = ++mSeq;
        scheduleDrain();
    }
    void fwdSnpRsp(const mem::SnoopResponse& s) {
        mSnpRspQ.push_back(s);
        scheduleDrain();
    }
    // cache -> core: DataOutPorts, no posedge constraint, forward immediately.
    void fwdResp(const mem::Response& r) { mDown.resp_out.send(r, 1 + mLinkLatency); }
    void fwdInval(const mem::Snoop& s) { mDown.inval_out.send(s, 1 + mLinkLatency); }

    void scheduleDrain() {
        // Align to the core clock's next rising edge so the sync sends land on a posedge.
        // A strictly-positive delay also avoids scheduling into the currently-firing cycle
        // (a lower-priority same-cycle event, which the scheduler rejects).
        const auto* clk = getClock();
        const auto period = clk->getPeriod();
        auto to_edge = (period - (clk->currentTick() % period)) % period;
        if (to_edge == 0) to_edge = period;
        mDrain.scheduleRelativeTick(to_edge, clk->getScheduler());
    }

    void drain() {
        // One packet per port per cycle (the port sends once per cycle); reschedule while
        // a backlog remains. allow_slide slides a colliding arrival to the next free cycle.
        const uint32_t d = 1 + mLinkLatency;
        if (!mReqQ.empty()) {
            mUp.req_out->sendAndAllowSlide(mReqQ.front(), d);
            mReqQ.pop_front();
        }
        if (!mWbQ.empty()) {
            mUp.wb_out->sendAndAllowSlide(mWbQ.front(), d);
            mWbQ.pop_front();
        }
        if (!mSnpRspQ.empty()) {
            mUp.snprsp_out->sendAndAllowSlide(mSnpRspQ.front(), d);
            mSnpRspQ.pop_front();
        }
        if (!mReqQ.empty() || !mWbQ.empty() || !mSnpRspQ.empty()) scheduleDrain();
    }

    mem::MemSlave mDown;
    mem::SyncMemMaster mUp;
    uint32_t mLinkLatency;
    uint64_t mSeq{0};
    sparta::UniqueEvent<sparta::SchedulingPhase::PortUpdate> mDrain;
    std::deque<mem::Request> mReqQ;
    std::deque<mem::Writeback> mWbQ;
    std::deque<mem::SnoopResponse> mSnpRspQ;
};

}  // namespace cluster
