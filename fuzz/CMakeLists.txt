# SPDX-License-Identifier: GPL-3.0-or-later
# Each target is built twice: as a corpus replay test (ordinary builds, BUILD_TESTING) and, with
# SEEDARC_BUILD_FUZZERS, as a libFuzzer executable (MSVC /fsanitize=fuzzer; see docs/testing.md).
set(SEEDARC_FUZZ_TARGETS header path archive)

if(BUILD_TESTING AND NOT SEEDARC_BUILD_FUZZERS)
    foreach(target IN LISTS SEEDARC_FUZZ_TARGETS)
        add_executable(seedarc_fuzz_replay_${target} ${target}_fuzz.cpp replay_main.cpp)
        target_link_libraries(seedarc_fuzz_replay_${target} PRIVATE SeedArc::core)
        seedarc_target_defaults(seedarc_fuzz_replay_${target})
        add_test(NAME fuzz.replay.${target}
            COMMAND seedarc_fuzz_replay_${target} "${CMAKE_CURRENT_SOURCE_DIR}/corpus/${target}")
        set_tests_properties(fuzz.replay.${target} PROPERTIES TIMEOUT 30)
    endforeach()
endif()

if(SEEDARC_BUILD_FUZZERS)
    if(NOT MSVC)
        message(FATAL_ERROR "SEEDARC_BUILD_FUZZERS is only verified with MSVC /fsanitize=fuzzer")
    endif()
    # Instrument the library under test for coverage feedback and ASan. Only the fuzz targets
    # link the libFuzzer runtime, so a fuzz build tree must build just these targets
    # (the msvc-fuzz build preset does).
    target_compile_options(libseedarc PRIVATE /fsanitize=address
        /fsanitize-coverage=inline-8bit-counters /fsanitize-coverage=edge
        /fsanitize-coverage=trace-cmp /fsanitize-coverage=trace-div)
    foreach(target IN LISTS SEEDARC_FUZZ_TARGETS)
        add_executable(seedarc_fuzz_${target} ${target}_fuzz.cpp)
        target_link_libraries(seedarc_fuzz_${target} PRIVATE SeedArc::core)
        seedarc_target_defaults(seedarc_fuzz_${target})
        target_compile_options(seedarc_fuzz_${target} PRIVATE /fsanitize=address /fsanitize=fuzzer)
        target_link_options(seedarc_fuzz_${target} PRIVATE /INCREMENTAL:NO)
    endforeach()
endif()
