// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Tenstorrent USA, Inc.

#pragma once

#include <charconv>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <type_traits>

namespace predictors {

// One predictor structure's knobs: the host's generic knobs (seeded) plus the user's key=value
// entries. A user key the algorithm never reads is an error, as an unread Sparta parameter is.
class PredictorArgs {
   public:
    explicit PredictorArgs(std::string location)
        : mLocation(std::move(location)) {}

    // Seed host knobs before adding user entries, so an entry cannot shadow one.
    void add(const std::string& key_eq_value) {
        const size_t eq = key_eq_value.find('=');
        if (eq == std::string::npos || eq == 0) throw std::invalid_argument(mLocation + ": '" + key_eq_value + "' is not key=value");
        const std::string key = key_eq_value.substr(0, eq);
        const auto [it, added] = mValues.try_emplace(key, Value{key_eq_value.substr(eq + 1), ""});
        if (!added) {
            throw std::invalid_argument(mLocation + ": '" + key + "' " + (it->second.location.empty() ? "given twice" : "is set by " + it->second.location));
        }
    }

    void seed(std::string key, std::string value, std::string location) { mValues[std::move(key)] = Value{std::move(value), std::move(location)}; }

    template <typename T>
    T get(const std::string& key, T dflt) const {
        mRead.insert(key);
        const auto it = mValues.find(key);
        if (it == mValues.end()) return dflt;
        const std::string& s = it->second.text;
        if constexpr (std::is_same_v<T, std::string>) {
            return s;
        } else if constexpr (std::is_same_v<T, bool>) {
            if (s == "true" || s == "1") return true;
            if (s == "false" || s == "0") return false;
        } else {
            const bool hex = std::is_integral_v<T> && s.starts_with("0x");
            T v{};
            const char* end = s.data() + s.size();
            std::from_chars_result r;
            if constexpr (std::is_integral_v<T>) {
                r = std::from_chars(s.data() + (hex ? 2 : 0), end, v, hex ? 16 : 10);
            } else {
                r = std::from_chars(s.data(), end, v);
            }
            if (r.ec == std::errc() && r.ptr == end && !s.empty()) return v;
        }
        fail(key, "not a valid value");
    }

    [[noreturn]] void fail(const std::string& key, const std::string& why) const {
        const auto it = mValues.find(key);
        std::string where = mLocation + ": " + key;
        if (it != mValues.end()) where = (it->second.location.empty() ? where : it->second.location) + " = " + it->second.text;
        throw std::invalid_argument(where + ": " + why);
    }

    // Call after constructing `algorithm`, which reads every key it knows.
    void checkAllUsed(const std::string& algorithm) const {
        std::string known;
        for (const std::string& k : mRead) {
            const auto it = mValues.find(k);
            if (it == mValues.end() || it->second.location.empty()) known += (known.empty() ? "" : ", ") + k;
        }
        for (const auto& [k, v] : mValues) {
            if (v.location.empty() && !mRead.contains(k)) {
                throw std::invalid_argument(mLocation + ": unknown key '" + k + "' for " + algorithm + " (known: " + (known.empty() ? "none" : known) + ")");
            }
        }
    }

   private:
    struct Value {
        std::string text;
        std::string location;  // seeding host knob; empty for a user entry
    };

    std::string mLocation;
    std::map<std::string, Value> mValues;
    mutable std::set<std::string> mRead;
};

}  // namespace predictors
