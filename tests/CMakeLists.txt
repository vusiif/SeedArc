# SPDX-License-Identifier: GPL-3.0-or-later
add_executable(seedarc_core_tests core_tests.cpp)
target_link_libraries(seedarc_core_tests PRIVATE SeedArc::core)
seedarc_target_defaults(seedarc_core_tests)
add_test(NAME core.boundaries COMMAND seedarc_core_tests)
set_tests_properties(core.boundaries PROPERTIES TIMEOUT 10)

add_executable(seedarc_seed_strategy_tests seed_strategy_tests.cpp)
target_link_libraries(seedarc_seed_strategy_tests PRIVATE seedarc_seed_study)
seedarc_target_defaults(seedarc_seed_strategy_tests)
add_test(NAME seed.candidate_plans COMMAND seedarc_seed_strategy_tests)
set_tests_properties(seed.candidate_plans PROPERTIES TIMEOUT 10)

add_executable(seedarc_seed_file_override_tests seed_file_override_tests.cpp)
target_link_libraries(seedarc_seed_file_override_tests PRIVATE seedarc_seed_study)
seedarc_target_defaults(seedarc_seed_file_override_tests)
add_test(NAME seed.file_rank_overrides COMMAND seedarc_seed_file_override_tests)
set_tests_properties(seed.file_rank_overrides PROPERTIES TIMEOUT 10)

add_executable(seedarc_seed_path_override_tests seed_path_override_tests.cpp)
target_link_libraries(seedarc_seed_path_override_tests PRIVATE seedarc_seed_study)
seedarc_target_defaults(seedarc_seed_path_override_tests)
add_test(NAME seed.path_rank_overrides COMMAND seedarc_seed_path_override_tests)
set_tests_properties(seed.path_rank_overrides PROPERTIES TIMEOUT 10)

# DEV-013 research zstd dictionary adapter (portable, in-memory).
add_executable(seedarc_codec_dictionary_tests codec_dictionary_tests.cpp)
target_link_libraries(seedarc_codec_dictionary_tests PRIVATE seedarc_codec_dictionary)
seedarc_target_defaults(seedarc_codec_dictionary_tests)
add_test(NAME codec.dictionary_research COMMAND seedarc_codec_dictionary_tests)
set_tests_properties(codec.dictionary_research PROPERTIES TIMEOUT 60)

add_executable(seedarc_m3_compatibility_tests m3_compatibility_tests.cpp)
target_link_libraries(seedarc_m3_compatibility_tests PRIVATE SeedArc::core)
seedarc_target_defaults(seedarc_m3_compatibility_tests)
add_test(NAME archive.m3_compatibility COMMAND seedarc_m3_compatibility_tests
    "${PROJECT_SOURCE_DIR}/tests/fixtures/m3")
set_tests_properties(archive.m3_compatibility PROPERTIES TIMEOUT 10)

foreach(suite IN ITEMS archive path index)
    add_executable(seedarc_${suite}_tests ${suite}_tests.cpp)
    target_link_libraries(seedarc_${suite}_tests PRIVATE SeedArc::core)
    seedarc_target_defaults(seedarc_${suite}_tests)
    add_test(NAME ${suite}.boundaries COMMAND seedarc_${suite}_tests)
    set_tests_properties(${suite}.boundaries PROPERTIES TIMEOUT 10)
endforeach()

