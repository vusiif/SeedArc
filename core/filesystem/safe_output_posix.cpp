// SPDX-License-Identifier: GPL-3.0-or-later
// Linux implementation of SafeOutputDirectory / SafeOutputFile (ADR-0004). The Windows version
// and the shared pieces (fault hooks, budget check) are in safe_output.cpp.
#ifdef __linux__
#include "posix_common.hpp"
#include "safe_output_internal.hpp"
#include "seedarc/archive.hpp"
#include "seedarc/archive_path.hpp"
#include "seedarc/safe_output.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include <dirent.h>
#include <fcntl.h>
#include <stdio.h> // renameat2, RENAME_NOREPLACE (glibc)
#include <sys/random.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/xattr.h>
#include <unistd.h>

namespace seedarc::experimental {
namespace {
using internal::Fd;
using internal::object_id;
using internal::posix_error;

constexpr mode_t private_directory_mode = 0700;
constexpr mode_t private_file_mode = 0600;
constexpr unsigned long procfs_magic = 0x9FA0UL;
constexpr std::size_t temporary_name_units = 9 + 32 + 4; // ".seedarc-" + 32 hex + ".tmp"

Error closed_directory() { return {ErrorCode::io_error, "directory already discarded"}; }
Error closed_session() { return {ErrorCode::io_error, "output file session is closed"}; }

Error staged_changed(std::string what) {
    return {ErrorCode::integrity_error,
            "staged output changed before publication: " + std::move(what)};
}

// One validated basename, returned with its exact bytes (the path profile only admits ASCII off
// Windows, so the bytes and the validated components are the same text).
std::expected<std::string, Error> leaf(std::string_view name) {
    auto components = validate_archive_path(name);
    if (!components) {
        return std::unexpected(components.error());
    }
    if (components->size() != 1) {
        return std::unexpected(
            Error{ErrorCode::unsafe_path, "output operation requires one basename"});
    }
    return std::string{name};
}

std::expected<struct stat, Error> stat_fd(int fd) {
    struct stat status{};
    if (::fstat(fd, &status) != 0) {
        return std::unexpected(posix_error("read object metadata"));
    }
    return status;
}

// lstat of one child name; ENOENT is reported as nullopt-like through `missing`.
int stat_child(int directory, const std::string &name, struct stat &status) noexcept {
    return ::fstatat(directory, name.c_str(), &status, AT_SYMLINK_NOFOLLOW);
}

std::expected<void, Error> require_absent(int directory, const std::string &name) {
    struct stat status{};
    if (stat_child(directory, name, status) == 0) {
        return std::unexpected(Error{ErrorCode::output_conflict, "output name already exists"});
    }
    if (errno == ENOENT) {
        return {};
    }
    return std::unexpected(posix_error("probe output name"));
}

std::expected<std::string, Error> temporary_name() {
    std::array<unsigned char, 16> random{};
    std::size_t filled = 0;
    while (filled < random.size()) {
        const auto got = ::getrandom(random.data() + filled, random.size() - filled, 0);
        if (got < 0) {
            if (errno == EINTR) {
                continue;
            }
            return std::unexpected(posix_error("temporary name randomness"));
        }
        filled += static_cast<std::size_t>(got);
    }
    std::string name = ".seedarc-";
    constexpr char digits[] = "0123456789abcdef";
    for (const auto byte : random) {
        name += digits[byte >> 4U];
        name += digits[byte & 15U];
    }
    return name + ".tmp";
}

// Rejects anything but a directory / regular file on the pinned device.
std::expected<void, Error> require_kind(const struct stat &status, dev_t device, bool directory) {
    if ((directory ? !S_ISDIR(status.st_mode) : !S_ISREG(status.st_mode))) {
        return std::unexpected(
            Error{ErrorCode::unsafe_path, "symbolic link or unexpected output object type"});
    }
    if (status.st_dev != device) {
        return std::unexpected(
            Error{ErrorCode::unsafe_path, "output object is not on the pinned file system"});
    }
    return {};
}

// The directory in which this library creates and renames names must not let another user
// rename, remove or replace those names (ADR-0004 section 3): owned by us or root, and not
// writable by group / others unless the sticky bit is set.
std::expected<void, Error> require_protected_parent(const struct stat &status) {
    const bool trusted_owner = status.st_uid == ::geteuid() || status.st_uid == 0;
    const bool shared_write = (status.st_mode & (S_IWGRP | S_IWOTH)) != 0;
    const bool sticky = (status.st_mode & S_ISVTX) != 0;
    if (!trusted_owner || (shared_write && !sticky)) {
        return std::unexpected(Error{ErrorCode::unsupported_feature,
                                     "output directory can be modified by other users (owned by "
                                     "another user, or group/other-writable without the sticky "
                                     "bit)"});
    }
    return {};
}

// A staged object must carry no user extended attributes (the counterpart of NTFS named streams;
// security.* / system.* are maintained by the system and allowed).
std::expected<void, Error> require_no_user_xattrs(int fd) {
    std::vector<char> names(4096);
    for (;;) {
        const auto size = ::flistxattr(fd, names.data(), names.size());
        if (size >= 0) {
            names.resize(static_cast<std::size_t>(size));
            break;
        }
        if (errno == ENOTSUP) {
            return {};
        }
        if (errno != ERANGE || names.size() >= 65536) {
            return std::unexpected(posix_error("list extended attributes"));
        }
        names.resize(names.size() * 4);
    }
    for (std::size_t begin = 0; begin < names.size();) {
        const auto end = std::find(names.begin() + static_cast<std::ptrdiff_t>(begin), names.end(),
                                   '\0') -
                         names.begin();
        const std::string_view name{names.data() + begin, static_cast<std::size_t>(end) - begin};
        if (name.starts_with("user.")) {
            return std::unexpected(staged_changed("unexpected extended attribute"));
        }
        begin = static_cast<std::size_t>(end) + 1;
    }
    return {};
}

// Names in a directory, read through a private duplicate of its descriptor.
std::expected<std::vector<std::string>, Error> list_names(int directory) {
    const int duplicate = ::openat(directory, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (duplicate < 0) {
        return std::unexpected(posix_error("list directory"));
    }
    DIR *stream = ::fdopendir(duplicate);
    if (stream == nullptr) {
        const auto code = errno;
        ::close(duplicate);
        return std::unexpected(posix_error("list directory", code));
    }
    std::vector<std::string> names;
    for (;;) {
        errno = 0;
        const dirent *entry = ::readdir(stream);
        if (entry == nullptr) {
            const auto code = errno;
            ::closedir(stream);
            if (code != 0) {
                return std::unexpected(posix_error("list directory", code));
            }
            return names;
        }
        const std::string_view name{entry->d_name};
        if (name != "." && name != "..") {
            if (names.size() >= 1'000'000) {
                ::closedir(stream);
                return std::unexpected(
                    Error{ErrorCode::resource_limit, "directory has too many entries"});
            }
            names.emplace_back(name);
        }
    }
}

// Opens a directory this call has just created with mkdirat and checks it is ours and empty.
std::expected<Fd, Error> open_new_directory(int parent, const std::string &name, dev_t device,
                                            bool private_tree) {
    Fd fd{::openat(parent, name.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)};
    if (!fd.valid()) {
        return std::unexpected(posix_error("open new directory"));
    }
    const auto status = stat_fd(fd.get());
    if (!status) {
        return std::unexpected(status.error());
    }
    if (auto kind = require_kind(*status, device, true); !kind) {
        return std::unexpected(kind.error());
    }
    const auto children = list_names(fd.get());
    if (!children) {
        return std::unexpected(children.error());
    }
    if (status->st_uid != ::geteuid() || !children->empty()) {
        return std::unexpected(
            Error{ErrorCode::unsafe_path, "new directory was replaced by another object"});
    }
    if (private_tree && ::fchmod(fd.get(), private_directory_mode) != 0) {
        return std::unexpected(posix_error("restrict new directory"));
    }
    return fd;
}

// Publishes an O_TMPFILE without replacement. AT_EMPTY_PATH needs CAP_DAC_READ_SEARCH before
// Linux 6.10; the fallback links through procfs (verified to be procfs, not a mounted lookalike).
int link_temporary(int file, int directory, const std::string &name, bool force_proc) {
    if (!force_proc) {
        if (::linkat(file, "", directory, name.c_str(), AT_EMPTY_PATH) == 0) {
            return 0;
        }
        if (errno != ENOENT && errno != EPERM) {
            return -1;
        }
    }
    const Fd proc{::open("/proc/self/fd", O_RDONLY | O_DIRECTORY | O_CLOEXEC)};
    struct statfs info{};
    if (!proc.valid() || ::fstatfs(proc.get(), &info) != 0 ||
        static_cast<unsigned long>(info.f_type) != procfs_magic) {
        errno = ENOTSUP;
        return -1;
    }
    const auto self = std::to_string(file);
    return ::linkat(proc.get(), self.c_str(), directory, name.c_str(), AT_SYMLINK_FOLLOW);
}
} // namespace

struct SafeOutputDirectory::Impl {
    Fd fd; // O_RDONLY | O_DIRECTORY
    std::shared_ptr<Impl> parent;
    dev_t device = 0;
    std::size_t depth = 0;      // components below "/"
    std::size_t path_units = 0; // bytes of the equivalent "/a/b" path
    bool created = false;
    bool discarded = false;
    bool private_tree = false; // a staging directory or below it
    std::string name;          // current name in the parent (created directories)
    std::string staged_name;   // final name of a staging directory until commit_staged()
    ObjectId id;

