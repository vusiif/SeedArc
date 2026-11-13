// SPDX-License-Identifier: GPL-3.0-or-later
#include "input_tree.hpp"

#include "seedarc/archive_index.hpp"
#include "seedarc/archive_path.hpp"
#include "win_common.hpp"

#include <algorithm>
#include <cstring>
#include <utility>

// The Linux implementation is in input_tree_posix.cpp; other platforms get the stubs below.
#if defined(_WIN32) || !defined(__linux__)
namespace seedarc::experimental::internal {
#ifdef _WIN32
namespace {
constexpr ULONG object_flags = OBJ_CASE_INSENSITIVE | OBJ_DONT_REPARSE;
constexpr ACCESS_MASK directory_access =
    FILE_LIST_DIRECTORY | FILE_TRAVERSE | FILE_READ_ATTRIBUTES | SYNCHRONIZE;
constexpr ACCESS_MASK file_access = FILE_READ_DATA | FILE_READ_ATTRIBUTES | SYNCHRONIZE;
// Directories stay listable and writable by others but cannot be renamed or deleted while held.
constexpr ULONG directory_share = FILE_SHARE_READ | FILE_SHARE_WRITE;
constexpr std::size_t max_tree_depth = 64; // same as the archive path component limit

Error with_path(Error error, std::string_view path) {
    error.message += ": ";
    error.message += path;
    return error;
}

Error reparse_rejected(std::string_view path) {
    return with_path({ErrorCode::unsupported_feature,
                      "input contains a symbolic link, junction or other reparse point, which is "
                      "rejected rather than followed"},
                     path);
}

struct ObjectInfo {
    DWORD attributes = 0;
    std::uint64_t size = 0;
    std::int64_t last_write = 0;
    ObjectId id;
};

std::expected<ObjectInfo, Error> describe(HANDLE handle) {
    FILE_BASIC_INFO basic{};
    FILE_STANDARD_INFO standard{};
    if (!GetFileInformationByHandleEx(handle, FileBasicInfo, &basic, sizeof(basic)) ||
        !GetFileInformationByHandleEx(handle, FileStandardInfo, &standard, sizeof(standard))) {
        return std::unexpected(win_error("read input metadata"));
    }
    const auto id = object_id(handle);
    if (!id) {
        return std::unexpected(id.error());
    }
    if (standard.EndOfFile.QuadPart < 0) {
        return std::unexpected(Error{ErrorCode::io_error, "input reports a negative size"});
    }
    return ObjectInfo{basic.FileAttributes, static_cast<std::uint64_t>(standard.EndOfFile.QuadPart),
                      basic.LastWriteTime.QuadPart, *id};
}

std::expected<Handle, Error> open_child(HANDLE parent, const std::wstring &name, ACCESS_MASK access,
                                        bool directory, std::string_view path) {
    HANDLE raw = INVALID_HANDLE_VALUE;
    const auto status =
        nt_relative(parent, name, access, directory ? directory_share : FILE_SHARE_READ, FILE_OPEN,
                    FILE_SYNCHRONOUS_IO_NONALERT | FILE_OPEN_REPARSE_POINT |
                        (directory ? FILE_DIRECTORY_FILE : FILE_NON_DIRECTORY_FILE),
                    0, object_flags, raw);
    if (status < 0) {
        return std::unexpected(with_path(nt_error("open input", status), path));
    }
    return Handle{raw};
}

std::expected<std::string, Error> to_utf8(std::wstring_view name) {
    const auto invalid = [&name] {
        return std::unexpected(
            Error{ErrorCode::unsafe_path,
                  "input name is empty or not valid UTF-16: " + ascii_for_message(name)});
    };
    if (name.empty() || name.size() > 32767) {
        return invalid();
    }
    const auto length = static_cast<int>(name.size());
    const int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, name.data(), length,
                                          nullptr, 0, nullptr, nullptr);
    if (count <= 0) {
        return invalid();
    }
    std::string text(static_cast<std::size_t>(count), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, name.data(), length, text.data(), count,
                            nullptr, nullptr) != count) {
        return invalid();
    }
    return text;
}

struct Listed {
    std::wstring name;
    DWORD attributes = 0;
};

