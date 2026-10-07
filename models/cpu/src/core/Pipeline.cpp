// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Tenstorrent USA, Inc.

#include "Pipeline.hpp"

#include <algorithm>
#include <iomanip>
#include <iostream>

#include "Common/FlushArbiter.hpp"
#include "Common/PipelineVisualizer.hpp"
#include "Common/WritePortArbiter.hpp"
#include "Frontend/BranchPredictor/BranchPredictor.hpp"
#include "Frontend/DecodeQueue/DecodeQueue.hpp"
#include "Frontend/DecodeStructures/DecodeStructures.hpp"
#include "Frontend/FetchQueue/FetchQueue.hpp"
#include "Frontend/FetchStructures/FetchStructures.hpp"
#include "Frontend/FrontendMemoryStructures/FrontendMemoryStructures.hpp"
#include "LoadStoreUnit/BackendMemoryStructures/BackendMemoryStructures.hpp"
#include "LoadStoreUnit/LSQ/LSQ.hpp"
#include "MemoryHierarchy/L2Cache.hpp"
#include "Midcore/Execute/Execute.hpp"
#include "Midcore/Issue/Issue.hpp"
#include "Midcore/Rename/Rename.hpp"
#include "Midcore/Writeback/Writeback.hpp"

namespace core {

PipelineClock::PipelineClock(sparta::TreeNode* node, const PipelineClockParams*)
    : sparta::Unit(node),
      mTickEvent(&unit_event_set_, "tick", CREATE_SPARTA_HANDLER(PipelineClock, tick), 0) {}

void PipelineClock::setStages(frontend::FetchStructures* fetch, frontend::FrontendMemoryStructures* memory, frontend::FetchQueue* fetch_queue,
                              frontend::DecodeStructures* decode, frontend::DecodeQueue* decode_queue, frontend::BranchPredictor* bp, midcore::Rename* rename,
                              midcore::Issue* issue, midcore::Execute* execute, midcore::LSQ* lsq, midcore::BackendMemoryStructures* dcache,
                              midcore::Writeback* writeback) {
    mFetch = fetch;
    mMemory = memory;
    mFetchQueue = fetch_queue;
    mDecode = decode;
    mDecodeQueue = decode_queue;
    mBp = bp;
    mRename = rename;
    mIssue = issue;
    mExecute = execute;
    mLsq = lsq;
    mDcache = dcache;
    mWriteback = writeback;
}

void PipelineClock::start() {
    mStartTime = mLastReportTime = std::chrono::steady_clock::now();
    mTickEvent.schedule(1);
}

void PipelineClock::tick() {
    // Process pending flushes first (high priority)
    if (mFlushArbiter) mFlushArbiter->tick();

    mWriteback->tick();
    mLsq->tick();
    mDcache->tick();
    // L2 ticks after both L1 caches so that fill responses queued this cycle
    // are dispatched to L1s via ports (delay=1) and arrive next cycle.
    if (mL2) mL2->tick();
    mExecute->tick();

    // Write-port arbitration: LSQ and Execute have submitted candidates.
    if (mArbiter && mArbiter->enabled()) {
        mArbiter->arbitrate();
        mExecute->processWritePortGrants();
        mLsq->processWritePortGrants();
        mArbiter->clear();
    }

    mIssue->tick();
    mRename->tick();
    mDecodeQueue->tick();
    mDecode->tick();
    mFetchQueue->tick();
    mMemory->tick();
    mFetch->tick();

    ++mCycle;
    if (mCycle % kReportInterval == 0) {
        reportStats();
    }

    // Debug tick - writes per-cycle detail to debug file if enabled
    if (mVisualizer) {
        mVisualizer->debugTick(mCycle);
    }

    // Deadlock detection: check if retirement rate has collapsed
    uint64_t retired_now = mWriteback->numRetired();
    if (retired_now == mLastRetiredDeadlock) {
        ++mNoProgressCycles;
        if (mNoProgressCycles == kDeadlockThreshold) {
            deadlockDiag();
        }
    } else {
        mLastRetiredDeadlock = retired_now;
        mNoProgressCycles = 0;
    }

    mTickEvent.schedule(1);
}

void PipelineClock::simulationTerminating_() { reportStats(); }

void PipelineClock::reportStats() {
    uint64_t retired = mWriteback->numRetired();
    double ipc = (mCycle > 0) ? static_cast<double>(retired) / mCycle : 0.0;
    uint64_t delta_ret = retired - mLastRetired;
    double hb_ipc = static_cast<double>(delta_ret) / kReportInterval;
    mLastRetired = retired;

    // Wall-clock throughput: KIPS = retired instr/s, KHz = simulated cycles/s, both
    // in thousands. Cumulative figures use the whole run; heartbeat figures use the
    // window since the previous report.
    auto now = std::chrono::steady_clock::now();
    double elapsed_s = std::chrono::duration<double>(now - mStartTime).count();
    double window_s = std::chrono::duration<double>(now - mLastReportTime).count();
    uint64_t delta_cycles = mCycle - mLastReportCycle;
    double kips = (elapsed_s > 0.0) ? retired / elapsed_s / 1000.0 : 0.0;
    double khz = (elapsed_s > 0.0) ? mCycle / elapsed_s / 1000.0 : 0.0;
    double hb_kips = (window_s > 0.0) ? delta_ret / window_s / 1000.0 : 0.0;
    double hb_khz = (window_s > 0.0) ? delta_cycles / window_s / 1000.0 : 0.0;
    mLastReportTime = now;
    mLastReportCycle = mCycle;

    uint64_t icHits = mMemory->numHits();
    uint64_t icMisses = mMemory->numMisses();
    uint64_t icTotal = icHits + icMisses;
    double icRate = (icTotal > 0) ? 100.0 * icHits / icTotal : 0.0;

    uint64_t dcHits = mDcache->numHits();
    uint64_t dcMisses = mDcache->numMisses();
    uint64_t dcTotal = dcHits + dcMisses;
    double dcRate = (dcTotal > 0) ? 100.0 * dcHits / dcTotal : 0.0;

    uint64_t bpCorrect = mBp->numCorrect();
    uint64_t bpMispred = mBp->numMispredicted();
    uint64_t bpTotal = bpCorrect + bpMispred;
    double bpAcc = (bpTotal > 0) ? 100.0 * bpCorrect / bpTotal : 0.0;

    uint64_t l2Hits = mL2 ? mL2->numHits() : 0;
    uint64_t l2Misses = mL2 ? mL2->numMisses() : 0;
    uint64_t l2Total = l2Hits + l2Misses;
    double l2Rate = (l2Total > 0) ? 100.0 * l2Hits / l2Total : 0.0;

    std::cerr << std::fixed << std::setprecision(3) << "\n=== Pipeline Stats @ cycle " << mCycle << " ===\n"
              << "  IPC:       " << ipc << "  heartbeat=" << hb_ipc << "  (retired=" << retired << ", cycles=" << mCycle << ")\n"
              << "  Speed:     " << kips << " KIPS  " << khz << " KHz  (heartbeat: " << hb_kips << " KIPS  " << hb_khz << " KHz, wall=" << elapsed_s << "s)\n"
              << "  Fetch:     fetched=" << mFetch->numFetched() << "  buffer_full_stall_cycles=" << mFetch->numBufferFullStallCycles() << "\n"
              << "  I-Cache:   hits=" << icHits << "  misses=" << icMisses << "  (hit_rate=" << icRate << "%)\n"
              << "  FetchQ:    enqueued=" << mFetchQueue->numEnqueued() << "  branch_wait_cycles=" << mFetchQueue->numBranchWaitCycles() << "\n"
              << "  Decode:    decoded=" << mDecode->numDecoded() << "  mispred_stall_cycles=" << mDecode->numMispredStallCycles() << "\n"
              << "  DecodeQ:   enqueued=" << mDecodeQueue->numEnqueued() << "\n"
              << "  BP:        correct=" << bpCorrect << "  mispredicted=" << bpMispred << "  (accuracy=" << bpAcc << "%)\n"
              << "  Rename:    dispatched=" << mRename->numDispatched() << "  prf_stall_cycles=" << mRename->numPrfStallCycles() << "\n"
              << "  Issue:     issued=" << mIssue->numIssued() << "  stall_cycles=" << mIssue->numStallCycles() << "\n"
              << "  Execute:   executed=" << mExecute->numExecuted() << "\n"
              << "  LSQ:       loads=" << mLsq->numLoads() << "  stores=" << mLsq->numStores() << "\n"
              << "  D-Cache:   hits=" << dcHits << "  misses=" << dcMisses << "  (hit_rate=" << dcRate << "%)\n";
    if (mL2) {
        std::cerr << "  L2-Cache:  hits=" << l2Hits << "  misses=" << l2Misses << "  (hit_rate=" << l2Rate << "%)\n";
    }
    std::cerr << "  Writeback: retired=" << retired << "\n";

    // Top-down level 1, cumulative. Dispatched uops that did not retire, plus
    // recovery bubbles, are bad speculation (see Rename::accountSlots_).
    const uint64_t slots = mRename->topdownSlots();
    if (slots > 0) {
        const double total = static_cast<double>(slots);
        const uint64_t wasted = mRename->numDispatched() - std::min(retired, mRename->numDispatched());
        std::cerr << "  TopDown:   retiring=" << 100.0 * retired / total << "%  bad_spec=" << 100.0 * (wasted + mRename->topdownRecoverySlots()) / total
                  << "%  frontend=" << 100.0 * mRename->topdownFrontendBoundSlots() / total
                  << "%  backend=" << 100.0 * mRename->topdownBackendBoundSlots() / total << "%\n";
    }
}

void PipelineClock::deadlockDiag() {
    std::cerr << "\n=== DEADLOCK DETECTED @ cycle " << mCycle << " (no retirement for " << kDeadlockThreshold << " cycles) ===\n";

    // ROB state
    auto& rob = mWriteback->rob();
    std::cerr << "  ROB: size=" << rob.size() << "/" << rob.capacity() << " head_completed=" << rob.headCompleted();
    if (!rob.empty()) {
        auto& head = rob.head();
        std::cerr << " head_tag=" << head.tag << " head_pc=0x" << std::hex << head.pc << std::dec << " head_uop=" << static_cast<int>(head.uop_type);
    }
    std::cerr << "\n";

    // Issue queue state
    std::cerr << "  Issue: issued=" << mIssue->numIssued() << " stall_cycles=" << mIssue->numStallCycles() << "\n";

    // Execute state
    std::cerr << "  Execute: executed=" << mExecute->numExecuted() << "\n";

    // LSQ state
    std::cerr << "  LSQ: loads=" << mLsq->numLoads() << " stores=" << mLsq->numStores() << " completion_order=" << mLsq->numCompletionOrder() << "\n";

    // Rename state
    std::cerr << "  Rename: dispatched=" << mRename->numDispatched() << " prf_stalls=" << mRename->numPrfStallCycles() << "\n";

    std::cerr << "=== END DEADLOCK DIAG ===\n\n";
}

}  // namespace core
