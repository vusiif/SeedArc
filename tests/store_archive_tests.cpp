// SPDX-License-Identifier: GPL-3.0-or-later
// End-to-end Store archive tests: byte-exact round trips, hostile archives, limits, conflicts,
// rejected inputs and injected I/O failures. Runs inside a private directory below argv[1].
#include "safe_output_internal.hpp"
#include "seedarc/archive_index.hpp"
#include "seedarc/safe_output.hpp"
#include "seedarc/store_archive.hpp"
#include "test_fixture.hpp"
#ifdef _WIN32
#include "win_test_support.hpp"
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <map>
#include <optional>
#include <random>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;
using seedarc::ErrorCode;
using namespace seedarc::experimental;
using seedarc::experimental::internal::ScopedWriteFaults;
using seedarc::experimental::internal::WriteFaults;

namespace {
using Bytes = std::vector<std::byte>;
using Snapshot = std::map<std::string, std::optional<Bytes>>; // nullopt: directory

int failures = 0;

void check(bool condition, std::string_view description) {
    if (!condition) {
        std::cerr << "FAIL: " << description << '\n';
        ++failures;
    }
}

template <typename T>
void check_code(const std::expected<T, seedarc::Error> &result, ErrorCode code,
                std::string_view description) {
    if (result || result.error().code != code || result.error().message.empty()) {
        std::cerr << "FAIL: " << description << " (got "
                  << (result ? "success" : result.error().message) << ")\n";
        ++failures;
    }
}

template <typename T>
bool succeeded(const std::expected<T, seedarc::Error> &result, std::string_view description) {
    if (!result) {
        std::cerr << "FAIL: " << description << ": " << result.error().message << '\n';
        ++failures;
    }
    return result.has_value();
}

Bytes random_bytes(std::size_t size, std::uint64_t seed) {
    std::mt19937_64 engine{seed};
    Bytes bytes(size);
    for (std::size_t i = 0; i < size; ++i) {
        bytes[i] = static_cast<std::byte>(engine() & 0xffU);
    }
    return bytes;
}

Bytes text(std::string_view value) {
    Bytes bytes(value.size());
    std::ranges::transform(value, bytes.begin(), [](char c) { return static_cast<std::byte>(c); });
    return bytes;
}

Bytes read_all(const fs::path &path) {
    std::ifstream stream(path, std::ios::binary);
    const std::vector<char> raw{std::istreambuf_iterator<char>{stream},
                                std::istreambuf_iterator<char>{}};
    Bytes bytes(raw.size());
    std::ranges::transform(raw, bytes.begin(), [](char c) { return static_cast<std::byte>(c); });
    return bytes;
}

void write_file(const fs::path &path, std::span<const std::byte> bytes) {
    std::ofstream stream(path, std::ios::binary);
    stream.write(reinterpret_cast<const char *>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    if (!stream) {
        std::cerr << "FAIL: cannot write test fixture\n";
        ++failures;
    }
}

fs::path u8(std::u8string_view name) { return fs::path{std::u8string{name}}; }

// Off Windows only the ASCII subset of the archive path profile exists (ADR-0004 section 5): the
// fixtures use an ASCII stand-in there, and non_ascii_refusal_tests() checks the refusal.
fs::path native_name([[maybe_unused]] std::u8string_view windows,
                     [[maybe_unused]] std::string_view posix) {
#ifdef _WIN32
    return u8(windows);
#else
    return fs::path{std::string{posix}};
#endif
}

Snapshot snapshot(const fs::path &root) {
    Snapshot result;
    for (const auto &item : fs::recursive_directory_iterator(root)) {
        const auto relative = item.path().lexically_relative(root).generic_u8string();
        std::string key{relative.begin(), relative.end()};
        if (item.is_directory()) {
            result.emplace(std::move(key), std::nullopt);
        } else {
            result.emplace(std::move(key), read_all(item.path()));
        }
    }
    return result;
}

bool no_leftovers(const fs::path &directory) {
    for (const auto &item : fs::directory_iterator(directory)) {
        if (item.path().filename().wstring().starts_with(L".seedarc-")) {
            return false;
        }
    }
    return true;
}

double seconds_since(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

// A tree covering nesting, empty files and directories, sibling ordering and non-ASCII names.
void build_tree(const fs::path &root) {
    fs::create_directories(root / "a" / "deep" / "er" / "still");
    fs::create_directories(root / "empty-dir" / "nested-empty");
    const auto named = root / native_name(u8"中文目录", "Mixed Dir");
    fs::create_directories(named);
    write_file(root / "a.txt", text("sorts between a and a/..."));
    write_file(root / "a" / "b.bin", random_bytes(10'000, 1));
    write_file(root / "a" / "zero", {});
    write_file(root / "a" / "deep" / "er" / "still" / "leaf", text("x"));
    write_file(named / native_name(u8"文件 名.txt", "file name.txt"),
               text("non-ASCII directory and file"));
    write_file(root / native_name(u8"café.txt", "cafe.txt"), text("NFC e-acute"));
    write_file(root / native_name(u8"\U0001f600.bin", "#smile.bin"), random_bytes(300, 2));
    write_file(root / "with space.txt", text("space"));
    write_file(root / "UPPER.TXT", text("upper"));
}

// compress -> verify (file and memory) -> extract -> compare. Returns the archive path.
fs::path round_trip(const fs::path &work, const std::string &label, const fs::path &input,
                    const CompressOptions &options = {}) {
    const auto archive = work / (label + ".sarc");
    const auto output = work / (label + "-out");
    const auto compressed = compress_archive(input, archive, options);
    if (!succeeded(compressed, label + ": compress")) {
        return archive;
    }
    check(fs::file_size(archive) == compressed->archive_bytes, label + ": archive size reported");
    const auto verified = verify_archive(archive);
    check(succeeded(verified, label + ": verify") && verified->entries == compressed->entries &&
              verified->blocks == compressed->blocks &&
              verified->total_output == compressed->total_output,
          label + ": verify summary matches");
    check(verify_archive_bytes(read_all(archive)).has_value(), label + ": verify from memory");
    const auto extracted = extract_archive(archive, output);
    if (succeeded(extracted, label + ": extract")) {
        if (fs::is_directory(input)) {
            check(snapshot(input) == snapshot(output), label + ": byte-exact tree round trip");
        } else {
            const auto restored = snapshot(output);
            const auto name = input.filename().generic_u8string();
            const auto found = restored.find(std::string{name.begin(), name.end()});
            check(restored.size() == 1 && found != restored.end() && found->second &&
                      *found->second == read_all(input),
                  label + ": byte-exact single file round trip");
        }
    }
    check(no_leftovers(work), label + ": no temporaries left");
    return archive;
}

void round_trip_tests(const fs::path &work) {
    {
        const auto input = work / "empty-input";
        fs::create_directory(input);
        const auto archive = round_trip(work, "empty-dir", input);
        check(fs::file_size(archive) == archive_header_bytes, "empty archive is only a header");
    }
    {
        const auto input = work / "empty-file.bin";
        write_file(input, {});
        round_trip(work, "empty-file", input);
    }
    {
        const auto input = work / native_name(u8"单文件.dat", "single-file.dat");
        write_file(input, random_bytes(70'000, 3));
        CompressOptions options;
        options.chunk_size_bytes = 4096;
        round_trip(work, "single-file-multi-block", input, options);
    }
    {
        // Sizes around the chunk boundary, including exact multiples.
        const auto input = work / "sizes";
        fs::create_directory(input);
        std::uint64_t seed = 10;
        for (const std::size_t size :
             {std::size_t{1}, std::size_t{4095}, std::size_t{4096}, std::size_t{4097},
              std::size_t{8192}, std::size_t{3 * 4096 + 17}}) {
            write_file(input / ("f" + std::to_string(size)), random_bytes(size, seed++));
        }
        CompressOptions options;
        options.chunk_size_bytes = 4096;
        round_trip(work, "chunk-boundaries", input, options);
        options.chunk_size_bytes = 1;
        round_trip(work, "one-byte-chunks", input / "f4095", options);
    }
    const auto tree = work / "tree";
    build_tree(tree);
    {
        const auto first = round_trip(work, "tree", tree);
        // Same input, same options: byte-identical archive.
        const auto second = work / "tree-again.sarc";
        check(compress_archive(tree, second).has_value() && read_all(first) == read_all(second),
              "repeated compression is byte-identical");
    }
    {
        // The archive is written inside the tree it archives and must not contain itself.
        const auto before = snapshot(tree);
        const auto inside = tree / "a" / "self.sarc";
        if (succeeded(compress_archive(tree, inside), "compress into the input tree")) {
            const auto output = work / "self-out";
            check(extract_archive(inside, output).has_value() && snapshot(output) == before,
                  "output archive is not enumerated as input");
            check(no_leftovers(tree / "a"), "no temporaries inside the input tree");
            fs::remove(inside);
        }
    }
    {
        const auto input = work / "many";
        fs::create_directory(input);
        std::mt19937_64 sizes{7};
        for (int directory = 0; directory < 12; ++directory) {
            const auto parent = input / ("dir" + std::to_string(directory));
            fs::create_directory(parent);
            for (int file = 0; file < 100; ++file) {
                write_file(parent / ("f" + std::to_string(file)),
                           random_bytes(static_cast<std::size_t>(sizes() % 300),
                                        static_cast<std::uint64_t>(directory * 1000 + file)));
            }
        }
        const auto start = std::chrono::steady_clock::now();
        round_trip(work, "many-small-files", input);
        std::cout << "many small files (1200 files, 12 directories): " << seconds_since(start)
                  << " s for compress + verify + extract + compare\n";
    }
    {
        const auto input = work / "entropy.bin";
        write_file(input, random_bytes(5 * 512 * 1024, 99)); // 2.5 MiB, three default chunks
        const auto start = std::chrono::steady_clock::now();
        round_trip(work, "high-entropy", input);
        std::cout << "high entropy (2.5 MiB): " << seconds_since(start)
                  << " s for compress + verify + extract + compare\n";
    }
}

// Builds archive bytes around an arbitrary (possibly hostile) index.
Bytes make_archive(const ArchiveIndex &index, std::span<const std::byte> data,
                   std::uint32_t chunk_size) {
    const auto index_bytes = encode_index(index);
    ArchiveHeader header;
    header.index_bytes = index_bytes.size();
    header.entry_count = index.entries.size();
    header.block_count = index.blocks.size();
    header.data_bytes = data.size();
    header.chunk_size_bytes = chunk_size;
    for (const auto &entry : index.entries) {
        header.total_output += entry.size;
    }
    Bytes archive;
    const auto encoded = encode_header(header);
    if (!succeeded(encoded, "encode crafted header")) {
        return archive;
    }
    archive.assign(encoded->begin(), encoded->end());
    archive.insert(archive.end(), data.begin(), data.end());
    archive.insert(archive.end(), index_bytes.begin(), index_bytes.end());
    return archive;
}

// Every reader entry point must reject the archive with `code`, and extraction must leave
// neither the target nor temporaries behind.
void expect_rejected(const fs::path &work, const std::string &label,
                     std::span<const std::byte> archive, ErrorCode code,
                     const ArchiveLimits &limits = {}) {
    const auto file = work / "rejected.sarc";
    const auto target = work / "rejected-out";
    write_file(file, archive);
    check_code(verify_archive_bytes(archive, limits), code, label + ": verify from memory");
    check_code(verify_archive(file, limits), code, label + ": verify");
    check_code(extract_archive(file, target, limits), code, label + ": extract");
    check(!fs::exists(target), label + ": no extraction target");
    check(no_leftovers(work), label + ": no temporaries left");
    fs::remove(file);
}

void hostile_archive_tests(const fs::path &work) {
    const auto input = work / "hostile-input";
    fs::create_directories(input / "d");
    write_file(input / "one", random_bytes(3000, 20));
    write_file(input / "d" / "two", random_bytes(500, 21));
    const auto archive = work / "hostile-base.sarc";
    CompressOptions options;
    options.chunk_size_bytes = 1024;
    const auto summary = compress_archive(input, archive, options);
    if (!succeeded(summary, "hostile base archive")) {
        return;
    }
    const auto good = read_all(archive);
    const auto data_bytes = static_cast<std::size_t>(summary->total_output);
    const auto index_bytes = good.size() - archive_header_bytes - data_bytes;

    for (const std::size_t size :
         {std::size_t{0}, std::size_t{50}, archive_header_bytes, archive_header_bytes + 10,
          good.size() - index_bytes, good.size() - 1}) {
        expect_rejected(work, "truncated to " + std::to_string(size), std::span{good}.first(size),
                        ErrorCode::malformed_archive);
    }
    {
        auto longer = good;
        longer.push_back(std::byte{0});
        expect_rejected(work, "trailing byte", longer, ErrorCode::malformed_archive);
    }
    for (const std::size_t position :
         {std::size_t{30}, archive_header_bytes, archive_header_bytes + data_bytes - 1,
          archive_header_bytes + data_bytes + 5, good.size() - 1}) {
        auto changed = good;
        changed[position] ^= std::byte{0x40};
        expect_rejected(work, "flipped byte " + std::to_string(position), changed,
                        ErrorCode::integrity_error);
    }
    {
        auto changed = good;
        changed[0] = std::byte{'X'};
        expect_rejected(work, "wrong magic", changed, ErrorCode::malformed_archive);
    }

    // Crafted indexes with valid checksums: the content itself is hostile.
    const auto payload = text("x");
    const BlockDescriptor block{0, 1, 1, 0, 0, crc32c(payload)};
    const auto single = [&](const char *path) {
        return make_archive({{{path, false, 1, 0, 1}}, {block}}, payload, 1024);
    };
    for (const char *path : {"../evil", "sub/../../evil", "/evil", "C:evil", "evil:stream", "nul",
                             "a\\evil", "evil."}) {
        expect_rejected(work, std::string{"hostile path "} + path, single(path),
                        ErrorCode::unsafe_path);
    }
    check(!fs::exists(work / "evil") && !fs::exists(work.parent_path() / "evil"),
          "no hostile path escaped");
    expect_rejected(
        work, "case collision",
        make_archive({{{"File", true, 0, 0, 0}, {"file", true, 0, 0, 0}}, {}}, {}, 1024),
        ErrorCode::unsafe_path);
    expect_rejected(work, "missing parent directory",
                    make_archive({{{"ghost/child", false, 1, 0, 1}}, {block}}, payload, 1024),
                    ErrorCode::malformed_archive);
    expect_rejected(
        work, "duplicate entry",
        make_archive({{{"same", true, 0, 0, 0}, {"same", true, 0, 0, 0}}, {}}, {}, 1024),
        ErrorCode::malformed_archive);
    expect_rejected(
        work, "wrong block checksum",
        make_archive({{{"f", false, 1, 0, 1}}, {{0, 1, 1, 0, 0, 0x12345678U}}}, payload, 1024),
        ErrorCode::integrity_error);
    expect_rejected(
        work, "unknown codec",
        make_archive({{{"f", false, 1, 0, 1}}, {{0, 1, 1, 7, 0, crc32c(payload)}}}, payload, 1024),
        ErrorCode::unsupported_feature);
    {
        // Two files claiming the same block.
        const auto two = text("xy");
        expect_rejected(work, "shared block",
                        make_archive({{{"f", false, 1, 0, 1}, {"g", false, 1, 0, 1}},
                                      {block, {1, 1, 1, 0, 0, crc32c(std::span{two}.subspan(1))}}},
                                     two, 1024),
                        ErrorCode::malformed_archive);
    }

    // Declared sizes against reader limits: exactly at the limit passes, one below fails.
    const auto reader_limit = [&](const std::string &label,
                                  const std::function<void(ArchiveLimits &, int)> &set) {
        ArchiveLimits at;
        set(at, 0);
        check(verify_archive(archive, at).has_value(), label + " exactly at the limit");
        ArchiveLimits over;
        set(over, 1);
        expect_rejected(work, label + " over the limit", good, ErrorCode::resource_limit, over);
    };
    reader_limit("entries", [&](ArchiveLimits &l, int d) { l.max_entries = summary->entries - d; });
    reader_limit("blocks", [&](ArchiveLimits &l, int d) { l.max_blocks = summary->blocks - d; });
    reader_limit("total output",
                 [&](ArchiveLimits &l, int d) { l.max_total_output = data_bytes - d; });
    reader_limit("data bytes", [&](ArchiveLimits &l, int d) { l.max_data_bytes = data_bytes - d; });
    reader_limit("index bytes",
                 [&](ArchiveLimits &l, int d) { l.max_index_bytes = index_bytes - d; });
    reader_limit("chunk size", [&](ArchiveLimits &l, int d) {
        l.max_chunk_bytes = 1024U - static_cast<std::uint32_t>(d);
    });
}

void limit_and_conflict_tests(const fs::path &work) {
    const auto input = work / "limits-input";
    fs::create_directories(input / "d");
    write_file(input / "one", random_bytes(3000, 30));
    write_file(input / "d" / "two", random_bytes(500, 31));
    CompressOptions base;
    base.chunk_size_bytes = 1024;
    const auto reference = work / "limits-reference.sarc";
    const auto summary = compress_archive(input, reference, base);
    if (!succeeded(summary, "limits reference archive")) {
        return;
    }
    const auto index_bytes = summary->archive_bytes - archive_header_bytes - summary->total_output;
    int serial = 0;
    const auto writer_limit = [&](const std::string &label,
                                  const std::function<void(ArchiveLimits &, int)> &set) {
        auto options = base;
        set(options.limits, 0);
        const auto at = work / ("limit-at-" + std::to_string(serial++) + ".sarc");
        check(compress_archive(input, at, options).has_value() &&
                  read_all(at) == read_all(reference),
              label + " exactly at the writer limit");
        set(options.limits, 1);
        const auto over = work / "limit-over.sarc";
        check_code(compress_archive(input, over, options), ErrorCode::resource_limit,
                   label + " over the writer limit");
        check(!fs::exists(over) && no_leftovers(work), label + ": nothing written over limit");
    };
    writer_limit("entries", [&](ArchiveLimits &l, int d) { l.max_entries = summary->entries - d; });
    writer_limit("blocks", [&](ArchiveLimits &l, int d) { l.max_blocks = summary->blocks - d; });
    writer_limit("total output",
                 [&](ArchiveLimits &l, int d) { l.max_total_output = summary->total_output - d; });
    writer_limit("data bytes",
                 [&](ArchiveLimits &l, int d) { l.max_data_bytes = summary->total_output - d; });
    writer_limit("index bytes",
                 [&](ArchiveLimits &l, int d) { l.max_index_bytes = index_bytes - d; });
    writer_limit("chunk size", [&](ArchiveLimits &l, int d) {
        l.max_chunk_bytes = 1024U - static_cast<std::uint32_t>(d);
    });
    {
        auto options = base;
        options.chunk_size_bytes = 0;
        check_code(compress_archive(input, work / "zero-chunk.sarc", options),
                   ErrorCode::invalid_argument, "zero chunk size rejected");
    }

    // Outputs are never replaced.
    const auto reference_bytes = read_all(reference);
    check_code(compress_archive(input, reference, base), ErrorCode::output_conflict,
               "existing archive is not overwritten");
    check(read_all(reference) == reference_bytes, "existing archive unchanged");
    const auto existing = work / "existing-target";
    fs::create_directory(existing);
    write_file(existing / "keep", text("keep me"));
    check_code(extract_archive(reference, existing), ErrorCode::output_conflict,
               "existing directory is not an extraction target");
    check(snapshot(existing).size() == 1 && read_all(existing / "keep") == text("keep me"),
          "existing directory unchanged");
    check_code(extract_archive(reference, reference), ErrorCode::output_conflict,
               "existing file is not an extraction target");
    check(read_all(reference) == reference_bytes, "archive unchanged by conflicting extract");
    check(!compress_archive(input, work / "no-such-parent" / "x.sarc", base),
          "missing output parent rejected");
    check(!extract_archive(reference, work / "no-such-parent" / "out"),
          "missing extraction parent rejected");
    check(!fs::exists(work / "no-such-parent"), "missing parent is not created");
    check(!compress_archive(work / "no-such-input", work / "x.sarc", base),
          "missing input rejected");
    check(!verify_archive(work / "no-such-archive"), "missing archive rejected");
    check(!fs::exists(work / "x.sarc") && no_leftovers(work), "failed runs leave nothing");
}

void rejected_input_tests(const fs::path &work) {
    const auto input = work / "links-input";
    const auto outside = work / "links-outside";
    fs::create_directories(input / "real");
    fs::create_directory(outside);
    write_file(input / "real" / "file", text("inside"));
    write_file(outside / "secret", text("outside"));
    const auto archive = work / "links.sarc";
#ifdef _WIN32
    fs::create_directory(input / "link");
    const bool junction = seedarc_test::set_junction(input / "link", outside);
    check(junction, "create junction fixture");
    if (junction) {
        check_code(compress_archive(input, archive), ErrorCode::unsupported_feature,
                   "junction inside the input is rejected, not followed");
        check_code(compress_archive(input / "link", archive), ErrorCode::unsupported_feature,
                   "junction as the input root is rejected");
        check(!fs::exists(archive) && no_leftovers(work), "rejected input leaves no archive");
    }
    check(RemoveDirectoryW((input / "link").c_str()) != 0, "remove junction fixture");
#else
    // Symbolic links to a directory, to a file, dangling, and as the input root.
    fs::create_directory_symlink(outside, input / "link");
    check_code(compress_archive(input, archive), ErrorCode::unsupported_feature,
               "directory symlink inside the input is rejected, not followed");
    check_code(compress_archive(input / "link", archive), ErrorCode::unsupported_feature,
               "symlink as the input root is rejected");
    fs::remove(input / "link");
    fs::create_symlink(outside / "secret", input / "real" / "file-link");
    check_code(compress_archive(input, archive), ErrorCode::unsupported_feature,
               "file symlink deep inside the input is rejected");
    fs::remove(input / "real" / "file-link");
    fs::create_symlink(outside / "missing", input / "dangling");
    check_code(compress_archive(input, archive), ErrorCode::unsupported_feature,
               "dangling symlink inside the input is rejected");
    fs::remove(input / "dangling");
    // Devices, FIFOs and sockets are rejected (a FIFO is the one a user can create).
    check(::mkfifo((input / "fifo").c_str(), 0600) == 0, "create FIFO fixture");
    check_code(compress_archive(input, archive), ErrorCode::unsupported_feature,
               "FIFO inside the input is rejected (and the open does not block)");
    check_code(compress_archive(input / "fifo", archive), ErrorCode::unsupported_feature,
               "FIFO as the input root is rejected");
    fs::remove(input / "fifo");
    check(!fs::exists(archive) && no_leftovers(work), "rejected input leaves no archive");
    check(compress_archive(input, archive).has_value(), "input accepted once links are gone");
    fs::remove(archive);
    // An input on a file system outside the whitelist (procfs) is refused.
    check_code(compress_archive("/proc/self/status", archive), ErrorCode::unsupported_feature,
               "input on an unsupported file system (procfs) is rejected");
    check(!fs::exists(archive), "refused procfs input leaves no archive");
#endif
    check(read_all(outside / "secret") == text("outside"), "link target untouched");

    // A name the archive path profile cannot store is an explicit error, never a silent rename.
    const auto odd = work / "odd-input";
    fs::create_directory(odd);
    write_file(odd / "bad~name", text("tilde"));
    check_code(compress_archive(odd, archive), ErrorCode::unsafe_path,
               "unrepresentable input name rejected");
    check(!fs::exists(archive) && no_leftovers(work), "unrepresentable input leaves no archive");

    // A file another program holds open for writing cannot be read consistently.
    const auto busy = work / "busy-input";
    fs::create_directory(busy);
    write_file(busy / "log", text("still being written"));
#ifdef _WIN32
    const auto writer =
        CreateFileW((busy / "log").c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    check(writer != INVALID_HANDLE_VALUE, "open concurrent writer");
    check_code(compress_archive(busy, archive), ErrorCode::io_error,
               "input held open for writing is rejected");
    check(!fs::exists(archive) && no_leftovers(work), "busy input leaves no archive");
    if (writer != INVALID_HANDLE_VALUE) {
        CloseHandle(writer);
    }
    check(compress_archive(busy, archive).has_value(), "input readable once the writer closed");
#else
    // POSIX cannot refuse other writers (ADR-0004 section 4): an idle writer does not prevent
    // compression; actual modification is detected instead (fault_injection_tests).
    {
        std::ofstream writer{busy / "log", std::ios::binary | std::ios::app};
        check(writer.good(), "open concurrent writer");
        check(compress_archive(busy, archive).has_value(),
              "input held open (unmodified) is compressed on POSIX");
    }
    fs::remove(archive);
    check(compress_archive(busy, archive).has_value(), "input readable once the writer closed");
#endif
}

#ifndef _WIN32
// ADR-0004 section 5: non-ASCII names are refused explicitly on Linux, on both sides, and
// nothing is written.
void non_ascii_refusal_tests(const fs::path &work) {
    const auto input = work / "non-ascii-input";
    fs::create_directory(input);
    write_file(input / "plain.txt", text("ascii"));
    write_file(input / u8(u8"café.txt"), text("NFC e-acute"));
    const auto archive = work / "non-ascii.sarc";
    check_code(compress_archive(input, archive), ErrorCode::unsupported_feature,
               "non-ASCII input name refused on Linux");
    check_code(compress_archive(input / u8(u8"café.txt"), archive), ErrorCode::unsupported_feature,
               "non-ASCII input root name refused on Linux");
    check(!fs::exists(archive) && no_leftovers(work), "refused input leaves no archive");
    const auto payload = text("x");
    const BlockDescriptor block{0, 1, 1, 0, 0, crc32c(payload)};
    for (const char *path : {"caf\xc3\xa9.txt", "\xe4\xb8\xad", "\xf0\x9f\x98\x80"}) {
        expect_rejected(work, std::string{"non-ASCII archive path "} + path,
                        make_archive({{{path, false, 1, 0, 1}}, {block}}, payload, 1024),
                        ErrorCode::unsupported_feature);
    }
    // Malformed UTF-8 stays an unsafe path, as on Windows.
    expect_rejected(work, "malformed UTF-8 archive path",
                    make_archive({{{"bad\xc0\xaf", false, 1, 0, 1}}, {block}}, payload, 1024),
                    ErrorCode::unsafe_path);
    fs::remove(input / u8(u8"café.txt"));
    check(compress_archive(input, archive).has_value(), "ASCII-only input accepted");
    fs::remove(archive);
}
#endif

fs::path find_staging_directory(const fs::path &work) {
    for (const auto &item : fs::directory_iterator(work)) {
        if (item.is_directory() && item.path().filename().wstring().starts_with(L".seedarc-")) {
            return item.path();
        }
    }
    return {};
}

void fault_injection_tests(const fs::path &work) {
    // Canonical order: a-small1, a-small2, big, d, d/x. The small files are committed before
    // `big` fails, so rollback has to remove already committed files and directories.
    const auto input = work / "faults-input";
    fs::create_directories(input / "d");
    write_file(input / "a-small1", random_bytes(100, 40));
    write_file(input / "a-small2", random_bytes(200, 41));
    write_file(input / "big", random_bytes(5000, 42));
    write_file(input / "d" / "x", random_bytes(50, 43));
    CompressOptions options;
    options.chunk_size_bytes = 1024;
    const auto archive = work / "faults.sarc";
    if (!succeeded(compress_archive(input, archive, options), "fault test archive")) {
        return;
    }
    const auto target = work / "faults-out";
    const auto extract_fails = [&](const std::string &label, const WriteFaults &faults,
                                   ErrorCode code, std::stop_token stop = {}) {
        const ScopedWriteFaults scope{faults};
        check_code(extract_archive(archive, target, {}, std::move(stop)), code, label);
        check(!fs::exists(target), label + ": target never appears");
        check(no_leftovers(work), label + ": staging directory removed");
    };
    {
        WriteFaults faults;
        faults.fail_write_at = 3000;
        extract_fails("extract: write failure after committed files", faults, ErrorCode::io_error);
    }
    {
        std::stop_source stop;
        WriteFaults faults;
        faults.after_write = [&](std::uint64_t written) {
            if (written >= 2048) {
                stop.request_stop();
            }
        };
        extract_fails("extract: cancelled in the middle of a file", faults, ErrorCode::cancelled,
                      stop.get_token());
    }
    {
        std::stop_source stop;
        stop.request_stop();
        extract_fails("extract: cancelled before it starts", {}, ErrorCode::cancelled,
                      stop.get_token());
    }
    {
        WriteFaults faults;
        faults.fail_flush = true;
        extract_fails("extract: flush failure", faults, ErrorCode::io_error);
    }
    {
        WriteFaults faults;
        faults.corrupt_read_back = true;
        extract_fails("extract: read-back mismatch", faults, ErrorCode::integrity_error);
    }
    {
        WriteFaults faults;
        faults.fail_commit = true;
        extract_fails("extract: file commit failure", faults, ErrorCode::io_error);
    }
    {
        WriteFaults faults;
        faults.max_bytes_per_write = 7;
        const ScopedWriteFaults scope{faults};
        check(extract_archive(archive, target).has_value() && snapshot(target) == snapshot(input),
              "extract: short writes still give a byte-exact result");
    }
    {
        // Rollback only removes what this run created: a foreign file dropped into the staging
        // directory survives, and the failure to remove that directory is reported.
        const auto second = work / "faults-out-2";
        fs::path staging;
        WriteFaults faults;
        faults.fail_write_at = 3000;
        faults.after_write = [&](std::uint64_t) {
            if (staging.empty()) {
                staging = find_staging_directory(work);
                if (!staging.empty()) {
                    write_file(staging / "foreign.txt", text("not ours"));
                }
            }
        };
        const ScopedWriteFaults scope{faults};
        const auto result = extract_archive(archive, second);
        check_code(result, ErrorCode::io_error, "extract: failure with a foreign file present");
        check(!result && result.error().message.find("cleanup incomplete") != std::string::npos,
              "incomplete cleanup is reported");
        check(!fs::exists(second), "target never appears when cleanup is incomplete");
        const auto left = staging.empty() ? Snapshot{} : snapshot(staging);
        check(left.size() == 1 && left.contains("foreign.txt") &&
                  read_all(staging / "foreign.txt") == text("not ours"),
              "rollback removed its own objects and kept the foreign file");
        if (!staging.empty()) {
            fs::remove(staging / "foreign.txt");
            fs::remove(staging);
        }
        check(no_leftovers(work), "foreign-file fixture cleaned up");
    }

    const auto output = work / "faults-compress.sarc";
    const auto compress_fails = [&](const std::string &label, const WriteFaults &faults,
                                    ErrorCode code, std::stop_token stop = {}) {
        const ScopedWriteFaults scope{faults};
        check_code(compress_archive(input, output, options, std::move(stop)), code, label);
        check(!fs::exists(output), label + ": no archive");
        check(no_leftovers(work), label + ": temporary removed");
    };
    {
        WriteFaults faults;
        faults.fail_write_at = 3000;
        compress_fails("compress: write failure", faults, ErrorCode::io_error);
    }
    {
        std::stop_source stop;
        WriteFaults faults;
        faults.after_write = [&](std::uint64_t written) {
            if (written >= 2000) {
                stop.request_stop();
            }
        };
        compress_fails("compress: cancelled", faults, ErrorCode::cancelled, stop.get_token());
    }
    {
        WriteFaults faults;
        faults.fail_commit = true;
        compress_fails("compress: commit failure", faults, ErrorCode::io_error);
    }
    {
        WriteFaults faults;
        faults.corrupt_read_back = true;
        compress_fails("compress: read-back mismatch", faults, ErrorCode::integrity_error);
    }
    {
        // The input changes after enumeration (the first write is the archive header).
        bool changed = false;
        WriteFaults faults;
        faults.after_write = [&](std::uint64_t) {
            if (!changed) {
                changed = true;
                std::ofstream{input / "a-small1", std::ios::binary | std::ios::app} << "more";
            }
        };
        const ScopedWriteFaults scope{faults};
        const auto result = compress_archive(input, output, options);
        check_code(result, ErrorCode::io_error, "compress: input changed after enumeration");
        check(!result && result.error().message.find("changed") != std::string::npos,
              "input change is named in the error");
        check(!fs::exists(output) && no_leftovers(work), "changed input leaves no archive");
    }
}

// The primitives the extraction transaction is built from.
void staging_primitive_tests(const fs::path &work) {
    const auto base = work / "primitives";
    fs::create_directory(base);
    auto directory = SafeOutputDirectory::open_existing(base);
    if (!succeeded(directory, "open primitive test directory")) {
        return;
    }
    const auto payload = text("payload");
    {
        auto staged = directory->create_staging_child("final");
        if (succeeded(staged, "create staging directory")) {
            check(!fs::exists(base / "final") && !no_leftovers(base),
                  "staging directory has a temporary name");
            check(staged->write_new_file("inside", payload, crc32c(payload)).has_value(),
                  "write inside staging directory");
            check(staged->commit_staged().has_value(), "commit staging directory");
            check(read_all(base / "final" / "inside") == payload && no_leftovers(base),
                  "committed directory has the final name and content");
            check_code(staged->commit_staged(), ErrorCode::invalid_argument,
                       "staging directory commits once");
        }
        check_code(directory->create_staging_child("final"), ErrorCode::output_conflict,
                   "staging refuses an existing final name");
#ifdef _WIN32
        check_code(directory->create_staging_child("FINAL"), ErrorCode::output_conflict,
                   "staging name check is case-insensitive");
#else
        {
            // Linux file systems are case-sensitive: "FINAL" is a different, free name.
            auto upper = directory->create_staging_child("FINAL");
            check(upper && upper->discard_empty().has_value(),
                  "staging name check is case-sensitive on Linux");
        }
#endif
        check_code(directory->begin_file("final"), ErrorCode::output_conflict,
                   "file session refuses an existing name");
        check(no_leftovers(base), "refused staging leaves nothing");
    }
    {
        auto staged = directory->create_staging_child("discarded");
        check(staged && staged->discard_empty().has_value(), "discard empty staging directory");
        staged = std::unexpected(seedarc::Error{ErrorCode::io_error, "closed"});
        check(!fs::exists(base / "discarded") && no_leftovers(base), "discarded staging is gone");
    }
    {
        {
            auto file = directory->begin_file("streamed");
            if (succeeded(file, "begin streamed file")) {
                const auto data = random_bytes(200'000, 50);
                check(file->append(std::span{data}.first(1)).has_value() &&
                          file->append(std::span{data}.subspan(1)).has_value() &&
                          file->bytes_written() == data.size() && file->checksum() == crc32c(data),
                      "streamed appends track size and CRC32C");
                check_code(file->commit(data.size() - 1), ErrorCode::integrity_error,
                           "commit with the wrong size fails");
                check_code(file->append(payload), ErrorCode::io_error,
                           "failed session stays closed");
            }
        }
        check(!fs::exists(base / "streamed") && no_leftovers(base),
              "failed session removed its temporary");
        {
            auto abandoned = directory->begin_file("abandoned");
            check(abandoned && abandoned->append(payload).has_value(), "write abandoned session");
        }
        check(!fs::exists(base / "abandoned") && no_leftovers(base),
              "abandoned session removed its temporary");
    }
    {
        // Identity-checked removal: nothing is removed unless it is the recorded object.
        // Both handles are closed again before the removal checks below.
        const auto id = [&]() -> std::expected<ObjectId, seedarc::Error> {
            const auto owned = directory->create_child("owned");
            return owned ? owned->identity() : std::unexpected(owned.error());
        }();
        const auto written = [&]() -> std::expected<ObjectId, seedarc::Error> {
            auto file = directory->begin_file("owned-file");
            if (!file) {
                return std::unexpected(file.error());
            }
            const auto file_id = file->identity();
            if (auto appended = file->append(payload); !appended) {
                return std::unexpected(appended.error());
            }
            if (auto committed = file->commit(payload.size()); !committed) {
                return std::unexpected(committed.error());
            }
            return file_id;
        }();
        if (succeeded(id, "owned directory identity") && succeeded(written, "owned file")) {
            const auto file_id = *written;
            check(file_id != *id, "distinct objects have distinct identities");
            check_code(directory->remove_owned_child("owned", file_id, true),
                       ErrorCode::unsafe_path, "directory with another identity is kept");
            check_code(directory->remove_owned_child("owned-file", *id, false),
                       ErrorCode::unsafe_path, "file with another identity is kept");
            check(!directory->remove_owned_child("owned", *id, false), "kind mismatch is refused");
            check_code(directory->open_owned_child("owned", file_id), ErrorCode::unsafe_path,
                       "directory with another identity is not opened");
            check(fs::is_directory(base / "owned") && read_all(base / "owned-file") == payload,
                  "refused removals changed nothing");
            check(directory->open_owned_child("owned", *id).has_value(),
                  "owned directory reopened by identity");
            check(directory->remove_owned_child("owned-file", file_id, false).has_value() &&
                      directory->remove_owned_child("owned", *id, true).has_value(),
                  "owned objects removed by identity");
            check(!fs::exists(base / "owned") && !fs::exists(base / "owned-file"),
                  "owned objects are gone");
        }
    }
}
} // namespace

int main(int argc, char *argv[]) {
    if (argc != 2) {
        return 1;
    }
    const auto base = fs::absolute(argv[1]).lexically_normal();
    fs::create_directories(base);
    const auto work = base / ("m2-store-" + test_fixture::unique_suffix());
    if (!fs::create_directory(work)) {
        return 1;
    }
#ifndef _WIN32
    fs::permissions(work, fs::perms::owner_all, fs::perm_options::replace);
#endif
    round_trip_tests(work);
    hostile_archive_tests(work);
    limit_and_conflict_tests(work);
    rejected_input_tests(work);
#ifndef _WIN32
    non_ascii_refusal_tests(work);
#endif
    fault_injection_tests(work);
    staging_primitive_tests(work);

    // Everything under `work` was created by this process. The only reparse point was removed as
    // a link above; refuse the recursive cleanup if any is still present.
    // Bounded helpers: libstdc++ remove_all() can spin on a vanishing foreign entry (DEV-011).
    const bool reparse_free = test_fixture::reparse_free(work);
    check(reparse_free, "test tree contains no reparse points before cleanup");
    if (reparse_free) {
        const auto ec = test_fixture::remove_tree(work);
        check(!ec && !fs::exists(work), "remove the private test tree");
    }
    return failures == 0 ? 0 : 1;
}
