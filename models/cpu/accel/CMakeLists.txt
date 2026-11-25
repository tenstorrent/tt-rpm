# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: 2026 Tenstorrent USA, Inc.

# Accelerator interface: header-only common interface + reference devices + standalone unit
# harness. Wired into the top-level build; the device sources are additionally compiled into
# the core and cluster model targets (their CMakeLists) to prove the interface compiles into
# both include roots.

set(ACCEL_DEVICE_SOURCES
    ${RPM_ROOT}/models/cpu/accel/devices/DmaEngine.cpp
)

# Standalone harness: feeds AccelCommands, checks completions. Links Sparta only
# (no Whisper) — the oracle hooks are stub-memory backed.
add_executable(accel_dma_test
    tests/dma_engine_test.cpp
    ${ACCEL_DEVICE_SOURCES}
)

target_include_directories(accel_dma_test PRIVATE
    ${RPM_ROOT}
)
target_include_directories(accel_dma_test SYSTEM PRIVATE
    ${SPARTA_INCLUDE_DIRS}
    ${Boost_INCLUDE_DIRS}
    ${HDF5_INCLUDE_DIRS}
    ${SQLite3_INCLUDE_DIRS}
)

target_link_libraries(accel_dma_test
    ${SPARTA_LIBRARIES}
    HDF5::HDF5
    ${SQLite3_LIBRARIES}
    yaml-cpp
    ZLIB::ZLIB
    Threads::Threads
    Boost::date_time
    Boost::iostreams
    Boost::serialization
    Boost::timer
    Boost::program_options
)
if(NOT APPLE)
    target_link_libraries(accel_dma_test rt)
endif()

target_compile_options(accel_dma_test PRIVATE -Wall -Wextra -Wpedantic -Ulinux)
