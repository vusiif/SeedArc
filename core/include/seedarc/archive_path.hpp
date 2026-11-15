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

// Archive path profile (ADR-0001, Unicode rules per ADR-0005): strict UTF-8 whose code points are
// all assigned in the pinned Unicode version and already NFC; same result on every platform.
// Components are UTF-16 on Windows and UTF-32 elsewhere.
[[nodiscard]] std::expected<std::vector<std::wstring>, Error>
validate_archive_path(std::string_view path, const PathLimits &limits = {});

[[nodiscard]] std::expected<void, Error> validate_manifest_paths(std::span<const PathEntry> entries,
                                                                 const PathLimits &limits = {});
} // namespace seedarc::experimental
