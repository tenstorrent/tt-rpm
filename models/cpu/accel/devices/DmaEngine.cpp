// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Tenstorrent USA, Inc.

#include "models/cpu/accel/devices/DmaEngine.hpp"

#include <algorithm>
#include <iostream>

#include "sparta/utils/SpartaAssert.hpp"

namespace accel {

DmaEngine::DmaEngine(sparta::TreeNode* node, const Params& params)
    : sparta::Unit(node),
      mParams(params),
      mMaster(&unit_port_set_, "up"),
      mTickEvent(&unit_event_set_, "tick", CREATE_SPARTA_HANDLER(DmaEngine, tick), 0),
      mCmdsCompleted(&unit_stat_set_, "cmds_completed", "Copy commands completed", sparta::Counter::COUNT_NORMAL),
      mLinesRead(&unit_stat_set_, "lines_read", "Source lines requested (ReadOnce)", sparta::Counter::COUNT_NORMAL),
      mLinesWritten(&unit_stat_set_, "lines_written", "Destination lines requested (WriteUnique)", sparta::Counter::COUNT_NORMAL),
      mPokeFailures(&unit_stat_set_, "poke_failures", "Oracle poke failures (oracle divergence)", sparta::Counter::COUNT_NORMAL),
      mCmdQueueFullCycles(&unit_stat_set_, "cmd_queue_full_cycles", "Cycles the command ingress was full (source back-pressured)",
                          sparta::Counter::COUNT_NORMAL),
      mCmdsFaulted(&unit_stat_set_, "cmds_faulted", "Commands whose range wraps the address space (completed without copying)", sparta::Counter::COUNT_NORMAL) {
    sparta_assert(mParams.line_size > 0 && (mParams.line_size & (mParams.line_size - 1)) == 0, "DmaEngine line_size must be a power of two");
    sparta_assert(mParams.max_outstanding > 0, "DmaEngine max_outstanding must be > 0");
    sparta_assert(mParams.cmd_queue_depth > 0, "DmaEngine cmd_queue_depth must be > 0");
    mMaster.resp_in.registerConsumerHandler(CREATE_SPARTA_HANDLER_WITH_DATA(DmaEngine, handleResp, mem::Response));
    mMaster.inval_in.registerConsumerHandler(CREATE_SPARTA_HANDLER_WITH_DATA(DmaEngine, handleSnoop, mem::Snoop));
}

mem::MemMaster& DmaEngine::memMaster(uint32_t i) {
    sparta_assert(i == 0, "DmaEngine has exactly one MemMaster");
    return mMaster;
}

void DmaEngine::setRequesterId(uint32_t master, uint32_t id) {
    sparta_assert(master == 0, "DmaEngine has exactly one MemMaster");
    mRequesterId = id;
}

void DmaEngine::setOracleHooks(PeekFn peek, PokeFn poke) {
    mPeek = std::move(peek);
    mPoke = std::move(poke);
}

void DmaEngine::setCommandSource(CommandSourceIf* src) {
    mSource = src;
    if (mSource) mSource->attach(this);
}

void DmaEngine::enqueue(const AccelCommand& cmd) {
    sparta_assert(canAccept(), "DmaEngine: enqueue on a full command queue (depth " << mParams.cmd_queue_depth << "); the source must check canAccept()");
    mCmdQueue.push_back(cmd);
}

bool DmaEngine::tryDequeueCompletion(AccelCompletion& out) {
    if (mCompletions.empty()) return false;
    out = mCompletions.front();
    mCompletions.pop();
    return true;
}

void DmaEngine::start() {
    if (mParams.self_tick) mTickEvent.schedule(1);
}

void DmaEngine::tick() {
    finishCompleted_();
    // Drain the ingress before refilling it, so a source that stalled on credits sees the
    // room freed by this cycle's admissions.
    admit_();
    if (mSource) mSource->tick();
    if (!canAccept()) ++mCmdQueueFullCycles;
    issueAll_();
    if (mParams.self_tick) mTickEvent.schedule(1);
}

// Admit queued commands while there is issuing room. A zero-length or faulted copy has no
// requests and completes on the next tick's finishCompleted_().
void DmaEngine::admit_() {
    const uint64_t ls = mParams.line_size;
    // Room counts admitted-but-unissued lines too, or mActive absorbs the whole ingress and
    // canAccept() never back-pressures the source.
    uint64_t claimed = mInflight;
    for (const auto& pc : mActive) claimed += pc.total - pc.next_req;
    while (!mCmdQueue.empty() && claimed < mParams.max_outstanding) {
        const AccelCommand cmd = mCmdQueue.front();
        mCmdQueue.pop_front();
        sparta_assert(cmd.opcode == kOpcodeMemcpy, "DmaEngine: unsupported opcode " << cmd.opcode);

        PendingCommand pc;
        pc.cmd = cmd;
        const uint64_t src = cmd.arg0;
        const uint64_t dst = cmd.arg1;
        const uint64_t len = cmd.arg2;
        if (len > 0 && (src + (len - 1) < src || dst + (len - 1) < dst)) {
            ++mCmdsFaulted;  // descriptor `len` comes from guest memory
            pc.cmd.arg2 = 0;
        } else if (len > 0) {
            pc.src_line = mem::lineAddr(src, ls);
            pc.dst_line = mem::lineAddr(dst, ls);
            pc.read_count = (mem::lineAddr(src + len - 1, ls) - pc.src_line) / ls + 1;
            pc.total = pc.read_count + (mem::lineAddr(dst + len - 1, ls) - pc.dst_line) / ls + 1;
        }
        claimed += pc.total;
        mActive.push_back(std::move(pc));
    }
}

void DmaEngine::issueAll_() {
    const uint64_t ls = mParams.line_size;
    for (auto it = mActive.begin(); it != mActive.end(); ++it) {
        auto& pc = *it;
        while (pc.next_req < pc.total && mInflight < mParams.max_outstanding) {
            const bool is_read = pc.next_req < pc.read_count;
            mem::Request req;
            req.line_addr = is_read ? pc.src_line + pc.next_req * ls : pc.dst_line + (pc.next_req - pc.read_count) * ls;
            req.requester_id = mRequesterId;
            req.source = mem::CacheSource::DCACHE;
            req.txn_id = mNextTxn++;
            req.opcode = is_read ? mem::ReqOpcode::ReadOnce : mem::ReqOpcode::WriteUnique;
            mTxnToCmd.emplace(req.txn_id, it);
            mMaster.req_out.send(req, 1);
            ++pc.next_req;
            ++mInflight;
            if (is_read)
                ++mLinesRead;
            else
                ++mLinesWritten;
        }
        mMaxInflightSeen = std::max(mMaxInflightSeen, mInflight);
    }
}

void DmaEngine::handleResp(const mem::Response& resp) {
    auto it = mTxnToCmd.find(resp.txn_id);
    sparta_assert(it != mTxnToCmd.end(), "DmaEngine: response for unknown txn " << resp.txn_id);
    ++(it->second->completed);
    --mInflight;
    mTxnToCmd.erase(it);
}

// The engine caches no lines and its opcodes keep it out of the directory, so a snoop is always
// a miss: ack it so the home node never waits on this requester.
void DmaEngine::handleSnoop(const mem::Snoop& snp) {
    mem::SnoopResponse ack;
    ack.line_addr = snp.line_addr;
    ack.requester_id = mRequesterId;
    ack.txn_id = snp.txn_id;
    ack.final_state = mem::LineState::I;
    ack.was_dirty = false;
    mMaster.snprsp_out.send(ack, 1);
}

void DmaEngine::finishCompleted_() {
    for (auto it = mActive.begin(); it != mActive.end();) {
        if (it->next_req == it->total && it->completed == it->total) {
            finish_(*it);
            it = mActive.erase(it);
        } else {
            ++it;
        }
    }
}

// All lines have landed: move the data through the oracle and emit the completion. The copy
// goes in chunks (the hooks take 32-bit lengths), backward when dst overlaps above src.
void DmaEngine::finish_(PendingCommand& pc) {
    const uint64_t src = pc.cmd.arg0, dst = pc.cmd.arg1, len = pc.cmd.arg2;
    constexpr uint64_t kChunk = 4096;
    std::vector<uint8_t> buf(std::min(len, kChunk));
    const bool backward = dst > src && dst - src < len;
    for (uint64_t done = 0; done < len;) {
        const uint64_t n = std::min(kChunk, len - done);
        const uint64_t off = backward ? len - done - n : done;
        if (mPeek) mPeek(src + off, buf.data(), static_cast<uint32_t>(n));
        if (mPoke && !mPoke(dst + off, buf.data(), static_cast<uint32_t>(n))) {
            ++mPokeFailures;
            std::cerr << "[dma_engine] oracle poke failed dst=0x" << std::hex << dst + off << " len=" << std::dec << n << "\n";
            break;
        }
        done += n;
    }
    AccelCompletion c;
    c.tag = pc.cmd.tag;
    c.result = 0;
    c.writes_result = false;
    if (mSink)
        mSink->complete(c);
    else
        mCompletions.push(c);
    ++mCmdsCompleted;
}

}  // namespace accel
