// SPDX-License-Identifier: GPL-3.0-or-later
#include "safe_output_internal.hpp"
#include "seedarc/archive.hpp"
#include "seedarc/safe_output.hpp"

#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <winioctl.h>

namespace fs = std::filesystem;
using namespace seedarc::experimental;

namespace {
int failures = 0;
void check(bool condition, const char *description) {
    if (!condition) {
        std::cerr << "FAIL: " << description << "; Windows error=" << GetLastError() << '\n';
        ++failures;
    }
}

bool set_junction(const fs::path &link, const fs::path &target, DWORD access = GENERIC_WRITE) {
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

std::string read(const fs::path &path) {
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{}};
}

bool no_temporaries(const fs::path &directory) {
    for (const auto &item : fs::directory_iterator(directory)) {
        if (item.path().filename().wstring().starts_with(L".seedarc-")) {
            return false;
        }
    }
    return true;
}

std::size_t components_below_drive(const fs::path &path) {
    const auto relative = path.relative_path();
    return static_cast<std::size_t>(std::distance(relative.begin(), relative.end()));
}

void budget_and_volume_unit_tests() {
    using namespace seedarc::experimental::internal;
    check(is_volume_guid_root(L"\\\\?\\Volume{0a1b2c3d-4e5f-6789-abcd-ef0123456789}\\"),
          "volume GUID root accepted");
    check(is_volume_guid_root(L"\\\\?\\Volume{0A1B2C3D-4E5F-6789-ABCD-EF0123456789}\\"),
          "uppercase volume GUID root accepted");
    for (const auto *path :
         {L"\\\\?\\Volume{0a1b2c3d-4e5f-6789-abcd-ef0123456789}\\Users\\", // SUBST target
          L"\\\\?\\Volume{0a1b2c3d-4e5f-6789-abcd-ef0123456789}",          // no root slash
          L"\\\\?\\Volume{0a1b2c3d-4e5f-6789-abcd-ef012345678}\\",         // short GUID
          L"\\\\?\\Volume{0a1b2c3d_4e5f-6789-abcd-ef0123456789}\\",        // bad dash
          L"\\\\?\\Volume{0a1b2c3d-4e5f-6789-abcd-ef012345678g}\\",        // bad hex
          L"\\\\?\\UNC\\server\\share\\", L"C:\\", L""}) {
        check(!is_volume_guid_root(path), "non-root final path rejected");
    }
    check(admit_output_component(max_output_depth - 1, 2, 1, true).has_value(),
          "last allowed directory depth");
    const auto too_deep = admit_output_component(max_output_depth, 2, 1, true);
    check(!too_deep && too_deep.error().code == seedarc::ErrorCode::resource_limit,
          "directory depth limit");
    check(admit_output_component(max_output_depth, 2, 1, false).has_value(),
          "file allowed in deepest directory");
    check(admit_output_component(0, max_output_path_units - 256, 255, false).has_value(),
          "path units exactly at budget");
    check(!admit_output_component(0, max_output_path_units - 256, 256, false),
          "path units one over budget");
    check(!admit_output_component(0, max_output_path_units, 0, false), "path already at budget");
    check(!admit_output_component(0, static_cast<std::size_t>(-1), 1, false),
          "path units overflow guarded");
    check(!admit_output_component(0, 2, static_cast<std::size_t>(-1), false),
          "component units overflow guarded");
}

std::vector<std::byte> pattern(std::size_t size) {
    std::vector<std::byte> bytes(size);
    for (std::size_t i = 0; i < size; ++i) {
        bytes[i] = static_cast<std::byte>((i * 131 + 7) & 0xff);
    }
    return bytes;
}

void fault_injection_tests(const SafeOutputDirectory &directory, const fs::path &output) {
    using seedarc::experimental::internal::ScopedWriteFaults;
    using seedarc::experimental::internal::WriteFaults;
    const auto data = pattern(200'000);
    const auto crc = crc32c(data);
    {
        WriteFaults faults;
        faults.max_bytes_per_write = 7;
        const ScopedWriteFaults scope{faults};
        const auto result = directory.write_new_file("short-writes", data, crc);
        check(result.has_value(), "short writes are completed by the write loop");
        const auto stored = read(output / "short-writes");
        check(stored.size() == data.size() &&
                  std::equal(stored.begin(), stored.end(), data.begin(),
                             [](char a, std::byte b) { return static_cast<std::byte>(a) == b; }),
              "short-write result is byte exact");
    }
    const auto expect_clean_failure = [&](const char *name, const WriteFaults &faults,
                                          seedarc::ErrorCode code, std::stop_token stop,
                                          const char *description, std::string *message = nullptr) {
        const ScopedWriteFaults scope{faults};
        const auto result = directory.write_new_file(name, data, crc, stop);
        check(!result && result.error().code == code, description);
        check(!fs::exists(output / name), "failed write leaves no final file");
        check(no_temporaries(output), "failed write leaves no temporary file");
        if (message != nullptr && !result) {
            *message = result.error().message;
        }
    };
    {
        WriteFaults faults;
        faults.fail_write_at = 70'000; // after one full 64 KiB chunk reached the disk
        expect_clean_failure("disk-full", faults, seedarc::ErrorCode::io_error, {},
                             "I/O failure in the middle of a write");
    }
    {
        std::stop_source stop;
        std::uint64_t observed = 0;
        WriteFaults faults;
        faults.after_write = [&](std::uint64_t written) {
            observed = written;
            if (written >= 65'536) {
                stop.request_stop();
            }
        };
        expect_clean_failure("cancel-mid-write", faults, seedarc::ErrorCode::cancelled,
                             stop.get_token(), "cancellation after partial write");
        check(observed >= 65'536 && observed < data.size(), "cancellation happened mid-write");
    }
    {
        // Stop requested after the last byte: write, flush and read-back succeed, commit must not.
        std::stop_source stop;
        WriteFaults faults;
        faults.after_write = [&](std::uint64_t written) {
            if (written == data.size()) {
                stop.request_stop();
            }
        };
        std::string message;
        expect_clean_failure("cancel-before-commit", faults, seedarc::ErrorCode::cancelled,
                             stop.get_token(), "cancellation before commit", &message);
        check(message.find("before commit") != std::string::npos,
              "cancellation reached the pre-commit check");
    }
    {
        WriteFaults faults;
        faults.fail_flush = true;
        expect_clean_failure("flush-fails", faults, seedarc::ErrorCode::io_error, {},
                             "flush failure");
    }
    {
        WriteFaults faults;
        faults.corrupt_read_back = true;
        expect_clean_failure("readback-corrupt", faults, seedarc::ErrorCode::integrity_error, {},
                             "read-back verification failure");
    }
    {
        WriteFaults faults;
        faults.fail_commit = true;
        expect_clean_failure("commit-fails", faults, seedarc::ErrorCode::io_error, {},
                             "commit failure");
    }
    {
        WriteFaults faults;
        faults.fail_write_at = 0;
        faults.fail_explicit_cleanup = true;
        std::string message;
        expect_clean_failure("cleanup-fails", faults, seedarc::ErrorCode::io_error, {},
                             "write failure with failed explicit cleanup", &message);
        check(message.find("retried at handle close") != std::string::npos &&
                  message.find(".seedarc-") != std::string::npos,
              "failed cleanup is reported with the temporary name");
    }
    {
        // Faults are thread-local: another thread's write is unaffected.
        WriteFaults faults;
        faults.fail_write_at = 0;
        const ScopedWriteFaults scope{faults};
        bool other_thread_ok = false;
        std::thread{[&] {
            other_thread_ok = directory.write_new_file("other-thread", data, crc).has_value();
        }}.join();
        check(other_thread_ok, "fault injection does not leak across threads");
    }
}

void depth_limit_test(const SafeOutputDirectory &directory, const fs::path &output) {
    const auto start = components_below_drive(output);
    std::vector<SafeOutputDirectory> chain;
    chain.push_back(directory);
    std::expected<SafeOutputDirectory, seedarc::Error> next = chain.back().create_child("d");
    while (next && chain.size() <= max_output_depth) {
        chain.push_back(std::move(*next));
        next = chain.back().create_child("d");
    }
    check(!next && next.error().code == seedarc::ErrorCode::resource_limit &&
              next.error().message.find("depth") != std::string::npos,
          "real directory chain stops at the depth limit");
    check(chain.size() - 1 == max_output_depth - start, "depth limit counts from the drive root");
    const std::array<std::byte, 1> one{std::byte{'x'}};
    check(chain.back().write_new_file("leaf", one, crc32c(one)).has_value(),
          "file can still be written at maximum depth");
    // Unwind: remove the leaf file, then discard and close each directory deepest first.
    {
        auto leaf_path = output;
        for (std::size_t i = 1; i < chain.size(); ++i) {
            leaf_path /= "d";
        }
        std::error_code ignored;
        fs::remove(fs::path{L"\\\\?\\" + (leaf_path / "leaf").wstring()}, ignored);
        check(!ignored, "remove leaf file at maximum depth");
    }
    while (chain.size() > 1) {
        check(chain.back().discard_empty().has_value(), "discard chain directory");
        chain.pop_back();
    }
    check(!fs::exists(output / "d"), "depth chain fully removed");
}
} // namespace

int main(int argc, char *argv[]) {
    if (argc != 2) {
        return 1;
    }
    const auto base = fs::absolute(argv[1]).lexically_normal();
    fs::create_directories(base);
    const auto root = base / ("m1-io-" + std::to_string(GetCurrentProcessId()) + "-" +
                              std::to_string(GetTickCount64()));
    if (!fs::create_directory(root)) {
        return 1;
    }
    // Every cleanup below names a known file or empty directory; never recursively delete.
    const auto outside = root / "outside";
    const auto output = root / "output";
    fs::create_directory(outside);
    fs::create_directory(output);
    {
        std::ofstream{outside / "sentinel", std::ios::binary} << "unchanged";
    }
    {
        std::ofstream{output / "existing", std::ios::binary} << "original";
    }
    const bool hardlink_created = CreateHardLinkW((output / "hardlink").c_str(),
                                                  (outside / "sentinel").c_str(), nullptr) != 0;
    check(hardlink_created, "create real hardlink fixture");
    fs::create_directory(output / "junction");
    const bool junction_created = set_junction(output / "junction", outside);
    check(junction_created, "create real junction fixture");

    const std::array<std::byte, 3> bytes{std::byte{'a'}, std::byte{'b'}, std::byte{'c'}};
    const auto crc = crc32c(bytes);
    // Regression guard: kernel32 rename resolves bare names against the current directory.
    // Point it at `outside` so any such resolution is detected instead of polluting the build tree.
    const auto previous_cwd = fs::current_path();
    fs::current_path(outside);
    budget_and_volume_unit_tests();
    {
        std::ofstream{outside / "movable", std::ios::binary} << "moved";
    }
    // Another program already holding the output directory with write access must not block
    // pinning it (directories are pinned with read/write sharing, never delete sharing).
    const auto writer = CreateFileW(output.c_str(), FILE_ADD_FILE | SYNCHRONIZE,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                    OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    check(writer != INVALID_HANDLE_VALUE, "open concurrent directory writer handle");
    {
        auto directory = SafeOutputDirectory::open_existing(output);
        if (!directory) {
            std::cerr << "open root: " << directory.error().message << '\n';
            return 1;
        }
        const auto written = directory->write_new_file("new", bytes, crc);
        if (!written) {
            std::cerr << "write new: " << written.error().message << '\n';
        }
        check(written.has_value(), "validated atomic no-replace commit");
        check(read(output / "new") == "abc", "persist exact bytes");
        check(directory->write_new_file("empty", {}, 0).has_value(), "empty file commit");
        check(!directory->write_new_file("existing", bytes, crc), "existing target rejected");
        check(!directory->write_new_file("hardlink", bytes, crc),
              "existing hardlink target rejected");
        check(!directory->write_new_file("junction", bytes, crc),
              "existing junction target rejected");
        check(!directory->write_new_file("../escaped", bytes, crc), "traversal rejected");
        check(!directory->write_new_file("nested/file", bytes, crc),
              "multiple components rejected");
        check(!directory->write_new_file("bad-crc", bytes, crc ^ 1),
              "checksum failure cleans temporary");
        std::stop_source stop;
        stop.request_stop();
        {
            const auto cancelled =
                directory->write_new_file("cancelled", bytes, crc, stop.get_token());
            check(!cancelled && cancelled.error().code == seedarc::ErrorCode::cancelled,
                  "cancel before write reports cancelled");
        }
        check(!directory->discard_empty(), "existing directory cannot be deleted by primitive");
        check(!directory->create_child("existing"), "directory create cannot replace file");
        check(!directory->create_child("junction"), "directory create cannot follow junction");
        {
            auto child = directory->create_child("child");
            check(child.has_value(), "atomic new directory creation");
            if (child) {
                check(!child->write_new_file("bad", bytes, crc ^ 1), "child checksum cleanup");
                check(child->discard_empty().has_value(), "owned empty directory handle cleanup");
                check(!child->write_new_file("after-discard", bytes, crc),
                      "reject discarded handle");
            }
        }
        check(!MoveFileExW(output.c_str(), (root / "renamed").c_str(), 0),
              "pinned parent cannot be renamed");
        check(MoveFileExW((outside / "movable").c_str(), (output / "moved-in").c_str(), 0) != 0,
              "other programs can still move files into a pinned directory");
        fault_injection_tests(*directory, output);
        depth_limit_test(*directory, output);
        check(!SafeOutputDirectory::open_existing(output / "junction"),
              "reject junction in output root");
        check(!SafeOutputDirectory::open_existing(output / ".." / "outside"),
              "reject dot components in root");
        for (const auto name : {"C:relative", "\\\\server\\share", "\\\\?\\C:\\x"}) {
            check(!SafeOutputDirectory::open_existing(name), "reject unsupported root namespace");
        }
        {
            auto mutation = directory->create_child("mutation");
            check(mutation.has_value(), "create reparse mutation target");
            if (mutation) {
                const bool changed =
                    set_junction(output / "mutation", outside, FILE_WRITE_ATTRIBUTES);
                const auto result = mutation->write_new_file("escape", bytes, crc);
                check(!fs::exists(outside / "escape"),
                      "attributes-only reparse change cannot redirect relative write");
                if (changed) {
                    std::cout << "attributes-only reparse mutation succeeded; relative operation "
                              << (result ? "remained on pinned object" : "failed closed") << '\n';
                } else {
                    std::cout << "attributes-only reparse mutation rejected, error="
                              << GetLastError() << '\n';
                }
            }
        }
        for (const auto &item : fs::directory_iterator(output)) {
            check(!item.path().filename().wstring().starts_with(L".seedarc-"),
                  "no temporary leftovers");
        }
        check(!fs::exists(output / "bad-crc") && !fs::exists(output / "cancelled"),
              "no failed final files");
        check(read(output / "existing") == "original", "existing target unchanged");
        check(read(outside / "sentinel") == "unchanged", "outside hardlink sentinel unchanged");
        for (const auto &item : fs::directory_iterator(outside)) {
            check(item.path().filename() == "sentinel",
                  "no output resolved against the process current directory");
        }
    }
    fs::current_path(previous_cwd);
    if (writer != INVALID_HANDLE_VALUE) {
        CloseHandle(writer);
    }
    // Non-recursive cleanup after all pinned handles have closed; reparse links removed as links.
    const auto mutation_attrs = GetFileAttributesW((output / "mutation").c_str());
    if (mutation_attrs != INVALID_FILE_ATTRIBUTES &&
        (mutation_attrs & FILE_ATTRIBUTE_REPARSE_POINT) == 0) {
        fs::remove(output / "mutation" / "escape");
    }
    RemoveDirectoryW((output / "mutation").c_str());
    RemoveDirectoryW((output / "junction").c_str());
    for (const auto name :
         {"new", "empty", "existing", "hardlink", "moved-in", "short-writes", "other-thread"}) {
        fs::remove(output / name);
    }
    fs::remove(outside / "sentinel");
    fs::remove(outside / "movable"); // only present if the move-in check failed
    fs::remove(output);
    fs::remove(outside);
    check(fs::remove(root), "remove only owned empty test root");
    return failures == 0 ? 0 : 1;
}
