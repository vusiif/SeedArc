# SPDX-License-Identifier: GPL-3.0-or-later
add_executable(seedarc_core_tests core_tests.cpp)
target_link_libraries(seedarc_core_tests PRIVATE SeedArc::core)
seedarc_target_defaults(seedarc_core_tests)
add_test(NAME core.boundaries COMMAND seedarc_core_tests)
set_tests_properties(core.boundaries PROPERTIES TIMEOUT 10)

foreach(suite IN ITEMS archive path index)
    add_executable(seedarc_${suite}_tests ${suite}_tests.cpp)
    target_link_libraries(seedarc_${suite}_tests PRIVATE SeedArc::core)
    seedarc_target_defaults(seedarc_${suite}_tests)
    add_test(NAME ${suite}.boundaries COMMAND seedarc_${suite}_tests)
    set_tests_properties(${suite}.boundaries PROPERTIES TIMEOUT 10)
endforeach()

if(WIN32)
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

    # One 64 MiB file through compress / verify / extract with a bound on the peak working set.
    add_executable(seedarc_store_memory_test store_memory_test.cpp)
    target_link_libraries(seedarc_store_memory_test PRIVATE SeedArc::core psapi)
    seedarc_target_defaults(seedarc_store_memory_test)
    add_test(NAME archive.store_bounded_memory COMMAND seedarc_store_memory_test "${PROJECT_SOURCE_DIR}/.sandbox-test" 64)
    set_tests_properties(archive.store_bounded_memory PROPERTIES TIMEOUT 600)
endif()

set(cli_cases help short_help help_command version missing unknown extra search benchmark
    compress_usage extract_usage verify_usage verify_missing)
if(WIN32)
    list(APPEND cli_cases round_trip)
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