if(WIN32)
    add_executable(seedarc_seed_archive_tests seed_archive_tests.cpp)
    target_link_libraries(seedarc_seed_archive_tests PRIVATE seedarc_seed_study)
    seedarc_target_defaults(seedarc_seed_archive_tests)
    add_test(NAME seed.archive_integration COMMAND seedarc_seed_archive_tests "${PROJECT_SOURCE_DIR}/.sandbox-test")
    set_tests_properties(seed.archive_integration PROPERTIES TIMEOUT 120)

    add_executable(seedarc_seed_adaptive_tests seed_adaptive_tests.cpp)
    target_link_libraries(seedarc_seed_adaptive_tests PRIVATE seedarc_seed_study)
    seedarc_target_defaults(seedarc_seed_adaptive_tests)
    add_test(NAME seed.adaptive_file COMMAND seedarc_seed_adaptive_tests "${PROJECT_SOURCE_DIR}/.sandbox-test")
    set_tests_properties(seed.adaptive_file PROPERTIES TIMEOUT 120)

    add_executable(seedarc_safe_output_tests safe_output_tests.cpp)
    target_link_libraries(seedarc_safe_output_tests PRIVATE SeedArc::core)
    # Internal test seam (fault injection, volume-root predicate); not a public header.
    target_include_directories(seedarc_safe_output_tests PRIVATE "${PROJECT_SOURCE_DIR}/core/filesystem")
    seedarc_target_defaults(seedarc_safe_output_tests)
    add_test(NAME filesystem.safe_output COMMAND seedarc_safe_output_tests "${PROJECT_SOURCE_DIR}/.sandbox-test")
    set_tests_properties(filesystem.safe_output PROPERTIES TIMEOUT 30)

    # End-to-end Store archive round trips, hostile archives and injected failures.
    add_executable(seedarc_store_archive_tests store_archive_tests.cpp)
    target_link_libraries(seedarc_store_archive_tests PRIVATE SeedArc::core)
    target_include_directories(seedarc_store_archive_tests PRIVATE "${PROJECT_SOURCE_DIR}/core/filesystem")
    seedarc_target_defaults(seedarc_store_archive_tests)
    add_test(NAME archive.store_round_trip COMMAND seedarc_store_archive_tests "${PROJECT_SOURCE_DIR}/.sandbox-test")
    set_tests_properties(archive.store_round_trip PROPERTIES TIMEOUT 300)

    add_executable(seedarc_store_acceptance_tests store_acceptance_tests.cpp)
    target_link_libraries(seedarc_store_acceptance_tests PRIVATE SeedArc::core)
    target_include_directories(seedarc_store_acceptance_tests PRIVATE "${PROJECT_SOURCE_DIR}/core/filesystem")
    seedarc_target_defaults(seedarc_store_acceptance_tests)
    add_test(NAME archive.store_acceptance COMMAND seedarc_store_acceptance_tests "${PROJECT_SOURCE_DIR}/.sandbox-test")
    set_tests_properties(archive.store_acceptance PROPERTIES TIMEOUT 30)

    # M3: the codec boundary (internal header) and Zstandard / LZMA2 archives end to end.
    add_executable(seedarc_codec_tests codec_tests.cpp)
    target_link_libraries(seedarc_codec_tests PRIVATE SeedArc::core)
    target_include_directories(seedarc_codec_tests PRIVATE "${PROJECT_SOURCE_DIR}/core")
    seedarc_target_defaults(seedarc_codec_tests)
    add_test(NAME codec.round_trip COMMAND seedarc_codec_tests "${PROJECT_SOURCE_DIR}/.sandbox-test")
    set_tests_properties(codec.round_trip PROPERTIES TIMEOUT 300)

    # One large file through compress / verify / extract with a bound on the peak working set.
    add_executable(seedarc_store_memory_test store_memory_test.cpp)
    target_link_libraries(seedarc_store_memory_test PRIVATE SeedArc::core psapi)
    seedarc_target_defaults(seedarc_store_memory_test)
    add_test(NAME archive.store_bounded_memory COMMAND seedarc_store_memory_test "${PROJECT_SOURCE_DIR}/.sandbox-test" 64)
    set_tests_properties(archive.store_bounded_memory PROPERTIES TIMEOUT 600)
    add_test(NAME codec.zstd_bounded_memory COMMAND seedarc_store_memory_test "${PROJECT_SOURCE_DIR}/.sandbox-test" 64 zstd 16)
    set_tests_properties(codec.zstd_bounded_memory PROPERTIES TIMEOUT 600)
    add_test(NAME codec.lzma2_bounded_memory COMMAND seedarc_store_memory_test "${PROJECT_SOURCE_DIR}/.sandbox-test" 64 lzma2 32)
    set_tests_properties(codec.lzma2_bounded_memory PROPERTIES TIMEOUT 600)
endif()

set(cli_cases help short_help help_command version missing unknown extra search benchmark
    compress_usage extract_usage verify_usage verify_missing codec_usage)
if(WIN32)
    list(APPEND cli_cases round_trip codec_round_trip)
endif()
foreach(case IN LISTS cli_cases)
    add_test(NAME cli.${case}
        COMMAND "${CMAKE_COMMAND}"
            "-DSEEDARC_CLI=$<TARGET_FILE:seedarc_cli>"
            "-DSEEDARC_VERSION=${PROJECT_VERSION}"
            "-DCASE=${case}"
            "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/cli-work-$<CONFIG>"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/check_cli.cmake")
    set_tests_properties(cli.${case} PROPERTIES TIMEOUT 60)
endforeach()
