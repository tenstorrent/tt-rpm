# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: 2026 Tenstorrent USA, Inc.

cmake_minimum_required(VERSION 3.14)
project(cluster LANGUAGES CXX)

# C++23 required by the core model (cluster reuses its units)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Repo root: three levels up from models/cluster/src/
set(RPM_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../.." CACHE PATH "RPM repo root")
set(CORE_DIR "${RPM_ROOT}/models/cpu/src/core")
set(CLUSTER_COMMON "${RPM_ROOT}/models/cluster/common")
include(${RPM_ROOT}/models/cpu/predictors/predictors.cmake)

# Every out-of-order core model source except the single-core entry point, so this build
# cannot drift from models/cpu/src/core/CMakeLists.txt.
file(GLOB_RECURSE CORE_SOURCES CONFIGURE_DEPENDS
    ${CORE_DIR}/*.cpp
    ${RPM_ROOT}/models/cpu/prefetch/algorithms/src/*.cpp
    ${RPM_ROOT}/models/cpu/tlb/src/*.cpp)
list(FILTER CORE_SOURCES EXCLUDE REGEX "/Common/(main|ChipSim)\\.cpp$")
list(APPEND CORE_SOURCES ${PREDICTOR_SOURCES})

# External dependencies (submodules under ext/)
set(WHISPER_ROOT "${RPM_ROOT}/ext/whisper" CACHE PATH "Whisper ISS submodule root")
set(SPARTA_ROOT  "${RPM_ROOT}/ext/map/sparta" CACHE PATH "Sparta root (submodule)")
set(SPARTA_BUILD "${SPARTA_ROOT}/release")

set(WHISPER_BUILD "${WHISPER_ROOT}/build-${CMAKE_SYSTEM_NAME}" CACHE PATH "Whisper build dir")
if(NOT EXISTS "${WHISPER_BUILD}/librvcore.a")
  message(FATAL_ERROR "Whisper not built. Run: bash scripts/build_scripts/build_deps.sh")
endif()
if(NOT EXISTS "${WHISPER_ROOT}/trace-reader/TraceReader.a")
  message(FATAL_ERROR "Whisper trace-reader not built. Run: bash scripts/build_scripts/build_deps.sh")
endif()

find_package(Boost 1.74.0 REQUIRED COMPONENTS date_time iostreams serialization timer program_options)
find_package(yaml-cpp 0.7 REQUIRED)
find_package(RapidJSON 1.1 REQUIRED)
find_package(SQLite3 3.19 REQUIRED)
find_package(ZLIB REQUIRED)
find_package(HDF5 1.10 REQUIRED COMPONENTS CXX)
find_package(Threads REQUIRED)

add_executable(cluster
    main.cpp
    # Cluster template (generic infrastructure)
    Cluster.cpp
    Cluster.hpp
    ClusterCache.hpp
    ClusterSim.cpp
    ClusterSim.hpp
    ${CLUSTER_COMMON}/ClusterCoreIf.hpp
    # Out-of-order core model units + the droppable OOO core (models/cpu/src/core)
    ${CORE_SOURCES}
    # Shared cpu/common sources
    ${RPM_ROOT}/models/cpu/common/ExecutionDriver.cpp
    ${RPM_ROOT}/models/cpu/common/Instruction.cpp
    ${RPM_ROOT}/models/cpu/common/SnapshotUtil.cpp
    # Accelerator device sources (interfaces are header-only; see models/cpu/accel). Compiled
    # here to prove the interface builds in the cluster include root.
    ${RPM_ROOT}/models/cpu/accel/devices/DmaEngine.cpp
)

target_include_directories(cluster PRIVATE
    ${RPM_ROOT}
    ${RPM_ROOT}/models/cpu
    ${RPM_ROOT}/models/cpu/common
    ${CORE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_include_directories(cluster SYSTEM PRIVATE
    ${SPARTA_ROOT}
    ${SPARTA_ROOT}/simdb/include
    ${WHISPER_ROOT}
    ${WHISPER_ROOT}/third_party
    ${WHISPER_ROOT}/trace-reader
    ${Boost_INCLUDE_DIRS}
    ${YAML_CPP_INCLUDE_DIR}
    ${SQLite3_INCLUDE_DIRS}
    ${HDF5_INCLUDE_DIRS}
    ${RAPIDJSON_INCLUDE_DIRS}
    ${RapidJSON_INCLUDE_DIRS}
    ${ZLIB_INCLUDE_DIRS}
    ${ZLIB_INCLUDE_DIRECTORIES}
)

target_link_libraries(cluster
    ${SPARTA_BUILD}/libsparta.a
    ${SPARTA_BUILD}/simdb/libsimdb.a
    ${WHISPER_BUILD}/librvcore.a
    ${WHISPER_ROOT}/trace-reader/TraceReader.a
    ${WHISPER_ROOT}/virtual_memory/libvirtual_memory.a
    ${WHISPER_ROOT}/pci/libpci.a
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

if(EXISTS "${WHISPER_ROOT}/third_party/softfloat/build/RISCV-GCC/softfloat.a")
    target_link_libraries(cluster ${WHISPER_ROOT}/third_party/softfloat/build/RISCV-GCC/softfloat.a)
endif()
if(NOT APPLE)
    target_link_libraries(cluster rt)
endif()

# -Ulinux: whisper's Args.hpp uses 'linux' as a variable name, which clashes
# with the 'linux' predefined macro in gnu++ mode.
target_compile_options(cluster PRIVATE -Wall -Wextra -Ulinux)

# Fabric harness: fake core + DmaEngine through the real Cluster/ClusterCache/bridges.
# Exercises the MMIO doorbell peel, ring front-end, and the coherence on/off differential.
add_executable(accel_fabric_test
    ${CMAKE_CURRENT_SOURCE_DIR}/../tests/accel_fabric_test.cpp
    Cluster.cpp
    ${CORE_DIR}/MemoryHierarchy/Replacement.cpp
    ${RPM_ROOT}/models/cpu/accel/devices/DmaEngine.cpp
)
target_include_directories(accel_fabric_test PRIVATE
    ${RPM_ROOT}
    ${RPM_ROOT}/models/cpu
    ${RPM_ROOT}/models/cpu/common
    ${CORE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}
)
target_include_directories(accel_fabric_test SYSTEM PRIVATE
    ${SPARTA_ROOT}
    ${SPARTA_ROOT}/simdb/include
    ${Boost_INCLUDE_DIRS}
    ${HDF5_INCLUDE_DIRS}
    ${SQLite3_INCLUDE_DIRS}
)
target_link_libraries(accel_fabric_test
    ${SPARTA_BUILD}/libsparta.a
    ${SPARTA_BUILD}/simdb/libsimdb.a
    HDF5::HDF5
    ${SQLite3_LIBRARIES}
    yaml-cpp
    ZLIB::ZLIB
    Threads::Threads
    Boost::date_time
    Boost::iostreams
    Boost::serialization
    Boost::timer
)
if(NOT APPLE)
    target_link_libraries(accel_fabric_test rt)
endif()
target_compile_options(accel_fabric_test PRIVATE -Wall -Wextra -Ulinux)

# Stage the cluster config + the dropped-in core model's CPU config next to the
# binary so the run directory is self-contained (the cluster reads ./config.yaml,
# which points at ./core_config.yaml).
foreach(_cfg config.yaml core_config.yaml core_big.yaml core_little.yaml config_heterogeneous.yaml config_heterogeneous_3core.yaml)
    add_custom_command(TARGET cluster POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_CURRENT_SOURCE_DIR}/${_cfg}" "$<TARGET_FILE_DIR:cluster>/${_cfg}")
endforeach()
