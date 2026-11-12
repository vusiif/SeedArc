// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "seedarc/archive.hpp"
#include "seedarc/error.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <span>
#include <stop_token>

namespace seedarc::experimental {
// Store-only archives in the experimental container (ADR-0001 / ADR-0002). Not a stable format.
struct CompressOptions {
    std::uint32_t chunk_size_bytes = 1024U * 1024;
    ArchiveLimits limits{};
};

struct ArchiveSummary {
    std::uint64_t entries = 0;
    std::uint64_t blocks = 0;
    std::uint64_t total_output = 0;  // bytes of file content
    std::uint64_t archive_bytes = 0; // complete archive size
};

// Archives one regular file or one directory tree (its contents, not the directory's own name).
// The archive is written to a temporary next to `output_archive` and committed without
// replacement; an existing output is an output_conflict. Reparse points are rejected, and a file
// that changes while it is read fails the whole operation. Nothing is left behind on failure.
[[nodiscard]] std::expected<ArchiveSummary, Error>
compress_store(const std::filesystem::path &input, const std::filesystem::path &output_archive,
               const CompressOptions &options = {}, std::stop_token stop = {});

// Runs the complete parse / validate / decode pipeline and discards the decoded bytes.
[[nodiscard]] std::expected<ArchiveSummary, Error>
verify_archive(const std::filesystem::path &archive, const ArchiveLimits &limits = {},
               std::stop_token stop = {});

// The same pipeline over an archive held in memory (used by tests and fuzzing).
[[nodiscard]] std::expected<ArchiveSummary, Error>
verify_archive_bytes(std::span<const std::byte> archive, const ArchiveLimits &limits = {});

// Extracts into `new_directory`, which must not exist; its parent must. Everything is written to
// a temporary directory next to it that is renamed into place only after the whole archive has
// been decoded and verified. On failure or cancellation the objects this run created are removed
// again (by identity, never by path) and the target name never appears.
[[nodiscard]] std::expected<ArchiveSummary, Error>
extract_archive(const std::filesystem::path &archive, const std::filesystem::path &new_directory,
                const ArchiveLimits &limits = {}, std::stop_token stop = {});
} // namespace seedarc::experimental