// Names of one directory through its handle. At most `max_names` are accepted.
std::expected<std::vector<Listed>, Error> list_directory(HANDLE directory, std::size_t max_names) {
    constexpr auto header_bytes = offsetof(FILE_ID_BOTH_DIR_INFO, FileName);
    const auto malformed = [] {
        return std::unexpected(Error{ErrorCode::io_error, "malformed directory listing"});
    };
    std::vector<Listed> result;
    std::vector<std::byte> buffer(64 * 1024);
    auto info_class = FileIdBothDirectoryRestartInfo;
    for (;;) {
        if (!GetFileInformationByHandleEx(directory, info_class, buffer.data(),
                                          static_cast<DWORD>(buffer.size()))) {
            if (GetLastError() == ERROR_NO_MORE_FILES) {
                return result;
            }
            return std::unexpected(win_error("enumerate input directory"));
        }
        info_class = FileIdBothDirectoryInfo;
        for (std::size_t offset = 0;;) {
            if (buffer.size() - offset < header_bytes) {
                return malformed();
            }
            FILE_ID_BOTH_DIR_INFO record{};
            std::memcpy(&record, buffer.data() + offset, header_bytes);
            if (record.FileNameLength % sizeof(wchar_t) != 0 ||
                record.FileNameLength > buffer.size() - offset - header_bytes) {
                return malformed();
            }
            std::wstring name(record.FileNameLength / sizeof(wchar_t), L'\0');
            std::memcpy(name.data(), buffer.data() + offset + header_bytes, record.FileNameLength);
            if (name != L"." && name != L"..") {
                if (result.size() >= max_names) {
                    return std::unexpected(
                        Error{ErrorCode::resource_limit, "input has more entries than the limit"});
                }
                result.push_back({std::move(name), record.FileAttributes});
            }
            if (record.NextEntryOffset == 0) {
                break;
            }
            if (record.NextEntryOffset > buffer.size() - offset) {
                return malformed();
            }
            offset += record.NextEntryOffset;
        }
    }
}

struct Walker {
    const ArchiveLimits &limits;
    const ObjectId *exclude;
    std::stop_token stop;
    std::uint64_t volume;
    std::vector<InputEntry> entries;
    std::vector<std::wstring> components;
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

