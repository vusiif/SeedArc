// SPDX-License-Identifier: GPL-3.0-or-later
// M2 acceptance regressions: a staged tree changed between verification and publication must not
// be published (P1), and cancellation must be honoured by runs with no file data (P2).
#include "safe_output_internal.hpp"
#include "seedarc/store_archive.hpp"
#include "test_fixture.hpp"

#include <array>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <aclapi.h>
#else
#include <sys/stat.h>
#include <sys/xattr.h>
#include <unistd.h>
#endif

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
        if (child.is_directory() && child.path().filename().string().starts_with(".seedarc-")) {
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

#ifdef _WIN32
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
#else
// POSIX counterpart of the private staging DACL (ADR-0004): owned by this user, directories
// exactly 0700 and files exactly 0600, no user extended attributes.
bool private_mode(const fs::path &path, bool) {
    struct stat status{};
    if (::lstat(path.c_str(), &status) != 0 || status.st_uid != ::geteuid()) {
        return false;
    }
    const auto expected = S_ISDIR(status.st_mode) ? 0700 : 0600;
    return (status.st_mode & 07777) == static_cast<mode_t>(expected) &&
           (S_ISDIR(status.st_mode) || S_ISREG(status.st_mode));
}
#endif
} // namespace

int main(int argc, char *argv[]) {
    if (argc != 2) {
        return 2;
    }
    const auto base = fs::absolute(argv[1]).lexically_normal();
    fs::create_directories(base);
    const auto work = base / ("m2-acceptance-" + test_fixture::unique_suffix());
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
        if (!compress_archive(work / "input", archive, options) ||
            !compress_archive(work / "empty", empty_archive) ||
            !compress_archive(work / "empty-file", empty_file_archive)) {
            std::cerr << "SETUP FAILED: cannot create acceptance archives at " << work << '\n';
            return 2;
        }

        // P1: changes to the staged tree are detected before publication.
        tamper_case(work / "p1-content", archive, "P1 content of a committed file",
                    [](const fs::path &staging) { write(staging / "a", "tampered"); });
        tamper_case(work / "p1-foreign", archive, "P1 foreign entry in the staging root",
                    [](const fs::path &staging) { write(staging / "foreign.txt", "x"); });
#ifdef _WIN32
        tamper_case(
            work / "p1-stream", archive, "P1 named stream on a committed file",
            [](const fs::path &staging) { write(fs::path{staging / "a"}.concat(":hidden"), "x"); });
#else
        // POSIX counterparts of a named stream, and Linux-only ways to keep a hold on content.
        if (::setxattr(work.c_str(), "user.seedarc-probe", "x", 1, 0) == 0) {
            ::removexattr(work.c_str(), "user.seedarc-probe");
            tamper_case(work / "p1-xattr", archive, "P1 user xattr on a committed file",
                        [](const fs::path &staging) {
                            ::setxattr((staging / "a").c_str(), "user.hidden", "x", 1, 0);
                        });
        } else {
            std::cout << "skip P1 user xattr: not supported by this file system\n";
        }
        tamper_case(work / "p1-hardlink", archive, "P1 extra hard link to a committed file",
                    [&work](const fs::path &staging) {
                        fs::create_hard_link(staging / "a", work / "p1-hardlink-kept");
                    });
        tamper_case(work / "p1-mode", archive, "P1 permissions widened on a committed file",
                    [](const fs::path &staging) {
                        fs::permissions(staging / "a", fs::perms::others_read,
                                        fs::perm_options::add);
                    });
        tamper_case(work / "p1-replace", archive, "P1 committed file replaced (same content)",
                    [](const fs::path &staging) {
                        fs::rename(staging / "a", staging / "a.orig");
                        write(staging / "a", "original");
                        fs::permissions(staging / "a",
                                        fs::perms::owner_read | fs::perms::owner_write,
                                        fs::perm_options::replace);
                        fs::remove(staging / "a.orig");
                    });
        // A symbolic link substituted for a staged file is covered by filesystem.safe_output
        // (rollback correctly keeps the link, which this fixture's cleanup would then refuse).
#endif
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
#ifdef _WIN32
        check(clean.has_value() && private_dacl(restored, true) &&
                  private_dacl(restored / "a", false) && private_dacl(restored / "d", false) &&
                  private_dacl(restored / "d" / "c", false),
              "published tree carries only the private staging DACL");
#else
        check(clean.has_value() && private_mode(restored, true) &&
                  private_mode(restored / "a", false) && private_mode(restored / "d", false) &&
                  private_mode(restored / "d" / "c", false),
              "published tree is private (owner only: 0700 / 0600)");
#endif
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
    // reparse_free() also rejects a reparse point at `work` itself.
    const bool safe = work.parent_path() == base && test_fixture::reparse_free(work);
    // Bounded removal instead of remove_all() (libstdc++ can spin on a vanishing entry, DEV-011).
    const auto cleanup = safe ? test_fixture::remove_tree(work) : std::error_code{};
    if (!safe || cleanup) {
        std::cerr << "fixture retained at " << work << (cleanup ? ": " + cleanup.message() : "")
                  << '\n';
    }
    std::cout << (failures == 0 ? "all acceptance checks passed" : "acceptance checks failed")
              << '\n';
    return failures == 0 ? 0 : 1;
}
