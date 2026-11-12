# SPDX-License-Identifier: GPL-3.0-or-later
add_executable(seedarc_cli main.cpp)
set_target_properties(seedarc_cli PROPERTIES OUTPUT_NAME seedarc)
target_link_libraries(seedarc_cli PRIVATE SeedArc::core)
seedarc_target_defaults(seedarc_cli)
if(WIN32)
    target_link_libraries(seedarc_cli PRIVATE shell32)
endif()
