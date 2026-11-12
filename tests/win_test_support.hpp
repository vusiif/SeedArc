// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Windows-only helpers shared by the file system tests.

#include <cstddef>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <winioctl.h>

namespace seedarc_test {
// Turns the existing empty directory `link` into a junction to `target`.
inline bool set_junction(const std::filesystem::path &link, const std::filesystem::path &target,
                         DWORD access = GENERIC_WRITE) {
    const auto substitute = std::wstring{L"\\??\\"} + target.wstring();
    const auto print = target.wstring();
    const auto sub_bytes = substitute.size() * sizeof(wchar_t);
    const auto print_bytes = print.size() * sizeof(wchar_t);
    const auto data_size = 8 + sub_bytes + sizeof(wchar_t) + print_bytes + sizeof(wchar_t);
    std::vector<std::byte> data(8 + data_size);
    const DWORD tag = IO_REPARSE_TAG_MOUNT_POINT;
    const auto length = static_cast<USHORT>(data_size);
    const USHORT sub_offset = 0;
    const auto sub_length = static_cast<USHORT>(sub_bytes);
    const auto print_offset = static_cast<USHORT>(sub_bytes + sizeof(wchar_t));
    const auto print_length = static_cast<USHORT>(print_bytes);
    std::memcpy(data.data(), &tag, sizeof(tag));
    std::memcpy(data.data() + 4, &length, sizeof(length));
    std::memcpy(data.data() + 8, &sub_offset, sizeof(sub_offset));
    std::memcpy(data.data() + 10, &sub_length, sizeof(sub_length));
    std::memcpy(data.data() + 12, &print_offset, sizeof(print_offset));
    std::memcpy(data.data() + 14, &print_length, sizeof(print_length));
    std::memcpy(data.data() + 16, substitute.data(), sub_bytes);
    std::memcpy(data.data() + 16 + print_offset, print.data(), print_bytes);
    const auto handle = CreateFileW(
        link.c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return false;
    }
    DWORD returned = 0;
    const bool result =
        DeviceIoControl(handle, FSCTL_SET_REPARSE_POINT, data.data(),
                        static_cast<DWORD>(data.size()), nullptr, 0, &returned, nullptr) != 0;
    const auto error = GetLastError();
    CloseHandle(handle);
    SetLastError(error);
    return result;
}
} // namespace seedarc_test
