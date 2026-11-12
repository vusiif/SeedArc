// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "seedarc/error.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>

namespace seedarc::experimental {
// Candidate cumulative budgets for any object reachable through SafeOutputDirectory, counted from
// the drive root ("C:\" is depth 0). Path units are UTF-16 code units of the equivalent DOS path
// with one separator per component; they stay below the 32767-unit NT path ceiling so that the
// output remains reachable by ordinary path-based tools.
inline constexpr std::size_t max_output_depth = 128;
inline constexpr std::size_t max_output_path_units = 32'000;

// Identity of a file system object: volume serial number plus 128-bit file identifier.
struct ObjectId {
    std::uint64_t volume = 0;
    std::array<std::byte, 16> file{};

    bool operator==(const ObjectId &) const = default;
};

class SafeOutputFile;

// Handle-relative output primitives. Each operation uses one basename below a pinned directory.
class SafeOutputDirectory {
  public:
    [[nodiscard]] static std::expected<SafeOutputDirectory, Error>
    open_existing(const std::filesystem::path &absolute_directory);

    [[nodiscard]] std::expected<SafeOutputDirectory, Error>
    create_child(std::string_view basename) const;

    // Buffered single-file write (at most 4 MiB), implemented on top of begin_file().
    [[nodiscard]] std::expected<void, Error> write_new_file(std::string_view basename,
                                                            std::span<const std::byte> contents,
                                                            std::uint32_t expected_crc32c,
                                                            std::stop_token stop = {}) const;

    // Streaming write: creates an exclusive temporary file in this directory. Fails with
    // output_conflict if `basename` already exists; the commit itself never replaces either.
    [[nodiscard]] std::expected<SafeOutputFile, Error> begin_file(std::string_view basename) const;

    // Creates a uniquely named temporary child directory that commit_staged() later renames to
    // `final_basename` without replacement. Fails with output_conflict if that name exists.
    [[nodiscard]] std::expected<SafeOutputDirectory, Error>
    create_staging_child(std::string_view final_basename) const;

    // Only for a directory from create_staging_child(); every handle below it must be closed.
    [[nodiscard]] std::expected<void, Error> commit_staged();

    [[nodiscard]] std::expected<ObjectId, Error> identity() const;

    // Pre-publication checks for a staged tree. Only for directories this library created or
    // opened with open_owned_child(). require_exact_children succeeds only if the directory holds
    // exactly these basenames (exact spelling) and no named data streams. verify_owned_file
    // re-reads a child with writers and deleters excluded and requires the recorded identity, one
    // link, no named streams, `size` bytes and the CRC32C. Mismatches are integrity_error.
    [[nodiscard]] std::expected<void, Error>
    require_exact_children(std::span<const std::string> basenames) const;
    [[nodiscard]] std::expected<void, Error>
    verify_owned_file(std::string_view basename, const ObjectId &expected, std::uint64_t size,
                      std::uint32_t crc32c, std::stop_token stop = {}) const;

    // Rollback support. Both act on an existing child only if it is a non-reparse object of the
    // expected kind on the pinned volume whose identity equals `expected`; nothing is ever
    // removed by path or without that identity match.
    [[nodiscard]] std::expected<SafeOutputDirectory, Error>
    open_owned_child(std::string_view basename, const ObjectId &expected) const;
    [[nodiscard]] std::expected<void, Error>
    remove_owned_child(std::string_view basename, const ObjectId &expected, bool directory) const;

    // Only a directory created by create_child / create_staging_child may be discarded; it must
    // be empty.
    [[nodiscard]] std::expected<void, Error> discard_empty();

  private:
    friend class SafeOutputFile;
    struct Impl;
    explicit SafeOutputDirectory(std::shared_ptr<Impl> impl);
    std::shared_ptr<Impl> impl_;
};

// One file being written. Bytes go to an exclusive temporary; commit() flushes, verifies the size
// and re-reads the file against the running CRC32C, then renames it to the final name without
// replacement. A session that is destroyed or fails before a successful commit deletes its
// temporary through its own handle. After any failure the session only reports errors.
class SafeOutputFile {
  public:
    SafeOutputFile(SafeOutputFile &&) noexcept;
    SafeOutputFile &operator=(SafeOutputFile &&) noexcept;
    ~SafeOutputFile();

    [[nodiscard]] const ObjectId &identity() const noexcept;
    [[nodiscard]] std::uint64_t bytes_written() const noexcept;
    [[nodiscard]] std::uint32_t checksum() const noexcept; // CRC32C of the bytes appended so far

    [[nodiscard]] std::expected<void, Error> append(std::span<const std::byte> bytes);

    // For formats whose header depends on what follows. reserve_prefix() must come before any
    // append() and writes `size` (at most 4096) zero bytes; write_prefix() later replaces exactly
    // those bytes. The prefix is excluded from checksum() and compared byte for byte at commit.
    [[nodiscard]] std::expected<void, Error> reserve_prefix(std::size_t size);
    [[nodiscard]] std::expected<void, Error> write_prefix(std::span<const std::byte> bytes);
    [[nodiscard]] std::expected<void, Error> commit(std::uint64_t expected_size,
                                                    std::stop_token stop = {});

  private:
    friend class SafeOutputDirectory;
    struct Impl;
    explicit SafeOutputFile(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};
} // namespace seedarc::experimental
