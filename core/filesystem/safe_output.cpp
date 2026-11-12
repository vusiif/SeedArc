// SPDX-License-Identifier: GPL-3.0-or-later
#include "seedarc/safe_output.hpp"

#include "safe_output_internal.hpp"
#include "seedarc/archive.hpp"
#include "seedarc/archive_path.hpp"
#include "win_common.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <bcrypt.h>

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

#ifdef _WIN32
Error win_error(std::string stage, DWORD code) {
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

std::string ascii_for_message(std::wstring_view text) {
    std::string result;
    result.reserve(text.size());
    for (const auto c : text) {
        result += c >= 0x20 && c < 0x7f ? static_cast<char>(c) : '?';
    }
    return result;
}

NTSTATUS nt_relative(HANDLE parent, std::wstring_view name, ACCESS_MASK access, ULONG share,
                     ULONG disposition, ULONG options, ULONG attributes, ULONG object_flags,
                     HANDLE &out, PSECURITY_DESCRIPTOR security) noexcept {
    // A relative NT name with a separator would address a deeper object; callers pass exactly
    // one component, and this keeps that true even if a caller is wrong.
    constexpr NTSTATUS status_object_name_invalid = static_cast<NTSTATUS>(0xC0000033L);
    if (name.empty() || name.size() > 255 ||
        name.find_first_of(L"\\/") != std::wstring_view::npos) {
        return status_object_name_invalid;
    }
    UNICODE_STRING text{};
    text.Length = static_cast<USHORT>(name.size() * sizeof(wchar_t));
    text.MaximumLength = text.Length;
    text.Buffer = const_cast<wchar_t *>(name.data());
    OBJECT_ATTRIBUTES object{};
    InitializeObjectAttributes(&object, &text, object_flags, parent, security);
    IO_STATUS_BLOCK io{};
    HANDLE raw = INVALID_HANDLE_VALUE;
    const auto status = NtCreateFile(&raw, access, &object, &io, nullptr, attributes, share,
                                     disposition, options, nullptr, 0);
    if (status >= 0) {
        out = raw;
    }
    return status;
}

std::expected<ObjectId, Error> object_id(HANDLE handle) {
    FILE_ID_INFO info{};
    if (!GetFileInformationByHandleEx(handle, FileIdInfo, &info, sizeof(info))) {
        return std::unexpected(win_error("read object identity"));
    }
    ObjectId id;
    id.volume = info.VolumeSerialNumber;
    static_assert(sizeof(info.FileId.Identifier) == sizeof(id.file));
    std::memcpy(id.file.data(), info.FileId.Identifier, id.file.size());
    return id;
}
#endif
} // namespace internal

