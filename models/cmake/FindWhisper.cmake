# SPDX-FileCopyrightText: © 2026 Tenstorrent AI ULC
#
# SPDX-License-Identifier: Apache-2.0

# FindWhisper.cmake — locate a pre-built Whisper ISS inside the RPM tree.
#
# Sets:
#   WHISPER_FOUND
#   WHISPER_ROOT          - source root (ext/whisper)
#   WHISPER_INCLUDE_DIRS  - include directories
#   WHISPER_LIBRARIES     - static libraries to link

if(NOT DEFINED RPM_ROOT)
    message(FATAL_ERROR "RPM_ROOT must be set before including FindWhisper")
endif()

set(WHISPER_ROOT "${RPM_ROOT}/ext/whisper" CACHE PATH "Whisper source root")
set(WHISPER_BUILD_DIR "${WHISPER_ROOT}/build-${CMAKE_SYSTEM_NAME}" CACHE PATH "Whisper build directory")

set(_rvcore_lib     "${WHISPER_BUILD_DIR}/librvcore.a")
set(_tracereader_lib "${WHISPER_ROOT}/trace-reader/TraceReader.a")
set(_virtmem_lib    "${WHISPER_ROOT}/virtual_memory/libvirtual_memory.a")
set(_pci_lib        "${WHISPER_ROOT}/pci/libpci.a")

if(EXISTS "${_rvcore_lib}" AND EXISTS "${_tracereader_lib}")
    set(WHISPER_FOUND TRUE)

    set(WHISPER_LIBRARIES
        "${_rvcore_lib}"
        "${_tracereader_lib}"
    )
    if(EXISTS "${_virtmem_lib}")
        list(APPEND WHISPER_LIBRARIES "${_virtmem_lib}")
    endif()
    if(EXISTS "${_pci_lib}")
        list(APPEND WHISPER_LIBRARIES "${_pci_lib}")
    endif()

    # Softfloat (optional)
    set(_softfloat "${WHISPER_ROOT}/third_party/softfloat/build/RISCV-GCC/softfloat.a")
    if(EXISTS "${_softfloat}")
        list(APPEND WHISPER_LIBRARIES "${_softfloat}")
    endif()

    set(WHISPER_INCLUDE_DIRS
        "${WHISPER_ROOT}"
        "${WHISPER_ROOT}/third_party"
        "${WHISPER_ROOT}/trace-reader"
    )
    message(STATUS "Found Whisper: ${WHISPER_BUILD_DIR}")
else()
    set(WHISPER_FOUND FALSE)
    if(Whisper_FIND_REQUIRED)
        message(FATAL_ERROR
            "Whisper not built. Run:\n"
            "  scripts/build_scripts/build_deps.sh\n"
            "Expected: ${_rvcore_lib}")
    endif()
endif()
