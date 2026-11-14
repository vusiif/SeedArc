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
endif()

# DEV-013 shared-dictionary feasibility study (portable; research container only).
add_executable(seedarc_dict_study dict_study.cpp)
target_link_libraries(seedarc_dict_study PRIVATE seedarc_codec_dictionary)
if(WIN32)
    target_link_libraries(seedarc_dict_study PRIVATE psapi)
endif()
seedarc_target_defaults(seedarc_dict_study)
