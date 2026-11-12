// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Internal Windows helpers shared by the safe output and input enumeration code.
// Not installed and not part of the public API.

#ifdef _WIN32
#include "seedarc/error.hpp"
#include "seedarc/safe_output.hpp"

#include <expected>
#include <string>
#include <string_view>
#include <utility>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <winternl.h>

namespace seedarc::experimental::internal {
// Owns a handle. With `cleanup`, the object is marked for deletion when the handle closes unless
// retain() was called (used for temporaries and freshly created directories).
class Handle {
  public:
    explicit Handle(HANDLE value = INVALID_HANDLE_VALUE, bool cleanup = false) noexcept
        : value_(value), cleanup_(cleanup) {}
    ~Handle() {
        if (valid()) {
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
    [[nodiscard]] HANDLE get() const noexcept { return value_; }
    [[nodiscard]] bool valid() const noexcept {
        return value_ != INVALID_HANDLE_VALUE && value_ != nullptr;
    }
    void retain() noexcept { cleanup_ = false; }

  private:
    HANDLE value_;
    bool cleanup_;
};

inline constexpr NTSTATUS status_object_name_not_found = static_cast<NTSTATUS>(0xC0000034L);

[[nodiscard]] Error win_error(std::string stage, DWORD code = GetLastError());
[[nodiscard]] Error nt_error(std::string stage, NTSTATUS status);

// Only for names this library generated or already validated; non-ASCII becomes '?'.
[[nodiscard]] std::string ascii_for_message(std::wstring_view text);

// NtCreateFile relative to `parent` with a single-component name. Returns the NTSTATUS; on
// success `out` receives the raw handle (the caller wraps it immediately). `security` applies only
// when the call creates the object.
[[nodiscard]] NTSTATUS nt_relative(HANDLE parent, std::wstring_view name, ACCESS_MASK access,
                                   ULONG share, ULONG disposition, ULONG options, ULONG attributes,
                                   ULONG object_flags, HANDLE &out,
                                   PSECURITY_DESCRIPTOR security = nullptr) noexcept;

// Volume serial number plus 128-bit file identifier of an open object.
[[nodiscard]] std::expected<ObjectId, Error> object_id(HANDLE handle);
} // namespace seedarc::experimental::internal
#endif