    [[nodiscard]] std::expected<void, Error> admit(std::size_t units, bool directory) const {
        return internal::admit_output_component(depth, path_units, units, directory);
    }

    [[nodiscard]] std::expected<std::shared_ptr<Impl>, Error>
    child(Fd child_fd, std::string child_name, std::size_t units, bool own) const {
        const auto status = stat_fd(child_fd.get());
        if (!status) {
            return std::unexpected(status.error());
        }
        auto result = std::make_shared<Impl>();
        result->fd = std::move(child_fd);
        result->device = device;
        result->depth = depth + 1;
        result->path_units = path_units + 1 + units;
        result->created = own;
        result->private_tree = private_tree;
        result->name = std::move(child_name);
        result->id = object_id(*status);
        return result;
    }
};

struct SafeOutputFile::Impl {
    std::shared_ptr<SafeOutputDirectory::Impl> directory; // keeps the parent open
    Fd file;                                              // O_TMPFILE until commit
    std::string final_name;
    ObjectId id;
    Crc32c crc;
    std::uint64_t written = 0;
    bool finished = false;
    std::vector<std::byte> prefix;
    bool prefix_written = true;

    // The temporary has no name, so it disappears when the descriptor closes. The explicit step
    // releases its space right away; if that fails the error says so.
    std::unexpected<Error> fail(Error error) {
        if (!finished) {
            finished = true;
            const auto *faults = internal::active_write_faults();
            const bool cleaned = faults != nullptr && faults->fail_explicit_cleanup
                                     ? false
                                     : ::ftruncate(file.get(), 0) == 0;
            if (!cleaned) {
                error.message += "; explicit cleanup failed; the unnamed O_TMPFILE temporary is "
                                 "released when its descriptor closes";
            }
        }
        return std::unexpected(std::move(error));
    }

