// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Tenstorrent USA, Inc.

#include "FetchUnit.hpp"

#include "models/cpu/common/ExecutionDriver.hpp"
#include "models/cpu/common/whisper_include_fix.hpp"

void FetchUnit::fetchNext() {
    if (mExecutionDriver) {
        // Whisper-driven fetch: get PC, peek+execute in ISS, send instruction.
        // Flush: not wired here (no branch prediction). When adding branch resolution,
        // call mExecutionDriver->flushInstruction(mispredicted_tag) on wrong-path detection.
        if (mExecutionDriver->isFinished()) {
            return;  // No more instructions; don't schedule again
        }
        for (uint64_t i = 0; i < mFetchWidth; ++i) {
            if (mExecutionDriver->isFinished()) break;
            uint64_t pc = mFirstWhisperFetch ? mExecutionDriver->getInitPc() : mExecutionDriver->getNextPc();
            mFirstWhisperFetch = false;
            if (pc == 0) break;  // program ended (e.g. ecall); don't fetch from 0
            cpu::InstPtr rich_inst = mExecutionDriver->peekInstruction(pc);
            if (!rich_inst) break;
            bool ok = mExecutionDriver->executeInstruction(rich_inst);
            if (!ok) break;
            Instruction inst;
            inst.pc = rich_inst->getFetchVA();
            inst.driver_tag = static_cast<uint64_t>(rich_inst->getId().getInstNum());
            const auto& pac = *rich_inst->getPerfPtr();
            inst.is_load = pac.isLoad() || pac.isAmo() || pac.isVectorLoad();
            inst.is_store = pac.isStore() || pac.isSc() || pac.isAmo() || pac.isVectorStore() || pac.isCbo_zero();
            inst.is_mem = inst.is_load || inst.is_store;
            inst.mem_addr = inst.is_mem ? pac.dataVa() : 0;
            ILOG("[fetch] cycle " << getClock()->currentCycle() << " tag " << inst.driver_tag << " pc=0x" << std::hex << inst.pc << std::dec
                                  << (inst.is_mem ? (inst.is_load ? " load" : " store") : " alu"));
            inst_out.send(inst, 1);
        }
    } else {
        // Synthetic sequential fetch (no Whisper: fake mem op for pipeline traffic)
        for (uint64_t i = 0; i < mFetchWidth; ++i) {
            Instruction inst;
            inst.pc = mPc;
            inst.is_mem = true;
            inst.is_load = true;
            inst.mem_addr = 0xDEADBEEF;  // placeholder when no ExecutionDriver
            ILOG("[fetch] cycle " << getClock()->currentCycle() << " sending instruction " << (i + 1) << "/" << mFetchWidth << " pc=0x" << std::hex << inst.pc
                                  << std::dec);
            inst_out.send(inst, 1);
            mPc += mInstrSize;
        }
    }
    mEvFetch.schedule(1);
}