#ifdef _WIN32
namespace {
using internal::ascii_for_message;
using internal::Handle;
using internal::nt_error;
using internal::nt_relative;
using internal::object_id;
using internal::win_error;

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

// Rejects reparse points, objects of the wrong kind and objects on another volume.
std::expected<void, Error> verify_object(HANDLE handle, std::uint64_t volume, bool directory) {
    FILE_ATTRIBUTE_TAG_INFO info{};
    if (!GetFileInformationByHandleEx(handle, FileAttributeTagInfo, &info, sizeof(info))) {
        return std::unexpected(win_error("read object attributes"));
    }
    if ((info.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 ||
        ((info.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) != directory) {
        return std::unexpected(
            Error{ErrorCode::unsafe_path, "reparse or unexpected output object type"});
    }
    const auto id = object_id(handle);
    if (!id) {
        return std::unexpected(id.error());
    }
    if (id->volume != volume) {
        return std::unexpected(
            Error{ErrorCode::unsafe_path, "output object is not on the pinned root volume"});
    }
    return {};
}

// Directories this library creates or owns are also opened with FILE_LIST_DIRECTORY so that the
// staged tree can be enumerated before it is published; pre-existing parents need only traversal.
std::expected<Handle, Error> relative_open(HANDLE parent, std::uint64_t volume,
                                           const std::wstring &name, bool directory, bool create,
                                           bool owned = false,
                                           PSECURITY_DESCRIPTOR security = nullptr) {
    const ACCESS_MASK access =
        directory ? FILE_TRAVERSE | FILE_READ_ATTRIBUTES | SYNCHRONIZE | (create ? DELETE : 0) |
                        (create || owned ? FILE_LIST_DIRECTORY : 0)
                  : FILE_READ_DATA | FILE_WRITE_DATA | FILE_READ_ATTRIBUTES | DELETE | SYNCHRONIZE;
    // Creation never opens a pre-existing object. Existing directories are opened as
    // reparse objects first and checked, rather than traversing their targets.
    const ULONG options = FILE_SYNCHRONOUS_IO_NONALERT |
                          (directory && create ? FILE_DIRECTORY_FILE : FILE_OPEN_REPARSE_POINT) |
                          (!directory ? FILE_NON_DIRECTORY_FILE : 0);
    HANDLE raw = INVALID_HANDLE_VALUE;
    const auto status = nt_relative(parent, name, access, directory ? directory_share : 0,
                                    create ? FILE_CREATE : FILE_OPEN, options,
                                    directory ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL,
                                    OBJ_CASE_INSENSITIVE | OBJ_DONT_REPARSE, raw, security);
    if (status < 0) {
        return std::unexpected(nt_error("relative create/open", status));
    }
    Handle handle{raw, create};
    if (auto verified = verify_object(raw, volume, directory); !verified) {
        return std::unexpected(verified.error());
    }
    return handle;
}

// The final name of a commit must be free. The commit itself never replaces, so this is an early,
// cheap failure rather than the safety mechanism. The object itself is probed, never its target.
std::expected<void, Error> require_absent(HANDLE parent, const std::wstring &name) {
    HANDLE raw = INVALID_HANDLE_VALUE;
    const auto status = nt_relative(
        parent, name, FILE_READ_ATTRIBUTES | SYNCHRONIZE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, FILE_OPEN,
        FILE_SYNCHRONOUS_IO_NONALERT | FILE_OPEN_REPARSE_POINT, 0, OBJ_CASE_INSENSITIVE, raw);
    if (status >= 0) {
        CloseHandle(raw);
        return std::unexpected(Error{ErrorCode::output_conflict, "output name already exists"});
    }
    if (status == internal::status_object_name_not_found) {
        return {};
    }
    return std::unexpected(nt_error("probe output name", status));
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
        auto error = nt_error("commit without replacement", status);
        SetLastError(RtlNtStatusToDosError(status)); // lets the caller classify the failure
        return std::unexpected(std::move(error));
    }
    return {};
}

bool transient_sharing_failure(DWORD code) noexcept {
    return code == ERROR_ACCESS_DENIED || code == ERROR_SHARING_VIOLATION;
}

// Removing a directory can fail while a just-deleted child is still held open by a scanner or
// indexer (its name disappears only when that handle closes). Retried for about 2 s.
std::expected<void, Error> mark_delete_directory(HANDLE handle) {
    for (int attempt = 0;; ++attempt) {
        auto result = mark_delete(handle);
        if (result || attempt >= 20 || GetLastError() != ERROR_DIR_NOT_EMPTY) {
            return result;
        }
        Sleep(100);
    }
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

// Security for a staging directory: a protected DACL that grants full access only to the current
// token user, SYSTEM and Administrators, inherited by every object created below it. It is set by
// the creating NtCreateFile call, so other principals that can write into the target's parent can
// never open, add to or change the staged tree, from creation through publication. The DACL stays
// on the published tree (no inheritance from the target parent).
struct StagingSecurity {
    std::vector<std::max_align_t> token_user;
    std::vector<std::max_align_t> acl;
    SECURITY_DESCRIPTOR descriptor{};
};

std::expected<void, Error> init_staging_security(StagingSecurity &out) {
    HANDLE raw = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw)) {
        return std::unexpected(win_error("open process token"));
    }
    const Handle token{raw};
    DWORD needed = 0;
    (void)GetTokenInformation(token.get(), TokenUser, nullptr, 0, &needed);
    if (needed == 0) {
        return std::unexpected(win_error("query token user size"));
    }
    out.token_user.resize((needed + sizeof(std::max_align_t) - 1) / sizeof(std::max_align_t));
    if (!GetTokenInformation(token.get(), TokenUser, out.token_user.data(), needed, &needed)) {
        return std::unexpected(win_error("query token user"));
    }
    constexpr auto sid_slots =
        (SECURITY_MAX_SID_SIZE + sizeof(std::max_align_t) - 1) / sizeof(std::max_align_t);
    std::array<std::max_align_t, sid_slots> system_sid{};
    std::array<std::max_align_t, sid_slots> admins_sid{};
    DWORD system_size = SECURITY_MAX_SID_SIZE;
    DWORD admins_size = SECURITY_MAX_SID_SIZE;
    if (!CreateWellKnownSid(WinLocalSystemSid, nullptr, system_sid.data(), &system_size) ||
        !CreateWellKnownSid(WinBuiltinAdministratorsSid, nullptr, admins_sid.data(),
                            &admins_size)) {
        return std::unexpected(win_error("build well-known SID"));
    }
    const std::array<PSID, 3> sids{reinterpret_cast<TOKEN_USER *>(out.token_user.data())->User.Sid,
                                   system_sid.data(), admins_sid.data()};
    DWORD acl_bytes = sizeof(ACL);
    for (auto *sid : sids) {
        acl_bytes += sizeof(ACCESS_ALLOWED_ACE) - sizeof(DWORD) + GetLengthSid(sid);
    }
    out.acl.resize((acl_bytes + sizeof(std::max_align_t) - 1) / sizeof(std::max_align_t));
    auto *acl = reinterpret_cast<PACL>(out.acl.data());
    if (!InitializeAcl(acl, acl_bytes, ACL_REVISION)) {
        return std::unexpected(win_error("initialize staging ACL"));
    }
    for (auto *sid : sids) {
        if (!AddAccessAllowedAceEx(acl, ACL_REVISION, OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE,
                                   FILE_ALL_ACCESS, sid)) {
            return std::unexpected(win_error("build staging ACL"));
        }
    }
    if (!InitializeSecurityDescriptor(&out.descriptor, SECURITY_DESCRIPTOR_REVISION) ||
        !SetSecurityDescriptorDacl(&out.descriptor, TRUE, acl, FALSE) ||
        !SetSecurityDescriptorControl(&out.descriptor, SE_DACL_PROTECTED, SE_DACL_PROTECTED)) {
        return std::unexpected(win_error("build staging security descriptor"));
    }
    return {};
}

Error staged_changed(std::string what) {
    return {ErrorCode::integrity_error,
            "staged output changed before publication: " + std::move(what)};
}

// A staged file must carry only its unnamed data stream; a staged directory none at all.
std::expected<void, Error> require_no_named_streams(HANDLE handle, bool directory) {
    std::array<std::max_align_t, 1024 / sizeof(std::max_align_t)> buffer{};
    if (!GetFileInformationByHandleEx(handle, FileStreamInfo, buffer.data(),
                                      static_cast<DWORD>(sizeof(buffer)))) {
        const auto code = GetLastError();
        if (directory && code == ERROR_HANDLE_EOF) {
            return {};
        }
        if (code == ERROR_MORE_DATA) {
            return std::unexpected(staged_changed("unexpected data streams"));
        }
        return std::unexpected(win_error("query data streams", code));
    }
    const auto *info = reinterpret_cast<const FILE_STREAM_INFO *>(buffer.data());
    const std::wstring_view name{info->StreamName, info->StreamNameLength / sizeof(wchar_t)};
    if (directory || info->NextEntryOffset != 0 || name != L"::$DATA") {
        return std::unexpected(staged_changed("unexpected data streams"));
    }
    return {};
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
    std::wstring staged_name; // final name of a staging directory until commit_staged()

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

struct SafeOutputFile::Impl {
    std::shared_ptr<SafeOutputDirectory::Impl> directory; // keeps the parent pinned
    Handle file;
    std::wstring temp_name;
    std::wstring final_name;
    ObjectId id;
    Crc32c crc;
    std::uint64_t written = 0;
    bool finished = false; // committed or failed; no further writes

    Impl(std::shared_ptr<SafeOutputDirectory::Impl> parent, Handle handle, std::wstring temporary,
         std::wstring name, const ObjectId &identity)
        : directory(std::move(parent)), file(std::move(handle)), temp_name(std::move(temporary)),
          final_name(std::move(name)), id(identity) {}

    // Marks the temporary for deletion through its own handle. If that explicit step fails, the
    // error says so and names the temporary; closing the handle retries once more.
    std::unexpected<Error> fail(Error error) {
        if (!finished) {
            finished = true;
            const auto *faults = internal::active_write_faults();
            const auto cleanup = faults != nullptr && faults->fail_explicit_cleanup
                                     ? std::expected<void, Error>{std::unexpected(Error{
                                           ErrorCode::io_error, "handle cleanup failed: injected"})}
                                     : mark_delete(file.get());
            if (!cleanup) {
                error.message += "; " + cleanup.error().message + "; temporary " +
                                 ascii_for_message(temp_name) + " retried at handle close";
            }
        }
        return std::unexpected(std::move(error));
    }
};
#else
struct SafeOutputDirectory::Impl {};
struct SafeOutputFile::Impl {
    ObjectId id;
};
#endif

namespace {
#ifdef _WIN32
Error closed_directory() { return {ErrorCode::io_error, "directory already discarded"}; }
Error closed_session() { return {ErrorCode::io_error, "output file session is closed"}; }
#else
Error unsupported() { return {ErrorCode::unsupported_feature, "safe output requires Windows"}; }
#endif
} // namespace

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
    const auto root_id = object_id(root.get());
    if (!root_id) {
        return std::unexpected(root_id.error());
    }
    const auto volume = &root_id->volume;
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
    return std::unexpected(unsupported());
#endif
}

std::expected<SafeOutputDirectory, Error>
SafeOutputDirectory::create_child(std::string_view basename) const {
#ifdef _WIN32
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
    auto child = relative_open(impl_->handle.get(), impl_->volume, *name, true, true);
    if (!child) {
        return std::unexpected(child.error());
    }
    return SafeOutputDirectory{std::make_shared<Impl>(std::move(*child), impl_, impl_->volume,
                                                      impl_->depth + 1,
                                                      impl_->path_units + 1 + name->size(), true)};
#else
    (void)basename;
    return std::unexpected(unsupported());
#endif
}

std::expected<void, Error> SafeOutputDirectory::write_new_file(std::string_view basename,
                                                               std::span<const std::byte> contents,
                                                               std::uint32_t expected_crc32c,
                                                               std::stop_token stop) const {
#ifdef _WIN32
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
        const auto count = (std::min)(piece, contents.size() - offset);
        if (auto appended = file->append(contents.subspan(offset, count)); !appended) {
            return appended;
        }
    }
    if (file->checksum() != expected_crc32c) {
        return file->impl_->fail({ErrorCode::integrity_error, "content CRC32C mismatch"});
    }
    return file->commit(contents.size(), std::move(stop));
#else
    (void)basename;
    (void)contents;
    (void)expected_crc32c;
    (void)stop;
    return std::unexpected(unsupported());
#endif
}

