// SPDX-License-Identifier: GPL-3.0-or-later
// DEV-013 research dictionary adapter: round trip, size benefit on similar small records,
// strict rejection of wrong dictionaries / frames, and resource limits. In-memory, portable.
#include "codec/codec.hpp"
#include "codec/dictionary.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <expected>
#include <iostream>
#include <span>
#include <string>
#include <vector>

namespace codec = seedarc::experimental::codec;
namespace research = seedarc::experimental::codec::research;
using seedarc::ErrorCode;

namespace {
int failures = 0;
void check(bool condition, const char *message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}
std::uint64_t next(std::uint64_t &state) {
    state = state * 6364136223846793005ULL + 1442695040888963407ULL;
    return state >> 33;
}
std::vector<std::byte> bytes_of(const std::string &text) {
    std::vector<std::byte> out(text.size());
    std::ranges::transform(text, out.begin(), [](char c) { return static_cast<std::byte>(c); });
    return out;
}
// Small, similar configuration-like records: what a shared dictionary is meant to exploit.
std::vector<std::byte> record(std::uint64_t &state) {
    std::string text = "# SeedArc fixture\nset(PACKAGE_NAME \"pkg" +
                       std::to_string(next(state) % 997) + "\")\nset(PACKAGE_VERSION \"" +
                       std::to_string(next(state) % 9) + "." + std::to_string(next(state) % 30) +
                       "\")\nfind_dependency(Threads REQUIRED)\n"
                       "include(\"${CMAKE_CURRENT_LIST_DIR}/targets.cmake\")\n";
    for (auto i = next(state) % 4; i != 0; --i) {
        text += "set_property(TARGET pkg::lib PROPERTY INTERFACE_LINK_LIBRARIES \"dep" +
                std::to_string(next(state) % 50) + "\")\n";
    }
    return bytes_of(text);
}
std::size_t encoded_size(std::span<const std::byte> input,
                         const std::span<const std::byte> *dictionary) {
    std::vector<std::byte> out(input.size() - 1);
    if (dictionary != nullptr) {
        auto encoder = research::DictionaryEncoder::create(3, *dictionary);
        auto size =
            encoder ? encoder->encode(input, out) : std::expected<std::size_t, seedarc::Error>{0};
        return size && *size != 0 ? *size : input.size();
    }
    auto encoder = codec::Encoder::create(codec::zstd_id, 3);
    auto size =
        encoder ? encoder->encode(input, out) : std::expected<std::size_t, seedarc::Error>{0};
    return size && *size != 0 ? *size : input.size();
}
} // namespace

