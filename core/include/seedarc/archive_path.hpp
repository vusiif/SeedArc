// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "seedarc/error.hpp"

#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace seedarc::experimental {
struct PathLimits {
    std::size_t max_path_bytes = 4096;
    std::size_t max_components = 64;
    std::size_t max_entries = 100000;
    std::size_t max_metadata_bytes = 16 * 1024 * 1024;
};

struct PathEntry {
    std::string path;
    bool directory = false;
};

// Windows NFC UTF-8 profile. Unsupported platforms fail closed.
[[nodiscard]] std::expected<std::vector<std::wstring>, Error>
validate_archive_path(std::string_view path, const PathLimits &limits = {});

[[nodiscard]] std::expected<void, Error> validate_manifest_paths(std::span<const PathEntry> entries,
                                                                 const PathLimits &limits = {});
} // namespace seedarc::experimental