std::expected<SafeOutputFile, Error>
SafeOutputDirectory::begin_file(std::string_view basename) const {
#ifdef _WIN32
    if (!impl_ || impl_->discarded) {
        return std::unexpected(closed_directory());
    }
    auto name = leaf(basename);
    if (!name) {
        return std::unexpected(name.error());
    }
    if (auto admitted = impl_->admit((std::max)(name->size(), temporary_name_units), false);
        !admitted) {
        return std::unexpected(admitted.error());
    }
    if (auto absent = require_absent(impl_->handle.get(), *name); !absent) {
        return std::unexpected(absent.error());
    }
    auto temp_name = temporary_name();
    if (!temp_name) {
        return std::unexpected(temp_name.error());
    }
    auto file = relative_open(impl_->handle.get(), impl_->volume, *temp_name, false, true);
    if (!file) {
        return std::unexpected(file.error());
    }
    const auto id = object_id(file->get());
    if (!id) {
        return std::unexpected(id.error()); // the handle deletes the temporary when it closes
    }
    return SafeOutputFile{std::make_unique<SafeOutputFile::Impl>(
        impl_, std::move(*file), std::move(*temp_name), std::move(*name), *id)};
#else
    (void)basename;
    return std::unexpected(unsupported());
#endif
}

std::expected<SafeOutputDirectory, Error>
SafeOutputDirectory::create_staging_child(std::string_view final_basename) const {
#ifdef _WIN32
    if (!impl_ || impl_->discarded) {
        return std::unexpected(closed_directory());
    }
    auto name = leaf(final_basename);
    if (!name) {
        return std::unexpected(name.error());
    }
    // Budget with the longer of the two names so that the commit cannot push paths over budget.
    const auto units = (std::max)(name->size(), temporary_name_units);
    if (auto admitted = impl_->admit(units, true); !admitted) {
        return std::unexpected(admitted.error());
    }
    if (auto absent = require_absent(impl_->handle.get(), *name); !absent) {
        return std::unexpected(absent.error());
    }
    auto temp_name = temporary_name();
    if (!temp_name) {
        return std::unexpected(temp_name.error());
    }
    StagingSecurity security;
    if (auto built = init_staging_security(security); !built) {
        return std::unexpected(built.error());
    }
    auto child = relative_open(impl_->handle.get(), impl_->volume, *temp_name, true, true, true,
                               &security.descriptor);
    if (!child) {
        return std::unexpected(child.error());
    }
    auto staged = std::make_shared<Impl>(std::move(*child), impl_, impl_->volume, impl_->depth + 1,
                                         impl_->path_units + 1 + units, true);
    staged->staged_name = std::move(*name);
    return SafeOutputDirectory{std::move(staged)};
#else
    (void)final_basename;
    return std::unexpected(unsupported());
#endif
}

