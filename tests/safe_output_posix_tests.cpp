// SPDX-License-Identifier: GPL-3.0-or-later
// Linux counterpart of safe_output_tests.cpp (ADR-0004): the same primitives against real
// symbolic links, hard links, existing names, renamed and replaced directories, and injected
// failures, plus the Linux-specific rules (protected parent, file system whitelist, private
// staging tree, procfs publication fallback). Usage: <sandbox directory>.
#include "safe_output_internal.hpp"
#include "seedarc/archive.hpp"
#include "seedarc/safe_output.hpp"
#include "test_fixture.hpp"

#include <array>
#include <cerrno>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

#include <sys/stat.h>
#include <sys/xattr.h>
#include <unistd.h>

namespace fs = std::filesystem;
using seedarc::ErrorCode;
using namespace seedarc::experimental;
using seedarc::experimental::internal::ScopedWriteFaults;
using seedarc::experimental::internal::WriteFaults;

namespace {
int failures = 0;
void check(bool condition, const std::string &description) {
    if (!condition) {
        std::cerr << "FAIL: " << description << " (errno " << errno << ")\n";
        ++failures;
    }
}

template <class T>
void check_code(const std::expected<T, seedarc::Error> &result, ErrorCode code,
                const std::string &description) {
    if (result || result.error().code != code) {
        std::cerr << "FAIL: " << description << " (got "
                  << (result ? std::string{"success"} : result.error().message) << ")\n";
        ++failures;
    }
}

std::string read(const fs::path &path) {
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{}};
}

std::vector<fs::path> temporaries(const fs::path &directory) {
    std::vector<fs::path> found;
    for (const auto &item : fs::directory_iterator(directory)) {
        if (item.path().filename().string().starts_with(".seedarc-")) {
            found.push_back(item.path());
        }
    }
    return found;
}

mode_t mode_of(const fs::path &path) {
    struct stat status{};
    return ::lstat(path.c_str(), &status) == 0 ? status.st_mode & 07777 : 0;
}

std::size_t components_below_root(const fs::path &path) {
    const auto relative = path.relative_path();
    return static_cast<std::size_t>(std::distance(relative.begin(), relative.end()));
}

std::vector<std::byte> pattern(std::size_t size) {
    std::vector<std::byte> bytes(size);
    for (std::size_t i = 0; i < size; ++i) {
        bytes[i] = static_cast<std::byte>((i * 131 + 7) & 0xff);
    }
    return bytes;
}

const std::array<std::byte, 3> abc{std::byte{'a'}, std::byte{'b'}, std::byte{'c'}};