    std::expected<void, Error> walk(HANDLE directory, const std::string &prefix) {
        if (components.size() >= max_tree_depth) {
            return std::unexpected(
                with_path({ErrorCode::resource_limit, "input directory tree is too deep"}, prefix));
        }
        // One extra name leaves room for the excluded output archive.
        const auto remaining =
            limits.max_entries - (std::min)(std::uint64_t{entries.size()}, limits.max_entries);
        const auto listed = list_directory(
            directory, static_cast<std::size_t>(
                           (std::min)(remaining, std::uint64_t{static_cast<std::size_t>(-1) - 1})) +
                           1);
        if (!listed) {
            return std::unexpected(with_path(listed.error(), prefix));
        }
        for (const auto &item : *listed) {
            if (stop.stop_requested()) {
                return std::unexpected(Error{ErrorCode::cancelled, "compression cancelled"});
            }
            const auto name = to_utf8(item.name);
            if (!name) {
                return std::unexpected(with_path(name.error(), prefix));
            }
            const auto path = prefix.empty() ? *name : prefix + '/' + *name;
            if ((item.attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
                return std::unexpected(reparse_rejected(path));
            }
            const bool is_directory = (item.attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            // Files are opened for metadata only here, so an exclusively held file (such as the
            // archive being written) can still be identified and excluded.
            const auto handle =
                open_child(directory, item.name,
                           is_directory ? directory_access : FILE_READ_ATTRIBUTES | SYNCHRONIZE,
                           is_directory, path);
            if (!handle) {
                return std::unexpected(handle.error());
            }
            const auto info = describe(handle->get());
            if (!info) {
                return std::unexpected(with_path(info.error(), path));
            }
            if ((info->attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
                return std::unexpected(reparse_rejected(path));
            }
            if (((info->attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) != is_directory) {
                return std::unexpected(with_path(
                    {ErrorCode::io_error, "input changed while it was being enumerated"}, path));
            }
            if (info->id.volume != volume) {
                return std::unexpected(with_path(
                    {ErrorCode::unsupported_feature, "input crosses a volume boundary"}, path));
            }
            if (!is_directory && exclude != nullptr && info->id == *exclude) {
                continue;
            }
            components.push_back(item.name);
            auto added = add({path, components, is_directory, is_directory ? 0 : info->size,
                              info->last_write, info->id});
            if (added && is_directory) {
                added = walk(handle->get(), path);
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
    Handle root;
    bool directory = false;
    std::wstring name; // final component of the input path
    std::uint64_t volume = 0;

    Impl(Handle handle, bool is_directory, std::wstring root_name, std::uint64_t root_volume)
        : root(std::move(handle)), directory(is_directory), name(std::move(root_name)),
          volume(root_volume) {}
};

struct InputFile::Impl {
    Handle file;
    std::string path;
    std::uint64_t size = 0;
    std::int64_t last_write = 0;
    std::uint64_t consumed = 0;

    Impl(Handle handle, std::string entry_path, std::uint64_t entry_size, std::int64_t entry_time)
        : file(std::move(handle)), path(std::move(entry_path)), size(entry_size),
          last_write(entry_time) {}

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
    while (native.size() > 3 && (native.back() == L'\\' || native.back() == L'/')) {
        native.pop_back();
    }
    if (native.find(L'\0') != std::wstring::npos) {
        return std::unexpected(Error{ErrorCode::invalid_argument, "invalid input path"});
    }
    const auto separator = native.find_last_of(L"\\/");
    auto name = separator == std::wstring::npos ? native : native.substr(separator + 1);
    // Drive-letter paths get the extended-length prefix so long inputs remain reachable.
    const bool drive_path =
        native.size() >= 3 && native[1] == L':' && native[2] == L'\\' &&
        ((native[0] >= L'A' && native[0] <= L'Z') || (native[0] >= L'a' && native[0] <= L'z'));
    const auto device = drive_path ? L"\\\\?\\" + native : native;

    const auto attributes = GetFileAttributesW(device.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        return std::unexpected(win_error("open input"));
    }
    const bool is_directory = (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    // The path the user typed may pass through links; the input object itself may not be one.
    Handle root{CreateFileW(device.c_str(), is_directory ? directory_access : file_access,
                            is_directory ? directory_share : FILE_SHARE_READ, nullptr,
                            OPEN_EXISTING,
                            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
    if (!root.valid()) {
        return std::unexpected(win_error("open input"));
    }
    const auto info = describe(root.get());
    if (!info) {
        return std::unexpected(info.error());
    }
    if ((info->attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        return std::unexpected(reparse_rejected(ascii_for_message(name)));
    }
    if (((info->attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) != is_directory) {
        return std::unexpected(Error{ErrorCode::io_error, "input changed while it was opened"});
    }
    return InputTree{
        std::make_unique<Impl>(std::move(root), is_directory, std::move(name), info->id.volume)};
}

std::expected<std::vector<InputEntry>, Error> InputTree::enumerate(const ArchiveLimits &limits,
                                                                   const ObjectId *exclude,
                                                                   std::stop_token stop) const {
    Walker walker{limits, exclude, std::move(stop), impl_->volume, {}, {}};
    if (impl_->directory) {
        if (auto walked = walker.walk(impl_->root.get(), {}); !walked) {
            return std::unexpected(walked.error());
        }
    } else {
        const auto name = to_utf8(impl_->name);
        if (!name) {
            return std::unexpected(name.error());
        }
        const auto info = describe(impl_->root.get());
        if (!info) {
            return std::unexpected(info.error());
        }
        if (auto added = walker.add({*name, {}, false, info->size, info->last_write, info->id});
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
    const auto finish = [&entry](Handle file) -> std::expected<InputFile, Error> {
        const auto info = describe(file.get());
        if (!info) {
            return std::unexpected(with_path(info.error(), entry.path));
        }
        if ((info->attributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) != 0 ||
            info->id != entry.id || info->size != entry.size ||
            info->last_write != entry.last_write) {
            return std::unexpected(with_path(
                {ErrorCode::io_error, "input changed after it was enumerated"}, entry.path));
        }
        return InputFile{std::make_unique<InputFile::Impl>(std::move(file), entry.path, entry.size,
                                                           entry.last_write)};
    };
    if (entry.components.empty()) {
        if (impl_->directory) {
            return std::unexpected(Error{ErrorCode::invalid_argument, "entry has no path"});
        }
        HANDLE duplicate = INVALID_HANDLE_VALUE;
        if (!DuplicateHandle(GetCurrentProcess(), impl_->root.get(), GetCurrentProcess(),
                             &duplicate, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
            return std::unexpected(win_error("reopen input"));
        }
        Handle file{duplicate};
        LARGE_INTEGER zero{};
        if (!SetFilePointerEx(file.get(), zero, nullptr, FILE_BEGIN)) {
            return std::unexpected(win_error("rewind input"));
        }
        return finish(std::move(file));
    }
    std::vector<Handle> chain;
    chain.reserve(entry.components.size());
    auto parent = impl_->root.get();
    for (std::size_t i = 0; i + 1 < entry.components.size(); ++i) {
        auto directory =
            open_child(parent, entry.components[i], directory_access, true, entry.path);
        if (!directory) {
            return std::unexpected(directory.error());
        }
        chain.push_back(std::move(*directory));
        parent = chain.back().get();
    }
    auto file = open_child(parent, entry.components.back(), file_access, false, entry.path);
    if (!file) {
        return std::unexpected(file.error());
    }
    return finish(std::move(*file));
}

std::expected<std::size_t, Error> InputFile::read(std::span<std::byte> buffer) {
    std::size_t filled = 0;
    while (filled < buffer.size()) {
        const auto count =
            static_cast<DWORD>((std::min)(buffer.size() - filled, std::size_t{1024 * 1024}));
        DWORD got = 0;
        if (!ReadFile(impl_->file.get(), buffer.data() + filled, count, &got, nullptr)) {
            return std::unexpected(with_path(win_error("read input"), impl_->path));
        }
        if (got == 0) {
            break;
        }
        if (got > count) {
            return std::unexpected(Error{ErrorCode::io_error, "read returned too many bytes"});
        }
        filled += got;
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
    DWORD got = 0;
    if (!ReadFile(impl_->file.get(), &extra, 1, &got, nullptr)) {
        return std::unexpected(with_path(win_error("read input"), impl_->path));
    }
    const auto info = describe(impl_->file.get());
    if (!info) {
        return std::unexpected(with_path(info.error(), impl_->path));
    }
    if (got != 0 || info->size != impl_->size || info->last_write != impl_->last_write) {
        return std::unexpected(impl_->changed());
    }
    return {};
}
#else
namespace {
Error unsupported() {
    return {ErrorCode::unsupported_feature, "input enumeration requires Windows"};
}
} // namespace

struct InputTree::Impl {};
struct InputFile::Impl {};

std::expected<InputTree, Error> InputTree::open(const std::filesystem::path &) {
    return std::unexpected(unsupported());
}

std::expected<std::vector<InputEntry>, Error>
InputTree::enumerate(const ArchiveLimits &, const ObjectId *, std::stop_token) const {
    return std::unexpected(unsupported());
}

std::expected<InputFile, Error> InputTree::open_file(const InputEntry &) const {
    return std::unexpected(unsupported());
}

std::expected<std::size_t, Error> InputFile::read(std::span<std::byte>) {
    return std::unexpected(unsupported());
}

std::expected<void, Error> InputFile::confirm_unchanged() { return std::unexpected(unsupported()); }
#endif

InputTree::InputTree(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
InputTree::InputTree(InputTree &&) noexcept = default;
InputTree &InputTree::operator=(InputTree &&) noexcept = default;
InputTree::~InputTree() = default;

InputFile::InputFile(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
InputFile::InputFile(InputFile &&) noexcept = default;
InputFile &InputFile::operator=(InputFile &&) noexcept = default;
InputFile::~InputFile() = default;
} // namespace seedarc::experimental::internal
#endif // defined(_WIN32) || !defined(__linux__)