std::expected<void, Error> SafeOutputDirectory::commit_staged() {
#ifdef _WIN32
    if (!impl_ || impl_->discarded || impl_->staged_name.empty()) {
        return std::unexpected(
            Error{ErrorCode::invalid_argument, "not an uncommitted staging directory"});
    }
    // A directory cannot be renamed while any object below it is open. Our own handles are closed
    // by contract, but indexers and scanners briefly open freshly written files, so a sharing
    // failure is retried for a bounded time (about 3 s) before it is reported.
    for (int attempt = 0;; ++attempt) {
        const auto renamed = rename_in_place_no_replace(impl_->handle.get(), impl_->staged_name);
        if (renamed) {
            impl_->staged_name.clear();
            return {};
        }
        if (attempt >= 30 || renamed.error().code != ErrorCode::io_error ||
            !transient_sharing_failure(GetLastError())) {
            return renamed;
        }
        Sleep(100);
    }
#else
    return std::unexpected(unsupported());
#endif
}

std::expected<ObjectId, Error> SafeOutputDirectory::identity() const {
#ifdef _WIN32
    if (!impl_ || impl_->discarded) {
        return std::unexpected(closed_directory());
    }
    return object_id(impl_->handle.get());
#else
    return std::unexpected(unsupported());
#endif
}

