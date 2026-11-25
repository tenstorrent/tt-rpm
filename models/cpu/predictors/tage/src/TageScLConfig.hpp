// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Tenstorrent USA, Inc.

#pragma once

#include <cstdint>
#include <vector>

namespace predictors::tage {

// History feeding one statistical-corrector GEHL component.
enum class ScSource : uint8_t {
    BackwardTaken,  // global: taken backward conditional branches
    Taken,          // global: conditional outcomes
    Path,           // global path history shared with TAGE
    Local,          // per-PC local history (ScComponent::local selects the table)
    ImliHistory,    // outcome history of the current inner-loop iteration (IMLI-OH)
    ImliCount,      // inner-most loop iteration counter (IMLI-SIC)
};

struct ScComponent {
    ScSource source;
    uint8_t local{0};
    std::vector<uint8_t> lengths;  // one table per length, longest first
    uint8_t log_size;
    bool key_with_inter_pred{false};  // index also hashes the TAGE/loop prediction
};

struct LocalHistoryTable {
    uint8_t log_entries;
    uint8_t pc_shift;        // index = (pc ^ pc >> pc_shift) mod entries
    bool mix_pc_low{false};  // fold pc[3:0] into each update
};

// Geometry and policy of one TAGE-SC-L instance; see config64KB()/config8KB().
struct TageScLConfig {
    // TAGE. Tables 1..num_tables pair up as two ways per history length: even tables
    // always exist, odd ones only in [assoc_first, assoc_end).
    int num_tables;
    int min_hist;
    int max_hist;
    int high_first;  // tables >= this share the high bank array and get +4 tag bits
    int assoc_first;
    int assoc_end;
    std::vector<int> disabled_tables;
    int log_bank;  // entries per bank
    int banks_low;
    int banks_high;
    int tag_bits;
    int ctr_bits{3};
    int u_bits;
    int log_bimodal;
    int bimodal_hyst_shift{2};  // one hysteresis bit per 2^shift prediction bits
    int path_bits{27};
    int log_use_alt;
    int use_alt_bits{5};
    int max_alloc{2};
    int u_reset_period{1024};  // allocation-failure budget before u counters age
    int tick_alloc_weight;
    bool tick_counts_true_penalty;
    bool alloc_skips_strong;    // a u=0 victim with a confident counter is weakened, not replaced
    bool alloc_random_protect;  // new entries get u=1 with probability 1/32
    bool mute_weak_entries;     // an entry whose counter turns weak loses its u bit
    bool compact_hash;          // wider folds reduced into small banks (8KB tuning)
    int ghist_buffer_bits{13};  // circular buffer; must cover max_hist plus in-flight bits

    // Statistical corrector.
    std::vector<ScComponent> sc;
    std::vector<LocalHistoryTable> local;
    int imli_count_bits{8};
    int log_bias;
    bool bias_bank_uses_pc;
    bool sc_halve_short_tables;  // the two shortest tables of each component get half the entries
    int sc_ctr_bits{6};
    int sc_weight_bits{6};
    int log_threshold_tables{6};
    int threshold_bits{12};
    int pc_threshold_bits{8};
    int threshold_init{35};
    int threshold_bonus;  // added per component whose weight is doubled
    int chooser_bits{7};

    // Loop predictor (4-way skewed).
    int log_loop;
    int loop_iter_bits{10};
    int loop_tag_bits{10};
    int loop_conf_max{15};
};

// CBP-5 limited-size (64KB) TAGE-SC-L.
inline TageScLConfig config64KB() {
    TageScLConfig c;
    c.num_tables = 36;
    c.min_hist = 6;
    c.max_hist = 3000;
    c.high_first = 13;
    c.assoc_first = 9;
    c.assoc_end = 23;
    c.disabled_tables = {4, 8, 30, 34};
    c.log_bank = 10;
    c.banks_low = 10;
    c.banks_high = 20;
    c.tag_bits = 8;
    c.u_bits = 1;
    c.log_bimodal = 13;
    c.log_use_alt = 4;
    c.tick_alloc_weight = 2;
    c.tick_counts_true_penalty = false;
    c.alloc_skips_strong = true;
    c.alloc_random_protect = false;
    c.mute_weak_entries = false;
    c.compact_hash = false;
    c.sc = {
        {ScSource::BackwardTaken, 0, {40, 24, 10}, 10, true},
        {ScSource::Path, 0, {25, 16, 9}, 9},
        {ScSource::Local, 0, {11, 6, 3}, 10},
        {ScSource::Local, 1, {16, 11, 6}, 9},
        {ScSource::Local, 2, {9, 4}, 10},
        {ScSource::ImliHistory, 0, {10, 4}, 9},
        {ScSource::ImliCount, 0, {8}, 8},
    };
    c.local = {{8, 2}, {4, 5, true}, {4, 10}};
    c.log_bias = 8;
    c.bias_bank_uses_pc = true;
    c.sc_halve_short_tables = true;
    c.threshold_bonus = 12;
    c.log_loop = 5;
    return c;
}

// CBP-5 8KB TAGE-SC-L.
inline TageScLConfig config8KB() {
    TageScLConfig c;
    c.num_tables = 30;
    c.min_hist = 4;
    c.max_hist = 1000;
    c.high_first = 11;
    c.assoc_first = 7;
    c.assoc_end = 21;
    c.log_bank = 7;
    c.banks_low = 9;
    c.banks_high = 17;
    c.tag_bits = 8;
    c.u_bits = 2;
    c.log_bimodal = 12;
    c.log_use_alt = 3;
    c.tick_alloc_weight = 5;
    c.tick_counts_true_penalty = true;
    c.alloc_skips_strong = false;
    c.alloc_random_protect = true;
    c.mute_weak_entries = true;
    c.compact_hash = true;
    c.sc = {
        {ScSource::Taken, 0, {6, 3}, 7},
        {ScSource::BackwardTaken, 0, {16, 8}, 7},
        {ScSource::Local, 0, {6, 3}, 7},
        {ScSource::ImliCount, 0, {8}, 7},
    };
    c.local = {{6, 2}};
    c.log_bias = 7;
    c.bias_bank_uses_pc = false;
    c.sc_halve_short_tables = false;
    c.threshold_bonus = 0;  // the CBP-5 8KB code computes this term but discards it
    c.log_loop = 3;
    return c;
}

}  // namespace predictors::tage
