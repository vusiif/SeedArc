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

# DEV-017: append study (portable; research container v3 only).
add_executable(seedarc_append_study append_study.cpp)
target_link_libraries(seedarc_append_study PRIVATE seedarc_codec_dictionary SeedArc::core)
if(WIN32)
    target_link_libraries(seedarc_append_study PRIVATE psapi)
endif()
seedarc_target_defaults(seedarc_append_study)
if(BUILD_TESTING)
    add_test(NAME research.append_study COMMAND "${CMAKE_COMMAND}"
        "-DTOOL=$<TARGET_FILE:seedarc_append_study>" "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/append-check-$<CONFIG>"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/check_append.cmake")
    set_tests_properties(research.append_study PROPERTIES TIMEOUT 300)
endif()

# DEV-017: append study, research container v4 (generation records, redundancy levels, recovery).
add_executable(seedarc_append4_study append4_study.cpp)
target_link_libraries(seedarc_append4_study PRIVATE seedarc_codec_dictionary SeedArc::core)
target_include_directories(seedarc_append4_study PRIVATE "${PROJECT_SOURCE_DIR}/third_party")
if(WIN32)
    target_link_libraries(seedarc_append4_study PRIVATE psapi shell32)
endif()
seedarc_target_defaults(seedarc_append4_study)
if(BUILD_TESTING)
    add_test(NAME research.append4_study COMMAND "${CMAKE_COMMAND}"
        "-DTOOL=$<TARGET_FILE:seedarc_append4_study>" "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/append4-check-$<CONFIG>"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/check_append4.cmake")
    set_tests_properties(research.append4_study PROPERTIES TIMEOUT 600)
    add_test(NAME research.solid_seed COMMAND "${CMAKE_COMMAND}"
        "-DTOOL=$<TARGET_FILE:seedarc_append4_study>" "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/solid-seed-check-$<CONFIG>"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/check_solid_seed.cmake")
    set_tests_properties(research.solid_seed PROPERTIES TIMEOUT 600)
    add_test(NAME research.solid_search COMMAND "${CMAKE_COMMAND}"
        "-DTOOL=$<TARGET_FILE:seedarc_append4_study>" "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/solid-search-check-$<CONFIG>"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/check_solid_search.cmake")
    set_tests_properties(research.solid_search PROPERTIES TIMEOUT 600)
    add_test(NAME research.bounded_solid COMMAND "${CMAKE_COMMAND}"
        "-DTOOL=$<TARGET_FILE:seedarc_append4_study>"
        "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/bounded-solid-check-$<CONFIG>"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/check_bounded_solid.cmake")
    set_tests_properties(research.bounded_solid PROPERTIES TIMEOUT 600)
    add_test(NAME research.bounded_extract COMMAND "${CMAKE_COMMAND}"
        "-DTOOL=$<TARGET_FILE:seedarc_append4_study>"
        "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/bounded-extract-check-$<CONFIG>"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/check_bounded_extract.cmake")
    set_tests_properties(research.bounded_extract PROPERTIES TIMEOUT 600)
    add_test(NAME research.solid_recipe COMMAND "${CMAKE_COMMAND}"
        "-DTOOL=$<TARGET_FILE:seedarc_append4_study>"
        "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/solid-recipe-check-$<CONFIG>"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/check_solid_recipe.cmake")
    set_tests_properties(research.solid_recipe PROPERTIES TIMEOUT 900)
    add_test(NAME research.drift_probe COMMAND "${CMAKE_COMMAND}"
        "-DTOOL=$<TARGET_FILE:seedarc_append4_study>"
        "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/drift-probe-check-$<CONFIG>"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/check_drift_probe.cmake")
    set_tests_properties(research.drift_probe PROPERTIES TIMEOUT 600)
endif()

# DEV-032: fuzz target for the research container reader. Ordinary builds replay the committed
# corpus as a test; SEEDARC_BUILD_FUZZERS with Clang on Linux builds the libFuzzer executable.
if(BUILD_TESTING AND NOT SEEDARC_BUILD_FUZZERS)
    add_executable(seedarc_fuzz_replay_append4 append4_fuzz.cpp "${PROJECT_SOURCE_DIR}/fuzz/replay_main.cpp")
    target_link_libraries(seedarc_fuzz_replay_append4 PRIVATE seedarc_codec_dictionary SeedArc::core)
    target_include_directories(seedarc_fuzz_replay_append4 PRIVATE "${PROJECT_SOURCE_DIR}/third_party")
    if(WIN32)
        target_link_libraries(seedarc_fuzz_replay_append4 PRIVATE psapi shell32)
    endif()
    seedarc_target_defaults(seedarc_fuzz_replay_append4)
    add_test(NAME research.append4_fuzz_replay
        COMMAND seedarc_fuzz_replay_append4 "${CMAKE_CURRENT_SOURCE_DIR}/fuzz-corpus/append4")
    set_tests_properties(research.append4_fuzz_replay PROPERTIES TIMEOUT 120)
endif()
if(SEEDARC_BUILD_FUZZERS AND NOT MSVC)
    set(seedarc_append4_fuzz_flags -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=undefined
        -fno-omit-frame-pointer)
    add_executable(seedarc_fuzz_append4 append4_fuzz.cpp)
    target_link_libraries(seedarc_fuzz_append4 PRIVATE seedarc_codec_dictionary SeedArc::core)
    target_include_directories(seedarc_fuzz_append4 PRIVATE "${PROJECT_SOURCE_DIR}/third_party")
    seedarc_target_defaults(seedarc_fuzz_append4)
    target_compile_options(seedarc_fuzz_append4 PRIVATE ${seedarc_append4_fuzz_flags})
    target_link_options(seedarc_fuzz_append4 PRIVATE ${seedarc_append4_fuzz_flags})
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