std::expected<void, Error>
SafeOutputDirectory::require_exact_children(std::span<const std::string> basenames) const {
#ifdef _WIN32
    if (!impl_ || impl_->discarded) {
        return std::unexpected(closed_directory());
    }
    std::vector<std::wstring> expected;
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
    if (auto streams = require_no_named_streams(impl_->handle.get(), true); !streams) {
        return streams;
    }
    std::vector<std::max_align_t> buffer(65536 / sizeof(std::max_align_t));
    const auto bytes = static_cast<DWORD>(buffer.size() * sizeof(std::max_align_t));
    auto info_class = FileFullDirectoryRestartInfo;
    std::size_t found = 0;
    for (;;) {
        if (!GetFileInformationByHandleEx(impl_->handle.get(), info_class, buffer.data(), bytes)) {
            if (GetLastError() == ERROR_NO_MORE_FILES) {
                break;
            }
            return std::unexpected(win_error("list staged directory"));
        }
        info_class = FileFullDirectoryInfo;
        const auto *base = reinterpret_cast<const std::byte *>(buffer.data());
        for (std::size_t offset = 0;;) {
            const auto *info = reinterpret_cast<const FILE_FULL_DIR_INFO *>(base + offset);
            const std::wstring_view name{info->FileName, info->FileNameLength / sizeof(wchar_t)};
            if (name != L"." && name != L"..") {
                // Exact spelling: a name that differs only in case is still a foreign object.
                if (!std::ranges::binary_search(expected, name)) {
                    return std::unexpected(
                        staged_changed("unexpected entry " + ascii_for_message(name)));
                }
                ++found;
            }
            if (info->NextEntryOffset == 0) {
                break;
            }
            offset += info->NextEntryOffset;
        }
    }
    if (found != expected.size()) {
        return std::unexpected(staged_changed("entry missing"));
    }
    return {};
#else
    (void)basenames;
    return std::unexpected(unsupported());
#endif
}

