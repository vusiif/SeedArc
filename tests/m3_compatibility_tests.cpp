// SPDX-License-Identifier: GPL-3.0-or-later
#include "seedarc/store_archive.hpp"

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string_view>
#include <vector>

int main(int argc, char *argv[]) {
    if (argc != 2) {
        return 2;
    }
    for (const auto *name : std::array{"store-empty.sarc", "zstd-text.sarc", "lzma2-text.sarc"}) {
        std::ifstream stream(std::filesystem::path{argv[1]} / name, std::ios::binary);
        if (!stream) {
            std::cerr << "cannot open M3 fixture: " << name << '\n';
            return 1;
        }
        const std::vector<char> chars{std::istreambuf_iterator<char>{stream}, {}};
        std::vector<std::byte> bytes(chars.size());
        std::ranges::transform(chars, bytes.begin(), [](char value) {
            return static_cast<std::byte>(static_cast<unsigned char>(value));
        });
        const auto result = seedarc::experimental::verify_archive_bytes(bytes);
        const auto empty = std::string_view{name} == "store-empty.sarc";
        if (!result || result->entries != (empty ? 0U : 1U) ||
            result->blocks != (empty ? 0U : 1U) || result->total_output != (empty ? 0U : 262144U) ||
            result->archive_bytes != bytes.size()) {
            std::cerr << "M3 fixture failed: " << name << '\n';
            return 1;
        }
        bytes.pop_back();
        if (seedarc::experimental::verify_archive_bytes(bytes)) {
            std::cerr << "truncated M3 fixture accepted: " << name << '\n';
            return 1;
        }
    }
    std::cout << "M3 compatibility fixtures passed\n";
}
