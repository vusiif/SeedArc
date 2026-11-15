# SPDX-License-Identifier: GPL-3.0-or-later
add_executable(seedarc_seed_plan seed_plan.cpp)
target_link_libraries(seedarc_seed_plan PRIVATE seedarc_seed_study)
seedarc_target_defaults(seedarc_seed_plan)

add_executable(seedarc_seed_scope_plan seed_scope_plan.cpp)
target_link_libraries(seedarc_seed_scope_plan PRIVATE seedarc_seed_study)
seedarc_target_defaults(seedarc_seed_scope_plan)

add_executable(seedarc_seed_scope_contract seed_scope_contract.cpp)
target_link_libraries(seedarc_seed_scope_contract PRIVATE seedarc_seed_study)
seedarc_target_defaults(seedarc_seed_scope_contract)

if(WIN32)
    add_executable(seedarc_seed_archive seed_archive.cpp)
    target_link_libraries(seedarc_seed_archive PRIVATE seedarc_seed_study psapi shell32)
    seedarc_target_defaults(seedarc_seed_archive)

    # ADR-0005 evidence: portable path profile vs the Win32 APIs it replaced.
    add_executable(seedarc_unicode_win_diff unicode_win_diff.cpp)
    target_link_libraries(seedarc_unicode_win_diff PRIVATE SeedArc::core normaliz)
    target_include_directories(seedarc_unicode_win_diff PRIVATE "${PROJECT_SOURCE_DIR}/core")
    seedarc_target_defaults(seedarc_unicode_win_diff)
endif()

# DEV-016: G5 budgeted grouping / dictionary search (portable; research container v2 only).
add_executable(seedarc_g5_study g5_study.cpp)
target_link_libraries(seedarc_g5_study PRIVATE seedarc_codec_dictionary)
target_include_directories(seedarc_g5_study PRIVATE "${PROJECT_SOURCE_DIR}/third_party")
if(WIN32)
    target_link_libraries(seedarc_g5_study PRIVATE psapi)
endif()
seedarc_target_defaults(seedarc_g5_study)
if(BUILD_TESTING)
    add_test(NAME research.g5_study COMMAND "${CMAKE_COMMAND}" "-DTOOL=$<TARGET_FILE:seedarc_g5_study>"
        "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/g5-check-$<CONFIG>"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/check_g5.cmake")
    set_tests_properties(research.g5_study PROPERTIES TIMEOUT 300)
endif()

# DEV-016: production path-profile check for corpus builders (portable).
add_executable(seedarc_path_check path_check.cpp)
target_link_libraries(seedarc_path_check PRIVATE SeedArc::core)
seedarc_target_defaults(seedarc_path_check)

# DEV-013 shared-dictionary feasibility study (portable; research container only).
add_executable(seedarc_dict_study dict_study.cpp)
target_link_libraries(seedarc_dict_study PRIVATE seedarc_codec_dictionary)
if(WIN32)
    target_link_libraries(seedarc_dict_study PRIVATE psapi)
endif()
seedarc_target_defaults(seedarc_dict_study)
