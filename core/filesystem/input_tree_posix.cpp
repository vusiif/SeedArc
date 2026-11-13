// SPDX-License-Identifier: GPL-3.0-or-later
// Linux implementation of InputTree / InputFile (ADR-0004 section 4). The Windows version is in
// input_tree.cpp.
#ifdef __linux__
#include "input_tree.hpp"
#include "posix_common.hpp"
#include "seedarc/archive_index.hpp"
#include "seedarc/archive_path.hpp"

#include <algorithm>
#include <cerrno>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace seedarc::experimental::internal {
namespace {
constexpr std::size_t max_tree_depth = 64; // same as the archive path component limit
// O_NONBLOCK: a FIFO or device that replaced a regular file must not block (or act on) the open.
constexpr int file_flags = O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_NOCTTY | O_CLOEXEC;
constexpr int directory_flags = O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC;

Error with_path(Error error, std::string_view path) {
    error.message += ": ";
    error.message += path;
    return error;
}

Error link_rejected(std::string_view path) {
    return with_path({ErrorCode::unsupported_feature,
                      "input contains a symbolic link, which is rejected rather than followed"},
                     path);
}

Error special_rejected(std::string_view path) {
    return with_path({ErrorCode::unsupported_feature,
                      "input contains a device, FIFO or socket, which is rejected"},
                     path);
}

struct ObjectInfo {
    bool directory = false;
    std::uint64_t size = 0;
    std::int64_t last_write = 0; // modification time in nanoseconds since the epoch
    ObjectId id;
};

// Only directories and regular files are described; everything else is rejected by the caller.
std::expected<ObjectInfo, Error> describe(const struct stat &status) {
    std::int64_t time = 0;
    if (__builtin_mul_overflow(static_cast<std::int64_t>(status.st_mtim.tv_sec),
                               std::int64_t{1'000'000'000}, &time) ||
        __builtin_add_overflow(time, static_cast<std::int64_t>(status.st_mtim.tv_nsec), &time)) {
        return std::unexpected(
            Error{ErrorCode::unsupported_feature, "input modification time is out of range"});
    }
    if (status.st_size < 0) {
        return std::unexpected(Error{ErrorCode::io_error, "input reports a negative size"});
    }
    return ObjectInfo{S_ISDIR(status.st_mode), static_cast<std::uint64_t>(status.st_size), time,
                      object_id(status)};
}

std::expected<struct stat, Error> stat_fd(int fd) {
    struct stat status{};
    if (::fstat(fd, &status) != 0) {
        return std::unexpected(posix_error("read input metadata"));
    }
    return status;
}

// Names of one directory through a duplicate of its descriptor. At most `max_names`.
std::expected<std::vector<std::string>, Error> list_directory(int directory,
                                                               std::size_t max_names) {
    const int duplicate = ::openat(directory, ".", directory_flags);
    if (duplicate < 0) {
        return std::unexpected(posix_error("enumerate input directory"));
    }
    DIR *stream = ::fdopendir(duplicate);
    if (stream == nullptr) {
        const auto code = errno;
        ::close(duplicate);
        return std::unexpected(posix_error("enumerate input directory", code));
    }
    std::vector<std::string> names;
    for (;;) {
        errno = 0;
        const dirent *entry = ::readdir(stream);
        if (entry == nullptr) {
            const auto code = errno;
            ::closedir(stream);
            if (code != 0) {
                return std::unexpected(posix_error("enumerate input directory", code));
            }
            return names;
        }
        const std::string_view name{entry->d_name};
        if (name == "." || name == "..") {
            continue;
        }
        if (names.size() >= max_names) {
            ::closedir(stream);
            return std::unexpected(
                Error{ErrorCode::resource_limit, "input has more entries than the limit"});
        }
        names.emplace_back(name);
    }
}

struct Walker {
    const ArchiveLimits &limits;
    const ObjectId *exclude;
    std::stop_token stop;
    dev_t device;
    std::vector<InputEntry> entries;
    std::vector<std::string> components;
    std::uint64_t metadata_bytes = 0;
    std::uint64_t total_bytes = 0;

    std::expected<void, Error> add(InputEntry entry) {
        const auto limit = [&entry](const char *message) {
            return std::unexpected(with_path({ErrorCode::resource_limit, message}, entry.path));
        };
        if (entries.size() >= limits.max_entries) {
            return limit("input has more entries than the limit");
        }
        const std::uint64_t record = entry_record_fixed_bytes + entry.path.size();
        if (metadata_bytes > limits.max_index_bytes ||
            record > limits.max_index_bytes - metadata_bytes) {
            return limit("input paths exceed the index size limit");
        }
        if (total_bytes > limits.max_total_output ||
            entry.size > limits.max_total_output - total_bytes) {
            return limit("input exceeds the total size limit");
        }
        metadata_bytes += record;
        total_bytes += entry.size;
        entries.push_back(std::move(entry));
        return {};
    }

    std::expected<void, Error> walk(int directory, const std::string &prefix) {
        if (components.size() >= max_tree_depth) {
            return std::unexpected(
                with_path({ErrorCode::resource_limit, "input directory tree is too deep"}, prefix));
        }
        // One extra name leaves room for the excluded output archive (as on Windows).
        const auto remaining =
            limits.max_entries - std::min(std::uint64_t{entries.size()}, limits.max_entries);
        const auto listed = list_directory(
            directory, static_cast<std::size_t>(std::min(
                           remaining, std::uint64_t{std::numeric_limits<std::size_t>::max() - 1})) +
                           1);
        if (!listed) {
            return std::unexpected(with_path(listed.error(), prefix));
        }
        for (const auto &name : *listed) {
            if (stop.stop_requested()) {
                return std::unexpected(Error{ErrorCode::cancelled, "compression cancelled"});
            }
            const auto path = prefix.empty() ? name : prefix + '/' + name;
            struct stat status{};
            if (::fstatat(directory, name.c_str(), &status, AT_SYMLINK_NOFOLLOW) != 0) {
                return std::unexpected(with_path(
                    {ErrorCode::io_error, "input changed while it was being enumerated"}, path));
            }
            if (S_ISLNK(status.st_mode)) {
                return std::unexpected(link_rejected(path));
            }
            if (!S_ISDIR(status.st_mode) && !S_ISREG(status.st_mode)) {
                return std::unexpected(special_rejected(path));
            }
            if (status.st_dev != device) {
                return std::unexpected(with_path(
                    {ErrorCode::unsupported_feature, "input crosses a file system boundary"},
                    path));
            }
            const auto info = describe(status);
            if (!info) {
                return std::unexpected(with_path(info.error(), path));
            }
            if (!info->directory && exclude != nullptr && info->id == *exclude) {
                continue;
            }
            Fd child;
            if (info->directory) {
                child = Fd{::openat(directory, name.c_str(), directory_flags)};
                if (!child.valid()) {
                    if (errno == ELOOP || errno == ENOTDIR) {
                        return std::unexpected(with_path(
                            {ErrorCode::io_error, "input changed while it was being enumerated"},
                            path));
                    }
                    return std::unexpected(with_path(posix_error("open input"), path));
                }
                const auto opened = stat_fd(child.get());
                if (!opened) {
                    return std::unexpected(with_path(opened.error(), path));
                }
                if (object_id(*opened) != info->id) {
                    return std::unexpected(with_path(
                        {ErrorCode::io_error, "input changed while it was being enumerated"},
                        path));
                }
            }
            components.push_back(name);
            auto added = add({path, components, info->directory, info->directory ? 0 : info->size,
                              info->last_write, info->id});
            if (added && info->directory) {
                added = walk(child.get(), path);
            }
            components.pop_back();
            if (!added) {
                return added;
            }
        }
        return {};
    }
};
} // namespace

struct InputTree::Impl {
    Fd root;
    bool directory = false;
    std::string name; // final component of the input path
    dev_t device = 0;
};

struct InputFile::Impl {
    Fd file;
    std::string path;
    std::uint64_t size = 0;
    std::int64_t last_write = 0;
    std::uint64_t consumed = 0;

    [[nodiscard]] Error changed() const {
        return with_path({ErrorCode::io_error, "input changed while it was being compressed"},
                         path);
    }
};

std::expected<InputTree, Error> InputTree::open(const std::filesystem::path &input) {
    std::error_code ec;
    auto absolute = std::filesystem::absolute(input, ec);
    if (ec || absolute.empty()) {
        return std::unexpected(Error{ErrorCode::invalid_argument, "invalid input path"});
    }
    auto native = absolute.lexically_normal().native();
    while (native.size() > 1 && native.back() == '/') {
        native.pop_back();
    }
    if (native.find('\0') != std::string::npos) {
        return std::unexpected(Error{ErrorCode::invalid_argument, "invalid input path"});
    }
    const auto separator = native.find_last_of('/');
    auto name = separator == std::string::npos ? native : native.substr(separator + 1);
    // The path the user typed may pass through links; the input object itself may not be one.
    // An O_PATH descriptor identifies the object without opening it (no side effects on devices).
    const Fd probe{::open(native.c_str(), O_PATH | O_NOFOLLOW | O_CLOEXEC)};
    if (!probe.valid()) {
        return std::unexpected(posix_error("open input"));
    }
    const auto probed = stat_fd(probe.get());
    if (!probed) {
        return std::unexpected(probed.error());
    }
    if (S_ISLNK(probed->st_mode)) {
        return std::unexpected(link_rejected(name));
    }
    if (!S_ISDIR(probed->st_mode) && !S_ISREG(probed->st_mode)) {
        return std::unexpected(special_rejected(name));
    }
    const bool is_directory = S_ISDIR(probed->st_mode);
    Fd root{::open(native.c_str(), is_directory ? directory_flags : file_flags)};
    if (!root.valid()) {
        if (errno == ELOOP || errno == ENOTDIR) {
            return std::unexpected(Error{ErrorCode::io_error, "input changed while it was opened"});
        }
        return std::unexpected(posix_error("open input"));
    }
    const auto status = stat_fd(root.get());
    if (!status) {
        return std::unexpected(status.error());
    }
    if (object_id(*status) != object_id(*probed) || S_ISDIR(status->st_mode) != is_directory) {
        return std::unexpected(Error{ErrorCode::io_error, "input changed while it was opened"});
    }
    if (auto supported = require_supported_filesystem(root.get()); !supported) {
        return std::unexpected(supported.error());
    }
    auto impl = std::make_unique<Impl>();
    impl->root = std::move(root);
    impl->directory = is_directory;
    impl->name = std::move(name);
    impl->device = status->st_dev;
    return InputTree{std::move(impl)};
}

std::expected<std::vector<InputEntry>, Error> InputTree::enumerate(const ArchiveLimits &limits,
                                                                   const ObjectId *exclude,
                                                                   std::stop_token stop) const {
    Walker walker{limits, exclude, std::move(stop), impl_->device, {}, {}};
    if (impl_->directory) {
        if (auto walked = walker.walk(impl_->root.get(), {}); !walked) {
            return std::unexpected(walked.error());
        }
    } else {
        if (impl_->name.empty()) {
            return std::unexpected(Error{ErrorCode::unsafe_path, "input name is empty"});
        }
        const auto status = stat_fd(impl_->root.get());
        if (!status) {
            return std::unexpected(status.error());
        }
        const auto info = describe(*status);
        if (!info) {
            return std::unexpected(info.error());
        }
        if (auto added =
                walker.add({impl_->name, {}, false, info->size, info->last_write, info->id});
            !added) {
            return std::unexpected(added.error());
        }
    }
    auto &entries = walker.entries;
    std::ranges::sort(entries, archive_path_less, &InputEntry::path);
    std::vector<PathEntry> manifest;
    manifest.reserve(entries.size());
    for (const auto &entry : entries) {
        if (auto valid = validate_archive_path(entry.path); !valid) {
            auto error = valid.error();
            error.message = "input name cannot be stored (" + error.message + ")";
            return std::unexpected(with_path(std::move(error), entry.path));
        }
        manifest.push_back({entry.path, entry.directory});
    }
    if (auto valid = validate_manifest_paths(manifest); !valid) {
        return std::unexpected(valid.error());
    }
    return std::move(entries);
}

std::expected<InputFile, Error> InputTree::open_file(const InputEntry &entry) const {
    if (entry.directory) {
        return std::unexpected(Error{ErrorCode::invalid_argument, "entry is a directory"});
    }
    const auto finish = [&entry](Fd file) -> std::expected<InputFile, Error> {
        const auto status = stat_fd(file.get());
        if (!status) {
            return std::unexpected(with_path(status.error(), entry.path));
        }
        const auto info = describe(*status);
        if (!S_ISREG(status->st_mode) || !info || info->id != entry.id ||
            info->size != entry.size || info->last_write != entry.last_write) {
            return std::unexpected(with_path(
                {ErrorCode::io_error, "input changed after it was enumerated"}, entry.path));
        }
        auto impl = std::make_unique<InputFile::Impl>();
        impl->file = std::move(file);
        impl->path = entry.path;
        impl->size = entry.size;
        impl->last_write = entry.last_write;
        return InputFile{std::move(impl)};
    };
    const auto changed = [&entry] {
        return std::unexpected(
            with_path({ErrorCode::io_error, "input changed after it was enumerated"}, entry.path));
    };
    if (entry.components.empty()) {
        if (impl_->directory) {
            return std::unexpected(Error{ErrorCode::invalid_argument, "entry has no path"});
        }
        // Reads use pread with their own offset, so a duplicate descriptor is enough.
        Fd file{::fcntl(impl_->root.get(), F_DUPFD_CLOEXEC, 0)};
        if (!file.valid()) {
            return std::unexpected(posix_error("reopen input"));
        }
        return finish(std::move(file));
    }
    Fd parent;
    int current = impl_->root.get();
    for (std::size_t i = 0; i + 1 < entry.components.size(); ++i) {
        Fd next{::openat(current, entry.components[i].c_str(), directory_flags)};
        if (!next.valid()) {
            if (errno == ELOOP || errno == ENOTDIR || errno == ENOENT) {
                return changed();
            }
            return std::unexpected(with_path(posix_error("open input"), entry.path));
        }
        parent = std::move(next);
        current = parent.get();
    }
    Fd file{::openat(current, entry.components.back().c_str(), file_flags)};
    if (!file.valid()) {
        if (errno == ELOOP || errno == ENOENT) {
            return changed();
        }
        return std::unexpected(with_path(posix_error("open input"), entry.path));
    }
    return finish(std::move(file));
}

std::expected<std::size_t, Error> InputFile::read(std::span<std::byte> buffer) {
    std::size_t filled = 0;
    while (filled < buffer.size()) {
        const auto count = std::min(buffer.size() - filled, std::size_t{1024 * 1024});
        const auto got = ::pread(impl_->file.get(), buffer.data() + filled, count,
                                 static_cast<off_t>(impl_->consumed + filled));
        if (got < 0) {
            if (errno == EINTR) {
                continue;
            }
            return std::unexpected(with_path(posix_error("read input"), impl_->path));
        }
        if (got == 0) {
            break;
        }
        if (static_cast<std::size_t>(got) > count) {
            return std::unexpected(Error{ErrorCode::io_error, "read returned too many bytes"});
        }
        filled += static_cast<std::size_t>(got);
    }
    impl_->consumed += filled;
    if (impl_->consumed > impl_->size) {
        return std::unexpected(impl_->changed());
    }
    return filled;
}

std::expected<void, Error> InputFile::confirm_unchanged() {
    if (impl_->consumed != impl_->size) {
        return std::unexpected(impl_->changed());
    }
    std::byte extra{};
    ssize_t got = -1;
    do {
        got = ::pread(impl_->file.get(), &extra, 1, static_cast<off_t>(impl_->size));
    } while (got < 0 && errno == EINTR);
    if (got < 0) {
        return std::unexpected(with_path(posix_error("read input"), impl_->path));
    }
    const auto status = stat_fd(impl_->file.get());
    if (!status) {
        return std::unexpected(with_path(status.error(), impl_->path));
    }
    const auto info = describe(*status);
    if (got != 0 || !info || info->size != impl_->size || info->last_write != impl_->last_write) {
        return std::unexpected(impl_->changed());
    }
    return {};
}

InputTree::InputTree(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
InputTree::InputTree(InputTree &&) noexcept = default;
InputTree &InputTree::operator=(InputTree &&) noexcept = default;
InputTree::~InputTree() = default;

InputFile::InputFile(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
InputFile::InputFile(InputFile &&) noexcept = default;
InputFile &InputFile::operator=(InputFile &&) noexcept = default;
InputFile::~InputFile() = default;
} // namespace seedarc::experimental::internal
#endif
