// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Internal Linux helpers shared by the safe output and input enumeration code (ADR-0004).
// Not installed and not part of the public API.

#ifdef __linux__
#include "seedarc/error.hpp"
#include "seedarc/safe_output.hpp"

#include <cerrno>
#include <cstring>
#include <expected>
#include <string>
#include <string_view>
#include <utility>

#include <sys/stat.h>
#include <sys/statfs.h>
#include <unistd.h>

namespace seedarc::experimental::internal {
// Owns a file descriptor.
class Fd {
  public:
    explicit Fd(int value = -1) noexcept : value_(value) {}
    ~Fd() {
        if (value_ >= 0) {
            ::close(value_);
        }
    }
    Fd(Fd &&other) noexcept : value_(std::exchange(other.value_, -1)) {}
    Fd &operator=(Fd &&other) noexcept {
        if (this != &other) {
            if (value_ >= 0) {
                ::close(value_);
            }
            value_ = std::exchange(other.value_, -1);
        }
        return *this;
    }
    Fd(const Fd &) = delete;
    Fd &operator=(const Fd &) = delete;
    [[nodiscard]] int get() const noexcept { return value_; }
    [[nodiscard]] bool valid() const noexcept { return value_ >= 0; }
    [[nodiscard]] int release() noexcept { return std::exchange(value_, -1); }

  private:
    int value_;
};

// EEXIST is an output conflict; everything else an I/O error.
[[nodiscard]] inline Error posix_error(std::string stage, int code = errno) {
    return {code == EEXIST ? ErrorCode::output_conflict : ErrorCode::io_error,
            std::move(stage) + ": " + std::strerror(code) + " (errno " + std::to_string(code) +
                ")"};
}

// Device number plus inode number (the inode in the first 8 bytes, little-endian independent
// because it is only ever compared with another ObjectId from the same process).
[[nodiscard]] inline ObjectId object_id(const struct stat &status) noexcept {
    ObjectId id;
    id.volume = static_cast<std::uint64_t>(status.st_dev);
    const auto inode = static_cast<std::uint64_t>(status.st_ino);
    static_assert(sizeof(inode) <= sizeof(id.file));
    std::memcpy(id.file.data(), &inode, sizeof(inode));
    return id;
}

// File systems whose O_TMPFILE, RENAME_NOREPLACE, inode stability and fsync behaviour this code
// relies on (ADR-0004 section 2): ext2/3/4, xfs, btrfs, tmpfs. Everything else is refused.
[[nodiscard]] inline std::expected<void, Error> require_supported_filesystem(int fd) {
    struct statfs info{};
    if (::fstatfs(fd, &info) != 0) {
        return std::unexpected(posix_error("inspect file system"));
    }
    switch (static_cast<unsigned long>(info.f_type)) {
    case 0xEF53UL:     // ext2 / ext3 / ext4
    case 0x58465342UL: // xfs
    case 0x9123683EUL: // btrfs
    case 0x01021994UL: // tmpfs
        return {};
    default:
        return std::unexpected(
            Error{ErrorCode::unsupported_feature,
                  "file system type 0x" + [&info] {
                      constexpr std::string_view digits = "0123456789abcdef";
                      auto value = static_cast<unsigned long>(info.f_type);
                      std::string hex;
                      do {
                          hex.insert(hex.begin(), digits[value & 15U]);
                          value >>= 4U;
                      } while (value != 0);
                      return hex;
                  }() + " is not supported (only ext4, xfs, btrfs and tmpfs)"});
    }
}
} // namespace seedarc::experimental::internal
#endif
