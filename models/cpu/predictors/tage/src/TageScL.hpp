// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Tenstorrent USA, Inc.

#pragma once

#include <array>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

#include "predictors/common/DirectionPredictorInterface.hpp"
#include "predictors/tage/src/TageScLConfig.hpp"

namespace predictors::tage {

// TAGE-SC-L after Seznec, "TAGE-SC-L Branch Predictors Again" (CBP-5, 2016): a
// partially tagged geometric-history predictor, a loop predictor and a GEHL-style
// statistical corrector. History is speculative at predict; tables train at commit
// by re-reading the entries named by the indices stored in the checkpoint.
class TageScL : public DirectionPredictorInterface {
   public:
    static constexpr int kMaxTables = 40;
    static constexpr int kMaxLengths = kMaxTables / 2;
    static constexpr int kMaxScComponents = 8;
    static constexpr int kMaxScTables = 24;

    TageScL(const TageScLConfig& cfg, std::string name);

    DirectionResult predict(const BranchInfo& b) override;
    BPCheckpoint snapshot() const override;
    void speculativeUpdate(const BranchInfo& b, bool taken, addr_t target) override;
    void update(const DirectionUpdate& u) override;
    void restore(const BPCheckpoint& cp) override;
    void reset();
    std::vector<PredictorStat> stats() const override;

    // Counted as in the CBP-5 entry, plus the loop predictor's 7-bit use counter.
    uint64_t storageBits() const;

   private:
    // Speculative registers; a checkpoint copies them whole.
    struct SpecHistory {
        uint64_t seq{0};  // speculative updates so far; undo-log position
        uint32_t ghist_head{0};
        uint64_t ghist_pos{0};  // bits pushed so far, unwrapped
        uint32_t path{0};
        uint64_t backward_taken{0};
        uint64_t taken{0};
        uint32_t imli{0};
        std::array<std::array<uint32_t, 3>, kMaxLengths + 1> folds{};  // index, tag0, tag1 per length
    };

    // Table addresses computed at predict, reused at commit.
    struct Lookup {
        addr_t pc{0};
        std::array<uint32_t, kMaxTables + 1> index{};
        std::array<uint16_t, kMaxTables + 1> tag{};
        std::array<uint64_t, kMaxScComponents> sc_hist{};
    };

    class Checkpoint;

    // Everything the prediction derived from the tables; recomputed at commit.
    struct Decision {
        uint32_t bim_index{0};
        int hit{0};
        int alt{0};
        bool longest_pred{false};
        bool alt_pred{false};
        bool tage_pred{false};
        bool high_conf{false};
        bool med_conf{false};
        bool low_conf{false};
        bool alt_conf{false};
        int loop_slot{-1};
        bool loop_valid{false};
        bool loop_pred{false};
        bool inter_pred{false};
        uint32_t bias_index[3]{};
        int bias_sum{0};
        std::array<uint32_t, kMaxScTables> sc_index{};
        std::array<int, kMaxScComponents> sc_partial{};
        int sum{0};
        int threshold{0};
        bool sc_pred{false};
        bool final_pred{false};
    };

    struct TaggedEntry {
        int8_t ctr{0};
        uint8_t u{0};
        uint16_t tag{0};
    };

    struct LoopEntry {
        uint16_t trip{0};       // iterations of the last complete loop execution
        uint16_t iter{0};       // committed iteration count
        uint16_t spec_iter{0};  // iteration count including in-flight branches
        uint16_t tag{0};
        uint8_t conf{0};
        uint8_t age{0};
        bool dir{false};  // direction while iterating
    };

    enum class UndoKind : uint8_t { Local, ImliHistory, LoopIter };
    struct UndoRecord {
        uint64_t seq;
        UndoKind kind;
        uint8_t table;
        uint32_t index;
        uint64_t old;
    };

    void computeLookup(addr_t pc, Lookup& lk) const;
    Decision decide(const Lookup& lk, bool speculative) const;
    void trainLoop(addr_t pc, bool taken, bool allocate, const Decision& d);
    void trainSc(addr_t pc, bool taken, const Decision& d);
    void trainTage(const Lookup& lk, bool taken, const Decision& d);
    void allocate(const Lookup& lk, bool taken, const Decision& d);
    void pushHistoryBit(bool bit, uint32_t path_bits);
    void undoTo(uint64_t seq);

    TaggedEntry& entry(int t, const Lookup& lk);
    const TaggedEntry& entry(int t, const Lookup& lk) const;
    bool tableEnabled(int t) const { return mEnabled[t]; }
    int lengthOf(int t) const { return (t + 1) / 2; }
    uint32_t useAltIndex(const Decision& d) const;
    int bimodalValue(uint32_t idx) const;
    uint32_t localIndex(int l, addr_t pc) const;
    uint32_t loopSlot(addr_t pc, int way) const;
    uint16_t loopTag(addr_t pc) const;
    int findLoop(addr_t pc) const;
    uint32_t nextRandom();

    const TageScLConfig mCfg;
    const std::string mName;
    const int mNumLengths;

    // Static geometry.
    std::array<int, kMaxLengths + 1> mHistLen{};
    std::array<std::array<int, 3>, kMaxLengths + 1> mFoldWidth{};
    std::array<bool, kMaxTables + 1> mEnabled{};
    std::array<int, kMaxTables + 1> mTagBits{};
    std::vector<int> mScTableBits;   // per SC table, flattened over components
    std::vector<int> mScFirstTable;  // per component

    // TAGE.
    std::vector<TaggedEntry> mLowBanks;
    std::vector<TaggedEntry> mHighBanks;
    std::vector<uint8_t> mBimPred;
    std::vector<uint8_t> mBimHyst;
    std::vector<int8_t> mUseAlt;
    int mTick{0};

    // Statistical corrector.
    std::array<std::vector<int8_t>, 3> mBias;
    std::vector<std::vector<int8_t>> mScTables;
    std::vector<std::vector<int8_t>> mScWeight;  // per component, then per pc slot
    std::vector<int8_t> mBiasWeight;
    std::vector<int> mPcThreshold;
    int mThreshold{0};
    int8_t mChooserHigh{0};
    int8_t mChooserMed{0};
    std::vector<std::vector<uint64_t>> mLocal;
    std::vector<uint64_t> mImliHist;

    // Loop predictor.
    std::vector<LoopEntry> mLoop;
    int8_t mUseLoop{-1};

    // Speculative state.
    SpecHistory mSpec;
    std::vector<uint8_t> mGhist;
    uint64_t mGhistHighPos{0};  // furthest ghist_pos ever written
    std::deque<UndoRecord> mUndo;
    Lookup mLookup;
    bool mHaveLookup{false};

    uint64_t mRandState{0};

    uint64_t mTaggedProvider{0};
    uint64_t mLoopOverrides{0};
    uint64_t mScOverrides{0};
};

}  // namespace predictors::tage
