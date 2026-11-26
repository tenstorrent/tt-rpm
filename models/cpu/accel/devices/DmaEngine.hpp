// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Tenstorrent USA, Inc.

#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <list>
#include <queue>
#include <unordered_map>
#include <utility>
#include <vector>

#include "sparta/events/UniqueEvent.hpp"
#include "sparta/simulation/Unit.hpp"
#include "sparta/statistics/Counter.hpp"

#include "models/cpu/accel/common/AcceleratorIf.hpp"
#include "models/cpu/accel/common/CommandSourceIf.hpp"
#include "models/cpu/accel/common/CompletionSink.hpp"

// Reference AcceleratorIf device: copies `len` bytes
// from src to dst. Per command it issues mem::Requests on its single MemMaster — ReadOnce
// for every source line and WriteUnique for every destination line, so the MSI directory
// invalidates any core copies before the data lands but never records the engine, which
// caches nothing — honoring a configurable outstanding-request window. When the last line completes it moves the data through the
// oracle hooks (peek src -> poke dst; in production these bind to
// ExecutionDriver::peekPhysBytes/pokePhysBytes) and emits one AccelCompletion.
//
// The engine is coupling-agnostic: commands arrive via enqueue() and completions leave via the
// CompletionSink or tryDequeueCompletion(). Addresses are used exactly as given — translation
// is the oracle hook's concern (loose: PA passthrough; tight: the hook's transAddrNoUpdate).
namespace accel {

class DmaEngine : public sparta::Unit, public AcceleratorIf {
   public:
    static constexpr char name[] = "dma_engine";

    struct Params {
        uint32_t line_size{64};       // must match the fabric line size
        uint32_t max_outstanding{8};  // outstanding mem::Request window
        uint32_t cmd_queue_depth{8};  // command ingress depth; full => canAccept() is false
        bool self_tick{true};         // standalone use: drive tick() from an internal event
    };

    DmaEngine(sparta::TreeNode* node, const Params& params);

    // ---- AcceleratorIf ----
    void init(sparta::TreeNode*) override {}  // fully built in the constructor
    uint32_t numMemMasters() const override { return 1; }
    mem::MemMaster& memMaster(uint32_t i) override;
    void setRequesterId(uint32_t master, uint32_t id) override;
    bool tryDequeueCompletion(AccelCompletion& out) override;
    void tick() override;

    // ---- CommandPort ----
    bool canAccept() const override { return mCmdQueue.size() < mParams.cmd_queue_depth; }
    void enqueue(const AccelCommand& cmd) override;

    // ---- Device-specific wiring ----
    // Oracle data path. peek(addr, buf, len) reads the architectural bytes (Whisper in
    // production); poke(addr, buf, len) writes them and returns false on failure. A failed
    // poke is an oracle divergence: counted (mPokeFailures) and logged, and the command still
    // completes so the front-end is not wedged.
    using PeekFn = std::function<void(uint64_t addr, uint8_t* buf, uint32_t len)>;
    using PokeFn = std::function<bool(uint64_t addr, const uint8_t* buf, uint32_t len)>;
    void setOracleHooks(PeekFn peek, PokeFn poke);

    void setCommandSource(CommandSourceIf* src) override;
    void setCompletionSink(CompletionSink* sink) override { mSink = sink; }

    // Self-tick mode: schedule the per-cycle event (called by the Cluster/CoreImpl after
    // tree finalization).
    void start() override;

    // Test/introspection hook: high-water mark of in-flight mem::Requests.
    uint32_t maxInflightSeen() const { return mMaxInflightSeen; }

   private:
    // Requests are generated as they issue, so a command's size costs no host memory.
    struct PendingCommand {
        AccelCommand cmd;
        uint64_t src_line{0};    // first source line (ReadOnce)
        uint64_t dst_line{0};    // first destination line (WriteUnique)
        uint64_t read_count{0};  // requests [0, read_count) read the source, the rest write the destination
        uint64_t total{0};
        uint64_t next_req{0};   // next request to issue
        uint64_t completed{0};  // responses received
    };

    void handleResp(const mem::Response& resp);
    void handleSnoop(const mem::Snoop& snp);
    void admit_();
    void issueAll_();
    void finishCompleted_();
    void finish_(PendingCommand& pc);

    const Params mParams;
    mem::MemMaster mMaster;
    uint32_t mRequesterId{mem::kInvalidRequesterId};
    uint64_t mNextTxn{1};

    std::deque<AccelCommand> mCmdQueue;
    std::list<PendingCommand> mActive;  // stable iterators: txn -> command map points here
    std::unordered_map<uint64_t, std::list<PendingCommand>::iterator> mTxnToCmd;
    std::queue<AccelCompletion> mCompletions;
    CommandSourceIf* mSource{nullptr};
    CompletionSink* mSink{nullptr};

    uint32_t mInflight{0};
    uint32_t mMaxInflightSeen{0};

    PeekFn mPeek;
    PokeFn mPoke;

    sparta::UniqueEvent<> mTickEvent;

    sparta::Counter mCmdsCompleted;
    sparta::Counter mLinesRead;
    sparta::Counter mLinesWritten;
    sparta::Counter mPokeFailures;
    sparta::Counter mCmdQueueFullCycles;
    sparta::Counter mCmdsFaulted;
};

}  // namespace accel
