# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.28)
project(SeedArc VERSION 0.1.0 LANGUAGES CXX)

include(CTest)

function(seedarc_target_defaults target)
    target_compile_features(${target} PUBLIC cxx_std_23)
    set_target_properties(${target} PROPERTIES CXX_EXTENSIONS OFF)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive- /Zc:__cplusplus /utf-8)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endfunction()

option(SEEDARC_BUILD_FUZZERS "Build libFuzzer targets (MSVC only; use the msvc-fuzz preset)" OFF)

add_subdirectory(core)
add_subdirectory(cli)
if(BUILD_TESTING AND NOT SEEDARC_BUILD_FUZZERS)
    add_subdirectory(tests)
endif()
if(BUILD_TESTING OR SEEDARC_BUILD_FUZZERS)
    add_subdirectory(fuzz)
endif()
