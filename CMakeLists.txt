cmake_minimum_required(VERSION 3.17)
project(rpm LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(RPM_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")

# Custom find-modules for Sparta and Whisper
list(APPEND CMAKE_MODULE_PATH "${RPM_ROOT}/models/cmake")

# ---------- External dependencies ----------
find_package(Sparta REQUIRED)
find_package(Whisper REQUIRED)
find_package(Boost 1.74.0 REQUIRED COMPONENTS date_time iostreams serialization timer program_options)
find_package(yaml-cpp 0.7 REQUIRED)
find_package(RapidJSON 1.1 REQUIRED)
find_package(SQLite3 3.19 REQUIRED)
find_package(ZLIB REQUIRED)
find_package(HDF5 1.10 REQUIRED COMPONENTS CXX)
find_package(Threads REQUIRED)

# ---------- Shared CPU common library ----------
file(GLOB RPM_CPU_COMMON_SOURCES "${RPM_ROOT}/models/cpu/common/*.cpp")
if(RPM_CPU_COMMON_SOURCES)
    add_library(rpm_cpu_common STATIC ${RPM_CPU_COMMON_SOURCES})
    target_include_directories(rpm_cpu_common PUBLIC
        "${RPM_ROOT}"
        "${RPM_ROOT}/models/cpu/common"
    )
    target_include_directories(rpm_cpu_common SYSTEM PUBLIC
        ${SPARTA_INCLUDE_DIRS}
        ${WHISPER_INCLUDE_DIRS}
        ${Boost_INCLUDE_DIRS}
        ${HDF5_INCLUDE_DIRS}
    )
    target_link_libraries(rpm_cpu_common PUBLIC
        ${SPARTA_LIBRARIES}
        ${WHISPER_LIBRARIES}
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
        target_link_libraries(rpm_cpu_common PUBLIC rt)
    endif()
    target_compile_options(rpm_cpu_common PRIVATE -Wall -Wextra -Wpedantic)
endif()

# ---------- Model targets ----------
# Each model subdirectory can use rpm_cpu_common and the found packages.
# Models that have a CMakeLists.txt are added automatically.

set(RPM_MODELS
    simple
    core
)

foreach(model ${RPM_MODELS})
    set(_model_dir "${RPM_ROOT}/models/cpu/src/${model}")
    if(EXISTS "${_model_dir}/CMakeLists.txt")
        message(STATUS "Adding model: ${model}")
        add_subdirectory("${_model_dir}" "${CMAKE_BINARY_DIR}/${model}")
    else()
        message(STATUS "Skipping model: ${model} (no CMakeLists.txt)")
    endif()
endforeach()
