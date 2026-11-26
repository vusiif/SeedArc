// SPDX-License-Identifier: GPL-3.0-or-later
#include "codec/dictionary.hpp"

#include "codec/codec.hpp"

#include <lzma.h>
#include <zdict.h>
#include <zstd.h>
#include <zstd_errors.h>

#include <algorithm>
#include <array>
#include <numeric>
#include <string>
#include <utility>

namespace seedarc::experimental::codec::research {
namespace {
Error corrupt(std::string what) {
    return {ErrorCode::integrity_error, "corrupt dictionary-compressed block: " + std::move(what)};
}
bool zstd_ok(std::size_t result) noexcept { return ZSTD_isError(result) == 0; }

// Same rules as the production adapter in codec.cpp (kept local so that file is unchanged).
int window_log_for(std::uint64_t size) noexcept {
    int log = 10;
    while (log < 31 && (std::uint64_t{1} << log) < size) {
        ++log;
    }
    return log;
}
std::uint64_t zstd_window_size(std::span<const std::byte> frame, std::uint64_t content) noexcept {
    const auto descriptor = std::to_integer<unsigned>(frame[4]);
    if ((descriptor & 0x20U) != 0) {
        return content;
    }
    const auto window = std::to_integer<unsigned>(frame[5]);
    const auto base = std::uint64_t{1} << (10 + (window >> 3));
    return base + (base / 8) * (window & 7U);
}

struct CDictDeleter {
    void operator()(ZSTD_CDict *d) const noexcept { ZSTD_freeCDict(d); }
};
struct DDictDeleter {
    void operator()(ZSTD_DDict *d) const noexcept { ZSTD_freeDDict(d); }
};
struct CCtxDeleter {
    void operator()(ZSTD_CCtx *c) const noexcept { ZSTD_freeCCtx(c); }
};
struct DCtxDeleter {
    void operator()(ZSTD_DCtx *c) const noexcept { ZSTD_freeDCtx(c); }
};

std::expected<void, Error> check_dictionary(std::span<const std::byte> dictionary) {
    if (dictionary.size() < min_dictionary_bytes || dictionary.size() > max_dictionary_bytes) {
        return std::unexpected(Error{ErrorCode::resource_limit, "dictionary size out of range"});
    }
    if (dictionary_id(dictionary) == 0) {
        return std::unexpected(Error{ErrorCode::invalid_argument, "not a zstd dictionary"});
    }
    return {};
}
} // namespace

std::uint32_t dictionary_id(std::span<const std::byte> dictionary) noexcept {
    return dictionary.empty()
               ? 0U
               : static_cast<std::uint32_t>(ZDICT_getDictID(dictionary.data(), dictionary.size()));
}

std::expected<std::vector<std::byte>, Error> train_dictionary(std::span<const std::byte> samples,
                                                              std::span<const std::size_t> sizes,
                                                              std::size_t capacity) {
    if (capacity < min_dictionary_bytes || capacity > max_dictionary_bytes) {
        return std::unexpected(Error{ErrorCode::invalid_argument, "dictionary capacity"});
    }
    if (samples.size() > max_training_bytes || sizes.size() > max_training_samples) {
        return std::unexpected(Error{ErrorCode::resource_limit, "training input too large"});
    }
    if (std::accumulate(sizes.begin(), sizes.end(), std::size_t{0}) != samples.size()) {
        return std::unexpected(Error{ErrorCode::invalid_argument, "sample sizes do not add up"});
    }
    std::vector<std::byte> dictionary(capacity);
    const auto result = ZDICT_trainFromBuffer(dictionary.data(), dictionary.size(), samples.data(),
                                              sizes.data(), static_cast<unsigned>(sizes.size()));
    if (ZDICT_isError(result) != 0) {
        return std::unexpected(
            Error{ErrorCode::unsupported_feature,
                  std::string{"dictionary training failed: "} + ZDICT_getErrorName(result)});
    }
    dictionary.resize(result);
    if (auto checked = check_dictionary(dictionary); !checked) {
        return std::unexpected(checked.error());
    }
    return dictionary;
}

struct DictionaryEncoder::Impl {
    std::unique_ptr<ZSTD_CDict, CDictDeleter> dictionary;
    std::unique_ptr<ZSTD_CCtx, CCtxDeleter> context;
};

DictionaryEncoder::DictionaryEncoder(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
DictionaryEncoder::DictionaryEncoder(DictionaryEncoder &&) noexcept = default;
DictionaryEncoder &DictionaryEncoder::operator=(DictionaryEncoder &&) noexcept = default;
DictionaryEncoder::~DictionaryEncoder() = default;

std::expected<DictionaryEncoder, Error>
DictionaryEncoder::create(std::uint32_t level, std::span<const std::byte> dictionary) {
    if (!valid_block_codec(zstd_id, level)) {
        return std::unexpected(Error{ErrorCode::invalid_argument, "invalid zstd level"});
    }
    if (auto checked = check_dictionary(dictionary); !checked) {
        return std::unexpected(checked.error());
    }
    auto impl = std::make_unique<Impl>();
    impl->dictionary.reset(
        ZSTD_createCDict(dictionary.data(), dictionary.size(), static_cast<int>(level)));
    impl->context.reset(ZSTD_createCCtx());
    if (!impl->dictionary || !impl->context) {
        return std::unexpected(
            Error{ErrorCode::integrity_error,
                  "cannot load zstd dictionary (invalid content or out of memory)"});
    }
    return DictionaryEncoder{std::move(impl)};
}

std::expected<std::size_t, Error> DictionaryEncoder::encode(std::span<const std::byte> input,
                                                            std::span<std::byte> output) {
    if (input.empty() || output.size() >= input.size()) {
        return std::unexpected(Error{ErrorCode::invalid_argument, "invalid encode buffers"});
    }
    if (output.empty()) {
        return 0;
    }
    auto *context = impl_->context.get();
    if (!zstd_ok(ZSTD_CCtx_reset(context, ZSTD_reset_session_and_parameters)) ||
        !zstd_ok(ZSTD_CCtx_setParameter(context, ZSTD_c_checksumFlag, 0)) ||
        !zstd_ok(ZSTD_CCtx_setParameter(context, ZSTD_c_contentSizeFlag, 1)) ||
        !zstd_ok(ZSTD_CCtx_setParameter(context, ZSTD_c_dictIDFlag, 1)) ||
        !zstd_ok(ZSTD_CCtx_setParameter(context, ZSTD_c_nbWorkers, 0)) ||
        !zstd_ok(ZSTD_CCtx_refCDict(context, impl_->dictionary.get()))) {
        return std::unexpected(Error{ErrorCode::io_error, "cannot configure zstd encoder"});
    }
    const auto result =
        ZSTD_compress2(context, output.data(), output.size(), input.data(), input.size());
    if (zstd_ok(result)) {
        return result;
    }
    if (ZSTD_getErrorCode(result) == ZSTD_error_dstSize_tooSmall) {
        return 0;
    }
    return std::unexpected(Error{ErrorCode::io_error, std::string{"zstd compression failed: "} +
                                                          ZSTD_getErrorName(result)});
}

std::expected<std::vector<std::byte>, Error> compress_solid(std::span<const std::byte> input,
                                                            std::uint32_t level) {
    if (input.empty() || input.size() > max_solid_bytes || !valid_block_codec(zstd_id, level)) {
        return std::unexpected(Error{ErrorCode::invalid_argument, "invalid solid input or level"});
    }
    const std::unique_ptr<ZSTD_CCtx, CCtxDeleter> context{ZSTD_createCCtx()};
    if (!context) {
        return std::unexpected(Error{ErrorCode::resource_limit, "cannot create zstd context"});
    }
    const auto window =
        std::min<int>(static_cast<int>(max_solid_window_log), window_log_for(input.size()));
    if (!zstd_ok(ZSTD_CCtx_setParameter(context.get(), ZSTD_c_compressionLevel,
                                        static_cast<int>(level))) ||
        !zstd_ok(ZSTD_CCtx_setParameter(context.get(), ZSTD_c_windowLog, window)) ||
        !zstd_ok(ZSTD_CCtx_setParameter(context.get(), ZSTD_c_checksumFlag, 0)) ||
        !zstd_ok(ZSTD_CCtx_setParameter(context.get(), ZSTD_c_contentSizeFlag, 1)) ||
        !zstd_ok(ZSTD_CCtx_setParameter(context.get(), ZSTD_c_nbWorkers, 0))) {
        return std::unexpected(Error{ErrorCode::io_error, "cannot configure zstd encoder"});
    }
    std::vector<std::byte> out(ZSTD_compressBound(input.size()));
    const auto result =
        ZSTD_compress2(context.get(), out.data(), out.size(), input.data(), input.size());
    if (!zstd_ok(result)) {
        return std::unexpected(Error{ErrorCode::io_error, std::string{"zstd compression failed: "} +
                                                              ZSTD_getErrorName(result)});
    }
    out.resize(result);
    return out;
}

std::expected<void, Error> decompress_solid(std::span<const std::byte> frame,
                                            std::span<std::byte> output) {
    if (frame.empty() || output.empty() || output.size() > max_solid_bytes) {
        return std::unexpected(corrupt("empty or oversized solid frame"));
    }
    const auto size = ZSTD_findFrameCompressedSize(frame.data(), frame.size());
    if (!zstd_ok(size) || size != frame.size()) {
        return std::unexpected(corrupt("not exactly one zstd frame"));
    }
    const auto content = ZSTD_getFrameContentSize(frame.data(), frame.size());
    if (content == ZSTD_CONTENTSIZE_UNKNOWN || content == ZSTD_CONTENTSIZE_ERROR ||
        content != output.size()) {
        return std::unexpected(corrupt("zstd frame content size mismatch"));
    }
    if (ZSTD_getDictID_fromFrame(frame.data(), frame.size()) != 0) {
        return std::unexpected(corrupt("solid frame must not use a dictionary"));
    }
    const std::unique_ptr<ZSTD_DCtx, DCtxDeleter> context{ZSTD_createDCtx()};
    if (!context || !zstd_ok(ZSTD_DCtx_setParameter(context.get(), ZSTD_d_windowLogMax,
                                                    static_cast<int>(max_solid_window_log)))) {
        return std::unexpected(Error{ErrorCode::resource_limit, "cannot create zstd decoder"});
    }
    const auto result = ZSTD_decompressDCtx(context.get(), output.data(), output.size(),
                                            frame.data(), frame.size());
    if (!zstd_ok(result)) {
        if (ZSTD_getErrorCode(result) == ZSTD_error_frameParameter_windowTooLarge ||
            ZSTD_getErrorCode(result) == ZSTD_error_memory_allocation) {
            return std::unexpected(
                Error{ErrorCode::resource_limit, "zstd solid frame needs too much memory"});
        }
        return std::unexpected(corrupt(ZSTD_getErrorName(result)));
    }
    if (result != output.size()) {
        return std::unexpected(corrupt("zstd output size mismatch"));
    }
    return {};
}

std::expected<std::size_t, Error> decompress_solid_prefix(std::span<const std::byte> prefix,
                                                          std::span<std::byte> output) {
    if (prefix.empty() || output.empty() || output.size() > max_solid_bytes) {
        return std::unexpected(corrupt("empty or oversized solid frame"));
    }
    const std::unique_ptr<ZSTD_DCtx, DCtxDeleter> context{ZSTD_createDCtx()};
    if (!context || !zstd_ok(ZSTD_DCtx_setParameter(context.get(), ZSTD_d_windowLogMax,
                                                    static_cast<int>(max_solid_window_log)))) {
        return std::unexpected(Error{ErrorCode::resource_limit, "cannot create zstd decoder"});
    }
    ZSTD_inBuffer in{prefix.data(), prefix.size(), 0};
    ZSTD_outBuffer out{output.data(), output.size(), 0};
    while (in.pos < in.size && out.pos < out.size) {
        const auto before_in = in.pos, before_out = out.pos;
        const auto result = ZSTD_decompressStream(context.get(), &out, &in);
        if (!zstd_ok(result)) {
            if (ZSTD_getErrorCode(result) == ZSTD_error_frameParameter_windowTooLarge ||
                ZSTD_getErrorCode(result) == ZSTD_error_memory_allocation) {
                return std::unexpected(
                    Error{ErrorCode::resource_limit, "zstd solid frame needs too much memory"});
            }
            return std::unexpected(corrupt(ZSTD_getErrorName(result)));
        }
        if (result == 0 || (in.pos == before_in && out.pos == before_out)) {
            break; // frame complete, or no progress possible with this input
        }
    }
    return out.pos;
}

namespace {
// The LZMA2 dictionary that covers every match distance of `size` decoded bytes.
std::uint32_t lzma2_solid_dictionary(std::size_t size) noexcept {
    return static_cast<std::uint32_t>(std::clamp<std::size_t>(size, LZMA_DICT_SIZE_MIN, max_solid_bytes));
}
// Raw LZMA2 decoder filter chain for `size` output bytes, with the memory check of codec.cpp.
std::expected<void, Error> lzma2_solid_filters(std::size_t size, lzma_options_lzma &options,
                                               std::array<lzma_filter, 2> &filters) {
    options = {};
    options.dict_size = lzma2_solid_dictionary(size);
    filters = {{{LZMA_FILTER_LZMA2, &options}, {LZMA_VLI_UNKNOWN, nullptr}}};
    const auto memory = lzma_raw_decoder_memusage(filters.data());
    if (memory == UINT64_MAX || memory > std::uint64_t{options.dict_size} + (1U << 20)) {
        return std::unexpected(Error{ErrorCode::resource_limit, "LZMA2 frame needs too much memory"});
    }
    return {};
}
struct LzmaStreamEnd {
    lzma_stream *stream;
    ~LzmaStreamEnd() { lzma_end(stream); }
};
} // namespace

bool valid_solid_codec(std::uint32_t codec, std::uint32_t level) noexcept {
    return (codec == solid_zstd && valid_block_codec(zstd_id, level)) ||
           (codec == solid_lzma2 && valid_block_codec(lzma2_id, level));
}

std::expected<std::vector<std::byte>, Error>
compress_solid_codec(std::uint32_t codec, std::uint32_t level, std::span<const std::byte> input) {
    if (!valid_solid_codec(codec, level) || input.empty() || input.size() > max_solid_bytes) {
        return std::unexpected(Error{ErrorCode::invalid_argument, "invalid solid codec or input"});
    }
    if (codec == solid_zstd) {
        return compress_solid(input, level);
    }
    lzma_options_lzma options{};
    if (lzma_lzma_preset(&options, level) != 0) {
        return std::unexpected(Error{ErrorCode::invalid_argument, "invalid LZMA2 preset"});
    }
    options.dict_size = (std::min)(options.dict_size, lzma2_solid_dictionary(input.size()));
    const std::array<lzma_filter, 2> filters{
        {{LZMA_FILTER_LZMA2, &options}, {LZMA_VLI_UNKNOWN, nullptr}}};
    std::vector<std::byte> out(lzma_block_buffer_bound(input.size()));
    std::size_t written = 0;
    const auto result = lzma_raw_buffer_encode(
        filters.data(), nullptr, reinterpret_cast<const std::uint8_t *>(input.data()), input.size(),
        reinterpret_cast<std::uint8_t *>(out.data()), &written, out.size());
    if (result == LZMA_MEM_ERROR || result == LZMA_MEMLIMIT_ERROR) {
        return std::unexpected(Error{ErrorCode::resource_limit, "LZMA2 encoder out of memory"});
    }
    if (result != LZMA_OK) {
        return std::unexpected(Error{ErrorCode::io_error, "LZMA2 solid compression failed: code " +
                                                              std::to_string(static_cast<int>(result))});
    }
    out.resize(written);
    return out;
}

std::expected<void, Error> decompress_solid_codec(std::uint32_t codec,
                                                  std::span<const std::byte> frame,
                                                  std::span<std::byte> output) {
    if (codec == solid_zstd) {
        return decompress_solid(frame, output);
    }
    if (codec != solid_lzma2) {
        return std::unexpected(corrupt("unknown solid codec"));
    }
    if (frame.empty() || output.empty() || output.size() > max_solid_bytes) {
        return std::unexpected(corrupt("empty or oversized solid frame"));
    }
    lzma_options_lzma options{};
    std::array<lzma_filter, 2> filters{};
    if (auto ok = lzma2_solid_filters(output.size(), options, filters); !ok) {
        return ok;
    }
    std::size_t consumed = 0, produced = 0;
    const auto result = lzma_raw_buffer_decode(
        filters.data(), nullptr, reinterpret_cast<const std::uint8_t *>(frame.data()), &consumed,
        frame.size(), reinterpret_cast<std::uint8_t *>(output.data()), &produced, output.size());
    if (result == LZMA_MEM_ERROR || result == LZMA_MEMLIMIT_ERROR) {
        return std::unexpected(Error{ErrorCode::resource_limit, "LZMA2 frame needs too much memory"});
    }
    // LZMA_OK: the end marker was reached; the whole frame must be used and fill the output.
    if (result != LZMA_OK || consumed != frame.size() || produced != output.size()) {
        return std::unexpected(corrupt("LZMA2 solid frame is invalid, truncated or has trailing "
                                       "bytes (code " + std::to_string(static_cast<int>(result)) + ")"));
    }
    return {};
}

std::expected<std::size_t, Error> decompress_solid_prefix_codec(std::uint32_t codec,
                                                                std::span<const std::byte> prefix,
                                                                std::span<std::byte> output) {
    if (codec == solid_zstd) {
        return decompress_solid_prefix(prefix, output);
    }
    if (codec != solid_lzma2) {
        return std::unexpected(corrupt("unknown solid codec"));
    }
    if (prefix.empty() || output.empty() || output.size() > max_solid_bytes) {
        return std::unexpected(corrupt("empty or oversized solid frame"));
    }
    lzma_options_lzma options{};
    std::array<lzma_filter, 2> filters{};
    if (auto ok = lzma2_solid_filters(output.size(), options, filters); !ok) {
        return std::unexpected(ok.error());
    }
    lzma_stream stream = LZMA_STREAM_INIT;
    if (lzma_raw_decoder(&stream, filters.data()) != LZMA_OK) {
        return std::unexpected(Error{ErrorCode::resource_limit, "cannot create LZMA2 decoder"});
    }
    const LzmaStreamEnd end{&stream};
    stream.next_in = reinterpret_cast<const std::uint8_t *>(prefix.data());
    stream.avail_in = prefix.size();
    stream.next_out = reinterpret_cast<std::uint8_t *>(output.data());
    stream.avail_out = output.size();
    while (stream.avail_in > 0 && stream.avail_out > 0) {
        const auto before_in = stream.avail_in, before_out = stream.avail_out;
        const auto result = lzma_code(&stream, LZMA_RUN);
        if (result == LZMA_STREAM_END) {
            break;
        }
        if (result == LZMA_MEM_ERROR || result == LZMA_MEMLIMIT_ERROR) {
            return std::unexpected(Error{ErrorCode::resource_limit, "LZMA2 frame needs too much memory"});
        }
        if (result != LZMA_OK) {
            return std::unexpected(corrupt("LZMA2 solid frame is invalid (code " +
                                           std::to_string(static_cast<int>(result)) + ")"));
        }
        if (stream.avail_in == before_in && stream.avail_out == before_out) {
            break; // no progress possible with this input
        }
    }
    return output.size() - stream.avail_out;
}

struct DictionaryDecoder::Impl {
    std::unique_ptr<ZSTD_DDict, DDictDeleter> dictionary;
    std::unique_ptr<ZSTD_DCtx, DCtxDeleter> context;
    std::uint32_t id = 0;
    std::size_t size = 0;
};

DictionaryDecoder::DictionaryDecoder(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
DictionaryDecoder::DictionaryDecoder(DictionaryDecoder &&) noexcept = default;
DictionaryDecoder &DictionaryDecoder::operator=(DictionaryDecoder &&) noexcept = default;
DictionaryDecoder::~DictionaryDecoder() = default;

std::expected<DictionaryDecoder, Error>
DictionaryDecoder::create(std::span<const std::byte> dictionary) {
    if (auto checked = check_dictionary(dictionary); !checked) {
        return std::unexpected(checked.error());
    }
    auto impl = std::make_unique<Impl>();
    impl->dictionary.reset(ZSTD_createDDict(dictionary.data(), dictionary.size()));
    impl->context.reset(ZSTD_createDCtx());
    if (!impl->dictionary || !impl->context) {
        return std::unexpected(
            Error{ErrorCode::integrity_error,
                  "cannot load zstd dictionary (invalid content or out of memory)"});
    }
    impl->id = dictionary_id(dictionary);
    impl->size = dictionary.size();
    return DictionaryDecoder{std::move(impl)};
}

std::expected<void, Error> DictionaryDecoder::decode(std::span<const std::byte> input,
                                                     std::span<std::byte> output) {
    if (input.empty() || output.empty()) {
        return std::unexpected(corrupt("empty block"));
    }
    const auto frame = ZSTD_findFrameCompressedSize(input.data(), input.size());
    if (!zstd_ok(frame) || frame != input.size()) {
        return std::unexpected(corrupt("not exactly one zstd frame"));
    }
    const auto content = ZSTD_getFrameContentSize(input.data(), input.size());
    if (content == ZSTD_CONTENTSIZE_UNKNOWN || content == ZSTD_CONTENTSIZE_ERROR ||
        content != output.size()) {
        return std::unexpected(corrupt("zstd frame content size mismatch"));
    }
    if (ZSTD_getDictID_fromFrame(input.data(), input.size()) != impl_->id) {
        return std::unexpected(corrupt("frame does not use this dictionary"));
    }
    // Single-shot decoding keeps history in `output` plus the dictionary, so a declared window
    // beyond dictionary + block is refused, as the encoder never needs one.
    if (zstd_window_size(input, content) >
        (std::uint64_t{1} << window_log_for(output.size() + impl_->size))) {
        return std::unexpected(
            Error{ErrorCode::resource_limit, "zstd window is larger than dictionary + block"});
    }
    auto *context = impl_->context.get();
    if (!zstd_ok(ZSTD_DCtx_reset(context, ZSTD_reset_session_and_parameters))) {
        return std::unexpected(Error{ErrorCode::io_error, "cannot reset zstd decoder"});
    }
    const auto result = ZSTD_decompress_usingDDict(
        context, output.data(), output.size(), input.data(), input.size(), impl_->dictionary.get());
    if (!zstd_ok(result)) {
        if (ZSTD_getErrorCode(result) == ZSTD_error_frameParameter_windowTooLarge ||
            ZSTD_getErrorCode(result) == ZSTD_error_memory_allocation) {
            return std::unexpected(
                Error{ErrorCode::resource_limit, "zstd block needs too much memory"});
        }
        return std::unexpected(corrupt(ZSTD_getErrorName(result)));
    }
    if (result != output.size()) {
        return std::unexpected(corrupt("zstd output size mismatch"));
    }
    return {};
}
} // namespace seedarc::experimental::codec::research
