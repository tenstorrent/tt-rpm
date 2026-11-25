// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Tenstorrent USA, Inc.

// Fabric harness for the loose-coupling accelerator path: one fake CoreIf core and one
// DmaEngine attached through the real Cluster (build/bind/start), a real ClusterCache, and
// per-master ClusterBridges. Exercises:
//   - the MMIO doorbell peel in ClusterCache (MmioRouter) — the doorbell store's request is
//     acked without allocating a cache line, and the ring front-end is notified;
//   - the RingFrontEnd doorbell -> tail peek -> descriptor walk -> engine enqueue path;
//   - the DmaEngine's fabric copy traffic (ReadOnce src / WriteUnique dst) landing data in
//     the oracle (stub memory standing in for Whisper);
//   - the write-coherence differential: with coherence on, the engine's WriteUnique snoop-
//     invalidates the fake core's dst line (a later read re-fills and sees the new data);
//     with coherence off, the fake core keeps a stale S copy (the hazard coherence closes);
//   - the engine is never recorded in the directory, so the core's re-read does not snoop it.

#include <sys/wait.h>
#include <unistd.h>

#include <cstring>
#include <iostream>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "sparta/kernel/Scheduler.hpp"
#include "sparta/simulation/Clock.hpp"
#include "sparta/simulation/RootTreeNode.hpp"
#include "sparta/simulation/TreeNode.hpp"
#include "sparta/simulation/Unit.hpp"
#include "sparta/utils/SpartaAssert.hpp"

#include "Cluster.hpp"
#include "ClusterCache.hpp"
#include "models/cpu/accel/common/CompletionSink.hpp"
#include "models/cpu/accel/devices/DmaEngine.hpp"
#include "models/cpu/accel/frontends/RingFrontEnd.hpp"
#include "models/cpu/common/MemInterface.hpp"

namespace {

constexpr uint32_t kLine = 64;
constexpr uint64_t kBarBase = 0xF0000, kBarSize = 0x1000;  // inside the stub's 1MB (the doorbell value is oracle-peeked)
constexpr uint64_t kDoorbell = kBarBase;                   // u64 ring tail lives at BAR+0
constexpr uint64_t kRingBase = 0x8000;
constexpr uint32_t kRingSlots = 16;
constexpr uint64_t kSrc = 0x1000, kDst = 0x2000, kLen = 128;  // 2 src lines + 2 dst lines

// Byte-addressable stub memory standing in for the Whisper oracle.
struct StubMemory {
    std::vector<uint8_t> bytes;
    explicit StubMemory(size_t sz)
        : bytes(sz, 0) {}
    void peek(uint64_t addr, uint8_t* buf, uint32_t len) { std::memcpy(buf, bytes.data() + addr, len); }
    bool poke(uint64_t addr, const uint8_t* buf, uint32_t len) {
        if (addr + len > bytes.size()) return false;
        std::memcpy(bytes.data() + addr, buf, len);
        return true;
    }
};

// Minimal core: one MemMaster, a tiny line-state shadow with 64B data copies, a snoop
// responder, and completion tracking. Emulates a core that caches dst, gets (or fails to
// get) invalidated by the accelerator's write, and later re-reads dst.
class FakeCore : public sparta::Unit, public cluster::CoreIf {
   public:
    explicit FakeCore(sparta::TreeNode* node)
        : sparta::Unit(node),
          mMaster(&unit_port_set_, "up") {
        mMaster.resp_in.registerConsumerHandler(CREATE_SPARTA_HANDLER_WITH_DATA(FakeCore, handleResp, mem::Response));
        mMaster.inval_in.registerConsumerHandler(CREATE_SPARTA_HANDLER_WITH_DATA(FakeCore, handleSnoop, mem::Snoop));
    }

