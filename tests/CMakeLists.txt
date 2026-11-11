# SPDX-License-Identifier: GPL-3.0-or-later
add_executable(seedarc_core_tests core_tests.cpp)
target_link_libraries(seedarc_core_tests PRIVATE SeedArc::core)
seedarc_target_defaults(seedarc_core_tests)
add_test(NAME core.boundaries COMMAND seedarc_core_tests)
set_tests_properties(core.boundaries PROPERTIES TIMEOUT 10)

foreach(suite IN ITEMS archive path)
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
endif()

foreach(case IN ITEMS help short_help help_command version missing unknown extra compress extract verify search benchmark)
    add_test(NAME cli.${case}
        COMMAND "${CMAKE_COMMAND}"
            "-DSEEDARC_CLI=$<TARGET_FILE:seedarc_cli>"
            "-DSEEDARC_VERSION=${PROJECT_VERSION}"
            "-DCASE=${case}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/check_cli.cmake")
    set_tests_properties(cli.${case} PROPERTIES TIMEOUT 10)
endforeach()
