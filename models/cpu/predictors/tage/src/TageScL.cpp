// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Tenstorrent USA, Inc.

#include "predictors/tage/src/TageScL.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <stdexcept>

namespace predictors::tage {

namespace {

constexpr uint64_t mask(int bits) { return bits >= 64 ? ~0ULL : (1ULL << bits) - 1; }

// Saturating signed counter of `bits` bits.
void bump(int8_t& c, bool up, int bits) {
    if (up) {
        if (c < (1 << (bits - 1)) - 1) ++c;
    } else if (c > -(1 << (bits - 1))) {
        --c;
    }
}

void bump(int& c, bool up, int bits) { c = std::clamp(c + (up ? 1 : -1), -(1 << (bits - 1)), (1 << (bits - 1)) - 1); }

// |2c+1|: 1 is the weakest state of a signed counter.
int strength(int c) { return std::abs(2 * c + 1); }

uint32_t pcHash(uint64_t pc) { return static_cast<uint32_t>(pc ^ (pc >> 2)); }

// Fold `v` into `bits`-wide chunks.
uint32_t foldBits(uint64_t v, int bits) {
    uint32_t x = 0;
    for (; v != 0; v >>= bits) x ^= static_cast<uint32_t>(v & mask(bits));
    return x;
}

uint32_t rotl(uint32_t v, int r, int bits) {
    r %= bits;
    if (r == 0) return v & mask(bits);
    return static_cast<uint32_t>(((v << r) | (v >> (bits - r))) & mask(bits));
}

// GEHL index: the history is folded with a per-table stride so tables sharing it decorrelate.
uint32_t gehlIndex(uint64_t key, uint64_t hist, int table, int bits) {
    uint64_t x = key ^ hist;
    const int step = std::max(8 - table, 1);
    for (int s = step; s < 64 && (hist >> s) != 0; s += step) x ^= hist >> s;
    return static_cast<uint32_t>(x & mask(bits));
}

}  // namespace

class TageScL::Checkpoint final : public BPCheckpointBase {
   public:
    SpecHistory hist;
    bool have_lookup{false};
    Lookup lookup;
};

TageScL::TageScL(const TageScLConfig& cfg, std::string name)
    : mCfg(cfg),
      mName(std::move(name)),
      mNumLengths(cfg.num_tables / 2) {
    if (cfg.num_tables % 2 != 0 || cfg.num_tables > kMaxTables) throw std::invalid_argument(mName + ": num_tables must be even and <= 40");
    if (cfg.sc.size() > kMaxScComponents) throw std::invalid_argument(mName + ": too many SC components");

    mHistLen[1] = cfg.min_hist;
    for (int k = 2; k <= mNumLengths; ++k) {
        const double ratio = static_cast<double>(cfg.max_hist) / cfg.min_hist;
        mHistLen[k] = static_cast<int>(cfg.min_hist * std::pow(ratio, static_cast<double>(k - 1) / (mNumLengths - 1)) + 0.5);
    }
    if ((1 << cfg.ghist_buffer_bits) < 2 * cfg.max_hist) throw std::invalid_argument(mName + ": ghist buffer too small for max_hist");

    for (int t = 1; t <= cfg.num_tables; ++t) {
        mEnabled[t] = (t % 2 == 0) || (t >= cfg.assoc_first && t < cfg.assoc_end);
        mTagBits[t] = cfg.tag_bits + (t >= cfg.high_first ? 4 : 0);
    }
    for (int t : cfg.disabled_tables) mEnabled[t] = false;

    for (int k = 1; k <= mNumLengths; ++k) {
        const int tb = mTagBits[2 * k];
        if (cfg.compact_hash) {
            mFoldWidth[k] = {2 * cfg.log_bank + 3 + 2 * (k % 2 == 0), cfg.tag_bits + 5, cfg.tag_bits + 3};
        } else {
            mFoldWidth[k] = {cfg.log_bank, tb, tb - 1};
        }
    }

    for (const auto& c : cfg.sc) {
        mScFirstTable.push_back(static_cast<int>(mScTableBits.size()));
        const int n = static_cast<int>(c.lengths.size());
        for (int j = 0; j < n; ++j) mScTableBits.push_back(c.log_size - (cfg.sc_halve_short_tables && j >= n - 2 ? 1 : 0));
    }
    if (mScTableBits.size() > kMaxScTables) throw std::invalid_argument(mName + ": too many SC tables");

    reset();
}

void TageScL::reset() {
    const auto& c = mCfg;
    mLowBanks.assign(static_cast<size_t>(c.banks_low) << c.log_bank, TaggedEntry{});
    mHighBanks.assign(static_cast<size_t>(c.banks_high) << c.log_bank, TaggedEntry{});
    mBimPred.assign(1u << c.log_bimodal, 0);
    mBimHyst.assign(1u << (c.log_bimodal - c.bimodal_hyst_shift), 1);
    mUseAlt.assign(1u << c.log_use_alt, 0);
    mTick = 0;

    // Bias tables start out agreeing with the TAGE/loop prediction, which is index bit 0.
    static constexpr int8_t kBiasInit[4] = {-32, 31, -1, 0};
    static constexpr int8_t kBiasSkInit[4] = {-8, 7, -32, 31};
    for (int b = 0; b < 3; ++b) {
        mBias[b].resize(1u << c.log_bias);
        for (size_t j = 0; j < mBias[b].size(); ++j) mBias[b][j] = (b == 1 ? kBiasSkInit : kBiasInit)[j & 3];
    }
    mScTables.clear();
    for (int bits : mScTableBits) {
        std::vector<int8_t> t(1u << bits, 0);
        for (size_t j = 0; j < t.size(); j += 2) t[j] = -1;
        mScTables.push_back(std::move(t));
    }
    const size_t weight_slots = 1u << (c.log_threshold_tables / 2);
    mScWeight.assign(c.sc.size(), std::vector<int8_t>(weight_slots, 7));
    mBiasWeight.assign(weight_slots, 4);
    mPcThreshold.assign(1u << c.log_threshold_tables, 0);
    mThreshold = c.threshold_init << 3;
    mChooserHigh = 0;
    mChooserMed = 0;
    mLocal.clear();
    for (const auto& l : c.local) mLocal.emplace_back(1u << l.log_entries, 0);
    mImliHist.assign(1u << c.imli_count_bits, 0);

    mLoop.assign(1u << c.log_loop, LoopEntry{});
    mUseLoop = -1;

    mSpec = SpecHistory{};
    mGhist.assign(1u << c.ghist_buffer_bits, 0);
    mGhistHighPos = 0;
    mUndo.clear();
    mHaveLookup = false;
    mRandState = 0x9E3779B97F4A7C15ULL;
}

uint32_t TageScL::nextRandom() {
    mRandState ^= mRandState << 13;
    mRandState ^= mRandState >> 7;
    mRandState ^= mRandState << 17;
    return static_cast<uint32_t>(mRandState >> 16);
}

TageScL::TaggedEntry& TageScL::entry(int t, const Lookup& lk) { return (t < mCfg.high_first ? mLowBanks : mHighBanks)[lk.index[t]]; }

const TageScL::TaggedEntry& TageScL::entry(int t, const Lookup& lk) const { return (t < mCfg.high_first ? mLowBanks : mHighBanks)[lk.index[t]]; }

uint32_t TageScL::useAltIndex(const Decision& d) const { return static_cast<uint32_t>((((d.hit - 1) / 8) << 1 | d.alt_conf) & mask(mCfg.log_use_alt)); }

int TageScL::bimodalValue(uint32_t idx) const { return (mBimPred[idx] << 1) | mBimHyst[idx >> mCfg.bimodal_hyst_shift]; }

uint32_t TageScL::localIndex(int l, addr_t pc) const {
    const auto& spec = mCfg.local[l];
    return static_cast<uint32_t>((pc ^ (pc >> spec.pc_shift)) & mask(spec.log_entries));
}

uint32_t TageScL::loopSlot(addr_t pc, int way) const {
    const int set_bits = mCfg.log_loop - 2;
    const uint32_t set = static_cast<uint32_t>(pcHash(pc) & mask(set_bits)) << 2;
    const uint32_t skew = static_cast<uint32_t>((pc >> set_bits) & mask(set_bits));
    return (set ^ ((skew >> way) << 2)) + way;
}

uint16_t TageScL::loopTag(addr_t pc) const {
    const uint64_t t = (pc >> (mCfg.log_loop - 2)) & mask(2 * mCfg.loop_tag_bits);
    return static_cast<uint16_t>((t ^ (t >> mCfg.loop_tag_bits)) & mask(mCfg.loop_tag_bits));
}

int TageScL::findLoop(addr_t pc) const {
    const uint16_t tag = loopTag(pc);
    for (int w = 0; w < 4; ++w) {
        const uint32_t s = loopSlot(pc, w);
        if (mLoop[s].tag == tag) return static_cast<int>(s);
    }
    return -1;
}

void TageScL::computeLookup(addr_t pc, Lookup& lk) const {
    const auto& c = mCfg;
    lk.pc = pc;
    const uint32_t pc32 = static_cast<uint32_t>(pc);
    for (int k = 1; k <= mNumLengths; ++k) {
        const int t = 2 * k - 1;  // the two ways of a length share one hash
        const auto& f = mSpec.folds[k];
        const int path_len = std::min(mHistLen[k], c.path_bits);
        const uint32_t path_mix = rotl(foldBits(mSpec.path & mask(path_len), c.log_bank), t, c.log_bank);
        uint64_t idx = pc32 ^ (pc32 >> (std::abs(c.log_bank - t) + 1)) ^ f[0] ^ path_mix;
        const int tb = mTagBits[t];
        uint32_t tag;
        if (c.compact_hash) {
            idx ^= (idx >> c.log_bank) ^ (idx >> (2 * c.log_bank));
            const uint32_t prev = (k > 1) ? mSpec.folds[k - 1][0] : 0;
            uint32_t x = (prev << 2) ^ pc32 ^ (pc32 >> 2) ^ f[0] ^ rotl(path_mix, 1, c.log_bank) ^ f[1] ^ (f[2] << 1);
            tag = static_cast<uint32_t>((x ^ (x >> tb) ^ (x >> (2 * tb))) & mask(tb));
        } else {
            tag = static_cast<uint32_t>((pc32 ^ f[1] ^ (f[2] << 1)) & mask(tb));
        }
        lk.index[t] = static_cast<uint32_t>(idx & mask(c.log_bank));
        lk.tag[t] = static_cast<uint16_t>(tag);
        lk.index[t + 1] = lk.index[t] ^ static_cast<uint32_t>(tag & mask(c.log_bank));
        lk.tag[t + 1] = lk.tag[t];
    }

    // Each enabled table of a group takes the next bank, so one branch never conflicts with itself.
    auto spread = [&](int first, int end, int banks, int len) {
        uint32_t bank = static_cast<uint32_t>((pc ^ (mSpec.path & mask(std::min(len, c.path_bits)))) % banks);
        for (int t = first; t < end; ++t) {
            if (!tableEnabled(t)) continue;
            lk.index[t] += bank << c.log_bank;
            bank = (bank + 1) % banks;
        }
    };
    spread(c.high_first, c.num_tables + 1, c.banks_high, mHistLen[lengthOf(c.high_first)]);
    spread(1, c.high_first, c.banks_low, mHistLen[1]);

    for (size_t i = 0; i < c.sc.size(); ++i) {
        const auto& comp = c.sc[i];
        switch (comp.source) {
            case ScSource::BackwardTaken:
                lk.sc_hist[i] = mSpec.backward_taken;
                break;
            case ScSource::Taken:
                lk.sc_hist[i] = mSpec.taken;
                break;
            case ScSource::Path:
                lk.sc_hist[i] = mSpec.path;
                break;
            case ScSource::Local:
                lk.sc_hist[i] = mLocal[comp.local][localIndex(comp.local, pc)];
                break;
            case ScSource::ImliHistory:
                lk.sc_hist[i] = mImliHist[mSpec.imli];
                break;
            case ScSource::ImliCount:
                lk.sc_hist[i] = mSpec.imli;
                break;
        }
    }
}

TageScL::Decision TageScL::decide(const Lookup& lk, bool speculative) const {
    const auto& c = mCfg;
    const addr_t pc = lk.pc;
    Decision d;

    // TAGE: longest matching table provides, the next longest is the alternate.
    d.bim_index = static_cast<uint32_t>(pcHash(pc) & mask(c.log_bimodal));
    const int bim = bimodalValue(d.bim_index);
    const bool bim_pred = bim >= 2;
    d.high_conf = (bim == 0 || bim == 3);
    d.low_conf = !d.high_conf;
    d.alt_conf = d.high_conf;
    d.longest_pred = d.alt_pred = d.tage_pred = bim_pred;
    for (int t = c.num_tables; t > 0; --t) {
        if (tableEnabled(t) && entry(t, lk).tag == lk.tag[t]) {
            d.hit = t;
            break;
        }
    }
    for (int t = d.hit - 1; t > 0; --t) {
        if (tableEnabled(t) && entry(t, lk).tag == lk.tag[t]) {
            d.alt = t;
            break;
        }
    }
    if (d.hit > 0) {
        const int ctr = entry(d.hit, lk).ctr;
        d.longest_pred = ctr >= 0;
        if (d.alt > 0) {
            const int alt_ctr = entry(d.alt, lk).ctr;
            d.alt_pred = alt_ctr >= 0;
            d.alt_conf = strength(alt_ctr) > 1;
        }
        // USE_ALT_ON_NA: a weak provider is likely a fresh allocation.
        const bool use_alt = mUseAlt[useAltIndex(d)] >= 0;
        d.tage_pred = (use_alt && strength(ctr) <= 1) ? d.alt_pred : d.longest_pred;
        d.high_conf = strength(ctr) >= (1 << c.ctr_bits) - 1;
        d.low_conf = strength(ctr) == 1;
        d.med_conf = strength(ctr) == 5;
    }

    // Loop predictor overrides when confident and when it has been paying off.
    d.loop_slot = findLoop(pc);
    if (d.loop_slot >= 0) {
        const LoopEntry& e = mLoop[d.loop_slot];
        d.loop_valid = (e.conf == c.loop_conf_max) || (e.conf * e.trip > 128);
        const uint16_t iter = speculative ? e.spec_iter : e.iter;
        d.loop_pred = (iter + 1 == e.trip) ? !e.dir : e.dir;
    }
    d.inter_pred = (mUseLoop >= 0 && d.loop_valid) ? d.loop_pred : d.tage_pred;

    // Statistical corrector: bias tables keyed by TAGE confidence plus GEHL components.
    const uint32_t ph = pcHash(pc);
    const uint32_t ip = d.inter_pred;
    d.bias_index[0] = static_cast<uint32_t>((((ph << 1) ^ (d.low_conf && d.longest_pred != d.alt_pred)) << 1 | ip) & mask(c.log_bias));
    d.bias_index[1] = static_cast<uint32_t>((((pc ^ (pc >> (c.log_bias - 2))) << 1 ^ d.high_conf) << 1 | ip) & mask(c.log_bias));
    d.bias_index[2] = static_cast<uint32_t>(
        (ip | d.high_conf << 1 | d.low_conf << 2 | (d.alt != 0) << 3 | ((d.hit + 1) / 4) << 4 | (c.bias_bank_uses_pc ? ph << 7 : 0)) & mask(c.log_bias));
    const uint32_t wslot = ph & mask(c.log_threshold_tables / 2);
    int doubled = 0;
    for (int b = 0; b < 3; ++b) d.bias_sum += 2 * mBias[b][d.bias_index[b]] + 1;
    d.sum = d.bias_sum * (mBiasWeight[wslot] >= 0 ? 2 : 1);
    doubled += mBiasWeight[wslot] >= 0;
    for (size_t i = 0; i < c.sc.size(); ++i) {
        const auto& comp = c.sc[i];
        const uint64_t key = comp.key_with_inter_pred ? ((pc << 1) | ip) : pc;
        int partial = 0;
        for (size_t j = 0; j < comp.lengths.size(); ++j) {
            const int g = mScFirstTable[i] + static_cast<int>(j);
            const uint32_t idx = gehlIndex(key, lk.sc_hist[i] & mask(comp.lengths[j]), static_cast<int>(j), mScTableBits[g]);
            d.sc_index[g] = idx;
            partial += 2 * mScTables[g][idx] + 1;
        }
        d.sc_partial[i] = partial;
        d.sum += partial * (mScWeight[i][wslot] >= 0 ? 2 : 1);
        doubled += mScWeight[i][wslot] >= 0;
    }
    d.threshold = (mThreshold >> 3) + mPcThreshold[ph & mask(c.log_threshold_tables)] + c.threshold_bonus * doubled;
    d.sc_pred = d.sum >= 0;

    // Overrule a confident TAGE only when the SC sum is clearly away from zero.
    d.final_pred = d.inter_pred;
    if (d.sc_pred != d.inter_pred) {
        const int s = std::abs(d.sum);
        d.final_pred = d.sc_pred;
        if (d.high_conf) {
            if (s < d.threshold / 4) {
                d.final_pred = d.inter_pred;
            } else if (s < d.threshold / 2) {
                d.final_pred = (mChooserHigh < 0) ? d.sc_pred : d.inter_pred;
            }
        }
        if (d.med_conf && s < d.threshold / 4) d.final_pred = (mChooserMed < 0) ? d.sc_pred : d.inter_pred;
    }
    return d;
}

DirectionResult TageScL::predict(const BranchInfo& b) {
    computeLookup(b.pc, mLookup);
    mHaveLookup = true;
    const Decision d = decide(mLookup, true);
    mTaggedProvider += d.hit > 0;
    mLoopOverrides += d.inter_pred != d.tage_pred;
    mScOverrides += d.final_pred != d.inter_pred;
    return DirectionResult{d.final_pred};
}

std::vector<PredictorStat> TageScL::stats() const {
    return {{"tagged_provider", "Conditional-branch predictions provided by a tagged table rather than the bimodal, wrong path included", &mTaggedProvider},
            {"loop_overrides", "Predictions where the loop predictor overrode TAGE, wrong path included", &mLoopOverrides},
            {"sc_overrides", "Predictions where the statistical corrector overrode TAGE or the loop predictor, wrong path included", &mScOverrides}};
}

BPCheckpoint TageScL::snapshot() const {
    auto cp = std::make_unique<Checkpoint>();
    cp->hist = mSpec;
    cp->have_lookup = mHaveLookup;
    if (mHaveLookup) cp->lookup = mLookup;
    return BPCheckpoint(std::move(cp));
}

void TageScL::pushHistoryBit(bool bit, uint32_t path_bits) {
    const uint32_t buf_mask = static_cast<uint32_t>(mGhist.size() - 1);
    mSpec.ghist_head = (mSpec.ghist_head + 1) & buf_mask;
    mGhistHighPos = std::max(mGhistHighPos, ++mSpec.ghist_pos);
    mGhist[mSpec.ghist_head] = bit;
    mSpec.path = (mSpec.path << 1) ^ path_bits;
    for (int k = 1; k <= mNumLengths; ++k) {
        const uint32_t leaving = mGhist[(mSpec.ghist_head - mHistLen[k]) & buf_mask];
        for (int f = 0; f < 3; ++f) {
            const int w = mFoldWidth[k][f];
            uint32_t v = (mSpec.folds[k][f] << 1) ^ bit ^ (leaving << (mHistLen[k] % w));
            v ^= v >> w;
            mSpec.folds[k][f] = static_cast<uint32_t>(v & mask(w));
        }
    }
}

void TageScL::speculativeUpdate(const BranchInfo& b, bool taken, addr_t target) {
    const auto& c = mCfg;
    const addr_t pc = b.pc;
    const BranchType type = b.type;
    mHaveLookup = false;
    const uint64_t seq = mSpec.seq++;

    if (type == BranchType::Conditional) {
        const bool backward = target < pc;
        auto log = [&](UndoKind k, int table, uint32_t idx, uint64_t old) { mUndo.push_back(UndoRecord{seq, k, static_cast<uint8_t>(table), idx, old}); };
        for (const auto& comp : c.sc) {
            if (comp.source == ScSource::ImliHistory) {
                log(UndoKind::ImliHistory, 0, mSpec.imli, mImliHist[mSpec.imli]);
                mImliHist[mSpec.imli] = (mImliHist[mSpec.imli] << 1) | taken;
                break;
            }
        }
        if (backward) mSpec.imli = taken ? std::min<uint32_t>(mSpec.imli + 1, mask(c.imli_count_bits)) : 0;
        mSpec.backward_taken = (mSpec.backward_taken << 1) | (taken && backward);
        mSpec.taken = (mSpec.taken << 1) | taken;
        for (size_t l = 0; l < c.local.size(); ++l) {
            const uint32_t i = localIndex(static_cast<int>(l), pc);
            uint64_t& h = mLocal[l][i];
            log(UndoKind::Local, static_cast<int>(l), i, h);
            h = (h << 1) | taken;
            if (c.local[l].mix_pc_low) h ^= pc & 15;
        }
        const int slot = findLoop(pc);
        if (slot >= 0) {
            LoopEntry& e = mLoop[slot];
            log(UndoKind::LoopIter, 0, static_cast<uint32_t>(slot), e.spec_iter);
            e.spec_iter = (taken == e.dir) ? static_cast<uint16_t>((e.spec_iter + 1) & mask(c.loop_iter_bits)) : 0;
        }
    }

    // Two history bits per branch, three for indirect ones (CBP-5 history management).
    const int nbits = indirectTarget(type) ? 3 : 2;
    uint32_t h = pcHash(pc) ^ taken;
    uint32_t p = static_cast<uint32_t>(pc ^ (pc >> 2) ^ (pc >> 4));
    for (int i = 0; i < nbits; ++i, h >>= 1, p >>= 1) pushHistoryBit(h & 1, p & 127);
    mSpec.path &= static_cast<uint32_t>(mask(c.path_bits));
}

void TageScL::undoTo(uint64_t seq) {
    while (!mUndo.empty() && mUndo.back().seq >= seq) {
        const UndoRecord& r = mUndo.back();
        switch (r.kind) {
            case UndoKind::Local:
                mLocal[r.table][r.index] = r.old;
                break;
            case UndoKind::ImliHistory:
                mImliHist[r.index] = r.old;
                break;
            case UndoKind::LoopIter:
                mLoop[r.index].spec_iter = static_cast<uint16_t>(r.old);
                break;
        }
        mUndo.pop_back();
    }
}

void TageScL::restore(const BPCheckpoint& cp) {
    const Checkpoint* c = cp.as<Checkpoint>();
    if (!c) return;
    // Bits pushed since the checkpoint have wrapped onto the max_hist bits it still needs.
    if (mGhistHighPos - c->hist.ghist_pos > mGhist.size() - mCfg.max_hist) {
        throw std::logic_error(mName + ": restoring history " + std::to_string(mGhistHighPos - c->hist.ghist_pos) +
                               " bits back overruns the ghist buffer; bound fetch run-ahead or raise ghist_buffer_bits");
    }
    undoTo(c->hist.seq);
    mSpec = c->hist;
    mHaveLookup = c->have_lookup;
    if (mHaveLookup) mLookup = c->lookup;
}

void TageScL::update(const DirectionUpdate& u) {
    const Checkpoint* cp = u.at_predict.as<Checkpoint>();
    if (!cp || !cp->have_lookup) return;
    while (!mUndo.empty() && mUndo.front().seq <= cp->hist.seq) mUndo.pop_front();

    const Lookup& lk = cp->lookup;
    const Decision d = decide(lk, false);
    trainLoop(lk.pc, u.taken, d.final_pred != u.taken, d);
    trainSc(lk.pc, u.taken, d);
    trainTage(lk, u.taken, d);
}

void TageScL::trainLoop(addr_t pc, bool taken, bool allocate, const Decision& d) {
    const auto& c = mCfg;
    if (d.loop_valid && d.final_pred != d.loop_pred) bump(mUseLoop, d.loop_pred == taken, 7);

    if (d.loop_slot >= 0) {
        LoopEntry& e = mLoop[d.loop_slot];
        if (d.loop_valid) {
            if (taken != d.loop_pred) {
                e = LoopEntry{0, 0, 0, e.tag, 0, 0, e.dir};
                return;
            }
            if ((d.loop_pred != d.tage_pred || (nextRandom() & 7) == 0) && e.age < c.loop_conf_max) ++e.age;
        }
        e.iter = static_cast<uint16_t>((e.iter + 1) & mask(c.loop_iter_bits));
        if (e.iter > e.trip) {  // ran past the recorded trip count: relearn it
            e.conf = 0;
            e.trip = 0;
        }
        if (taken != e.dir) {
            if (e.iter == e.trip) {
                if (e.conf < c.loop_conf_max) ++e.conf;
                if (e.trip < 3) {  // too short to be worth predicting
                    e.dir = taken;
                    e.trip = 0;
                    e.age = 0;
                    e.conf = 0;
                }
            } else if (e.trip == 0) {
                e.conf = 0;
                e.trip = e.iter;
            } else {
                e.trip = 0;
                e.conf = 0;
            }
            e.iter = 0;
        }
    } else if (allocate && (nextRandom() & 3) == 0) {
        LoopEntry& e = mLoop[loopSlot(pc, static_cast<int>(nextRandom() & 3))];
        if (e.age == 0) {
            e = LoopEntry{0, 0, 0, loopTag(pc), 0, 7, !taken};  // most mispredictions are loop exits
        } else {
            --e.age;
        }
    }
}

void TageScL::trainSc(addr_t pc, bool taken, const Decision& d) {
    const auto& c = mCfg;
    const int s = std::abs(d.sum);
    if (d.inter_pred != d.sc_pred && s < d.threshold) {
        if (d.high_conf && s < d.threshold / 2 && s >= d.threshold / 4) bump(mChooserHigh, d.inter_pred == taken, c.chooser_bits);
        if (d.med_conf && s < d.threshold / 4) bump(mChooserMed, d.inter_pred == taken, c.chooser_bits);
    }
    if (d.sc_pred == taken && s >= d.threshold) return;

    const uint32_t ph = pcHash(pc);
    int& pc_thr = mPcThreshold[ph & mask(c.log_threshold_tables)];
    const bool up = d.sc_pred != taken;
    bump(pc_thr, up, c.pc_threshold_bits);
    bump(mThreshold, up, c.threshold_bits);

    // A weight learns only when doubling its component would flip the sum.
    const uint32_t wslot = ph & mask(c.log_threshold_tables / 2);
    auto trainWeight = [&](int8_t& w, int partial) {
        const int once = d.sum - (w >= 0 ? partial : 0);
        if ((once + partial >= 0) != (once >= 0)) bump(w, (partial >= 0) == taken, c.sc_weight_bits);
    };
    trainWeight(mBiasWeight[wslot], d.bias_sum);
    for (int b = 0; b < 3; ++b) bump(mBias[b][d.bias_index[b]], taken, c.sc_ctr_bits);
    for (size_t i = 0; i < c.sc.size(); ++i) {
        trainWeight(mScWeight[i][wslot], d.sc_partial[i]);
        for (size_t j = 0; j < c.sc[i].lengths.size(); ++j) {
            const int g = mScFirstTable[i] + static_cast<int>(j);
            bump(mScTables[g][d.sc_index[g]], taken, c.sc_ctr_bits);
        }
    }
}

void TageScL::trainTage(const Lookup& lk, bool taken, const Decision& d) {
    const auto& c = mCfg;
    bool alloc = (d.tage_pred != taken) && lengthOf(d.hit) < mNumLengths;
    if (d.hit > 0 && strength(entry(d.hit, lk).ctr) <= 1) {
        if (d.longest_pred == taken) alloc = false;  // the fresh entry was right; let it mature
        if (d.longest_pred != d.alt_pred) bump(mUseAlt[useAltIndex(d)], d.alt_pred == taken, c.use_alt_bits);
    }
    if (d.final_pred == taken && (nextRandom() & 31) != 0) alloc = false;
    if (alloc) allocate(lk, taken, d);

    auto trainBimodal = [&] {
        int v = bimodalValue(d.bim_index);
        v = taken ? std::min(v + 1, 3) : std::max(v - 1, 0);
        mBimPred[d.bim_index] = static_cast<uint8_t>(v >> 1);
        mBimHyst[d.bim_index >> c.bimodal_hyst_shift] = static_cast<uint8_t>(v & 1);
    };
    auto muteIfWeak = [&](TaggedEntry& e) {
        if (strength(e.ctr) == 1) e.u = 0;
    };

    if (d.hit == 0) {
        trainBimodal();
        return;
    }
    TaggedEntry& provider = entry(d.hit, lk);
    // A fresh provider that mispredicted also trains the alternate, which was likely in use.
    if (strength(provider.ctr) == 1 && d.longest_pred != taken) {
        if (d.alt > 0) {
            TaggedEntry& alt = entry(d.alt, lk);
            if (c.mute_weak_entries) muteIfWeak(alt);
            bump(alt.ctr, taken, c.ctr_bits);
            if (c.mute_weak_entries) muteIfWeak(alt);
        } else {
            trainBimodal();
        }
    }
    if (c.mute_weak_entries) muteIfWeak(provider);
    bump(provider.ctr, taken, c.ctr_bits);
    muteIfWeak(provider);  // a sign change means it cannot have been useful
    if (d.alt > 0 && d.alt_pred == taken && strength(entry(d.alt, lk).ctr) == (1 << c.ctr_bits) - 1 && provider.u == 1 && d.longest_pred == taken) {
        provider.u = 0;  // a saturated alternate already gets this right
    }
    if (d.longest_pred != d.alt_pred && d.longest_pred == taken && provider.u < mask(c.u_bits)) ++provider.u;
}

void TageScL::allocate(const Lookup& lk, bool taken, const Decision& d) {
    const auto& c = mCfg;
    int budget = c.max_alloc;
    int penalty = 0;
    int true_penalty = 0;
    int allocated = 0;
    // Start one length above the provider, or two with probability 1/4.
    const int skip = ((nextRandom() & 127) < 32) ? 2 : 1;
    for (int k = lengthOf(d.hit) + skip; k <= mNumLengths; ++k) {
        const int first_way = static_cast<int>(nextRandom() & 1);
        bool done = false;
        for (int w : {first_way, 1 - first_way}) {
            const int t = 2 * k - 1 + w;
            if (!tableEnabled(t)) continue;
            TaggedEntry& e = entry(t, lk);
            if (e.u == 0) {
                if (c.alloc_skips_strong && strength(e.ctr) > 3) {
                    e.ctr += (e.ctr > 0) ? -1 : 1;
                    continue;
                }
                e.tag = lk.tag[t];
                e.ctr = taken ? 0 : -1;
                e.u = (c.alloc_random_protect && (nextRandom() & 31) == 0) ? 1 : 0;
                ++allocated;
                done = true;
                break;
            }
            if (c.alloc_random_protect && e.u == 1 && strength(e.ctr) == 1) {
                if ((nextRandom() & 7) == 0) e.u = 0;
            } else {
                ++true_penalty;
            }
            ++penalty;
        }
        if (done) {
            if (--budget == 0) break;
            ++k;  // leave a gap before the next allocation
        }
    }

    // Age the u bits once allocations keep failing on useful entries.
    mTick += penalty + (c.tick_counts_true_penalty ? true_penalty : 0) - c.tick_alloc_weight * allocated;
    mTick = std::max(mTick, 0);
    if (mTick >= c.u_reset_period) {
        for (auto* banks : {&mLowBanks, &mHighBanks}) {
            for (auto& e : *banks) {
                if (e.u > 0) --e.u;
            }
        }
        mTick = 0;
    }
}

uint64_t TageScL::storageBits() const {
    const auto& c = mCfg;
    const uint64_t entry_low = c.ctr_bits + c.u_bits + c.tag_bits;
    const uint64_t entry_high = entry_low + 4;
    uint64_t bits = (static_cast<uint64_t>(c.banks_low) << c.log_bank) * entry_low + (static_cast<uint64_t>(c.banks_high) << c.log_bank) * entry_high;
    bits += (1ULL << c.log_bimodal) + (1ULL << (c.log_bimodal - c.bimodal_hyst_shift));
    bits += (1ULL << c.log_use_alt) * c.use_alt_bits;
    bits += c.max_hist + c.path_bits + 10;  // global history, path history, tick counter

    bits += 3 * (1ULL << c.log_bias) * c.sc_ctr_bits;
    for (int b : mScTableBits) bits += (1ULL << b) * c.sc_ctr_bits;
    for (size_t i = 0; i < c.sc.size(); ++i) {
        const auto& comp = c.sc[i];
        const uint64_t longest = comp.lengths.front();
        if (comp.source == ScSource::Local) bits += (1ULL << c.local[comp.local].log_entries) * longest;
        if (comp.source == ScSource::ImliHistory) bits += (1ULL << c.imli_count_bits) * longest;
        if (comp.source == ScSource::BackwardTaken || comp.source == ScSource::Taken) bits += longest;
    }
    bits += c.imli_count_bits;
    bits += (c.sc.size() + 1) * (1ULL << (c.log_threshold_tables / 2)) * c.sc_weight_bits;
    bits += (1ULL << c.log_threshold_tables) * c.pc_threshold_bits + c.threshold_bits;
    bits += 2 * c.chooser_bits;

    bits += (1ULL << c.log_loop) * (2 * c.loop_iter_bits + c.loop_tag_bits + 4 + 4 + 1) + 7;
    return bits;
}

}  // namespace predictors::tage
