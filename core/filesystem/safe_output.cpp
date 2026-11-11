// SPDX-License-Identifier: GPL-3.0-or-later
#include "seedarc/safe_output.hpp"

#include "safe_output_internal.hpp"
#include "seedarc/archive.hpp"
#include "seedarc/archive_path.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <bcrypt.h>
#include <winternl.h>

#include <cstdio>

#ifndef __MINGW32__
// NtSetInformationFile is documented in the WDK (wdm.h / ntifs.h) and exported by ntdll; the
// Windows SDK winternl.h omits it. The information class is declared as ULONG rather than
// FILE_INFORMATION_CLASS: the SDK enumeration only defines FileDirectoryInformation = 1, so
// converting 10 to it would be outside the enumeration's value range (undefined behaviour). Both
// are 32-bit at the C ABI. If a future SDK declares this function, the differing signature fails
// to compile instead of hiding.
extern "C" NTSTATUS NTAPI NtSetInformationFile(HANDLE file, PIO_STATUS_BLOCK io, PVOID information,
                                               ULONG length, ULONG info_class);
#endif
#endif

namespace seedarc::experimental {
namespace internal {
namespace {
thread_local const WriteFaults *installed_faults = nullptr;
} // namespace

ScopedWriteFaults::ScopedWriteFaults(const WriteFaults &faults) noexcept
    : previous_(installed_faults) {
    installed_faults = &faults;
}

ScopedWriteFaults::~ScopedWriteFaults() { installed_faults = previous_; }

const WriteFaults *active_write_faults() noexcept { return installed_faults; }

std::expected<void, Error> admit_output_component(std::size_t depth, std::size_t path_units,
                                                  std::size_t component_units,
                                                  bool directory) noexcept {
    if (directory && (depth >= max_output_depth)) {
        return std::unexpected(
            Error{ErrorCode::resource_limit, "output directory depth limit exceeded"});
    }
    // Written to avoid overflow: path_units + 1 + component_units <= max_output_path_units.
    if (path_units >= max_output_path_units ||
        component_units > max_output_path_units - path_units - 1) {
        return std::unexpected(
            Error{ErrorCode::resource_limit, "output path length limit exceeded"});
    }
    return {};
}

bool is_volume_guid_root(std::wstring_view final_path) noexcept {
    // \\?\Volume{xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx}\  (case-insensitive hex digits)
    constexpr std::wstring_view prefix = L"\\\\?\\Volume{";
    constexpr std::size_t guid_chars = 36;
    if (final_path.size() != prefix.size() + guid_chars + 2 || !final_path.starts_with(prefix) ||
        final_path[prefix.size() + guid_chars] != L'}' || final_path.back() != L'\\') {
        return false;
    }
    for (std::size_t i = 0; i < guid_chars; ++i) {
        const auto c = final_path[prefix.size() + i];
        const bool dash = i == 8 || i == 13 || i == 18 || i == 23;
        const bool hex =
            (c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f') || (c >= L'A' && c <= L'F');
        if (dash ? c != L'-' : !hex) {
            return false;
        }
    }
    return true;
}
} // namespace internal

#ifdef _WIN32
namespace {
// FileRenameInformation from the WDK FILE_INFORMATION_CLASS enumeration.
#ifdef __MINGW32__
// MinGW-w64 declares NtSetInformationFile with its complete FILE_INFORMATION_CLASS enumeration.
constexpr FILE_INFORMATION_CLASS file_rename_information = FileRenameInformation;
static_assert(FileRenameInformation == 10);
#else
constexpr ULONG file_rename_information = 10;
#endif
// Pinned directories allow concurrent readers and writers (other programs may keep creating or
// moving files into them) but never FILE_SHARE_DELETE, so they cannot be renamed or removed while
// held. Files being written are opened without any sharing.
constexpr DWORD directory_share = FILE_SHARE_READ | FILE_SHARE_WRITE;

class Handle {
  public:
    explicit Handle(HANDLE value = INVALID_HANDLE_VALUE, bool cleanup = false) noexcept
        : value_(value), cleanup_(cleanup) {}
    ~Handle() {
        if (value_ != INVALID_HANDLE_VALUE && value_ != nullptr) {
            if (cleanup_) {
                FILE_DISPOSITION_INFO disposition{TRUE};
                SetFileInformationByHandle(value_, FileDispositionInfo, &disposition,
                                           sizeof(disposition));
            }
            CloseHandle(value_);
        }
    }
    Handle(Handle &&other) noexcept
        : value_(std::exchange(other.value_, INVALID_HANDLE_VALUE)), cleanup_(other.cleanup_) {}
    Handle &operator=(Handle &&) = delete;
    Handle(const Handle &) = delete;
    Handle &operator=(const Handle &) = delete;
    HANDLE get() const noexcept { return value_; }
    void retain() noexcept { cleanup_ = false; }