std::expected<void, Error> SafeOutputDirectory::verify_owned_file(std::string_view basename,
                                                                  const ObjectId &expected,
                                                                  std::uint64_t size,
                                                                  std::uint32_t crc32c,
                                                                  std::stop_token stop) const {
#ifdef _WIN32
    if (!impl_ || impl_->discarded) {
        return std::unexpected(closed_directory());
    }
    auto name = leaf(basename);
    if (!name) {
        return std::unexpected(name.error());
    }
    // Sharing only reads: an existing writer or deleter makes the open fail, and none can start
    // while the file is being re-read.
    HANDLE raw = INVALID_HANDLE_VALUE;
    const auto status =
        nt_relative(impl_->handle.get(), *name, FILE_READ_DATA | FILE_READ_ATTRIBUTES | SYNCHRONIZE,
                    FILE_SHARE_READ, FILE_OPEN,
                    FILE_SYNCHRONOUS_IO_NONALERT | FILE_OPEN_REPARSE_POINT |
                        FILE_NON_DIRECTORY_FILE | FILE_SEQUENTIAL_ONLY,
                    0, OBJ_CASE_INSENSITIVE | OBJ_DONT_REPARSE, raw);
    if (status < 0) {
        return std::unexpected(nt_error("open staged file for verification", status));
    }
    const Handle file{raw};
    if (auto verified = verify_object(raw, impl_->volume, false); !verified) {
        return verified;
    }
    const auto id = object_id(raw);
    if (!id) {
        return std::unexpected(id.error());
    }
    if (*id != expected) {
        return std::unexpected(staged_changed("file replaced: " + ascii_for_message(*name)));
    }
    FILE_STANDARD_INFO standard{};
    if (!GetFileInformationByHandleEx(raw, FileStandardInfo, &standard, sizeof(standard))) {
        return std::unexpected(win_error("query staged file"));
    }
    if (standard.NumberOfLinks != 1 || standard.DeletePending || standard.EndOfFile.QuadPart < 0 ||
        static_cast<std::uint64_t>(standard.EndOfFile.QuadPart) != size) {
        return std::unexpected(staged_changed("file size or links: " + ascii_for_message(*name)));
    }
    if (auto streams = require_no_named_streams(raw, false); !streams) {
        return streams;
    }
    Crc32c crc;
    std::vector<std::byte> buffer(static_cast<std::size_t>((std::min)(size, std::uint64_t{65536})));
    for (auto remaining = size; remaining != 0;) {
        if (stop.stop_requested()) {
            return std::unexpected(Error{ErrorCode::cancelled, "operation cancelled"});
        }
        DWORD read = 0;
        const auto count = static_cast<DWORD>((std::min)(remaining, std::uint64_t{buffer.size()}));
        if (!ReadFile(raw, buffer.data(), count, &read, nullptr) || read == 0 || read > count) {
            return std::unexpected(win_error("re-read staged file"));
        }
        crc.update(std::span{buffer}.first(read));
        remaining -= read;
    }
    if (crc.value() != crc32c) {
        return std::unexpected(staged_changed("file content: " + ascii_for_message(*name)));
    }
    return {};
#else
    (void)basename;
    (void)expected;
    (void)size;
    (void)crc32c;
    (void)stop;
    return std::unexpected(unsupported());
#endif
}

