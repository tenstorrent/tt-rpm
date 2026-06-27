// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Tenstorrent USA, Inc.

#pragma once

// Logging macros built on Sparta's log::MessageSource infrastructure.
//
// sparta::Unit provides three loggers automatically:
//   info_logger_   — general informational messages
//   debug_logger_  — verbose debug traces
//   warn_logger_   — warnings
//
// These are enabled per-unit at runtime via Sparta's --log / --debug-on
// command line options or via the logging API.  When disabled, the
// SPARTA_EXPECT_FALSE branch-prediction hint makes the check nearly free.
//
// Usage in any sparta::Unit subclass:
//   ILOG("tag=" << pkt.tag << " issued");   // info
//   DLOG("scoreboard state: " << dump());   // debug (verbose)
//   WLOG("ROB near full: " << rob.size());  // warning

namespace cpu {

#define ILOG(msg)                            \
    if (SPARTA_EXPECT_FALSE(info_logger_)) { \
        info_logger_ << msg;                 \
    }

#define DLOG(msg)                             \
    if (SPARTA_EXPECT_FALSE(debug_logger_)) { \
        debug_logger_ << msg;                 \
    }

#define WLOG(msg)                            \
    if (SPARTA_EXPECT_FALSE(warn_logger_)) { \
        warn_logger_ << msg;                 \
    }

}  // namespace cpu
