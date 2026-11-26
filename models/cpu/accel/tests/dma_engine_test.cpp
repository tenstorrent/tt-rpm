// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Tenstorrent USA, Inc.

// Standalone DmaEngine harness: feeds AccelCommands to a DmaEngine bound to a fake
// fixed-latency memory slave and checks completions plus data movement against a stub
// byte-addressable memory backing the oracle hooks. No pipeline, no Whisper.

#include <algorithm>
#include <cstring>
#include <iostream>
#include <vector>

#include "sparta/kernel/Scheduler.hpp"
#include "sparta/simulation/Clock.hpp"
#include "sparta/simulation/RootTreeNode.hpp"
#include "sparta/simulation/TreeNode.hpp"
#include "sparta/simulation/Unit.hpp"

#include "models/cpu/accel/common/CompletionSink.hpp"
#include "models/cpu/accel/devices/DmaEngine.hpp"
#include "models/cpu/common/MemInterface.hpp"

namespace {

// Fixed-latency memory: answers every REQ with a CompData response `latency` cycles later.
// Records every request for test introspection.
class FakeMemSlave : public sparta::Unit {
   public:
    FakeMemSlave(sparta::TreeNode* node, uint32_t latency)
        : sparta::Unit(node),
          mLatency(latency),
          mSlave(&unit_port_set_, "slave") {
        mSlave.req_in.registerConsumerHandler(CREATE_SPARTA_HANDLER_WITH_DATA(FakeMemSlave, handleReq, mem::Request));
    }
    mem::MemSlave& slave() { return mSlave; }

    std::vector<mem::Request> seen;

   private:
    void handleReq(const mem::Request& req) {
        seen.push_back(req);
        mem::Response resp;
        resp.line_addr = req.line_addr;
        resp.requester_id = req.requester_id;
        resp.source = req.source;
        resp.txn_id = req.txn_id;
        resp.resp_opcode = mem::RespOpcode::CompData;
        resp.granted_state = req.isWrite() ? mem::LineState::M : mem::LineState::S;
        mSlave.resp_out.send(resp, mLatency);
    }

    uint32_t mLatency;
    mem::MemSlave mSlave;
};

// Byte-addressable stub memory backing the DmaEngine's oracle hooks.
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

}  // namespace