  private:
    HANDLE value_;
    bool cleanup_;
};

Error win_error(std::string stage, DWORD code = GetLastError()) {
    const bool conflict = code == ERROR_ALREADY_EXISTS || code == ERROR_FILE_EXISTS;
    return {conflict ? ErrorCode::output_conflict : ErrorCode::io_error,
            std::move(stage) + ": Windows error " + std::to_string(code)};
}

Error nt_error(std::string stage, NTSTATUS status) {
    std::array<char, 11> hex{};
    std::snprintf(hex.data(), hex.size(), "0x%08lx", static_cast<unsigned long>(status));
    auto error = win_error(std::move(stage), RtlNtStatusToDosError(status));
    error.message += " (NTSTATUS " + std::string{hex.data()} + ")";
    return error;
}

// Only used for names this module generated or already validated; non-ASCII becomes '?'.
std::string ascii_for_message(std::wstring_view text) {
    std::string result;
    result.reserve(text.size());
    for (const auto c : text) {
        result += c >= 0x20 && c < 0x7f ? static_cast<char>(c) : '?';
    }
    return result;
}

std::expected<std::wstring, Error> leaf(std::string_view name) {
    auto components = validate_archive_path(name);
    if (!components) {
        return std::unexpected(components.error());
    }
    if (components->size() != 1) {
        return std::unexpected(
            Error{ErrorCode::unsafe_path, "output operation requires one basename"});
    }
    return std::move(components->front());
}

std::expected<std::uint64_t, Error> volume_serial(HANDLE handle) {
    FILE_ID_INFO info{};
    if (!GetFileInformationByHandleEx(handle, FileIdInfo, &info, sizeof(info))) {
        return std::unexpected(win_error("read object volume identity"));
    }
    return info.VolumeSerialNumber;
}

std::expected<Handle, Error> relative_open(HANDLE parent, std::uint64_t volume,
                                           const std::wstring &name, bool directory, bool create) {
    UNICODE_STRING text{};
    text.Length = static_cast<USHORT>(name.size() * sizeof(wchar_t));
    text.MaximumLength = text.Length;
    text.Buffer = const_cast<wchar_t *>(name.data());
    OBJECT_ATTRIBUTES attributes{};
    InitializeObjectAttributes(&attributes, &text, OBJ_CASE_INSENSITIVE | OBJ_DONT_REPARSE, parent,
                               nullptr);
    IO_STATUS_BLOCK io{};
    HANDLE raw = INVALID_HANDLE_VALUE;
    const auto access =
        directory ? FILE_TRAVERSE | FILE_READ_ATTRIBUTES | SYNCHRONIZE | (create ? DELETE : 0)
                  : FILE_READ_DATA | FILE_WRITE_DATA | FILE_READ_ATTRIBUTES | DELETE | SYNCHRONIZE;
    // Creation never opens a pre-existing object. Existing directories are opened as
    // reparse objects first and checked, rather than traversing their targets.
    const ULONG options = FILE_SYNCHRONOUS_IO_NONALERT |
                          (directory && create ? FILE_DIRECTORY_FILE : FILE_OPEN_REPARSE_POINT) |
                          (!directory ? FILE_NON_DIRECTORY_FILE : 0);
    const auto status = NtCreateFile(&raw, access, &attributes, &io, nullptr,
                                     directory ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL,
                                     directory ? directory_share : 0,
                                     create ? FILE_CREATE : FILE_OPEN, options, nullptr, 0);
    if (status < 0) {
        return std::unexpected(nt_error("relative create/open", status));
    }
    Handle handle{raw, create};
    FILE_ATTRIBUTE_TAG_INFO info{};
    if (!GetFileInformationByHandleEx(raw, FileAttributeTagInfo, &info, sizeof(info))) {
        return std::unexpected(win_error("read object attributes"));
    }
    if ((info.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 ||
        ((info.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) != directory) {
        return std::unexpected(
            Error{ErrorCode::unsafe_path, "reparse or unexpected output object type"});
    }
    const auto serial = volume_serial(raw);
    if (!serial) {
        return std::unexpected(serial.error());
    }
    if (*serial != volume) {
        return std::unexpected(
            Error{ErrorCode::unsafe_path, "output object is not on the pinned root volume"});
    }
    return handle;
}

std::expected<void, Error> mark_delete(HANDLE handle) {
    FILE_DISPOSITION_INFO disposition{TRUE};
    if (!SetFileInformationByHandle(handle, FileDispositionInfo, &disposition,
                                    sizeof(disposition))) {
        return std::unexpected(win_error("handle cleanup failed"));
    }
    return {};
}

std::expected<void, Error> rename_in_place_no_replace(HANDLE file, const std::wstring &name) {
    // Call the NT API directly. kernel32's SetFileInformationByHandle(FileRenameInfo) rewrites
    // names itself: with RootDirectory == NULL a bare name was observed to resolve against the
    // process current directory (moving the file out of the pinned parent), and with a
    // RootDirectory it failed with ERROR_INVALID_PARAMETER.
    // At the NT layer, RootDirectory == NULL plus a name without separators renames the file
    // within the directory that currently holds it. The file was created relative to the pinned
    // parent and is opened without sharing, and the parent is opened without FILE_SHARE_DELETE,
    // so neither can move. Passing the parent as RootDirectory would make the I/O manager reopen
    // the parent for the rename target, which is unnecessary for an in-place rename.
    if (name.empty() || name.find_first_of(L"\\/") != std::wstring::npos) {
        return std::unexpected(Error{ErrorCode::unsafe_path, "rename requires one basename"});
    }
    // FILE_RENAME_INFO has the same layout as the NT FILE_RENAME_INFORMATION structure.
    const auto bytes = sizeof(FILE_RENAME_INFO) + name.size() * sizeof(wchar_t);
    std::vector<std::max_align_t> storage((bytes + sizeof(std::max_align_t) - 1) /
                                          sizeof(std::max_align_t));
    auto *info = new (storage.data()) FILE_RENAME_INFO{};
    info->ReplaceIfExists = FALSE;
    info->RootDirectory = nullptr;
    info->FileNameLength = static_cast<DWORD>(name.size() * sizeof(wchar_t));
    std::memcpy(info->FileName, name.data(), info->FileNameLength);
    IO_STATUS_BLOCK io{};
    const auto status =
        NtSetInformationFile(file, &io, info, static_cast<ULONG>(bytes), file_rename_information);
    if (status < 0) {
        return std::unexpected(nt_error("commit without replacement", status));
    }
    return {};
}

constexpr std::size_t temporary_name_units = 9 + 32 + 4; // ".seedarc-" + 32 hex + ".tmp"

std::expected<std::wstring, Error> temporary_name() {
    std::array<UCHAR, 16> random{};
    const auto status = BCryptGenRandom(nullptr, random.data(), static_cast<ULONG>(random.size()),
                                        BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    if (status < 0) {
        return std::unexpected(
            win_error("temporary name randomness", RtlNtStatusToDosError(status)));
    }
    std::wstring name = L".seedarc-";
    constexpr wchar_t digits[] = L"0123456789abcdef";
    for (auto byte : random) {
        name += digits[byte >> 4];
        name += digits[byte & 15];
    }
    return name + L".tmp";
}
} // namespace

struct SafeOutputDirectory::Impl {
    Handle handle;
    std::shared_ptr<Impl> parent;
    std::uint64_t volume = 0;
    std::size_t depth = 0;      // components below the drive root
    std::size_t path_units = 0; // UTF-16 units of the equivalent "C:\a\b" path
    bool created = false;
    bool discarded = false;

    Impl(Handle h, std::shared_ptr<Impl> p, std::uint64_t vol, std::size_t d, std::size_t units,
         bool own)
        : handle(std::move(h)), parent(std::move(p)), volume(vol), depth(d), path_units(units),
          created(own) {
        handle.retain();
    }

    [[nodiscard]] std::expected<void, Error> admit(std::size_t units, bool directory) const {
        return internal::admit_output_component(depth, path_units, units, directory);
    }
};
#else
struct SafeOutputDirectory::Impl {};
#endif

SafeOutputDirectory::SafeOutputDirectory(std::shared_ptr<Impl> impl) : impl_(std::move(impl)) {}

std::expected<SafeOutputDirectory, Error>
SafeOutputDirectory::open_existing(const std::filesystem::path &path) {
#ifdef _WIN32
    const auto native = path.native();
    if (native.size() < 3 || native.size() > 4096 ||
        !((native[0] >= L'A' && native[0] <= L'Z') || (native[0] >= L'a' && native[0] <= L'z')) ||
        native[1] != L':' || (native[2] != L'\\' && native[2] != L'/') ||
        native.find(L'\0') != std::wstring::npos) {
        return std::unexpected(
            Error{ErrorCode::unsafe_path, "requires an absolute local drive directory"});
    }
    const std::wstring drive = native.substr(0, 2) + L"\\";
    if (GetDriveTypeW(drive.c_str()) != DRIVE_FIXED) {
        return std::unexpected(
            Error{ErrorCode::unsupported_feature, "only local fixed NTFS volumes supported"});
    }
    Handle root{CreateFileW(drive.c_str(), FILE_TRAVERSE | FILE_READ_ATTRIBUTES | SYNCHRONIZE,
                            directory_share, nullptr, OPEN_EXISTING,
                            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
    if (root.get() == INVALID_HANDLE_VALUE) {
        return std::unexpected(win_error("pin drive root"));
    }
    std::array<wchar_t, 32> filesystem{};
    FILE_ATTRIBUTE_TAG_INFO root_info{};
    if (!GetFileInformationByHandleEx(root.get(), FileAttributeTagInfo, &root_info,
                                      sizeof(root_info)) ||
        !GetVolumeInformationByHandleW(root.get(), nullptr, 0, nullptr, nullptr, nullptr,
                                       filesystem.data(), static_cast<DWORD>(filesystem.size()))) {
        return std::unexpected(win_error("inspect local volume"));
    }
    if ((root_info.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 ||
        std::wstring_view{filesystem.data()} != L"NTFS") {
        return std::unexpected(
            Error{ErrorCode::unsupported_feature, "non-NTFS or reparse root rejected"});
    }
    // The drive letter must name the root of a volume. SUBST and similar DOS-device mappings make
    // "X:\" an arbitrary directory, which would break the depth / path budget and the trust in the
    // initial mapping.
    std::array<wchar_t, 64> final_path{};
    const auto final_length = GetFinalPathNameByHandleW(
        root.get(), final_path.data(), static_cast<DWORD>(final_path.size()), VOLUME_NAME_GUID);
    if (final_length == 0 || final_length >= final_path.size()) {
        return std::unexpected(Error{ErrorCode::unsupported_feature,
                                     "drive letter does not resolve to a volume root"});
    }
    if (!internal::is_volume_guid_root({final_path.data(), final_length})) {
        return std::unexpected(Error{ErrorCode::unsupported_feature,
                                     "drive letter is mapped to a directory, not a volume root"});
    }
    const auto volume = volume_serial(root.get());
    if (!volume) {
        return std::unexpected(volume.error());
    }
    auto current = std::make_shared<Impl>(std::move(root), nullptr, *volume, 0, 2, false);
    std::size_t begin = 3;
    while (begin < native.size()) {
        const auto end = native.find_first_of(L"\\/", begin);
        const auto component = native.substr(begin, end == std::wstring::npos ? end : end - begin);
        if (component.empty() || component == L"." || component == L".." ||
            component.size() > 255 || component.back() == L'.' || component.back() == L' ' ||
            component.find_first_of(L":<>\"|?*") != std::wstring::npos || current->depth + 1 > 64) {
            return std::unexpected(Error{ErrorCode::unsafe_path, "unsafe output root component"});
        }
        if (auto admitted = current->admit(component.size(), true); !admitted) {
            return std::unexpected(admitted.error());
        }
        auto opened = relative_open(current->handle.get(), *volume, component, true, false);
        if (!opened) {
            return std::unexpected(opened.error());
        }
        current = std::make_shared<Impl>(std::move(*opened), current, *volume, current->depth + 1,
                                         current->path_units + 1 + component.size(), false);
        if (end == std::wstring::npos) {
            break;
        }
        begin = end + 1;
    }
    return SafeOutputDirectory{std::move(current)};
#else
    (void)path;
    return std::unexpected(Error{ErrorCode::unsupported_feature, "safe output requires Windows"});
#endif
}

std::expected<SafeOutputDirectory, Error>
SafeOutputDirectory::create_child(std::string_view basename) const {
#ifdef _WIN32
    if (!impl_ || impl_->discarded) {
        return std::unexpected(Error{ErrorCode::io_error, "directory already discarded"});
    }
    auto name = leaf(basename);
    if (!name) {
        return std::unexpected(name.error());
    }
    if (auto admitted = impl_->admit(name->size(), true); !admitted) {
        return std::unexpected(admitted.error());
    }
    auto child = relative_open(impl_->handle.get(), impl_->volume, *name, true, true);
    if (!child) {
        return std::unexpected(child.error());
    }
    return SafeOutputDirectory{std::make_shared<Impl>(std::move(*child), impl_, impl_->volume,
                                                      impl_->depth + 1,
                                                      impl_->path_units + 1 + name->size(), true)};
#else
    (void)basename;
    return std::unexpected(Error{ErrorCode::unsupported_feature, "safe output requires Windows"});
#endif
}

std::expected<void, Error> SafeOutputDirectory::write_new_file(std::string_view basename,
                                                               std::span<const std::byte> contents,
                                                               std::uint32_t expected_crc32c,
                                                               std::stop_token stop) const {
#ifdef _WIN32
    if (!impl_ || impl_->discarded) {
        return std::unexpected(Error{ErrorCode::io_error, "directory already discarded"});
    }
    auto name = leaf(basename);
    if (!name) {
        return std::unexpected(name.error());
    }
    if (auto admitted = impl_->admit((std::max)(name->size(), temporary_name_units), false);
        !admitted) {
        return std::unexpected(admitted.error());
    }
    if (contents.size() > 4 * 1024 * 1024) {
        return std::unexpected(
            Error{ErrorCode::resource_limit, "M1 write prototype is limited to 4 MiB"});
    }
    if (stop.stop_requested()) {
        return std::unexpected(Error{ErrorCode::cancelled, "write cancelled before creation"});
    }
    const auto *faults = internal::active_write_faults();
    auto temp_name = temporary_name();
    if (!temp_name) {
        return std::unexpected(temp_name.error());
    }
    auto file = relative_open(impl_->handle.get(), impl_->volume, *temp_name, false, true);
    if (!file) {
        return std::unexpected(file.error());
    }
    // The failure path marks the temporary for deletion through its own handle. If that explicit
    // step fails, the error says so and names the temporary; closing the handle retries once more.
    const auto fail = [&file, &temp_name, faults](Error error) -> std::expected<void, Error> {
        const auto cleanup = faults != nullptr && faults->fail_explicit_cleanup
                                 ? std::expected<void, Error>{std::unexpected(Error{
                                       ErrorCode::io_error, "handle cleanup failed: injected"})}
                                 : mark_delete(file->get());
        if (!cleanup) {
            error.message += "; " + cleanup.error().message + "; temporary " +
                             ascii_for_message(*temp_name) + " retried at handle close";
        }
        return std::unexpected(std::move(error));
    };
    std::size_t offset = 0;
    while (offset < contents.size()) {
        if (stop.stop_requested()) {
            return fail({ErrorCode::cancelled, "write cancelled"});
        }
        const auto requested =
            static_cast<DWORD>((std::min)(contents.size() - offset, std::size_t{65536}));
        auto count = requested;
        if (faults != nullptr && faults->max_bytes_per_write != 0) {
            count = static_cast<DWORD>((std::min)(std::size_t{count}, faults->max_bytes_per_write));
        }
        DWORD written = 0;
        BOOL ok = FALSE;
        if (faults != nullptr && offset >= faults->fail_write_at) {
            SetLastError(ERROR_DISK_FULL);
        } else {
            ok = WriteFile(file->get(), contents.data() + offset, count, &written, nullptr);
        }
        // A short write (written < requested) is legal; loop until everything is written.
        if (!ok || written == 0 || written > requested) {
            return fail(win_error("write temporary file"));
        }
        offset += written;
        if (faults != nullptr && faults->after_write) {
            faults->after_write(offset);
        }
    }
    if (crc32c(contents) != expected_crc32c) {
        return fail({ErrorCode::integrity_error, "content CRC32C mismatch"});
    }
    if (faults != nullptr && faults->fail_flush) {
        return fail(win_error("flush temporary file", ERROR_WRITE_FAULT));
    }
    if (!FlushFileBuffers(file->get())) {
        return fail(win_error("flush temporary file"));
    }
    LARGE_INTEGER zero{};
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file->get(), &size) ||
        size.QuadPart != static_cast<LONGLONG>(contents.size()) ||
        !SetFilePointerEx(file->get(), zero, nullptr, FILE_BEGIN)) {
        return fail(win_error("verify temporary file size/position"));
    }
    std::array<std::byte, 65536> buffer{};
    offset = 0;
    while (offset < contents.size()) {
        DWORD read = 0;
        const auto count = static_cast<DWORD>((std::min)(contents.size() - offset, buffer.size()));
        if (!ReadFile(file->get(), buffer.data(), count, &read, nullptr) || read == 0 ||
            read > count) {
            return fail(win_error("read back temporary file"));
        }
        if (faults != nullptr && faults->corrupt_read_back) {
            buffer[0] ^= std::byte{1};
        }
        if (!std::equal(buffer.begin(), buffer.begin() + read, contents.begin() + offset)) {
            return fail({ErrorCode::integrity_error, "temporary file byte verification failed"});
        }
        offset += read;
    }
    if (stop.stop_requested()) {
        return fail({ErrorCode::cancelled, "write cancelled before commit"});
    }
    if (faults != nullptr && faults->fail_commit) {
        return fail(win_error("commit without replacement", ERROR_DISK_FULL));
    }
    const auto renamed = rename_in_place_no_replace(file->get(), *name);
    if (!renamed) {
        return fail(renamed.error());
    }
    file->retain();
    return {};
#else
    (void)basename;
    (void)contents;
    (void)expected_crc32c;
    (void)stop;
    return std::unexpected(Error{ErrorCode::unsupported_feature, "safe output requires Windows"});
#endif
}

std::expected<void, Error> SafeOutputDirectory::discard_empty() {
#ifdef _WIN32
    if (!impl_ || !impl_->created || impl_->discarded) {
        return std::unexpected(
            Error{ErrorCode::unsafe_path, "cannot discard an existing or discarded directory"});
    }
    const auto result = mark_delete(impl_->handle.get());
    if (result) {
        impl_->discarded = true;
    }
    return result;
#else
    return std::unexpected(Error{ErrorCode::unsupported_feature, "safe output requires Windows"});
#endif
}
} // namespace seedarc::experimental
