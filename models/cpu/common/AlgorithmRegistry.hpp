// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Tenstorrent USA, Inc.

#pragma once

#include <functional>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace rpm {

// Generic string -> factory registry, one instance per <Iface, Params> pair.
// Algorithms self-register at static-init via the REGISTER_* macros; the hosting
// unit selects one by name. Creating an unknown key throws, listing known keys.
template <typename Iface, typename Params>
class Registry {
   public:
    using Factory = std::function<std::unique_ptr<Iface>(const Params&)>;

    static Registry& instance() {
        static Registry inst;
        return inst;
    }

    void registerType(const std::string& key, Factory factory) { mFactories[key] = std::move(factory); }

    std::unique_ptr<Iface> create(const std::string& key, const Params& params) const {
        auto it = mFactories.find(key);
        if (it == mFactories.end()) {
            throw std::runtime_error("Unknown algorithm '" + key + "'. Known: " + knownCsv());
        }
        return it->second(params);
    }

    std::vector<std::string> keys() const {
        std::vector<std::string> out;
        out.reserve(mFactories.size());
        for (const auto& [k, _] : mFactories) out.push_back(k);
        return out;
    }

   private:
    std::string knownCsv() const {
        std::string s;
        for (const auto& [k, _] : mFactories) s += (s.empty() ? "" : ", ") + k;
        return s;
    }

    std::map<std::string, Factory> mFactories;
};

}  // namespace rpm

#define RPM_REG_CONCAT_(a, b) a##b
#define RPM_REG_CONCAT(a, b) RPM_REG_CONCAT_(a, b)

// Concrete type must be constructible from const Params&. The symbol name is made
// unique per registration site (via __COUNTER__) so one Type may register under
// several keys without colliding.
#define RPM_REGISTER_ALGORITHM(Iface, Params, KEY, Type)                                                                                 \
    namespace {                                                                                                                          \
    const bool RPM_REG_CONCAT(_rpm_reg_, __COUNTER__) = [] {                                                                             \
        rpm::Registry<Iface, Params>::instance().registerType(KEY, [](const Params& p) { return std::unique_ptr<Iface>(new Type(p)); }); \
        return true;                                                                                                                     \
    }();                                                                                                                                 \
    }
