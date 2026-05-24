cmake_minimum_required(VERSION 3.16)

project(NanoMatch
    VERSION     0.2.0
    LANGUAGES   CXX
    DESCRIPTION "Ultra-low-latency limit order book matching engine"
)

set(CMAKE_CXX_STANDARD          17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS         OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS
        Debug Release RelWithDebInfo MinSizeRel)
endif()

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_library(nanomatch_flags INTERFACE)

target_compile_options(nanomatch_flags INTERFACE
    $<$<CXX_COMPILER_ID:GNU,Clang>:
        -Wall -Wextra -Wpedantic
        -march=native
        -fno-exceptions
        -fno-rtti
        -fno-omit-frame-pointer
    >
)
target_compile_options(nanomatch_flags INTERFACE
    $<$<AND:$<CXX_COMPILER_ID:GNU,Clang>,$<CONFIG:Release>>:
        -O3 -DNDEBUG -flto
    >
)
target_link_options(nanomatch_flags INTERFACE
    $<$<AND:$<CXX_COMPILER_ID:GNU,Clang>,$<CONFIG:Release>>:
        -flto
    >
)
target_compile_options(nanomatch_flags INTERFACE
    $<$<AND:$<CXX_COMPILER_ID:GNU,Clang>,$<CONFIG:Debug>>:
        -O0 -g3
    >
)

target_compile_options(nanomatch_flags INTERFACE
    $<$<CXX_COMPILER_ID:MSVC>:
        /W4
        /WX-
        /EHs-c-
        /GR-
        /arch:AVX2
        /Oy-
    >
)
target_compile_options(nanomatch_flags INTERFACE
    $<$<AND:$<CXX_COMPILER_ID:MSVC>,$<CONFIG:Release>>:
        /O2 /DNDEBUG /GL
    >
)
target_link_options(nanomatch_flags INTERFACE
    $<$<AND:$<CXX_COMPILER_ID:MSVC>,$<CONFIG:Release>>:
        /LTCG
    >
)
target_compile_options(nanomatch_flags INTERFACE
    $<$<AND:$<CXX_COMPILER_ID:MSVC>,$<CONFIG:Debug>>:
        /Od /Zi
    >
)

add_library(nanomatch INTERFACE)
target_include_directories(nanomatch INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(nanomatch INTERFACE nanomatch_flags)

add_executable(nanomatch_step1 main.cpp)
target_link_libraries(nanomatch_step1 PRIVATE nanomatch)

add_executable(nanomatch_step2 main_step2.cpp)
target_link_libraries(nanomatch_step2 PRIVATE nanomatch)

add_executable(nanomatch_step3 main_step3.cpp)
target_link_libraries(nanomatch_step3 PRIVATE nanomatch)

find_package(Threads REQUIRED)
add_executable(nanomatch_step4 main_step4.cpp)
target_link_libraries(nanomatch_step4 PRIVATE nanomatch Threads::Threads)

add_executable(nanomatch_step5 main_step5.cpp)
target_link_libraries(nanomatch_step5 PRIVATE nanomatch)

add_executable(latency_probe bench/latency_probe.cpp)
target_link_libraries(latency_probe PRIVATE nanomatch)

option(NANOMATCH_NO_GBENCH "Skip Google Benchmark fetch + bench_main target" OFF)
if(NOT NANOMATCH_NO_GBENCH)
    include(FetchContent)
    set(BENCHMARK_ENABLE_TESTING    OFF CACHE BOOL "" FORCE)
    set(BENCHMARK_ENABLE_INSTALL    OFF CACHE BOOL "" FORCE)
    set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)

    FetchContent_Declare(
        googlebenchmark
        GIT_REPOSITORY https://github.com/google/benchmark.git
        GIT_TAG        v1.8.3
        GIT_SHALLOW    TRUE
    )
    FetchContent_MakeAvailable(googlebenchmark)

    add_executable(bench_main bench/bench_main.cpp)
    target_link_libraries(bench_main PRIVATE nanomatch benchmark::benchmark)
    message(STATUS "Google Benchmark enabled (target: bench_main)")
else()
    message(STATUS "Google Benchmark disabled (-DNANOMATCH_NO_GBENCH=ON)")
endif()

enable_testing()
add_test(NAME step1_layout_validation     COMMAND nanomatch_step1)
add_test(NAME step2_matching_scenarios    COMMAND nanomatch_step2)
add_test(NAME step3_huge_page_pool        COMMAND nanomatch_step3)
add_test(NAME step4_spsc_ring             COMMAND nanomatch_step4)
add_test(NAME step5_ingestion             COMMAND nanomatch_step5)

message(STATUS "─────────────────────────────────────────────")
message(STATUS "  NanoMatch ${PROJECT_VERSION}")
message(STATUS "  Build type : ${CMAKE_BUILD_TYPE}")
message(STATUS "  Compiler   : ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "  C++ std    : ${CMAKE_CXX_STANDARD}")
message(STATUS "─────────────────────────────────────────────")