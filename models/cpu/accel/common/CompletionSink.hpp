// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Tenstorrent USA, Inc.

#pragma once

#include <functional>
#include <utility>

#include "models/cpu/accel/common/AcceleratorTypes.hpp"

// Completion/notification as a policy object: a device instance picks one sink and stays
// agnostic about how completion is delivered.
namespace accel {

class CompletionSink {
   public:
    virtual ~CompletionSink() = default;
    virtual void complete(const AccelCompletion&) = 0;
};

// std::function-backed sink for harnesses and simple front-ends.
class FunctionSink : public CompletionSink {
   public:
    explicit FunctionSink(std::function<void(const AccelCompletion&)> fn)
        : mFn(std::move(fn)) {}
    void complete(const AccelCompletion& c) override { mFn(c); }

   private:
    std::function<void(const AccelCompletion&)> mFn;
};

}  // namespace accel
