#include "Frontend/BranchPredictor/BranchPredictor.hpp"

#include "Common/SpeculationConfig.hpp"
#include "Logging.hpp"

namespace frontend {

BranchPredictor::BranchPredictor(sparta::TreeNode* node, const BranchPredictorParams* params)
    : sparta::Unit(node),
      mAccuracy(params->accuracy),
      mRng(params->rng_seed),
      mDist(0.0, 1.0),
      mNumCorrect(&unit_stat_set_, "num_correct", "Correct branch predictions", sparta::Counter::COUNT_NORMAL),
      mNumMispredicted(&unit_stat_set_, "num_mispredicted", "Branch mispredictions", sparta::Counter::COUNT_NORMAL) {
    predict_in.registerConsumerHandler(CREATE_SPARTA_HANDLER_WITH_DATA(BranchPredictor, receiveRequest, core::BranchPrediction));
}

void BranchPredictor::receiveRequest(const core::BranchPrediction& req) {
    core::BranchPrediction result = req;

    bool correct = mDist(mRng) < mAccuracy;

    if (correct) {
        result.mispredicted = false;
        ++mNumCorrect;
    } else {
        result.predicted_taken = !result.predicted_taken;
        result.mispredicted = true;
        ++mNumMispredicted;

        // Send flush request to FlushArbiter for both predicted-taken and predicted-not-taken
        // Both cases enter speculative/wrong-path mode
        core::FlushRequest flush;
        flush.source = core::FlushSource::BranchPredictor;
        flush.branch_tag = result.tag;
        flush.branch_fetch_seq = result.fetch_seq;  // Use fetch_seq for prediction clearing
        flush.branch_pc = result.pc;
        flush.branch_depth = result.wrong_path_depth;
        flush.predicted_taken = result.predicted_taken;

        if (result.predicted_taken) {
            flush.target_pc = result.target_pc;  // Redirect to branch target
            ILOG("[bp] PREDICTED TAKEN: sending flush to FlushArbiter for tag=" << result.tag << " target_pc=0x" << std::hex << flush.target_pc << std::dec);
        } else {
            flush.target_pc = result.pc + result.inst_size;  // Continue sequential (but in wrong-path mode)
            ILOG("[bp] PREDICTED NOT-TAKEN: sending flush to FlushArbiter for tag=" << result.tag << " target_pc=0x" << std::hex << flush.target_pc
                                                                                    << std::dec);
        }
        // 1-cycle delay: BP prediction takes time to propagate
        flush_out.send(flush, 1);
    }

    ILOG("[bp] pc=0x" << std::hex << result.pc << std::dec << " taken=" << result.predicted_taken << " mispred=" << result.mispredicted);

    predict_out.send(result, 0);
}

}  // namespace frontend
