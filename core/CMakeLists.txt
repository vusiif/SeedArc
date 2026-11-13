# SPDX-License-Identifier: GPL-3.0-or-later
configure_file(version_config.hpp.in generated/seedarc/version_config.hpp @ONLY)
add_library(libseedarc STATIC version.cpp checked_arithmetic.cpp archive.cpp archive_index.cpp
    store_archive.cpp filesystem/archive_path.cpp filesystem/safe_output.cpp
    filesystem/input_tree.cpp codec/codec.cpp)
add_library(SeedArc::core ALIAS libseedarc)
target_include_directories(libseedarc PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_include_directories(libseedarc PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated"
    "${CMAKE_CURRENT_SOURCE_DIR}")
seedarc_target_defaults(libseedarc)
if(WIN32)
    target_link_libraries(libseedarc PRIVATE ntdll bcrypt normaliz advapi32)
endif()

# Third-party codecs come from the vcpkg manifest (vcpkg.json, pinned baseline); only the
# adapters in codec/ include their headers (ADR-0003).
find_package(zstd CONFIG REQUIRED)
find_package(liblzma CONFIG REQUIRED)
target_link_libraries(libseedarc PRIVATE zstd::libzstd liblzma::liblzma)

# Seed candidates stay outside the production archive library until experimental selection.
if((BUILD_TESTING AND NOT SEEDARC_BUILD_FUZZERS) OR SEEDARC_BUILD_RESEARCH_TOOLS)
    add_library(seedarc_seed_study STATIC seed/strategy.cpp seed/file_overrides.cpp
        seed/path_overrides.cpp seed/archive_study.cpp seed/adaptive_file.cpp)
    target_link_libraries(seedarc_seed_study PRIVATE SeedArc::core)
    target_include_directories(seedarc_seed_study PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_SOURCE_DIR}/include")
    target_include_directories(seedarc_seed_study PRIVATE "${PROJECT_SOURCE_DIR}/third_party")
    seedarc_target_defaults(seedarc_seed_study)
endif()