std::expected<SafeOutputDirectory, Error>
SafeOutputDirectory::open_owned_child(std::string_view basename, const ObjectId &expected) const {
#ifdef _WIN32
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
    auto child = relative_open(impl_->handle.get(), impl_->volume, *name, true, false, true);
    if (!child) {
        return std::unexpected(child.error());
    }
    const auto id = object_id(child->get());
    if (!id) {
        return std::unexpected(id.error());
    }
    if (*id != expected) {
        return std::unexpected(
            Error{ErrorCode::unsafe_path, "output directory was replaced by another object"});
    }
    return SafeOutputDirectory{std::make_shared<Impl>(std::move(*child), impl_, impl_->volume,
                                                      impl_->depth + 1,
                                                      impl_->path_units + 1 + name->size(), false)};
#else
    (void)basename;
    (void)expected;
    return std::unexpected(unsupported());
#endif
}

std::expected<void, Error> SafeOutputDirectory::remove_owned_child(std::string_view basename,
                                                                   const ObjectId &expected,
                                                                   bool directory) const {
#ifdef _WIN32
    if (!impl_ || impl_->discarded) {
        return std::unexpected(closed_directory());
    }
    auto name = leaf(basename);
    if (!name) {
        return std::unexpected(name.error());
    }
    HANDLE raw = INVALID_HANDLE_VALUE;
    const auto status =
        nt_relative(impl_->handle.get(), *name, DELETE | FILE_READ_ATTRIBUTES | SYNCHRONIZE,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, FILE_OPEN,
                    FILE_SYNCHRONOUS_IO_NONALERT | FILE_OPEN_REPARSE_POINT |
                        (directory ? FILE_DIRECTORY_FILE : FILE_NON_DIRECTORY_FILE),
                    0, OBJ_CASE_INSENSITIVE | OBJ_DONT_REPARSE, raw);
    if (status < 0) {
        return std::unexpected(nt_error("open object for removal", status));
    }
    const Handle handle{raw};
    if (auto verified = verify_object(raw, impl_->volume, directory); !verified) {
        return verified;
    }
    const auto id = object_id(raw);
    if (!id) {
        return std::unexpected(id.error());
    }
    if (*id != expected) {
        return std::unexpected(
            Error{ErrorCode::unsafe_path, "refusing to remove an object this run did not create"});
    }
    return directory ? mark_delete_directory(raw) : mark_delete(raw);
#else
    (void)basename;
    (void)expected;
    (void)directory;
    return std::unexpected(unsupported());
#endif
}

std::expected<void, Error> SafeOutputDirectory::discard_empty() {
#ifdef _WIN32
    if (!impl_ || !impl_->created || impl_->discarded) {
        return std::unexpected(
            Error{ErrorCode::unsafe_path, "cannot discard an existing or discarded directory"});
    }
    const auto result = mark_delete_directory(impl_->handle.get());
    if (result) {
        impl_->discarded = true;
    }
    return result;
#else
    return std::unexpected(unsupported());
#endif
}