    // ---- CoreIf ----
    void init() override {}
    uint32_t numMemMasters() const override { return 1; }
    mem::MemMaster& memMaster(uint32_t i) override {
        sparta_assert(i == 0, "FakeCore has exactly one MemMaster");
        return mMaster;
    }
    const sparta::Clock* clock() const override { return getClock(); }
    void setRequesterId(uint32_t master, uint32_t id) override {
        sparta_assert(master == 0, "FakeCore has exactly one MemMaster");
        mRequesterId = id;
    }

    // ---- Harness API ----
    uint64_t sendReq(mem::ReqOpcode op, uint64_t line) {
        mem::Request req;
        req.line_addr = line;
        req.requester_id = mRequesterId;
        req.source = mem::CacheSource::DCACHE;
        req.txn_id = mNextTxn++;
        req.opcode = op;
        if (op == mem::ReqOpcode::ReadShared) mFillTxnToLine[req.txn_id] = line;  // reads fill the shadow
        mMaster.req_out.send(req, 1);
        return req.txn_id;
    }

    bool completed(uint64_t txn) const { return mDone.count(txn) != 0; }
    mem::RespOpcode respOpcode(uint64_t txn) const { return mRespOps.at(txn); }

    mem::LineState lineState(uint64_t line) const {
        auto it = mShadow.find(line);
        return it == mShadow.end() ? mem::LineState::I : it->second.state;
    }

    // What a program load would see out of the local shadow (caller guarantees the line is
    // valid — fills are pumped to completion by the harness before reads).
    void readLine(uint64_t line, uint8_t* out) {
        sparta_assert(lineState(line) != mem::LineState::I, "readLine on an invalid line");
        std::memcpy(out, mShadow[line].data.data(), kLine);
    }

    uint32_t snoopsSeen{0};

    // Harness helper: after a fill completes, pull the line's bytes from the oracle.
    void loadFillData(uint64_t line, StubMemory& mem) {
        auto& sl = mShadow[line];
        if (sl.data.empty()) sl.data.resize(kLine);
        mem.peek(line, sl.data.data(), kLine);
    }

   private:
    struct ShadowLine {
        mem::LineState state{mem::LineState::I};
        std::vector<uint8_t> data;
    };

    void handleResp(const mem::Response& resp) {
        mDone.insert(resp.txn_id);
        mRespOps[resp.txn_id] = resp.resp_opcode;
        auto it = mFillTxnToLine.find(resp.txn_id);
        if (it != mFillTxnToLine.end()) {
            mShadow[it->second].state = resp.granted_state;
            mFillTxnToLine.erase(it);
        }
    }

    void handleSnoop(const mem::Snoop& snp) {
        ++snoopsSeen;
        auto it = mShadow.find(snp.line_addr);
        const bool was_dirty = it != mShadow.end() && it->second.state == mem::LineState::M;
        if (it != mShadow.end()) it->second.state = mem::LineState::I;
        mem::SnoopResponse ack;
        ack.line_addr = snp.line_addr;
        ack.requester_id = mRequesterId;
        ack.txn_id = snp.txn_id;
        ack.final_state = mem::LineState::I;
        ack.was_dirty = was_dirty;
        mMaster.snprsp_out.send(ack, 1);
    }

    mem::MemMaster mMaster;
    uint32_t mRequesterId{mem::kInvalidRequesterId};
    uint64_t mNextTxn{1};
    std::unordered_set<uint64_t> mDone;
    std::unordered_map<uint64_t, mem::RespOpcode> mRespOps;
    std::unordered_map<uint64_t, uint64_t> mFillTxnToLine;
    std::unordered_map<uint64_t, ShadowLine> mShadow;
};

int g_failures = 0;
#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            ++g_failures;                                               \
            std::cerr << "FAIL: " #cond " (line " << __LINE__ << ")\n"; \
        }                                                               \
    } while (0)

template <typename Pred>
void runUntil(sparta::Scheduler& sched, Pred pred, uint64_t max_cycles) {
    for (uint64_t i = 0; i < max_cycles && !pred(); ++i) sched.run(1, true, false);
}