void budget_unit_tests() {
    using namespace seedarc::experimental::internal;
    check(max_output_path_units == 4095, "Linux path budget is PATH_MAX - 1 bytes");
    check(admit_output_component(max_output_depth - 1, 2, 1, true).has_value(),
          "last allowed directory depth");
    const auto too_deep = admit_output_component(max_output_depth, 2, 1, true);
    check(!too_deep && too_deep.error().code == ErrorCode::resource_limit,
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

void fault_injection_tests(const SafeOutputDirectory &directory, const fs::path &output) {
    const auto data = pattern(200'000);
    const auto crc = crc32c(data);
    {
        WriteFaults faults;
        faults.max_bytes_per_write = 7;
        const ScopedWriteFaults scope{faults};
        check(directory.write_new_file("short-writes", data, crc).has_value(),
              "short writes are completed by the write loop");
        const auto stored = read(output / "short-writes");
        check(stored.size() == data.size() &&
                  std::equal(stored.begin(), stored.end(), data.begin(),
                             [](char a, std::byte b) { return static_cast<std::byte>(a) == b; }),
              "short-write result is byte exact");
    }
    {
        WriteFaults faults;
        faults.force_proc_link = true;
        const ScopedWriteFaults scope{faults};
        check(directory.write_new_file("proc-linked", data, crc).has_value() &&
                  read(output / "proc-linked").size() == data.size(),
              "publication through /proc/self/fd (pre-6.10 fallback) works");
        check_code(directory.write_new_file("proc-linked", data, crc), ErrorCode::output_conflict,
                   "procfs publication does not replace an existing name");
    }
    const auto expect_clean_failure = [&](const char *name, const WriteFaults &faults,
                                          ErrorCode code, std::stop_token stop,
                                          const std::string &description,
                                          std::string *message = nullptr) {
        const ScopedWriteFaults scope{faults};
        const auto result = directory.write_new_file(name, data, crc, stop);
        check_code(result, code, description);
        check(!fs::exists(output / name), description + ": no final file");
        check(temporaries(output).empty(), description + ": no temporary file");
        if (message != nullptr && !result) {
            *message = result.error().message;
        }
    };
    {
        WriteFaults faults;
        faults.fail_write_at = 70'000;
        expect_clean_failure("disk-full", faults, ErrorCode::io_error, {},
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
        expect_clean_failure("cancel-mid-write", faults, ErrorCode::cancelled, stop.get_token(),
                             "cancellation after partial write");
        check(observed >= 65'536 && observed < data.size(), "cancellation happened mid-write");
    }
    {
        std::stop_source stop;
        WriteFaults faults;
        faults.after_write = [&](std::uint64_t written) {
            if (written == data.size()) {
                stop.request_stop();
            }
        };
        std::string message;
        expect_clean_failure("cancel-before-commit", faults, ErrorCode::cancelled,
                             stop.get_token(), "cancellation before commit", &message);
        check(message.find("before commit") != std::string::npos,
              "cancellation reached the pre-commit check");
    }
    {
        WriteFaults faults;
        faults.fail_flush = true;
        expect_clean_failure("flush-fails", faults, ErrorCode::io_error, {}, "flush failure");
    }
    {
        WriteFaults faults;
        faults.corrupt_read_back = true;
        expect_clean_failure("readback-corrupt", faults, ErrorCode::integrity_error, {},
                             "read-back verification failure");
    }
    {
        WriteFaults faults;
        faults.fail_commit = true;
        expect_clean_failure("commit-fails", faults, ErrorCode::io_error, {}, "commit failure");
    }
    {
        WriteFaults faults;
        faults.fail_write_at = 0;
        faults.fail_explicit_cleanup = true;
        std::string message;
        expect_clean_failure("cleanup-fails", faults, ErrorCode::io_error, {},
                             "write failure with failed explicit cleanup", &message);
        check(message.find("explicit cleanup failed") != std::string::npos &&
                  message.find("O_TMPFILE") != std::string::npos,
              "failed cleanup is reported (unnamed temporary released at close)");
    }
    {
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
    const auto start = components_below_root(output);
    std::vector<SafeOutputDirectory> chain;
    chain.push_back(directory);
    auto next = chain.back().create_child("d");
    while (next && chain.size() <= max_output_depth) {
        chain.push_back(std::move(*next));
        next = chain.back().create_child("d");
    }
    check(!next && next.error().code == ErrorCode::resource_limit &&
              next.error().message.find("depth") != std::string::npos,
          "real directory chain stops at the depth limit");
    check(chain.size() - 1 == max_output_depth - start, "depth limit counts from /");
    const std::array<std::byte, 1> one{std::byte{'x'}};
    check(chain.back().write_new_file("leaf", one, crc32c(one)).has_value(),
          "file can still be written at maximum depth");
    auto leaf_path = output;
    for (std::size_t i = 1; i < chain.size(); ++i) {
        leaf_path /= "d";
    }
    std::error_code ignored;
    fs::remove(leaf_path / "leaf", ignored);
    check(!ignored, "remove leaf file at maximum depth");
    while (chain.size() > 1) {
        check(chain.back().discard_empty().has_value(), "discard chain directory");
        chain.pop_back();
    }
    check(!fs::exists(output / "d"), "depth chain fully removed");
}

void path_budget_test(const SafeOutputDirectory &directory, const fs::path &output) {
    // Long components until the byte budget (not the depth limit) stops the chain.
    const std::string name(200, 'n');
    std::vector<SafeOutputDirectory> chain{directory};
    auto next = chain.back().create_child(name);
    while (next) {
        chain.push_back(std::move(*next));
        next = chain.back().create_child(name);
    }
    check(next.error().code == ErrorCode::resource_limit &&
              next.error().message.find("path length") != std::string::npos,
          "long names stop at the path byte budget");
    const auto used = output.native().size() + (chain.size() - 1) * (1 + name.size());
    check(used <= max_output_path_units && used + 1 + name.size() > max_output_path_units,
          "budget counts bytes of the equivalent absolute path");
    while (chain.size() > 1) {
        check(chain.back().discard_empty().has_value(), "discard long-name directory");
        chain.pop_back();
    }
}

// Staged tree checks: exact children, owner / mode, extended attributes, links, identity, and
// the guarded rename. Leaves "final" (published) in `output`.
void staging_tests(const SafeOutputDirectory &directory, const fs::path &output) {
    const auto crc = crc32c(abc);
    auto staged = directory.create_staging_child("final");
    if (!staged) {
        check(false, "create staging directory: " + staged.error().message);
        return;
    }
    const auto found = temporaries(output);
    check(found.size() == 1, "exactly one staging directory");
    if (found.size() != 1) {
        return;
    }
    const auto staging_path = found.front();
    check(mode_of(staging_path) == 0700, "staging directory is private (0700) from creation");
    ObjectId file_id;
    {
        auto file = staged->begin_file("f");
        check(file.has_value(), "begin file in staging");
        if (file) {
            file_id = file->identity();
            check(file->append(abc).has_value() && file->commit(abc.size()).has_value(),
                  "commit staged file");
        }
    }
    check(mode_of(staging_path / "f") == 0600, "staged file is private (0600)");
    ObjectId sub_id;
    {
        auto child = staged->create_child("sub");
        check(child.has_value() && mode_of(staging_path / "sub") == 0700,
              "staged directory is private (0700)");
        if (child) {
            sub_id = *child->identity();
        }
    }
    const std::vector<std::string> expected{"f", "sub"};
    check(staged->require_exact_children(expected).has_value(), "exact children accepted");
    check(staged->verify_owned_file("f", file_id, abc.size(), crc).has_value(),
          "staged file verifies");
    check_code(staged->verify_owned_file("f", file_id, abc.size(), crc ^ 1),
               ErrorCode::integrity_error, "wrong CRC detected");
    check_code(staged->verify_owned_file("f", file_id, abc.size() + 1, crc),
               ErrorCode::integrity_error, "wrong size detected");
    {
        // A foreign entry, even one that differs only in case, is not one of ours.
        std::ofstream{staging_path / "F"} << "foreign";
        check_code(staged->require_exact_children(expected), ErrorCode::integrity_error,
                   "case-variant foreign entry detected");
        fs::remove(staging_path / "F");
        check_code(staged->require_exact_children(std::vector<std::string>{"f"}),
                   ErrorCode::integrity_error, "unexpected entry detected");
        check_code(staged->require_exact_children(std::vector<std::string>{"f", "sub", "x"}),
                   ErrorCode::integrity_error, "missing entry detected");
    }
    {
        // A second link to a staged file would let its content change after verification.
        fs::create_hard_link(staging_path / "f", output / "outside-link");
        check_code(staged->verify_owned_file("f", file_id, abc.size(), crc),
                   ErrorCode::integrity_error, "extra hard link detected");
        fs::remove(output / "outside-link");
    }
    {
        check(::chmod((staging_path / "f").c_str(), 0644) == 0, "chmod staged file");
        check_code(staged->verify_owned_file("f", file_id, abc.size(), crc),
                   ErrorCode::integrity_error, "widened file permissions detected");
        check(::chmod((staging_path / "f").c_str(), 0600) == 0, "restore staged file mode");
        check(::chmod((staging_path / "sub").c_str(), 0755) == 0, "chmod staged directory");
        auto sub = staged->open_owned_child("sub", sub_id);
        check(sub.has_value(), "open owned staged directory");
        if (sub) {
            check_code(sub->require_exact_children({}), ErrorCode::integrity_error,
                       "widened directory permissions detected");
        }
        check(::chmod((staging_path / "sub").c_str(), 0700) == 0, "restore staged dir mode");
    }
    if (::setxattr((staging_path / "f").c_str(), "user.seedarc-test", "x", 1, 0) == 0) {
        check_code(staged->verify_owned_file("f", file_id, abc.size(), crc),
                   ErrorCode::integrity_error, "user extended attribute detected");
        ::removexattr((staging_path / "f").c_str(), "user.seedarc-test");
    } else {
        std::cout << "user xattrs unsupported here (errno " << errno
                  << "); xattr check not exercised\n";
    }
    {
        // Same name, different object: identity decides, for verification and for rollback.
        fs::rename(staging_path / "f", staging_path / "f.orig");
        std::ofstream{staging_path / "f"} << "abc";
        ::chmod((staging_path / "f").c_str(), 0600);
        check_code(staged->verify_owned_file("f", file_id, abc.size(), crc),
                   ErrorCode::integrity_error, "replaced file detected by identity");
        check_code(staged->remove_owned_child("f", file_id, false), ErrorCode::unsafe_path,
                   "rollback refuses to remove the substitute");
        check(fs::exists(staging_path / "f"), "substitute left in place");
        fs::remove(staging_path / "f");
        fs::create_symlink("f.orig", staging_path / "f");
        check_code(staged->verify_owned_file("f", file_id, abc.size(), crc),
                   ErrorCode::integrity_error, "symbolic link substitute detected");
        check_code(staged->remove_owned_child("f", file_id, false), ErrorCode::unsafe_path,
                   "rollback refuses to remove a symbolic link");
        fs::remove(staging_path / "f");
        fs::rename(staging_path / "f.orig", staging_path / "f");
        fs::rename(staging_path / "sub", staging_path / "sub.orig");
        fs::create_symlink("sub.orig", staging_path / "sub");
        check_code(staged->open_owned_child("sub", sub_id), ErrorCode::unsafe_path,
                   "symbolic link substitute for a staged directory rejected");
        fs::remove(staging_path / "sub");
        fs::rename(staging_path / "sub.orig", staging_path / "sub");
    }
    {
        // The staging directory itself moved away: commit must refuse to publish.
        const auto moved = output / "moved-staging";
        fs::rename(staging_path, moved);
        check_code(staged->commit_staged(), ErrorCode::integrity_error,
                   "commit refuses a moved staging directory");
        check(!fs::exists(output / "final"), "nothing published under the final name");
        fs::rename(moved, staging_path);
    }
    {
        fs::create_directory(output / "final");
        check_code(staged->commit_staged(), ErrorCode::output_conflict,
                   "commit does not replace an existing target");
        check(fs::is_empty(output / "final"), "existing target untouched");
        fs::remove(output / "final");
    }
    check(staged->commit_staged().has_value(), "commit staged directory");
    check(read(output / "final" / "f") == "abc" && fs::is_directory(output / "final" / "sub"),
          "published tree content");
    check(mode_of(output / "final") == 0700, "published tree keeps the private mode");
    check(!fs::exists(staging_path), "staging name gone after commit");
    check_code(staged->commit_staged(), ErrorCode::invalid_argument, "commit only once");
    // Identity-checked removal of what this run created.
    check(staged->remove_owned_child("f", file_id, false).has_value(), "remove owned file");
    check_code(staged->remove_owned_child("sub", file_id, true), ErrorCode::unsafe_path,
               "wrong identity refused");
    check(staged->remove_owned_child("sub", sub_id, true).has_value(), "remove owned directory");
    check(staged->discard_empty().has_value(), "discard emptied published directory");
    check(!fs::exists(output / "final"), "published directory removed by identity");
}

// Directories other users could modify are refused as output roots.
void protected_parent_tests(const fs::path &root) {
    const auto shared = root / "group-writable";
    fs::create_directory(shared);
    ::chmod(shared.c_str(), 0775);
    check_code(SafeOutputDirectory::open_existing(shared), ErrorCode::unsupported_feature,
               "group-writable directory without sticky bit rejected");
    ::chmod(shared.c_str(), 0757);
    check_code(SafeOutputDirectory::open_existing(shared), ErrorCode::unsupported_feature,
               "other-writable directory without sticky bit rejected");
    ::chmod(shared.c_str(), 01777);
    auto sticky = SafeOutputDirectory::open_existing(shared);
    check(sticky.has_value(), "sticky world-writable directory (like /tmp) accepted");
    if (sticky) {
        check(sticky->write_new_file("x", abc, crc32c(abc)).has_value(),
              "write into a sticky directory");
    }
    fs::remove(shared / "x");
    ::chmod(shared.c_str(), 0700);
    fs::remove(shared);
    if (::geteuid() != 0) {
        check_code(SafeOutputDirectory::open_existing("/proc"), ErrorCode::unsupported_feature,
                   "procfs is not an accepted file system");
        for (const auto *candidate : {"/mnt/c", "/sys"}) {
            std::error_code ec;
            if (fs::is_directory(candidate, ec)) {
                check(!SafeOutputDirectory::open_existing(candidate),
                      std::string{"unsupported or foreign file system rejected: "} + candidate);
            }
        }
    }
}

void read_only_test(const fs::path &root) {
    const auto locked = root / "read-only";
    fs::create_directory(locked);
    ::chmod(locked.c_str(), 0500);
    auto directory = SafeOutputDirectory::open_existing(locked);
    check(directory.has_value(), "read-only directory can be pinned");
    if (directory && ::geteuid() != 0) {
        check_code(directory->write_new_file("x", abc, crc32c(abc)), ErrorCode::io_error,
                   "write into a read-only directory fails");
        check_code(directory->create_staging_child("x"), ErrorCode::io_error,
                   "staging in a read-only directory fails");
        check_code(directory->create_child("x"), ErrorCode::io_error,
                   "directory in a read-only directory fails");
    }
    ::chmod(locked.c_str(), 0700);
    check(fs::is_empty(locked), "read-only directory unchanged");
    fs::remove(locked);
}
} // namespace

int main(int argc, char *argv[]) {
    if (argc != 2) {
        return 2;
    }
    const auto base = fs::absolute(argv[1]).lexically_normal();
    fs::create_directories(base);
    const auto root = base / ("m1-io-" + test_fixture::unique_suffix());
    if (!fs::create_directory(root)) {
        return 2;
    }
    ::chmod(root.c_str(), 0700);
    const auto outside = root / "outside";
    const auto output = root / "output";
    fs::create_directory(outside);
    fs::create_directory(output);
    ::chmod(output.c_str(), 0700);
    std::ofstream{outside / "sentinel", std::ios::binary} << "unchanged";
    std::ofstream{output / "existing", std::ios::binary} << "original";
    fs::create_hard_link(outside / "sentinel", output / "hardlink");
    fs::create_directory_symlink(outside, output / "symlink");
    fs::create_symlink(outside / "missing", output / "dangling");

    const auto crc = crc32c(abc);
    budget_unit_tests();
    std::ofstream{outside / "movable", std::ios::binary} << "moved";
    {
        auto directory = SafeOutputDirectory::open_existing(output);
        if (!directory) {
            std::cerr << "open root: " << directory.error().message << '\n';
            return 1;
        }
        check(directory->write_new_file("new", abc, crc).has_value(),
              "validated atomic no-replace commit");
        check(read(output / "new") == "abc", "persist exact bytes");
        check(directory->write_new_file("empty", {}, 0).has_value(), "empty file commit");
        check_code(directory->write_new_file("existing", abc, crc), ErrorCode::output_conflict,
                   "existing target rejected");
        check_code(directory->write_new_file("hardlink", abc, crc), ErrorCode::output_conflict,
                   "existing hardlink target rejected");
        check_code(directory->write_new_file("symlink", abc, crc), ErrorCode::output_conflict,
                   "existing symbolic link target rejected");
        check_code(directory->write_new_file("dangling", abc, crc), ErrorCode::output_conflict,
                   "dangling symbolic link target rejected (not followed)");
        check(!fs::exists(outside / "missing"), "dangling link target was not created");
        check(!directory->write_new_file("../escaped", abc, crc), "traversal rejected");
        check(!directory->write_new_file("nested/file", abc, crc), "multiple components rejected");
        check_code(directory->write_new_file("bad-crc", abc, crc ^ 1), ErrorCode::integrity_error,
                   "checksum failure cleans temporary");
        {
            std::stop_source stop;
            stop.request_stop();
            check_code(directory->write_new_file("cancelled", abc, crc, stop.get_token()),
                       ErrorCode::cancelled, "cancel before write reports cancelled");
        }
        check(!directory->discard_empty(), "existing directory cannot be deleted by primitive");
        check_code(directory->create_child("existing"), ErrorCode::output_conflict,
                   "directory create cannot replace file");
        check_code(directory->create_child("symlink"), ErrorCode::output_conflict,
                   "directory create cannot follow symbolic link");
        {
            auto child = directory->create_child("child");
            check(child.has_value(), "atomic new directory creation");
            if (child) {
                check(!child->write_new_file("bad", abc, crc ^ 1), "child checksum cleanup");
                check(child->discard_empty().has_value(), "owned empty directory cleanup");
                check(!child->write_new_file("after-discard", abc, crc),
                      "reject discarded handle");
            }
        }
        {
            // POSIX cannot stop a held directory from being renamed (Windows can); the pinned
            // descriptor still refers to the same directory, so writes follow it, not the name.
            fs::rename(output, root / "renamed");
            fs::create_directory(output);
            check(directory->write_new_file("after-rename", abc, crc).has_value(),
                  "write after the pinned directory was renamed");
            check(fs::exists(root / "renamed" / "after-rename") &&
                      !fs::exists(output / "after-rename"),
                  "write lands in the pinned directory, not in its old name's new occupant");
            fs::remove(output);
            fs::rename(root / "renamed", output);
            fs::remove(output / "after-rename");
        }
        fs::rename(outside / "movable", output / "moved-in");
        check(fs::exists(output / "moved-in"),
              "other programs can still move files into a pinned directory");
        fault_injection_tests(*directory, output);
        depth_limit_test(*directory, output);
        path_budget_test(*directory, output);
        staging_tests(*directory, output);
        check_code(SafeOutputDirectory::open_existing(output / "symlink"), ErrorCode::unsafe_path,
                   "reject symbolic link as output root");
        check_code(SafeOutputDirectory::open_existing(output / "symlink" / "x"),
                   ErrorCode::unsafe_path, "reject symbolic link inside output root path");
        check(!SafeOutputDirectory::open_existing(output / ".." / "outside"),
              "reject dot components in root");
        check(!SafeOutputDirectory::open_existing(root.string() + "//output"),
              "reject empty components in root");
        check_code(SafeOutputDirectory::open_existing(output / "existing"), ErrorCode::unsafe_path,
                   "reject a file as output root");
        for (const auto *name : {"relative", "", "./x"}) {
            check(!SafeOutputDirectory::open_existing(name), "reject relative root");
        }
        {
            // The directory is replaced by a symbolic link after it was created: the held
            // descriptor keeps writing into the original directory, never through the link.
            auto mutation = directory->create_child("mutation");
            check(mutation.has_value(), "create mutation target");
            if (mutation) {
                fs::rename(output / "mutation", output / "mutation.orig");
                fs::create_directory_symlink(outside, output / "mutation");
                check(mutation->write_new_file("escape", abc, crc).has_value(),
                      "write through the pinned descriptor");
                check(!fs::exists(outside / "escape") &&
                          fs::exists(output / "mutation.orig" / "escape"),
                      "replacing a pinned directory by a link cannot redirect writes");
                check_code(mutation->discard_empty(), ErrorCode::unsafe_path,
                           "discard refuses the link that replaced the directory");
                fs::remove(output / "mutation");
                fs::remove(output / "mutation.orig" / "escape");
                fs::rename(output / "mutation.orig", output / "mutation");
                check(mutation->discard_empty().has_value(), "discard original directory");
            }
        }
        check(temporaries(output).empty(), "no temporary leftovers");
        check(!fs::exists(output / "bad-crc") && !fs::exists(output / "cancelled"),
              "no failed final files");
        check(read(output / "existing") == "original", "existing target unchanged");
        check(read(outside / "sentinel") == "unchanged", "outside hardlink sentinel unchanged");
        for (const auto &item : fs::directory_iterator(outside)) {
            check(item.path().filename() == "sentinel", "nothing written outside the output");
        }
    }
    protected_parent_tests(root);
    read_only_test(root);
    // Non-recursive cleanup of known names only.
    for (const auto *name : {"new", "empty", "existing", "hardlink", "symlink", "dangling",
                             "moved-in", "short-writes", "proc-linked", "other-thread"}) {
        fs::remove(output / name);
    }
    fs::remove(outside / "sentinel");
    fs::remove(outside / "movable");
    fs::remove(output);
    fs::remove(outside);
    check(fs::remove(root), "remove only owned empty test root");
    std::cout << (failures == 0 ? "all safe output checks passed" : "safe output checks failed")
              << '\n';
    return failures == 0 ? 0 : 1;
}
