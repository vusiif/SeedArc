# SPDX-License-Identifier: GPL-3.0-or-later
configure_file(version_config.hpp.in generated/seedarc/version_config.hpp @ONLY)
add_library(libseedarc STATIC version.cpp checked_arithmetic.cpp archive.cpp archive_index.cpp
    store_archive.cpp filesystem/archive_path.cpp filesystem/safe_output.cpp
    filesystem/input_tree.cpp)
add_library(SeedArc::core ALIAS libseedarc)
target_include_directories(libseedarc PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_include_directories(libseedarc PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated"
    "${CMAKE_CURRENT_SOURCE_DIR}")
seedarc_target_defaults(libseedarc)
if(WIN32)
    target_link_libraries(libseedarc PRIVATE ntdll bcrypt normaliz advapi32)
endif()
