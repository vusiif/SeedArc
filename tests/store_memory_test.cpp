// SPDX-License-Identifier: GPL-3.0-or-later
// Bounded-memory check: round-trips one file much larger than any buffer the library may hold
// and reports the peak working set of this process. Usage: <sandbox directory> <size in MiB>.
#include "seedarc/store_archive.hpp"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <psapi.h>

namespace fs = std::filesystem;
using namespace seedarc::experimental;

namespace {
constexpr std::size_t piece = 64 * 1024;

// Deterministic content generated piece by piece, so the test never holds the file in memory.
class Generator {
  public:
    void fill(std::vector<char> &buffer) {
        for (auto &value : buffer) {
            state_ = state_ * 6364136223846793005ULL + 1442695040888963407ULL;
            value = static_cast<char>(state_ >> 56);
        }
    }

  private:
    std::uint64_t state_ = 0x5eedac;
};

std::uint64_t peak_working_set() {
    PROCESS_MEMORY_COUNTERS counters{};
    counters.cb = sizeof(counters);
    if (!GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters))) {
        return 0;
    }
    return counters.PeakWorkingSetSize;
}

double mib(std::uint64_t bytes) { return static_cast<double>(bytes) / (1024.0 * 1024.0); }
} // namespace

int main(int argc, char *argv[]) {
    if (argc != 3) {
        return 1;
    }
    const auto size = std::strtoull(argv[2], nullptr, 10) * 1024 * 1024;
    const auto base = fs::absolute(argv[1]).lexically_normal();
    fs::create_directories(base);
    const auto work = base / ("m2-memory-" + std::to_string(GetCurrentProcessId()) + "-" +
                              std::to_string(GetTickCount64()));
    if (size == 0 || !fs::create_directory(work)) {
        return 1;
    }
    const auto input = work / "large.bin";
    const auto archive = work / "large.sarc";
    const auto output = work / "out";
    int failures = 0;
    const auto fail = [&failures](const std::string &message) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    };
    {
        std::ofstream stream(input, std::ios::binary);
        Generator generator;
        std::vector<char> buffer(piece);
        for (std::uint64_t written = 0; written < size; written += piece) {
            generator.fill(buffer);
            stream.write(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        }
        if (!stream) {
            fail("cannot write the input fixture");
        }
    }
    const auto baseline = peak_working_set();
    const auto timed = [&](const char *name, const auto &operation) {
        const auto start = std::chrono::steady_clock::now();
        const auto result = operation();
        const auto elapsed =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        if (!result) {
            fail(std::string{name} + ": " + result.error().message);
        }
        std::cout << name << ": " << elapsed << " s (" << mib(size) / elapsed << " MiB/s)\n";
    };
    timed("compress", [&] { return compress_store(input, archive); });
    timed("verify", [&] { return verify_archive(archive); });
    timed("extract", [&] { return extract_archive(archive, output); });
    const auto peak = peak_working_set();
    {
        // Compare the restored file with regenerated content, again piece by piece.
        std::ifstream stream(output / "large.bin", std::ios::binary);
        Generator generator;
        std::vector<char> expected(piece);
        std::vector<char> actual(piece);
        bool same = static_cast<bool>(stream);
        for (std::uint64_t checked = 0; same && checked < size; checked += piece) {
            generator.fill(expected);
            stream.read(actual.data(), static_cast<std::streamsize>(actual.size()));
            same = stream.gcount() == static_cast<std::streamsize>(piece) && actual == expected;
        }
        same = same && stream.peek() == std::char_traits<char>::eof();
        if (!same) {
            fail("restored file differs from the input");
        }
    }
    std::cout << "input " << mib(size) << " MiB; peak working set before " << mib(baseline)
              << " MiB, after " << mib(peak) << " MiB\n";
#if defined(__SANITIZE_ADDRESS__)
    std::cout << "AddressSanitizer build: working set bound not asserted\n";
#else
    // One 1 MiB chunk buffer plus small I/O buffers are expected; the bound is far below the
    // input size, so loading the file (or the archive) into memory would be caught.
    constexpr std::uint64_t allowed_growth = 16ULL * 1024 * 1024;
    if (peak == 0 || size < 4 * allowed_growth || peak - baseline > allowed_growth) {
        fail("peak working set grew by more than 16 MiB (or the input is too small to tell)");
    }
#endif
    std::error_code ec;
    fs::remove(output / "large.bin", ec);
    fs::remove(output, ec);
    fs::remove(archive, ec);
    fs::remove(input, ec);
    if (!fs::remove(work, ec) || ec) {
        fail("cannot remove the private test directory");
    }
    return failures == 0 ? 0 : 1;
}
