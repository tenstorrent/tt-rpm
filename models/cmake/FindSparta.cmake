# SPDX-FileCopyrightText: © 2026 Tenstorrent AI ULC
#
# SPDX-License-Identifier: Apache-2.0

# FindSparta.cmake — locate a pre-built Sparta installation inside the RPM tree.
#
# Sets:
#   SPARTA_FOUND
#   SPARTA_ROOT          - source root (ext/map/sparta)
#   SPARTA_INCLUDE_DIRS  - include directories
#   SPARTA_LIBRARIES     - static libraries to link

if(NOT DEFINED RPM_ROOT)
    message(FATAL_ERROR "RPM_ROOT must be set before including FindSparta")
endif()

set(SPARTA_ROOT "${RPM_ROOT}/ext/map/sparta" CACHE PATH "Sparta source root")
set(SPARTA_BUILD_DIR "${SPARTA_ROOT}/release" CACHE PATH "Sparta build directory")

set(_sparta_lib "${SPARTA_BUILD_DIR}/libsparta.a")
set(_simdb_lib  "${SPARTA_BUILD_DIR}/simdb/libsimdb.a")

if(EXISTS "${_sparta_lib}" AND EXISTS "${_simdb_lib}")
    set(SPARTA_FOUND TRUE)
    set(SPARTA_LIBRARIES "${_sparta_lib}" "${_simdb_lib}")
    set(SPARTA_INCLUDE_DIRS
        "${SPARTA_ROOT}"
        "${SPARTA_ROOT}/simdb/include"
    )
    message(STATUS "Found Sparta: ${SPARTA_BUILD_DIR}")
else()
    set(SPARTA_FOUND FALSE)
    if(Sparta_FIND_REQUIRED)
        message(FATAL_ERROR
            "Sparta not built. Run:\n"
            "  scripts/build_scripts/build_deps.sh\n"
            "Expected: ${_sparta_lib}")
    endif()
endif()
