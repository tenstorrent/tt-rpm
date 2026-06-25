#pragma once

#include "sparta/ports/DataPort.hpp"
#include "sparta/simulation/ParameterSet.hpp"
#include "sparta/simulation/Unit.hpp"
#include "sparta/statistics/Counter.hpp"

#include "Common/PipelinePacket.hpp"

namespace core {

class FlushArbiterParams : public sparta::ParameterSet {
   public:
    FlushArbiterParams(sparta::TreeNode* n)
        : sparta::ParameterSet(n) {}

    PARAMETER(bool, log_enabled, false, "Enable debug logging for this unit")
};

class FlushArbiter : public sparta::Unit {
   public:
    static constexpr char name[] = "flush_arbiter";

    FlushArbiter(sparta::TreeNode* node, const FlushArbiterParams* params);

    // Input ports from BP and Execute
    sparta::DataInPort<FlushRequest> bp_flush_in{&unit_port_set_, "bp_flush_in"};
    sparta::DataInPort<FlushRequest> exe_flush_in{&unit_port_set_, "exe_flush_in"};
    // Trap/interrupt redirect from Writeback (full recovery, like an Execute flush)
    sparta::DataInPort<FlushRequest> retire_flush_in{&unit_port_set_, "retire_flush_in"};

    // Output ports to frontend units (BP flush path - lightweight redirect)
    sparta::DataOutPort<FlushRequest> flush_to_fq_out{&unit_port_set_, "flush_to_fq_out"};
    sparta::DataOutPort<FlushRequest> flush_to_icache_out{&unit_port_set_, "flush_to_icache_out"};
    sparta::DataOutPort<PredictedRedirect> redirect_to_fetch_out{&unit_port_set_, "redirect_to_fetch_out"};

    // Output ports for full misprediction recovery (Execute flush path)
    sparta::DataOutPort<FlushRequest> flush_to_decode_out{&unit_port_set_, "flush_to_decode_out"};
    sparta::DataOutPort<FlushRequest> flush_to_decode_queue_out{&unit_port_set_, "flush_to_decode_queue_out"};
    sparta::DataOutPort<FlushRequest> flush_to_issue_out{&unit_port_set_, "flush_to_issue_out"};
    sparta::DataOutPort<FlushRequest> flush_to_execute_out{&unit_port_set_, "flush_to_execute_out"};
    sparta::DataOutPort<FlushRequest> flush_to_lsq_out{&unit_port_set_, "flush_to_lsq_out"};
    sparta::DataOutPort<FlushRequest> flush_to_rename_out{&unit_port_set_, "flush_to_rename_out"};
    sparta::DataOutPort<FlushRequest> flush_to_writeback_out{&unit_port_set_, "flush_to_writeback_out"};
    sparta::DataOutPort<BranchRedirect> mispred_redirect_to_fetch_out{&unit_port_set_, "mispred_redirect_to_fetch_out"};

    // Called by PipelineClock each cycle to process pending flushes
    void tick();

    uint64_t numBpFlushes() const { return mNumBpFlushes.get(); }
    uint64_t numExeFlushes() const { return mNumExeFlushes.get(); }
    uint64_t numDroppedBpFlushes() const { return mNumDroppedBpFlushes.get(); }

   private:
    void receiveBpFlush_(const FlushRequest& req);
    void receiveExeFlush_(const FlushRequest& req);
    void processFlush_(const FlushRequest& req);

    // Only allow one flush per cycle.
    uint64_t mLastFlushCycle{0};

    bool mLogEnabled{false};

    sparta::Counter mNumBpFlushes;
    sparta::Counter mNumExeFlushes;
    sparta::Counter mNumDroppedBpFlushes;
};

}  // namespace core
