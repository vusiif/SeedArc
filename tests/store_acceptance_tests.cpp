// SPDX-License-Identifier: GPL-3.0-or-later
// M2 acceptance regressions: a staged tree changed between verification and publication must not
// be published (P1), and cancellation must be honoured by runs with no file data (P2).
#include "safe_output_internal.hpp"
#include "seedarc/store_archive.hpp"

#include <array>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <aclapi.h>

namespace fs = std::filesystem;
using namespace seedarc::experimental;
using namespace seedarc::experimental::internal;

namespace {
int failures = 0;

void check(bool condition, const std::string &description) {
    std::cout << (condition ? "ok   " : "FAIL ") << description << '\n';
    if (!condition) {
        ++failures;
    }
}

std::string read(const fs::path &path) {
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{}};
}

void write(const fs::path &path, const std::string &data) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream << data;
    if (!stream) {
        throw std::runtime_error("cannot write acceptance fixture");
    }
}

fs::path find_staging(const fs::path &directory) {
    for (const auto &child : fs::directory_iterator(directory)) {
        if (child.is_directory() && child.path().filename().wstring().starts_with(L".seedarc-")) {
            return child.path();
        }
    }
    return {};
}

// Extracts `archive` into `root / "restored"` while `tamper` changes the staging directory once,
// right after the first file ("a") has been committed and while a later file is being written.
void tamper_case(const fs::path &root, const fs::path &archive, const std::string &label,
                 const std::function<void(const fs::path &)> &tamper) {
    fs::create_directory(root);
    bool injected = false;
    WriteFaults faults;
    faults.after_write = [&](std::uint64_t) {
        if (injected) {
            return;
        }
        if (const auto staging = find_staging(root);
            !staging.empty() && fs::is_regular_file(staging / "a")) {
            tamper(staging);
            injected = true;
        }
    };
    const ScopedWriteFaults scope{faults};
    const auto extracted = extract_archive(archive, root / "restored");
    check(injected, label + ": tampering was injected");
    check(!extracted && extracted.error().code == seedarc::ErrorCode::integrity_error,
          label + ": extraction fails with integrity_error");
    if (!extracted) {
        std::cout << "     (" << extracted.error().message << ")\n";
    }
    check(!fs::exists(root / "restored"), label + ": target never appears");
}

// The published tree must carry the staging DACL: only the token user, SYSTEM and Administrators
// may appear. The root's DACL is protected; objects below it inherit from it.
bool private_dacl(const fs::path &path, bool root) {
    PACL dacl = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (GetNamedSecurityInfoW(path.c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr,
                              nullptr, &dacl, nullptr, &descriptor) != ERROR_SUCCESS) {
        return false;
    }
    SECURITY_DESCRIPTOR_CONTROL control = 0;
    DWORD revision = 0;
    bool ok = dacl != nullptr && GetSecurityDescriptorControl(descriptor, &control, &revision) &&
              ((control & SE_DACL_PROTECTED) != 0) == root && dacl->AceCount != 0;
    HANDLE token = nullptr;
    std::array<std::max_align_t, 64> user{};
    DWORD needed = 0;
    ok = ok && OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token) &&
         GetTokenInformation(token, TokenUser, user.data(), sizeof(user), &needed);
    if (token != nullptr) {
        CloseHandle(token);
    }
    std::array<std::max_align_t, 16> system_sid{};
    std::array<std::max_align_t, 16> admins_sid{};
    DWORD system_size = sizeof(system_sid);
    DWORD admins_size = sizeof(admins_sid);
    ok = ok && CreateWellKnownSid(WinLocalSystemSid, nullptr, system_sid.data(), &system_size) &&
         CreateWellKnownSid(WinBuiltinAdministratorsSid, nullptr, admins_sid.data(), &admins_size);
    for (DWORD i = 0; ok && i < dacl->AceCount; ++i) {
        void *ace = nullptr;
        ok = GetAce(dacl, i, &ace) &&
             static_cast<ACE_HEADER *>(ace)->AceType == ACCESS_ALLOWED_ACE_TYPE;
        if (ok) {
            PSID sid = &static_cast<ACCESS_ALLOWED_ACE *>(ace)->SidStart;
            ok = EqualSid(sid, reinterpret_cast<TOKEN_USER *>(user.data())->User.Sid) ||
                 EqualSid(sid, system_sid.data()) || EqualSid(sid, admins_sid.data());
        }
    }
    LocalFree(descriptor);
    return ok;
}
} // namespace

