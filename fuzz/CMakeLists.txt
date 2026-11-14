# SPDX-License-Identifier: GPL-3.0-or-later
# Each target is built twice: as a corpus replay test (ordinary builds, BUILD_TESTING) and, with
# SEEDARC_BUILD_FUZZERS, as a libFuzzer executable (MSVC /fsanitize=fuzzer, or Clang
# -fsanitize=fuzzer on Linux; see docs/testing.md and docs/linux-l1-validation.md).
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

if(SEEDARC_BUILD_FUZZERS AND NOT MSVC)
    # Linux L1: Clang libFuzzer + ASan + UBSan (clang-linux-fuzz preset). The library gets
    # coverage instrumentation without the libFuzzer main; only the fuzz targets link the
    # runtime, so as with MSVC the build preset builds just these targets.
    if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang" OR WIN32)
        message(FATAL_ERROR "SEEDARC_BUILD_FUZZERS needs MSVC, or Clang on Linux")
    endif()
    set(seedarc_fuzz_sanitizers -fsanitize=address,undefined -fno-sanitize-recover=undefined
        -fno-omit-frame-pointer)
    target_compile_options(libseedarc PRIVATE -fsanitize=fuzzer-no-link ${seedarc_fuzz_sanitizers})
    foreach(target IN LISTS SEEDARC_FUZZ_TARGETS)
        add_executable(seedarc_fuzz_${target} ${target}_fuzz.cpp)
        target_link_libraries(seedarc_fuzz_${target} PRIVATE SeedArc::core)
        seedarc_target_defaults(seedarc_fuzz_${target})
        target_compile_options(seedarc_fuzz_${target} PRIVATE -fsanitize=fuzzer
            ${seedarc_fuzz_sanitizers})
        target_link_options(seedarc_fuzz_${target} PRIVATE -fsanitize=fuzzer
            ${seedarc_fuzz_sanitizers})
    endforeach()
elseif(SEEDARC_BUILD_FUZZERS)
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