void runScenario(bool coherence, uint32_t link_latency = 0) {
    std::cout << "--- scenario: coherence " << (coherence ? "ON" : "OFF") << ", link_latency " << link_latency << " ---\n";

    sparta::Scheduler scheduler;
    sparta::RootTreeNode root("top", "accel fabric harness");
    sparta::Clock clk("clk", &scheduler);
    root.setClock(&clk);

    StubMemory stub(1 << 20);
    FakeCore* fake = nullptr;
    accel::DmaEngine* dma = nullptr;

    cluster::Cluster::Config cfg;
    cfg.num_cores = 1;
    cfg.cache_hit_latency = 12;
    cfg.cache_miss_latency = 100;
    cfg.cache_size_kb = 256;
    cfg.cache_associativity = 8;
    cfg.cache_line_size = kLine;
    cfg.coherence_enabled = coherence;

    cluster::Cluster cluster(
        &root,
        [&](sparta::TreeNode* parent, uint32_t) {
            auto* node = new sparta::TreeNode(parent, "fakecore0", "fake core");
            node->setClock(&clk);
            auto core = std::make_unique<FakeCore>(node);
            fake = core.get();
            return core;
        },
        cfg, &clk);
    accel::AttachSpec attach;
    attach.link_latency_cycles = link_latency;  // same clock domain; only the distance varies
    cluster.addAccelerator(
        [&](sparta::TreeNode* node, uint32_t) {
            accel::DmaEngine::Params p;
            p.line_size = kLine;
            p.max_outstanding = 8;
            p.self_tick = true;
            auto dev = std::make_unique<accel::DmaEngine>(node, p);
            dma = dev.get();
            return dev;
        },
        attach);

    cluster.build();
    cluster.bind();

    dma->setOracleHooks([&stub](uint64_t a, uint8_t* b, uint32_t l) { stub.peek(a, b, l); },
                        [&stub](uint64_t a, const uint8_t* b, uint32_t l) { return stub.poke(a, b, l); });

    accel::RingFrontEnd ring(dma, kRingBase, kRingSlots, kDoorbell, [&stub](uint64_t a, uint8_t* b, uint32_t l) { stub.peek(a, b, l); });
    cluster.cache()->mmio().addRegion(kBarBase, kBarSize, [&](const mem::Request& r) { ring.onDoorbell(r); });
    dma->setCommandSource(&ring);

    std::vector<accel::AccelCompletion> completions;
    accel::FunctionSink sink([&](const accel::AccelCompletion& c) { completions.push_back(c); });
    dma->setCompletionSink(&sink);

    root.enterConfiguring();
    root.enterFinalized();
    scheduler.finalize();
    cluster.start();

    // Core program (emulated): cache dst first so the DMA write must invalidate us.
    const uint64_t fill_txn = fake->sendReq(mem::ReqOpcode::ReadShared, kDst);
    runUntil(
        scheduler, [&] { return fake->completed(fill_txn); }, 1000);
    fake->loadFillData(kDst, stub);
    const mem::LineState filled = coherence ? mem::LineState::S : mem::LineState::E;  // nothing is shared without coherence
    CHECK(fake->lineState(kDst) == filled);

    // Retired stores (straight into the oracle): src pattern, ring descriptor, doorbell tail.
    for (uint64_t i = 0; i < kLen; ++i) stub.bytes[kSrc + i] = uint8_t(0x40 + i);
    const uint64_t desc[4] = {accel::kOpcodeMemcpy, kSrc, kDst, kLen};
    std::memcpy(&stub.bytes[kRingBase], desc, sizeof(desc));
    const uint64_t tail = 1;
    std::memcpy(&stub.bytes[kDoorbell], &tail, sizeof(tail));

    // Ring the doorbell: the store's downstream request is MMIO-peeled by the cache.
    const uint64_t db_txn = fake->sendReq(mem::ReqOpcode::ReadUnique, kBarBase);
    runUntil(
        scheduler, [&] { return fake->completed(db_txn); }, 1000);
    CHECK(fake->completed(db_txn));
    if (fake->completed(db_txn)) CHECK(fake->respOpcode(db_txn) == mem::RespOpcode::CompAck);

    // The engine fetches the ring, copies, completes.
    const uint64_t db_cycle = clk.currentCycle();
    runUntil(
        scheduler, [&] { return completions.size() == 1; }, 5000);
    const uint64_t copy_cycles = clk.currentCycle() - db_cycle;
    std::cout << "    doorbell -> completion: " << copy_cycles << " cycles\n";
    // Every fabric round trip pays the link each way, so a distant device cannot possibly
    // finish inside one link crossing.
    if (link_latency > 0) CHECK(copy_cycles > 2 * link_latency);
    CHECK(completions.size() == 1);
    if (!completions.empty()) CHECK(completions[0].tag == 0);
    CHECK(ring.head() == 1);

    // Data landed in the oracle.
    CHECK(std::memcmp(&stub.bytes[kDst], &stub.bytes[kSrc], kLen) == 0);

    // The coherence differential.
    if (coherence) {
        CHECK(fake->snoopsSeen >= 1);  // the engine's WriteUnique invalidated our dst line
        CHECK(fake->lineState(kDst) == mem::LineState::I);
    } else {
        CHECK(fake->snoopsSeen == 0);
        CHECK(fake->lineState(kDst) == filled);  // stale copy survives
    }

    // What a later core load of dst sees.
    if (fake->lineState(kDst) == mem::LineState::I) {
        const uint64_t reread_start = clk.currentCycle();
        const uint64_t txn = fake->sendReq(mem::ReqOpcode::ReadShared, kDst);
        runUntil(
            scheduler, [&] { return fake->completed(txn); }, 1000);
        CHECK(fake->completed(txn));
        const uint64_t reread_cycles = clk.currentCycle() - reread_start;
        std::cout << "    core re-read of dst: " << reread_cycles << " cycles\n";
        if (link_latency > 0) CHECK(reread_cycles < 2 * link_latency);  // no snoop across the link
        fake->loadFillData(kDst, stub);
    }
    uint8_t buf[kLine] = {};
    fake->readLine(kDst, buf);
    if (coherence) {
        CHECK(std::memcmp(buf, &stub.bytes[kSrc], kLine) == 0);  // re-fill sees the copy
    } else {
        CHECK(std::memcmp(buf, &stub.bytes[kSrc], kLine) != 0);  // stale shadow served
    }

    root.enterTeardown();
}

}  // namespace

