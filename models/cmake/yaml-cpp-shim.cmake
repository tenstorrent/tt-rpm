# SPDX-FileCopyrightText: © 2026 Tenstorrent AI ULC
#
# SPDX-License-Identifier: Apache-2.0

# Shim for yaml-cpp 0.7 compatibility.
# yaml-cpp 0.7 (Ubuntu 22.04) exports only "yaml-cpp" as a target name.
# yaml-cpp 0.8+ exports the namespaced "yaml-cpp::yaml-cpp".
# Sparta unconditionally references yaml-cpp::yaml-cpp, so we bridge the gap.
if(NOT TARGET yaml-cpp::yaml-cpp)
    find_package(yaml-cpp QUIET)
    if(TARGET yaml-cpp)
        add_library(yaml-cpp::yaml-cpp INTERFACE IMPORTED)
        set_target_properties(yaml-cpp::yaml-cpp PROPERTIES
            INTERFACE_LINK_LIBRARIES "yaml-cpp")
    endif()
endif()