int main() {
    std::uint64_t state = 2026;
    std::vector<std::byte> samples;
    std::vector<std::size_t> sizes;
    for (int i = 0; i < 400; ++i) {
        const auto r = record(state);
        samples.insert(samples.end(), r.begin(), r.end());
        sizes.push_back(r.size());
    }
    const auto dictionary = research::train_dictionary(samples, sizes, 4096);
    check(dictionary.has_value(), "dictionary trains from similar samples");
    if (!dictionary) {
        std::cerr << dictionary.error().message << '\n';
        return 1;
    }
    check(dictionary->size() <= 4096 && dictionary->size() >= research::min_dictionary_bytes,
          "dictionary respects the requested capacity");
    const std::span<const std::byte> dict{*dictionary};
    check(research::dictionary_id(dict) != 0, "trained dictionary has an ID");

    // Round trip and benefit on unseen records of the same kind.
    auto encoder = research::DictionaryEncoder::create(3, dict);
    auto decoder = research::DictionaryDecoder::create(dict);
    check(encoder.has_value() && decoder.has_value(), "encoder and decoder are created");
    std::size_t plain_total = 0;
    std::size_t dict_total = 0;
    std::vector<std::byte> frame;
    std::vector<std::byte> unseen;
    for (int i = 0; i < 50; ++i) {
        unseen = record(state);
        std::vector<std::byte> out(unseen.size() - 1);
        const auto size = encoder->encode(unseen, out);
        check(size.has_value(), "dictionary encode succeeds");
        if (!size || *size == 0) {
            continue;
        }
        std::vector<std::byte> restored(unseen.size());
        check(decoder->decode(std::span{out}.first(*size), restored).has_value() &&
                  restored == unseen,
              "dictionary frame round-trips byte for byte");
        frame.assign(out.begin(), out.begin() + static_cast<std::ptrdiff_t>(*size));
        dict_total += *size;
        plain_total += encoded_size(unseen, nullptr);
    }
    check(dict_total < plain_total, "dictionary makes similar small records smaller");

    // Rejections: other dictionary, plain frame, trailing bytes, wrong size, truncation.
    std::uint64_t other_state = 77;
    std::vector<std::byte> other_samples;
    std::vector<std::size_t> other_sizes;
    for (int i = 0; i < 400; ++i) {
        auto r = bytes_of("other corpus line " + std::to_string(next(other_state)) + " with " +
                          std::to_string(next(other_state) % 13) + " fields\n");
        other_samples.insert(other_samples.end(), r.begin(), r.end());
        other_sizes.push_back(r.size());
    }
    const auto other = research::train_dictionary(other_samples, other_sizes, 4096);
    check(other.has_value() && research::dictionary_id(*other) != research::dictionary_id(dict),
          "second dictionary has a different ID");
    std::vector<std::byte> restored(unseen.size());
    if (other) {
        auto wrong = research::DictionaryDecoder::create(*other);
        check(wrong && !wrong->decode(frame, restored), "frame for another dictionary rejected");
    }
    std::vector<std::byte> plain(unseen.size() - 1);
    auto plain_encoder = codec::Encoder::create(codec::zstd_id, 3);
    const auto plain_size = plain_encoder->encode(unseen, plain);
    if (plain_size && *plain_size != 0) {
        check(!decoder->decode(std::span{plain}.first(*plain_size), restored),
              "frame without the dictionary ID rejected");
    }
    auto trailing = frame;
    trailing.push_back(std::byte{0});
    check(!decoder->decode(trailing, restored), "trailing input rejected");
    std::vector<std::byte> short_output(unseen.size() - 1);
    check(!decoder->decode(frame, short_output), "content size mismatch rejected");
    check(!decoder->decode(std::span{frame}.first(frame.size() - 1), restored),
          "truncated frame rejected");
    check(!decoder->decode({}, restored), "empty frame rejected");

    // Limits: capacity range, oversized / non-dictionary inputs, too little training data.
    check(!research::train_dictionary(samples, sizes, research::max_dictionary_bytes + 1),
          "capacity above the ceiling rejected");
    check(!research::train_dictionary(samples, sizes, 16), "capacity below the floor rejected");
    std::vector<std::size_t> bad_sizes = sizes;
    bad_sizes.back() += 1;
    check(!research::train_dictionary(samples, bad_sizes, 4096),
          "inconsistent sample sizes rejected");
    const std::vector<std::byte> tiny(64, std::byte{'x'});
    const std::array<std::size_t, 1> one{tiny.size()};
    check(!research::train_dictionary(tiny, one, 4096), "too little training data fails cleanly");
    const std::vector<std::byte> huge(research::max_dictionary_bytes + 1, std::byte{0});
    const auto oversized = research::DictionaryDecoder::create(huge);
    check(!oversized && oversized.error().code == ErrorCode::resource_limit,
          "oversized dictionary rejected before allocation");
    const std::vector<std::byte> raw(4096, std::byte{'r'});
    check(!research::DictionaryDecoder::create(raw) && !research::DictionaryEncoder::create(3, raw),
          "raw content without a dictionary header rejected");
    check(!research::DictionaryEncoder::create(0, dict) &&
              !research::DictionaryEncoder::create(20, dict),
          "out-of-range zstd level rejected");

    std::cout << (failures == 0 ? "all dictionary checks passed" : "dictionary checks failed")
              << '\n';
    return failures == 0 ? 0 : 1;
}