int main(int argc, char** argv) {
    // Direct single-scenario mode for debugging: `accel_fabric_test on|off`.
    if (argc > 1) {
        runScenario(argv[1][0] == 'o' && argv[1][1] == 'n');
        if (g_failures == 0)
            std::cout << "accel_fabric_test: PASS\n";
        else
            std::cout << "accel_fabric_test: " << g_failures << " FAILURE(S)\n";
        return g_failures ? 1 : 0;
    }

    // One process per scenario: Sparta's scheduler is a process-wide singleton.
    // The third scenario re-runs the coherent copy from across a link.
    for (auto [coherence, link_latency] : {std::pair<bool, uint32_t>{true, 0}, {false, 0}, {true, 100}}) {
        const pid_t pid = fork();
        sparta_assert(pid >= 0, "fork failed");
        if (pid == 0) {
            runScenario(coherence, link_latency);
            std::cout.flush();
            std::cerr.flush();
            _exit(g_failures ? 1 : 0);
        }
        int status = 0;
        waitpid(pid, &status, 0);
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) ++g_failures;
    }

    if (g_failures == 0)
        std::cout << "accel_fabric_test: PASS\n";
    else
        std::cout << "accel_fabric_test: " << g_failures << " FAILURE(S)\n";
    return g_failures ? 1 : 0;
}
