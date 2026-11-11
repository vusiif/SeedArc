// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "seedarc/error.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <span>
#include <stop_token>
#include <string_view>

namespace seedarc::experimental {
// Candidate cumulative budgets for any object reachable through SafeOutputDirectory, counted from
// the drive root ("C:\" is depth 0). Path units are UTF-16 code units of the equivalent DOS path
// with one separator per component; they stay below the 32767-unit NT path ceiling so that the
// output remains reachable by ordinary path-based tools.
inline constexpr std::size_t max_output_depth = 128;
inline constexpr std::size_t max_output_path_units = 32'000;

// M1 primitives, not a complete extraction transaction. Each operation uses one basename.
class SafeOutputDirectory {
  public:
    [[nodiscard]] static std::expected<SafeOutputDirectory, Error>
    open_existing(const std::filesystem::path &absolute_directory);

    [[nodiscard]] std::expected<SafeOutputDirectory, Error>
    create_child(std::string_view basename) const;

    [[nodiscard]] std::expected<void, Error> write_new_file(std::string_view basename,
                                                            std::span<const std::byte> contents,
                                                            std::uint32_t expected_crc32c,
                                                            std::stop_token stop = {}) const;

    // Only a directory created by create_child may be discarded; it must be empty.
    [[nodiscard]] std::expected<void, Error> discard_empty();

  private:
    struct Impl;
    explicit SafeOutputDirectory(std::shared_ptr<Impl> impl);
    std::shared_ptr<Impl> impl_;
};
} // namespace seedarc::experimental