int main() {
    sparta::Scheduler scheduler;
    sparta::RootTreeNode root("top", "accel dma harness");
    sparta::Clock clk("clk", &scheduler);
    root.setClock(&clk);

    accel::DmaEngine::Params params;
    params.line_size = 64;
    params.max_outstanding = 4;
    params.self_tick = true;
    auto* dma_node = new sparta::TreeNode(&root, "dma0", "test dma engine");
    accel::DmaEngine dma(dma_node, params);

    auto* mem_node = new sparta::TreeNode(&root, "mem", "fake fixed-latency memory");
    FakeMemSlave mem(mem_node, 10);

    mem::connect(dma.memMaster(0), mem.slave());
    dma.setRequesterId(0, 3);

    StubMemory stub(1 << 16);
    dma.setOracleHooks([&stub](uint64_t a, uint8_t* b, uint32_t l) { stub.peek(a, b, l); },
                       [&stub](uint64_t a, const uint8_t* b, uint32_t l) { return stub.poke(a, b, l); });

    std::vector<accel::AccelCompletion> completions;
    accel::FunctionSink sink([&](const accel::AccelCompletion& c) { completions.push_back(c); });
    dma.setCompletionSink(&sink);

    root.enterConfiguring();
    root.enterFinalized();
    scheduler.finalize();  // DAG must be finalized before any event is scheduled
    dma.start();

    // ---- T1: single aligned line copy (64B) ----
    const uint64_t t1_src = 0x1000, t1_dst = 0x2000;
    for (uint64_t i = 0; i < 64; ++i) stub.bytes[t1_src + i] = uint8_t(0xA0 + i);
    dma.enqueue({accel::kOpcodeMemcpy, t1_src, t1_dst, 64, 0, /*tag=*/11});

    // ---- T2: 200B unaligned copy spanning 4 src + 4 dst lines ----
    const uint64_t t2_src = 0x1003, t2_dst = 0x2041, t2_len = 200;
    for (uint64_t i = 0; i < t2_len; ++i) stub.bytes[t2_src + i] = uint8_t(i * 7 + 1);
    dma.enqueue({accel::kOpcodeMemcpy, t2_src, t2_dst, t2_len, 0, /*tag=*/12});

    // ---- T3: back-to-back third copy (completion ordering) ----
    const uint64_t t3_src = 0x3000, t3_dst = 0x4000, t3_len = 130;
    for (uint64_t i = 0; i < t3_len; ++i) stub.bytes[t3_src + i] = uint8_t(0x11 + i);
    dma.enqueue({accel::kOpcodeMemcpy, t3_src, t3_dst, t3_len, 0, /*tag=*/13});

    runUntil(
        scheduler, [&] { return completions.size() == 3; }, 5000);

    CHECK(completions.size() == 3);
    if (completions.size() == 3) {
        CHECK(completions[0].tag == 11 && completions[1].tag == 12 && completions[2].tag == 13);
        for (const auto& c : completions) CHECK(!c.writes_result);
    }

    // Data landed correctly in the oracle-backed memory.
    CHECK(std::memcmp(&stub.bytes[t1_dst], &stub.bytes[t1_src], 64) == 0);
    CHECK(std::memcmp(&stub.bytes[t2_dst], &stub.bytes[t2_src], t2_len) == 0);
    CHECK(std::memcmp(&stub.bytes[t3_dst], &stub.bytes[t3_src], t3_len) == 0);

    // Request stream: reads are ReadOnce, writes are WriteUnique; every request carries
    // the assigned requester id. Line counts: T1 1+1, T2 4+4, T3 3+3.
    uint32_t n_read_once = 0, n_write_unique = 0;
    for (const auto& r : mem.seen) {
        CHECK(r.requester_id == 3);
        CHECK(r.source == mem::CacheSource::DCACHE);
        if (r.opcode == mem::ReqOpcode::ReadOnce)
            ++n_read_once;
        else if (r.opcode == mem::ReqOpcode::WriteUnique)
            ++n_write_unique;
    }
    CHECK(n_read_once == 8);
    CHECK(n_write_unique == 8);

    // Outstanding window respected and actually exercised (T2 alone has 8 requests).
    CHECK(dma.maxInflightSeen() <= params.max_outstanding);
    CHECK(dma.maxInflightSeen() == params.max_outstanding);

    // ---- T4: completion-queue path (sink detached -> tryDequeueCompletion) ----
    dma.setCompletionSink(nullptr);
    const uint64_t t4_src = 0x5000, t4_dst = 0x6000;
    for (uint64_t i = 0; i < 32; ++i) stub.bytes[t4_src + i] = uint8_t(0x5A - i);
    dma.enqueue({accel::kOpcodeMemcpy, t4_src, t4_dst, 32, 0, /*tag=*/14});
    accel::AccelCompletion c4{};
    runUntil(
        scheduler, [&] { return dma.tryDequeueCompletion(c4); }, 1000);
    CHECK(c4.tag == 14);
    CHECK(std::memcmp(&stub.bytes[t4_dst], &stub.bytes[t4_src], 32) == 0);

    // ---- T5: ingress credits ----
    // The scheduler is not run here, so nothing is admitted: the ingress fills and the
    // device must stop accepting. Zero-length copies keep this free of memory traffic.
    uint32_t accepted = 0;
    while (dma.canAccept()) {
        dma.enqueue({accel::kOpcodeMemcpy, 0, 0, /*len=*/0, 0, /*tag=*/100 + accepted});
        ++accepted;
    }
    CHECK(accepted == params.cmd_queue_depth);
    CHECK(!dma.canAccept());
    uint32_t drained5 = 0;
    accel::AccelCompletion c5{};
    runUntil(
        scheduler,
        [&] {
            while (dma.tryDequeueCompletion(c5)) ++drained5;
            return drained5 == accepted;
        },
        1000);
    CHECK(drained5 == accepted);
    CHECK(dma.canAccept());

    // ---- T7: a copy larger than the oracle chunk, overlapping above its source ----
    const uint64_t t7_src = 0x8000, t7_len = 10000, t7_dst = t7_src + 100;
    std::vector<uint8_t> t7_expect(t7_len);
    for (uint64_t i = 0; i < t7_len; ++i) stub.bytes[t7_src + i] = t7_expect[i] = uint8_t(i * 7 + i / 256);
    dma.enqueue({accel::kOpcodeMemcpy, t7_src, t7_dst, t7_len, 0, /*tag=*/300});
    accel::AccelCompletion c7{};
    runUntil(
        scheduler, [&] { return dma.tryDequeueCompletion(c7); }, 20000);
    CHECK(c7.tag == 300);
    CHECK(std::memcmp(&stub.bytes[t7_dst], t7_expect.data(), t7_len) == 0);

    // ---- T8: a descriptor whose range wraps the address space completes without traffic ----
    const size_t seen_before = mem.seen.size();
    dma.enqueue({accel::kOpcodeMemcpy, ~0ull - 10, 0x100, /*len=*/1ull << 40, 0, /*tag=*/301});
    accel::AccelCompletion c8{};
    runUntil(
        scheduler, [&] { return dma.tryDequeueCompletion(c8); }, 100);
    CHECK(c8.tag == 301);
    CHECK(mem.seen.size() == seen_before);

    // ---- T6: a continuously fed ingress back-pressures ----
    // Admission must honour the outstanding window, or commands pile up past the queue depth.
    uint64_t fed = 0, drained = 0, peak = 0;
    accel::AccelCompletion c6{};
    for (int cyc = 0; cyc < 500; ++cyc) {
        while (dma.canAccept()) dma.enqueue({accel::kOpcodeMemcpy, t1_src, t1_dst, 64, 0, /*tag=*/200 + fed++});
        peak = std::max(peak, fed - drained);
        scheduler.run(1, true, false);
        while (dma.tryDequeueCompletion(c6)) ++drained;
    }
    CHECK(drained > 0);
    CHECK(peak <= params.cmd_queue_depth + params.max_outstanding);

    root.enterTeardown();

    if (g_failures == 0)
        std::cout << "accel_dma_test: PASS\n";
    else
        std::cout << "accel_dma_test: " << g_failures << " FAILURE(S)\n";
    return g_failures ? 1 : 0;
}
