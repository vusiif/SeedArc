// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Internal to libseedarc: enumeration and bounded reading of compression input.
// Not installed and not part of the public API.

#include "seedarc/archive.hpp"
#include "seedarc/error.hpp"
#include "seedarc/safe_output.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <span>
#include <stop_token>
#include <string>
#include <vector>

namespace seedarc::experimental::internal {
struct InputEntry {
    std::string path;                     // canonical archive path (UTF-8, '/' separated)
    std::vector<std::wstring> components; // native names below the input root; empty: file root
    bool directory = false;
    std::uint64_t size = 0;
    std::int64_t last_write = 0;
    ObjectId id;
};

// One input file, opened without write sharing. read() fills the buffer except at end of file.
class InputFile {
  public:
    InputFile(InputFile &&) noexcept;
    InputFile &operator=(InputFile &&) noexcept;
    ~InputFile();

    [[nodiscard]] std::expected<std::size_t, Error> read(std::span<std::byte> buffer);
    // After the last read: the file ended exactly at the enumerated size and its size and
    // last-write time are still the enumerated ones.
    [[nodiscard]] std::expected<void, Error> confirm_unchanged();

  private:
    friend class InputTree;
    struct Impl;
    explicit InputFile(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

// A regular file or a directory tree. Below the root every object is opened relative to its
// parent handle without following reparse points; symbolic links, junctions and other reparse
// points are rejected, not skipped and not followed.
class InputTree {
  public:
    InputTree(InputTree &&) noexcept;
    InputTree &operator=(InputTree &&) noexcept;
    ~InputTree();

    [[nodiscard]] static std::expected<InputTree, Error> open(const std::filesystem::path &input);

    // Entries in canonical archive order with validated paths. `exclude` (the archive being
    // written) is left out by identity. Limits are enforced while enumerating.
    [[nodiscard]] std::expected<std::vector<InputEntry>, Error>
    enumerate(const ArchiveLimits &limits, const ObjectId *exclude, std::stop_token stop) const;

    // Re-opens a file entry and checks that it is still the enumerated object.
    [[nodiscard]] std::expected<InputFile, Error> open_file(const InputEntry &entry) const;

  private:
    struct Impl;
    explicit InputTree(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};
} // namespace seedarc::experimental::internal