SafeOutputFile::SafeOutputFile(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
SafeOutputFile::SafeOutputFile(SafeOutputFile &&) noexcept = default;
SafeOutputFile &SafeOutputFile::operator=(SafeOutputFile &&) noexcept = default;
SafeOutputFile::~SafeOutputFile() = default;

const ObjectId &SafeOutputFile::identity() const noexcept { return impl_->id; }

std::uint64_t SafeOutputFile::bytes_written() const noexcept {
#ifdef _WIN32
    return impl_->written;
#else
    return 0;
#endif
}

std::uint32_t SafeOutputFile::checksum() const noexcept {
#ifdef _WIN32
    return impl_->crc.value();
#else
    return 0;
#endif
}

std::expected<void, Error> SafeOutputFile::append(std::span<const std::byte> bytes) {
#ifdef _WIN32
    if (!impl_ || impl_->finished) {
        return std::unexpected(closed_session());
    }
    const auto *faults = internal::active_write_faults();
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const auto requested =
            static_cast<DWORD>((std::min)(bytes.size() - offset, std::size_t{65536}));
        auto count = requested;
        if (faults != nullptr && faults->max_bytes_per_write != 0) {
            count = static_cast<DWORD>((std::min)(std::size_t{count}, faults->max_bytes_per_write));
        }
        DWORD written = 0;
        BOOL ok = FALSE;
        if (faults != nullptr && impl_->written >= faults->fail_write_at) {
            SetLastError(ERROR_DISK_FULL);
        } else {
            ok = WriteFile(impl_->file.get(), bytes.data() + offset, count, &written, nullptr);
        }
        // A short write (written < requested) is legal; loop until everything is written.
        if (!ok || written == 0 || written > requested) {
            return impl_->fail(win_error("write temporary file"));
        }
        impl_->crc.update(bytes.subspan(offset, written));
        offset += written;
        impl_->written += written;
        if (faults != nullptr && faults->after_write) {
            faults->after_write(impl_->written);
        }
    }
    return {};
#else
    (void)bytes;
    return std::unexpected(unsupported());
#endif
}

std::expected<void, Error> SafeOutputFile::commit(std::uint64_t expected_size,
                                                  std::stop_token stop) {
#ifdef _WIN32
    if (!impl_ || impl_->finished) {
        return std::unexpected(closed_session());
    }
    const auto *faults = internal::active_write_faults();
    const auto file = impl_->file.get();
    if (impl_->written != expected_size) {
        return impl_->fail({ErrorCode::integrity_error, "output size differs from expected size"});
    }
    if (faults != nullptr && faults->fail_flush) {
        return impl_->fail(win_error("flush temporary file", ERROR_WRITE_FAULT));
    }
    if (!FlushFileBuffers(file)) {
        return impl_->fail(win_error("flush temporary file"));
    }
    LARGE_INTEGER zero{};
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart < 0 ||
        static_cast<std::uint64_t>(size.QuadPart) != impl_->written ||
        !SetFilePointerEx(file, zero, nullptr, FILE_BEGIN)) {
        return impl_->fail(win_error("verify temporary file size/position"));
    }
    // Re-read what reached the file system and compare it with the CRC32C of what was appended.
    Crc32c read_back;
    std::vector<std::byte> buffer(
        static_cast<std::size_t>((std::min)(impl_->written, std::uint64_t{65536})));
    for (auto remaining = impl_->written; remaining != 0;) {
        DWORD read = 0;
        const auto count = static_cast<DWORD>((std::min)(remaining, std::uint64_t{buffer.size()}));
        if (!ReadFile(file, buffer.data(), count, &read, nullptr) || read == 0 || read > count) {
            return impl_->fail(win_error("read back temporary file"));
        }
        if (faults != nullptr && faults->corrupt_read_back) {
            buffer[0] ^= std::byte{1};
        }
        read_back.update(std::span{buffer}.first(read));
        remaining -= read;
    }
    if (read_back.value() != impl_->crc.value()) {
        return impl_->fail({ErrorCode::integrity_error, "temporary file byte verification failed"});
    }
    if (stop.stop_requested()) {
        return impl_->fail({ErrorCode::cancelled, "write cancelled before commit"});
    }
    if (faults != nullptr && faults->fail_commit) {
        return impl_->fail(win_error("commit without replacement", ERROR_DISK_FULL));
    }
    if (const auto renamed = rename_in_place_no_replace(file, impl_->final_name); !renamed) {
        return impl_->fail(renamed.error());
    }
    impl_->file.retain();
    impl_->finished = true;
    return {};
#else
    (void)expected_size;
    (void)stop;
    return std::unexpected(unsupported());
#endif
}
} // namespace seedarc::experimental