int main(int argc, char *argv[]) {
    if (argc != 2) {
        return 2;
    }
    const auto base = fs::absolute(argv[1]).lexically_normal();
    fs::create_directories(base);
    const auto work = base / ("m2-acceptance-" + std::to_string(GetCurrentProcessId()) + "-" +
                              std::to_string(GetTickCount64()));
    if (!fs::create_directory(work)) {
        return 2;
    }
    try {
        fs::create_directories(work / "input" / "d");
        fs::create_directory(work / "empty");
        fs::create_directory(work / "empty-file");
        write(work / "input" / "a", "original");
        write(work / "input" / "b", std::string(8192, 'b'));
        write(work / "input" / "d" / "c", "nested");
        write(work / "empty-file" / "e", "");
        CompressOptions options;
        options.chunk_size_bytes = 1024;
        const auto archive = work / "tree.sarc";
        const auto empty_archive = work / "empty.sarc";
        const auto empty_file_archive = work / "empty-file.sarc";
        if (!compress_store(work / "input", archive, options) ||
            !compress_store(work / "empty", empty_archive) ||
            !compress_store(work / "empty-file", empty_file_archive)) {
            std::cerr << "SETUP FAILED: cannot create acceptance archives at " << work << '\n';
            return 2;
        }

        // P1: changes to the staged tree are detected before publication.
        tamper_case(work / "p1-content", archive, "P1 content of a committed file",
                    [](const fs::path &staging) { write(staging / "a", "tampered"); });
        tamper_case(work / "p1-foreign", archive, "P1 foreign entry in the staging root",
                    [](const fs::path &staging) { write(staging / "foreign.txt", "x"); });
        tamper_case(
            work / "p1-stream", archive, "P1 named stream on a committed file",
            [](const fs::path &staging) { write(fs::path{staging / "a"}.concat(":hidden"), "x"); });
        tamper_case(work / "p1-both", archive, "P1 acceptance scenario (content + foreign)",
                    [](const fs::path &staging) {
                        write(staging / "a", "tampered");
                        write(staging / "foreign.txt", "not in archive");
                    });

        // An untouched run still publishes a byte-exact tree with the private DACL.
        const auto restored = work / "restored";
        const auto clean = extract_archive(archive, restored);
        check(clean.has_value() && read(restored / "a") == "original" &&
                  read(restored / "b") == std::string(8192, 'b') &&
                  read(restored / "d" / "c") == "nested",
              "untampered extraction is byte-exact");
        check(clean.has_value() && private_dacl(restored, true) &&
                  private_dacl(restored / "a", false) && private_dacl(restored / "d", false) &&
                  private_dacl(restored / "d" / "c", false),
              "published tree carries only the private staging DACL");
        check(extract_archive(empty_archive, work / "empty-out").has_value() &&
                  fs::is_empty(work / "empty-out"),
              "empty archive extracts to an empty directory");
        check(extract_archive(empty_file_archive, work / "empty-file-out").has_value() &&
                  fs::file_size(work / "empty-file-out" / "e") == 0,
              "archive with one empty file extracts");

        // P2: a cancellation is honoured even when there is no file data to process.
        std::stop_source stop;
        stop.request_stop();
        for (const auto &[label, input] : std::vector<std::pair<std::string, fs::path>>{
                 {"empty archive", empty_archive}, {"empty-file archive", empty_file_archive}}) {
            const auto target = work / ("cancelled-" + input.stem().string());
            const auto extracted = extract_archive(input, target, {}, stop.get_token());
            check(!extracted && extracted.error().code == seedarc::ErrorCode::cancelled,
                  "P2 pre-cancelled extract of " + label + " reports cancelled");
            check(!fs::exists(target) && find_staging(work).empty(),
                  "P2 pre-cancelled extract of " + label + " leaves nothing");
            const auto verified = verify_archive(input, {}, stop.get_token());
            check(!verified && verified.error().code == seedarc::ErrorCode::cancelled,
                  "P2 pre-cancelled verify of " + label + " reports cancelled");
        }
    } catch (const std::exception &error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        ++failures;
    }

    // This test creates only regular files/directories. Check the exact absolute boundary,
    // reject any reparse object, then remove only this invocation's private fixture.
    bool safe = work.parent_path() == base &&
                (GetFileAttributesW(work.c_str()) & FILE_ATTRIBUTE_REPARSE_POINT) == 0;
    for (const auto &item : fs::recursive_directory_iterator(work)) {
        const auto attributes = GetFileAttributesW(item.path().c_str());
        safe = safe && attributes != INVALID_FILE_ATTRIBUTES &&
               (attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0;
    }
    if (safe) {
        fs::remove_all(work);
    } else {
        std::cerr << "fixture retained at " << work << '\n';
    }
    std::cout << (failures == 0 ? "all acceptance checks passed" : "acceptance checks failed")
              << '\n';
    return failures == 0 ? 0 : 1;
}