    // Writes at `offset` (short writes are retried). `append` counts the bytes in `written` and,
    // with `checksum`, in the running CRC32C.
    std::expected<void, Error> write_all(std::span<const std::byte> bytes, std::uint64_t offset,
                                         bool append, bool checksum) {
        const auto *faults = internal::active_write_faults();
        std::size_t done_total = 0;
        while (done_total < bytes.size()) {
            auto count = std::min(bytes.size() - done_total, std::size_t{65536});
            if (faults != nullptr && faults->max_bytes_per_write != 0) {
                count = std::min(count, faults->max_bytes_per_write);
            }
            ssize_t done = -1;
            if (faults != nullptr && written >= faults->fail_write_at) {
                errno = ENOSPC;
            } else {
                done = ::pwrite(file.get(), bytes.data() + done_total, count,
                                static_cast<off_t>(offset + done_total));
            }
            if (done < 0 && errno == EINTR) {
                continue;
            }
            if (done <= 0 || static_cast<std::size_t>(done) > count) {
                return fail(posix_error("write temporary file", done < 0 ? errno : EIO));
            }
            const auto piece = static_cast<std::size_t>(done);
            if (checksum) {
                crc.update(bytes.subspan(done_total, piece));
            }
            done_total += piece;
            if (append) {
                written += piece;
                if (faults != nullptr && faults->after_write) {
                    faults->after_write(written);
                }
            }
        }
        return {};
    }
};

SafeOutputDirectory::SafeOutputDirectory(std::shared_ptr<Impl> impl) : impl_(std::move(impl)) {}

std::expected<SafeOutputDirectory, Error>
SafeOutputDirectory::open_existing(const std::filesystem::path &path) {
    const auto &native = path.native();
    if (native.empty() || native.front() != '/' || native.size() > 4096 ||
        native.find('\0') != std::string::npos) {
        return std::unexpected(
            Error{ErrorCode::unsafe_path, "requires an absolute local directory"});
    }
    Fd current{::open("/", O_PATH | O_DIRECTORY | O_CLOEXEC)};
    if (!current.valid()) {
        return std::unexpected(posix_error("open file system root"));
    }
    std::vector<std::string> components;
    for (std::size_t begin = 1; begin < native.size();) {
        const auto end = native.find('/', begin);
        const auto component =
            native.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
        const bool trailing = end == native.size() - 1;
        if (component.empty() || component == "." || component == ".." ||
            component.size() > 255 || components.size() + 1 > 64) {
            return std::unexpected(Error{ErrorCode::unsafe_path, "unsafe output root component"});
        }
        components.push_back(component);
        if (end == std::string::npos || trailing) {
            break;
        }
        begin = end + 1;
    }
    std::size_t depth = 0;
    std::size_t units = 0;
    for (std::size_t i = 0; i < components.size(); ++i) {
        const auto &component = components[i];
        if (auto admitted =
                internal::admit_output_component(depth, units, component.size(), true);
            !admitted) {
            return std::unexpected(admitted.error());
        }
        const bool last = i + 1 == components.size();
        // Intermediate directories need only search permission; the target is opened for reading
        // so that it can be listed and synced. O_NOFOLLOW: a symbolic link anywhere fails.
        Fd next{::openat(current.get(), component.c_str(),
                         (last ? O_RDONLY : O_PATH) | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)};
        if (!next.valid()) {
            const auto code = errno;
            if (code == ELOOP || code == ENOTDIR) {
                return std::unexpected(Error{ErrorCode::unsafe_path,
                                             "output root contains a symbolic link or a "
                                             "non-directory component"});
            }
            return std::unexpected(posix_error("open output root component", code));
        }
        const auto status = stat_fd(next.get());
        if (!status) {
            return std::unexpected(status.error());
        }
        if (!S_ISDIR(status->st_mode)) {
            return std::unexpected(Error{ErrorCode::unsafe_path,
                                         "output root contains a symbolic link or a "
                                         "non-directory component"});
        }
        current = std::move(next);
        ++depth;
        units += 1 + component.size();
    }
    if (components.empty()) {
        current = Fd{::open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC)};
        if (!current.valid()) {
            return std::unexpected(posix_error("open file system root"));
        }
    }
    const auto status = stat_fd(current.get());
    if (!status) {
        return std::unexpected(status.error());
    }
    if (auto supported = internal::require_supported_filesystem(current.get()); !supported) {
        return std::unexpected(supported.error());
    }
    if (auto protected_parent = require_protected_parent(*status); !protected_parent) {
        return std::unexpected(protected_parent.error());
    }
    auto impl = std::make_shared<Impl>();
    impl->fd = std::move(current);
    impl->device = status->st_dev;
    impl->depth = depth;
    impl->path_units = units;
    impl->id = object_id(*status);
    return SafeOutputDirectory{std::move(impl)};
}

std::expected<SafeOutputDirectory, Error>
SafeOutputDirectory::create_child(std::string_view basename) const {
    if (!impl_ || impl_->discarded) {
        return std::unexpected(closed_directory());
    }
    auto name = leaf(basename);
    if (!name) {
        return std::unexpected(name.error());
    }
    if (auto admitted = impl_->admit(name->size(), true); !admitted) {
        return std::unexpected(admitted.error());
    }
    if (::mkdirat(impl_->fd.get(), name->c_str(),
                  impl_->private_tree ? private_directory_mode : 0777) != 0) {
        return std::unexpected(posix_error("create directory"));
    }
    auto fd = open_new_directory(impl_->fd.get(), *name, impl_->device, impl_->private_tree);
    if (!fd) {
        return std::unexpected(fd.error());
    }
    const auto units = name->size();
    auto child = impl_->child(std::move(*fd), std::move(*name), units, true);
    if (!child) {
        return std::unexpected(child.error());
    }
    (*child)->parent = impl_;
    return SafeOutputDirectory{std::move(*child)};
}

std::expected<void, Error> SafeOutputDirectory::write_new_file(std::string_view basename,
                                                               std::span<const std::byte> contents,
                                                               std::uint32_t expected_crc32c,
                                                               std::stop_token stop) const {
    if (contents.size() > 4 * 1024 * 1024) {
        return std::unexpected(
            Error{ErrorCode::resource_limit, "buffered write is limited to 4 MiB"});
    }
    if (stop.stop_requested()) {
        return std::unexpected(Error{ErrorCode::cancelled, "write cancelled before creation"});
    }
    auto file = begin_file(basename);
    if (!file) {
        return std::unexpected(file.error());
    }
    constexpr std::size_t piece = 65536;
    for (std::size_t offset = 0; offset < contents.size(); offset += piece) {
        if (stop.stop_requested()) {
            return file->impl_->fail({ErrorCode::cancelled, "write cancelled"});
        }
        const auto count = std::min(piece, contents.size() - offset);
        if (auto appended = file->append(contents.subspan(offset, count)); !appended) {
            return appended;
        }
    }
    if (file->checksum() != expected_crc32c) {
        return file->impl_->fail({ErrorCode::integrity_error, "content CRC32C mismatch"});
    }
    return file->commit(contents.size(), std::move(stop));
}

std::expected<SafeOutputFile, Error>
SafeOutputDirectory::begin_file(std::string_view basename) const {
    if (!impl_ || impl_->discarded) {
        return std::unexpected(closed_directory());
    }
    auto name = leaf(basename);
    if (!name) {
        return std::unexpected(name.error());
    }
    if (auto admitted = impl_->admit(std::max(name->size(), temporary_name_units), false);
        !admitted) {
        return std::unexpected(admitted.error());
    }
    if (auto absent = require_absent(impl_->fd.get(), *name); !absent) {
        return std::unexpected(absent.error());
    }
    // No O_EXCL: that would forbid the later linkat(). The file has no name until commit.
    Fd file{::openat(impl_->fd.get(), ".", O_TMPFILE | O_RDWR | O_CLOEXEC,
                     impl_->private_tree ? private_file_mode : 0666)};
    if (!file.valid()) {
        const auto code = errno;
        if (code == EOPNOTSUPP || code == EISDIR || code == EINVAL) {
            return std::unexpected(Error{ErrorCode::unsupported_feature,
                                         "file system does not support O_TMPFILE"});
        }
        return std::unexpected(posix_error("create temporary file", code));
    }
    const auto status = stat_fd(file.get());
    if (!status) {
        return std::unexpected(status.error());
    }
    if (auto kind = require_kind(*status, impl_->device, false); !kind) {
        return std::unexpected(kind.error());
    }
    if (impl_->private_tree && ::fchmod(file.get(), private_file_mode) != 0) {
        return std::unexpected(posix_error("restrict temporary file"));
    }
    auto session = std::make_unique<SafeOutputFile::Impl>();
    session->directory = impl_;
    session->file = std::move(file);
    session->final_name = std::move(*name);
    session->id = object_id(*status);
    return SafeOutputFile{std::move(session)};
}

std::expected<SafeOutputDirectory, Error>
SafeOutputDirectory::create_staging_child(std::string_view final_basename) const {
    if (!impl_ || impl_->discarded) {
        return std::unexpected(closed_directory());
    }
    auto name = leaf(final_basename);
    if (!name) {
        return std::unexpected(name.error());
    }
    const auto units = std::max(name->size(), temporary_name_units);
    if (auto admitted = impl_->admit(units, true); !admitted) {
        return std::unexpected(admitted.error());
    }
    if (auto absent = require_absent(impl_->fd.get(), *name); !absent) {
        return std::unexpected(absent.error());
    }
    auto temp_name = temporary_name();
    if (!temp_name) {
        return std::unexpected(temp_name.error());
    }
    if (::mkdirat(impl_->fd.get(), temp_name->c_str(), private_directory_mode) != 0) {
        return std::unexpected(posix_error("create staging directory"));
    }
    auto fd = open_new_directory(impl_->fd.get(), *temp_name, impl_->device, true);
    if (!fd) {
        return std::unexpected(fd.error());
    }
    auto staged = impl_->child(std::move(*fd), std::move(*temp_name), units, true);
    if (!staged) {
        return std::unexpected(staged.error());
    }
    (*staged)->parent = impl_;
    (*staged)->private_tree = true;
    (*staged)->staged_name = std::move(*name);
    return SafeOutputDirectory{std::move(*staged)};
}

std::expected<void, Error> SafeOutputDirectory::commit_staged() {
    if (!impl_ || impl_->discarded || impl_->staged_name.empty() || !impl_->parent) {
        return std::unexpected(
            Error{ErrorCode::invalid_argument, "not an uncommitted staging directory"});
    }
    const int parent = impl_->parent->fd.get();
    const auto names_us = [this, parent](const std::string &name) {
        struct stat status{};
        return stat_child(parent, name, status) == 0 && S_ISDIR(status.st_mode) &&
               object_id(status) == impl_->id;
    };
    if (!names_us(impl_->name)) {
        return std::unexpected(staged_changed("staging directory was moved or replaced"));
    }
    if (::renameat2(parent, impl_->name.c_str(), parent, impl_->staged_name.c_str(),
                    RENAME_NOREPLACE) != 0) {
        const auto code = errno;
        if (code == EINVAL || code == ENOSYS) {
            return std::unexpected(Error{ErrorCode::unsupported_feature,
                                         "file system does not support RENAME_NOREPLACE"});
        }
        return std::unexpected(posix_error("commit without replacement", code));
    }
    if (!names_us(impl_->staged_name)) {
        return std::unexpected(
            staged_changed("a different object was published under the target name"));
    }
    impl_->name = std::move(impl_->staged_name);
    impl_->staged_name.clear();
    (void)::fsync(parent); // best effort, as on Windows (no directory flush there)
    return {};
}

std::expected<ObjectId, Error> SafeOutputDirectory::identity() const {
    if (!impl_ || impl_->discarded) {
        return std::unexpected(closed_directory());
    }
    const auto status = stat_fd(impl_->fd.get());
    if (!status) {
        return std::unexpected(status.error());
    }
    return object_id(*status);
}

std::expected<void, Error>
SafeOutputDirectory::require_exact_children(std::span<const std::string> basenames) const {
    if (!impl_ || impl_->discarded) {
        return std::unexpected(closed_directory());
    }
    std::vector<std::string> expected;
    expected.reserve(basenames.size());
    for (const auto &basename : basenames) {
        auto name = leaf(basename);
        if (!name) {
            return std::unexpected(name.error());
        }
        expected.push_back(std::move(*name));
    }
    std::ranges::sort(expected);
    if (std::ranges::adjacent_find(expected) != expected.end()) {
        return std::unexpected(Error{ErrorCode::invalid_argument, "duplicate expected child"});
    }
    const auto status = stat_fd(impl_->fd.get());
    if (!status) {
        return std::unexpected(status.error());
    }
    if (impl_->private_tree &&
        (status->st_uid != ::geteuid() || (status->st_mode & 07777) != private_directory_mode)) {
        return std::unexpected(staged_changed("directory owner or permissions"));
    }
    if (auto attributes = require_no_user_xattrs(impl_->fd.get()); !attributes) {
        return attributes;
    }
    const auto names = list_names(impl_->fd.get());
    if (!names) {
        return std::unexpected(names.error());
    }
    for (const auto &name : *names) {
        // Exact bytes: a name that differs only in case is still a foreign object.
        if (!std::ranges::binary_search(expected, name)) {
            return std::unexpected(staged_changed("unexpected entry"));
        }
    }
    if (names->size() != expected.size()) {
        return std::unexpected(staged_changed("entry missing"));
    }
    return {};
}

std::expected<void, Error> SafeOutputDirectory::verify_owned_file(std::string_view basename,
                                                                  const ObjectId &expected,
                                                                  std::uint64_t size,
                                                                  std::uint32_t crc32c,
                                                                  std::stop_token stop) const {
    if (!impl_ || impl_->discarded) {
        return std::unexpected(closed_directory());
    }
    auto name = leaf(basename);
    if (!name) {
        return std::unexpected(name.error());
    }
    // O_NONBLOCK: a FIFO substituted for the file must not block the open.
    const Fd file{::openat(impl_->fd.get(), name->c_str(),
                           O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_NOCTTY | O_CLOEXEC)};
    if (!file.valid()) {
        if (errno == ELOOP) {
            return std::unexpected(staged_changed("file replaced by a symbolic link"));
        }
        return std::unexpected(posix_error("open staged file for verification"));
    }
    const auto status = stat_fd(file.get());
    if (!status) {
        return std::unexpected(status.error());
    }
    if (auto kind = require_kind(*status, impl_->device, false); !kind) {
        return kind;
    }
    if (object_id(*status) != expected) {
        return std::unexpected(staged_changed("file replaced"));
    }
    if (status->st_nlink != 1 || status->st_size < 0 ||
        static_cast<std::uint64_t>(status->st_size) != size) {
        return std::unexpected(staged_changed("file size or links"));
    }
    if (impl_->private_tree &&
        (status->st_uid != ::geteuid() || (status->st_mode & 07777) != private_file_mode)) {
        return std::unexpected(staged_changed("file owner or permissions"));
    }
    if (auto attributes = require_no_user_xattrs(file.get()); !attributes) {
        return attributes;
    }
    Crc32c crc;
    std::vector<std::byte> buffer(static_cast<std::size_t>(std::min(size, std::uint64_t{65536})));
    std::uint64_t position = 0;
    while (position < size) {
        if (stop.stop_requested()) {
            return std::unexpected(Error{ErrorCode::cancelled, "operation cancelled"});
        }
        const auto count = static_cast<std::size_t>(std::min(size - position,
                                                             std::uint64_t{buffer.size()}));
        const auto got = ::pread(file.get(), buffer.data(), count, static_cast<off_t>(position));
        if (got < 0 && errno == EINTR) {
            continue;
        }
        if (got <= 0 || static_cast<std::size_t>(got) > count) {
            return std::unexpected(posix_error("re-read staged file", got < 0 ? errno : EIO));
        }
        crc.update(std::span{buffer}.first(static_cast<std::size_t>(got)));
        position += static_cast<std::uint64_t>(got);
    }
    if (crc.value() != crc32c) {
        return std::unexpected(staged_changed("file content"));
    }
    return {};
}

std::expected<SafeOutputDirectory, Error>
SafeOutputDirectory::open_owned_child(std::string_view basename, const ObjectId &expected) const {
    if (!impl_ || impl_->discarded) {
        return std::unexpected(closed_directory());
    }
    auto name = leaf(basename);
    if (!name) {
        return std::unexpected(name.error());
    }
    if (auto admitted = impl_->admit(name->size(), true); !admitted) {
        return std::unexpected(admitted.error());
    }
    Fd fd{::openat(impl_->fd.get(), name->c_str(),
                   O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)};
    if (!fd.valid()) {
        if (errno == ELOOP || errno == ENOTDIR) {
            return std::unexpected(
                Error{ErrorCode::unsafe_path, "output directory was replaced by another object"});
        }
        return std::unexpected(posix_error("open owned directory"));
    }
    const auto status = stat_fd(fd.get());
    if (!status) {
        return std::unexpected(status.error());
    }
    if (auto kind = require_kind(*status, impl_->device, true); !kind) {
        return std::unexpected(kind.error());
    }
    if (object_id(*status) != expected) {
        return std::unexpected(
            Error{ErrorCode::unsafe_path, "output directory was replaced by another object"});
    }
    const auto units = name->size();
    auto child = impl_->child(std::move(fd), std::move(*name), units, false);
    if (!child) {
        return std::unexpected(child.error());
    }
    (*child)->parent = impl_;
    return SafeOutputDirectory{std::move(*child)};
}

std::expected<void, Error> SafeOutputDirectory::remove_owned_child(std::string_view basename,
                                                                   const ObjectId &expected,
                                                                   bool directory) const {
    if (!impl_ || impl_->discarded) {
        return std::unexpected(closed_directory());
    }
    auto name = leaf(basename);
    if (!name) {
        return std::unexpected(name.error());
    }
    struct stat status{};
    if (stat_child(impl_->fd.get(), *name, status) != 0) {
        return std::unexpected(posix_error("inspect object for removal"));
    }
    if (auto kind = require_kind(status, impl_->device, directory); !kind) {
        return kind;
    }
    if (object_id(status) != expected) {
        return std::unexpected(
            Error{ErrorCode::unsafe_path, "refusing to remove an object this run did not create"});
    }
    if (::unlinkat(impl_->fd.get(), name->c_str(), directory ? AT_REMOVEDIR : 0) != 0) {
        return std::unexpected(posix_error("remove owned object"));
    }
    return {};
}

std::expected<void, Error> SafeOutputDirectory::discard_empty() {
    if (!impl_ || !impl_->created || impl_->discarded || !impl_->parent) {
        return std::unexpected(
            Error{ErrorCode::unsafe_path, "cannot discard an existing or discarded directory"});
    }
    const int parent = impl_->parent->fd.get();
    struct stat status{};
    if (stat_child(parent, impl_->name, status) != 0) {
        return std::unexpected(posix_error("inspect directory for removal"));
    }
    if (!S_ISDIR(status.st_mode) || object_id(status) != impl_->id) {
        return std::unexpected(
            Error{ErrorCode::unsafe_path, "refusing to remove an object this run did not create"});
    }
    if (::unlinkat(parent, impl_->name.c_str(), AT_REMOVEDIR) != 0) {
        return std::unexpected(posix_error("remove empty directory"));
    }
    impl_->discarded = true;
    impl_->fd = Fd{};
    return {};
}

SafeOutputFile::SafeOutputFile(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
SafeOutputFile::SafeOutputFile(SafeOutputFile &&) noexcept = default;
SafeOutputFile &SafeOutputFile::operator=(SafeOutputFile &&) noexcept = default;
SafeOutputFile::~SafeOutputFile() = default;

const ObjectId &SafeOutputFile::identity() const noexcept { return impl_->id; }
std::uint64_t SafeOutputFile::bytes_written() const noexcept { return impl_->written; }
std::uint32_t SafeOutputFile::checksum() const noexcept { return impl_->crc.value(); }

std::expected<void, Error> SafeOutputFile::append(std::span<const std::byte> bytes) {
    if (!impl_ || impl_->finished) {
        return std::unexpected(closed_session());
    }
    return impl_->write_all(bytes, impl_->written, true, true);
}

std::expected<void, Error> SafeOutputFile::reserve_prefix(std::size_t size) {
    if (!impl_ || impl_->finished) {
        return std::unexpected(closed_session());
    }
    if (impl_->written != 0 || !impl_->prefix.empty() || size == 0 || size > 4096) {
        return std::unexpected(Error{ErrorCode::invalid_argument, "invalid prefix reservation"});
    }
    impl_->prefix.assign(size, std::byte{0});
    impl_->prefix_written = false;
    return impl_->write_all(impl_->prefix, 0, true, false);
}

std::expected<void, Error> SafeOutputFile::write_prefix(std::span<const std::byte> bytes) {
    if (!impl_ || impl_->finished) {
        return std::unexpected(closed_session());
    }
    if (impl_->prefix.empty() || bytes.size() != impl_->prefix.size()) {
        return std::unexpected(Error{ErrorCode::invalid_argument, "prefix size mismatch"});
    }
    if (auto rewritten = impl_->write_all(bytes, 0, false, false); !rewritten) {
        return rewritten;
    }
    impl_->prefix.assign(bytes.begin(), bytes.end());
    impl_->prefix_written = true;
    return {};
}

std::expected<void, Error> SafeOutputFile::commit(std::uint64_t expected_size,
                                                  std::stop_token stop) {
    if (!impl_ || impl_->finished) {
        return std::unexpected(closed_session());
    }
    const auto *faults = internal::active_write_faults();
    const int file = impl_->file.get();
    if (impl_->written != expected_size) {
        return impl_->fail({ErrorCode::integrity_error, "output size differs from expected size"});
    }
    if (!impl_->prefix_written) {
        return impl_->fail({ErrorCode::invalid_argument, "reserved prefix was never written"});
    }
    if (faults != nullptr && faults->fail_flush) {
        return impl_->fail(posix_error("flush temporary file", EIO));
    }
    if (::fsync(file) != 0) {
        return impl_->fail(posix_error("flush temporary file"));
    }
    const auto status = stat_fd(file);
    if (!status || status->st_size < 0 ||
        static_cast<std::uint64_t>(status->st_size) != impl_->written) {
        return impl_->fail(Error{ErrorCode::io_error, "verify temporary file size"});
    }
    // Re-read what reached the file system: the reserved prefix byte for byte, everything after
    // it against the CRC32C of what was appended.
    Crc32c read_back;
    bool prefix_matches = true;
    std::uint64_t position = 0;
    std::vector<std::byte> buffer(
        static_cast<std::size_t>(std::min(impl_->written, std::uint64_t{65536})));
    while (position < impl_->written) {
        const auto count = static_cast<std::size_t>(
            std::min(impl_->written - position, std::uint64_t{buffer.size()}));
        const auto got = ::pread(file, buffer.data(), count, static_cast<off_t>(position));
        if (got < 0 && errno == EINTR) {
            continue;
        }
        if (got <= 0 || static_cast<std::size_t>(got) > count) {
            return impl_->fail(posix_error("read back temporary file", got < 0 ? errno : EIO));
        }
        if (faults != nullptr && faults->corrupt_read_back) {
            buffer[0] ^= std::byte{1};
        }
        auto chunk = std::span{buffer}.first(static_cast<std::size_t>(got));
        if (position < impl_->prefix.size()) {
            const auto in_prefix = static_cast<std::size_t>(
                std::min(std::uint64_t{chunk.size()}, impl_->prefix.size() - position));
            prefix_matches = prefix_matches &&
                             std::ranges::equal(chunk.first(in_prefix),
                                                std::span{impl_->prefix}.subspan(
                                                    static_cast<std::size_t>(position), in_prefix));
            chunk = chunk.subspan(in_prefix);
        }
        read_back.update(chunk);
        position += static_cast<std::uint64_t>(got);
    }
    if (!prefix_matches || read_back.value() != impl_->crc.value()) {
        return impl_->fail({ErrorCode::integrity_error, "temporary file byte verification failed"});
    }
    if (stop.stop_requested()) {
        return impl_->fail({ErrorCode::cancelled, "write cancelled before commit"});
    }
    if (faults != nullptr && faults->fail_commit) {
        return impl_->fail(posix_error("commit without replacement", ENOSPC));
    }
    const int directory = impl_->directory->fd.get();
    if (link_temporary(file, directory, impl_->final_name,
                       faults != nullptr && faults->force_proc_link) != 0) {
        const auto code = errno;
        if (code == ENOENT || code == EPERM || code == ENOTSUP) {
            return impl_->fail(Error{ErrorCode::unsupported_feature,
                                     "cannot publish an O_TMPFILE here (linkat with "
                                     "AT_EMPTY_PATH and /proc/self/fd both refused)"});
        }
        return impl_->fail(posix_error("commit without replacement", code));
    }
    // The name now refers to our inode; it must be its only link.
    const auto linked = stat_fd(file);
    if (!linked || linked->st_nlink != 1) {
        impl_->finished = true;
        return std::unexpected(Error{ErrorCode::integrity_error,
                                     "published file has an unexpected link count"});
    }
    impl_->finished = true;
    (void)::fsync(directory); // best effort, as on Windows (no directory flush there)
    return {};
}
} // namespace seedarc::experimental
#endif
