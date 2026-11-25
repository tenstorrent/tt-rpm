# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: 2026 Tenstorrent USA, Inc.

cmake_minimum_required(VERSION 3.14)
project(core_cpu LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Repo root: four levels up from models/cpu/src/core/
set(RPM_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../..")

# External dependencies (submodules under ext/)
set(WHISPER_ROOT "${RPM_ROOT}/ext/whisper" CACHE PATH "Whisper ISS submodule root")
set(SPARTA_ROOT  "${RPM_ROOT}/ext/map/sparta" CACHE PATH "Sparta root (submodule)")
set(SPARTA_BUILD "${SPARTA_ROOT}/release")

# Whisper build dir
set(WHISPER_BUILD "${WHISPER_ROOT}/build-${CMAKE_SYSTEM_NAME}" CACHE PATH "Whisper build dir")
if(NOT EXISTS "${WHISPER_BUILD}/librvcore.a")
  message(FATAL_ERROR "Whisper not built. Run: cd ${WHISPER_ROOT} && make")
endif()
if(NOT EXISTS "${WHISPER_ROOT}/trace-reader/TraceReader.a")
  message(FATAL_ERROR "Whisper trace-reader not built. Run: cd ${WHISPER_ROOT}/trace-reader && make TraceReader.a")
endif()

# Dependencies
find_package(Boost 1.74.0 REQUIRED COMPONENTS date_time iostreams serialization timer program_options)
find_package(yaml-cpp 0.7 REQUIRED)
find_package(RapidJSON 1.1 REQUIRED)
find_package(SQLite3 3.19 REQUIRED)
find_package(ZLIB REQUIRED)
find_package(HDF5 1.10 REQUIRED COMPONENTS CXX)
find_package(Threads REQUIRED)

# Common headers + sources
set(COMMON_HEADERS
    Common/PipelinePacket.hpp
    Common/PipelineVisualizer.hpp
    Common/WritePortArbiter.hpp
    Common/FpOpClass.hpp
    Common/WritebackBuffer.hpp
    Common/FlushArbiter.hpp
    Common/PrefetchHost.hpp
    Common/CheckpointManager.hpp
    Common/SpeculationConfig.hpp
    Common/SpeculationUtils.hpp
    MemoryHierarchy/Cache.hpp
    MemoryHierarchy/CacheInterface.hpp
    MemoryHierarchy/CacheTracer.hpp
    MemoryHierarchy/L2Cache.hpp
    MemoryHierarchy/Dram/MemoryBackend.hpp
    MemoryHierarchy/Dram/DramController.hpp
    MemoryHierarchy/Dram/Dram.hpp
)

set(COMMON_SOURCES
    Common/PipelineVisualizer.cpp
    Common/WritePortArbiter.cpp
    Common/WritebackBuffer.cpp
    Common/FlushArbiter.cpp
    Common/PrefetchHost.cpp
    MemoryHierarchy/CacheInterface.cpp
    MemoryHierarchy/CacheTracer.cpp
    MemoryHierarchy/L2Cache.cpp
    MemoryHierarchy/Replacement.cpp
    MemoryHierarchy/Dram/DramController.cpp
    MemoryHierarchy/Dram/Dram.cpp
)

# Pipeline clock
set(PIPELINE_SOURCES
    Pipeline.cpp
    Pipeline.hpp
)

# Frontend source files
set(FRONTEND_SOURCES
    Frontend/FetchStructures/FetchStructures.cpp
    Frontend/FetchStructures/FetchStructures.hpp
    Frontend/FrontendMemoryStructures/FrontendMemoryStructures.cpp
    Frontend/FrontendMemoryStructures/FrontendMemoryStructures.hpp
    Frontend/FetchQueue/FetchQueue.cpp
    Frontend/FetchQueue/FetchQueue.hpp
    Frontend/DecodeStructures/DecodeStructures.cpp
    Frontend/DecodeStructures/DecodeStructures.hpp
    Frontend/DecodeQueue/DecodeQueue.cpp
    Frontend/DecodeQueue/DecodeQueue.hpp
    Frontend/BranchPredictor/BranchPredictor.cpp
    Frontend/BranchPredictor/BranchPredictor.hpp
    Frontend/BranchPredictor/TargetPredictor.cpp
    Frontend/BranchPredictor/TargetPredictor.hpp
)

# Midcore source files
set(MIDCORE_SOURCES
    Midcore/Rename/Rename.cpp
    Midcore/Rename/Rename.hpp
    Midcore/Issue/Issue.cpp
    Midcore/Issue/Issue.hpp
    Midcore/Issue/AgeMatrix.hpp
    Midcore/Execute/Execute.cpp
    Midcore/Execute/Execute.hpp
    LoadStoreUnit/LoadQueue.hpp
    LoadStoreUnit/StoreQueue.hpp
    LoadStoreUnit/LSQ/LSQ.cpp
    LoadStoreUnit/LSQ/LSQ.hpp
    LoadStoreUnit/BackendMemoryStructures/BackendMemoryStructures.cpp
    LoadStoreUnit/BackendMemoryStructures/BackendMemoryStructures.hpp
    Mmu/Mmu.cpp
    Mmu/Mmu.hpp
    Midcore/Writeback/Writeback.cpp
    Midcore/Writeback/Writeback.hpp
)

# Shared rpm/cpu/common sources
set(RPM_COMMON_SOURCES
    ${RPM_ROOT}/models/cpu/common/ExecutionDriver.cpp
    ${RPM_ROOT}/models/cpu/common/Instruction.cpp
    ${RPM_ROOT}/models/cpu/common/SnapshotUtil.cpp
)

# Accelerator device sources (interfaces are header-only; see models/cpu/accel). Compiled
# here to prove the interface builds in the core include root.
set(ACCEL_SOURCES
    ${RPM_ROOT}/models/cpu/accel/devices/DmaEngine.cpp
)

include(${RPM_ROOT}/models/cpu/predictors/predictors.cmake)

# TLB algorithm sources (the interface is header-only).
set(TLB_SOURCES
    ${RPM_ROOT}/models/cpu/tlb/src/SetAssocTlb.cpp
)

# Prefetcher algorithm sources (interfaces are header-only).
set(PREFETCH_SOURCES
    ${RPM_ROOT}/models/cpu/prefetch/algorithms/src/IpStride.cpp
    ${RPM_ROOT}/models/cpu/prefetch/algorithms/src/NextLine.cpp
    ${RPM_ROOT}/models/cpu/prefetch/algorithms/src/Stream.cpp
)

# Main executable
add_executable(core
    Common/main.cpp
    Common/ChipSim.cpp
    Common/ChipSim.hpp
    Common/CoreImpl.cpp
    Common/CoreImpl.hpp
    Common/Core.cpp
    Common/Core.hpp
    ${COMMON_HEADERS}
    ${COMMON_SOURCES}
    ${PIPELINE_SOURCES}
    ${FRONTEND_SOURCES}
    ${MIDCORE_SOURCES}
    ${RPM_COMMON_SOURCES}
    ${ACCEL_SOURCES}
    ${PREDICTOR_SOURCES}
    ${TLB_SOURCES}
    ${PREFETCH_SOURCES}
)

# Own source include directories
target_include_directories(core PRIVATE
    ${RPM_ROOT}
    ${RPM_ROOT}/models/cpu
    ${RPM_ROOT}/models/cpu/common
    ${CMAKE_CURRENT_SOURCE_DIR}
)

# External include directories (system headers, warnings suppressed)
target_include_directories(core SYSTEM PRIVATE
    ${SPARTA_ROOT}
    ${SPARTA_ROOT}/simdb/include
    ${WHISPER_ROOT}
    ${WHISPER_ROOT}/third_party
    ${WHISPER_ROOT}/trace-reader
    ${Boost_INCLUDE_DIRS}
    ${YAML_CPP_INCLUDE_DIR}
    ${SQLite3_INCLUDE_DIRS}
    ${HDF5_INCLUDE_DIRS}
    ${RapidJSON_INCLUDE_DIRS}
    ${ZLIB_INCLUDE_DIRS}
)

# Link libraries
target_link_libraries(core
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

# Softfloat
if(EXISTS "${WHISPER_ROOT}/third_party/softfloat/build/RISCV-GCC/softfloat.a")
    target_link_libraries(core ${WHISPER_ROOT}/third_party/softfloat/build/RISCV-GCC/softfloat.a)
endif()
if(NOT APPLE)
    target_link_libraries(core rt)
endif()

# -Ulinux: whisper's Args.hpp uses 'linux' as a variable name, which clashes
# with the 'linux' predefined macro in gnu++ mode.
target_compile_options(core PRIVATE -Wall -Wextra -Wpedantic -Wshadow -Ulinux)

# Copy config files into the build directory
execute_process(
    COMMAND ${CMAKE_COMMAND} -E copy
        ${CMAKE_CURRENT_SOURCE_DIR}/config.yaml
        ${CMAKE_BINARY_DIR}/config.yaml
)
