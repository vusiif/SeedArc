// SPDX-License-Identifier: GPL-3.0-or-later
// Deterministic corpus replay for fuzz targets in ordinary builds (no libFuzzer runtime).
// Usage: <target> <file-or-directory>...  Inputs larger than 1 MiB are rejected.
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size);

namespace fs = std::filesystem;

int main(int argc, char *argv[]) {
    std::vector<fs::path> inputs;
    for (int i = 1; i < argc; ++i) {
        const fs::path argument{argv[i]};
        if (fs::is_directory(argument)) {
            for (const auto &item : fs::directory_iterator(argument)) {
                if (item.is_regular_file()) {
                    inputs.push_back(item.path());
                }
            }
        } else {
            inputs.push_back(argument);
        }
    }
    std::ranges::sort(inputs); // stable replay order
    std::size_t replayed = 0;
    for (const auto &input : inputs) {
        if (fs::file_size(input) > 1024 * 1024) {
            std::cerr << "input too large: " << input.string() << '\n';
            return 1;
        }
        std::ifstream stream(input, std::ios::binary);
        const std::vector<char> bytes{std::istreambuf_iterator<char>{stream},
                                      std::istreambuf_iterator<char>{}};
        LLVMFuzzerTestOneInput(reinterpret_cast<const std::uint8_t *>(bytes.data()), bytes.size());
        ++replayed;
    }
    std::cout << "replayed " << replayed << " inputs\n";
    return replayed > 0 ? 0 : 1;
}
