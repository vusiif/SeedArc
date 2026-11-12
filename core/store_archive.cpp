// SPDX-License-Identifier: GPL-3.0-or-later
#include "seedarc/store_archive.hpp"

#include "filesystem/input_tree.hpp"
#include "seedarc/archive_index.hpp"
#include "seedarc/checked_arithmetic.hpp"
#include "seedarc/safe_output.hpp"

#include <algorithm>
#include <exception>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace seedarc::experimental {
namespace {
namespace fs = std::filesystem;

Error cancelled() { return {ErrorCode::cancelled, "operation cancelled"}; }

std::unexpected<Error> malformed(std::string message) {
    return std::unexpected(Error{ErrorCode::malformed_archive, std::move(message)});
}

struct Target {
    fs::path parent;
    std::string name; // UTF-8
};

// Splits an output path into an existing parent directory and one new name.
std::expected<Target, Error> split_target(const fs::path &path) {
    const auto invalid = [] {
        return std::unexpected(
            Error{ErrorCode::invalid_argument, "output path must name a new file or directory"});
    };
    std::error_code ec;
    auto absolute = fs::absolute(path, ec);
    if (ec || absolute.empty()) {
        return invalid();
    }
    absolute = absolute.lexically_normal();
    if (!absolute.has_filename()) {
        absolute = absolute.parent_path(); // drop a trailing separator
    }
    if (!absolute.has_filename() || !absolute.has_parent_path()) {
        return invalid();
    }
    try {
        const auto name = absolute.filename().u8string();
        return Target{absolute.parent_path(), std::string{name.begin(), name.end()}};
    } catch (const std::exception &) {
        return std::unexpected(
            Error{ErrorCode::unsafe_path, "output name cannot be represented as UTF-8"});
    }
}

// --- reading -----------------------------------------------------------------------------

class Source {
  public:
    virtual ~Source() = default;
    [[nodiscard]] virtual std::uint64_t size() const = 0;
    [[nodiscard]] virtual std::expected<void, Error> read_at(std::uint64_t offset,
                                                             std::span<std::byte> out) = 0;
};

class MemorySource final : public Source {
  public:
    explicit MemorySource(std::span<const std::byte> bytes) : bytes_(bytes) {}
    [[nodiscard]] std::uint64_t size() const override { return bytes_.size(); }
    [[nodiscard]] std::expected<void, Error> read_at(std::uint64_t offset,
                                                     std::span<std::byte> out) override {
        if (offset > bytes_.size() || out.size() > bytes_.size() - offset) {
            return malformed("archive is truncated");
        }
        std::ranges::copy(bytes_.subspan(static_cast<std::size_t>(offset), out.size()),
                          out.begin());
        return {};
    }

  private:
    std::span<const std::byte> bytes_;
};

class FileSource final : public Source {
  public:
    [[nodiscard]] std::expected<void, Error> open(const fs::path &path) {
        stream_.open(path, std::ios::binary);
        if (!stream_) {
            return std::unexpected(Error{ErrorCode::io_error, "cannot open archive for reading"});
        }
        stream_.seekg(0, std::ios::end);
        const auto end = stream_.tellg();
        if (!stream_ || end < 0) {
            return std::unexpected(Error{ErrorCode::io_error, "cannot determine archive size"});
        }
        size_ = static_cast<std::uint64_t>(end);
        return {};
    }
    [[nodiscard]] std::uint64_t size() const override { return size_; }
    [[nodiscard]] std::expected<void, Error> read_at(std::uint64_t offset,
                                                     std::span<std::byte> out) override {
        if (offset > size_ || out.size() > size_ - offset) {
            return malformed("archive is truncated");
        }
        stream_.clear();
        stream_.seekg(static_cast<std::streamoff>(offset));
        stream_.read(reinterpret_cast<char *>(out.data()),
                     static_cast<std::streamsize>(out.size()));
        if (!stream_ || static_cast<std::size_t>(stream_.gcount()) != out.size()) {
            return std::unexpected(Error{ErrorCode::io_error, "archive read failed"});
        }
        return {};
    }

  private:
    std::ifstream stream_;
    std::uint64_t size_ = 0;
};

// Receives verified content. The default implementation discards it (verify).
class Sink {
  public:
    virtual ~Sink() = default;
    // Called once, after the whole index has been validated and before any other callback.
    [[nodiscard]] virtual std::expected<void, Error> begin(const ArchiveIndex &) { return {}; }
    [[nodiscard]] virtual std::expected<void, Error> directory(const IndexEntry &) { return {}; }
    [[nodiscard]] virtual std::expected<void, Error> begin_file(const IndexEntry &) { return {}; }
    [[nodiscard]] virtual std::expected<void, Error> data(std::span<const std::byte>) { return {}; }
    [[nodiscard]] virtual std::expected<void, Error> end_file(const IndexEntry &) { return {}; }
};

// The one parse / validate / decode path shared by verify and extract (ADR-0002 section 1).
std::expected<ArchiveSummary, Error> run_pipeline(Source &source, const ArchiveLimits &limits,
                                                  Sink &sink, const std::stop_token &stop) {
    // Checked at entry and again after the last entry, so that a run with no entries (or only
    // empty files) still honours a cancellation instead of reporting success.
    if (stop.stop_requested()) {
        return std::unexpected(cancelled());
    }
    if (source.size() < archive_header_bytes) {
        return malformed("archive is shorter than its header");
    }
    std::array<std::byte, archive_header_bytes> header_bytes{};
    if (auto read = source.read_at(0, header_bytes); !read) {
        return std::unexpected(read.error());
    }
    const auto header = decode_header(header_bytes, limits);
    if (!header) {
        return std::unexpected(header.error());
    }
    // decode_header verified that this sum cannot overflow and that every part is within limits.
    const auto expected_size = archive_header_bytes + header->data_bytes + header->index_bytes;
    if (source.size() < expected_size) {
        return malformed("archive is truncated");
    }
    if (source.size() > expected_size) {
        return malformed("archive has undeclared trailing bytes");
    }
    // index_bytes is within max_index_bytes and present in the source, so this is bounded.
    std::vector<std::byte> index_bytes(static_cast<std::size_t>(header->index_bytes));
    if (auto read = source.read_at(archive_header_bytes + header->data_bytes, index_bytes); !read) {
        return std::unexpected(read.error());
    }
    const auto index = parse_index(*header, index_bytes, limits);
    if (!index) {
        return std::unexpected(index.error());
    }
    index_bytes = {};

    if (auto begun = sink.begin(*index); !begun) {
        return std::unexpected(begun.error());
    }
    std::vector<std::byte> buffer; // reused; never larger than one chunk (<= max_chunk_bytes)
    for (const auto &entry : index->entries) {
        if (stop.stop_requested()) {
            return std::unexpected(cancelled());
        }
        if (entry.directory) {
            if (auto made = sink.directory(entry); !made) {
                return std::unexpected(made.error());
            }
            continue;
        }
        if (auto begun = sink.begin_file(entry); !begun) {
            return std::unexpected(begun.error());
        }
        for (std::uint64_t i = 0; i < entry.block_count; ++i) {
            if (stop.stop_requested()) {
                return std::unexpected(cancelled());
            }
            const auto number = static_cast<std::size_t>(entry.first_block + i);
            const auto &block = index->blocks[number];
            buffer.resize(static_cast<std::size_t>(block.stored_size));
            if (auto read = source.read_at(archive_header_bytes + block.data_offset, buffer);
                !read) {
                return std::unexpected(read.error());
            }
            // Store: the stored bytes are the raw bytes (parse_index checked the lengths).
            if (crc32c(buffer) != block.raw_checksum) {
                return std::unexpected(
                    Error{ErrorCode::integrity_error,
                          "block " + std::to_string(number) + " CRC32C mismatch"});
            }
            if (auto written = sink.data(buffer); !written) {
                return std::unexpected(written.error());
            }
        }
        if (auto ended = sink.end_file(entry); !ended) {
            return std::unexpected(ended.error());
        }
    }
    if (stop.stop_requested()) {
        return std::unexpected(cancelled());
    }
    return ArchiveSummary{header->entry_count, header->block_count, header->total_output,
                          expected_size};
}

// --- extraction ----------------------------------------------------------------------------

// Writes into a staging directory. Entries arrive in depth-first pre-order with explicit parent
// directories (guaranteed by parse_index), so the open directories form a stack and every
// directory stays pinned while its contents are written.
class ExtractSink final : public Sink {
  public:
    ExtractSink(const SafeOutputDirectory &parent, std::string name, std::stop_token stop)
        : parent_(parent), name_(std::move(name)), stop_(std::move(stop)) {}

    [[nodiscard]] std::expected<void, Error> begin(const ArchiveIndex &index) override {
        auto staging = parent_.create_staging_child(name_);
        if (!staging) {
            return std::unexpected(staging.error());
        }
        stack_.push_back(std::move(*staging));
        created_.reserve(index.entries.size());
        return {};
    }

    [[nodiscard]] std::expected<void, Error> directory(const IndexEntry &entry) override {
        const auto name = enter(entry);
        if (!name) {
            return std::unexpected(name.error());
        }
        auto child = stack_.back().create_child(*name);
        if (!child) {
            return std::unexpected(child.error());
        }
        const auto id = child->identity();
        if (!id) {
            (void)child->discard_empty(); // not recorded yet, so remove it here
            return std::unexpected(id.error());
        }
        created_.push_back({*id, parent_entry(), std::string{*name}, true});
        owners_.push_back(created_.size() - 1);
        stack_.push_back(std::move(*child));
        return {};
    }

    [[nodiscard]] std::expected<void, Error> begin_file(const IndexEntry &entry) override {
        const auto name = enter(entry);
        if (!name) {
            return std::unexpected(name.error());
        }
        auto file = stack_.back().begin_file(*name);
        if (!file) {
            return std::unexpected(file.error());
        }
        pending_ = {file->identity(), parent_entry(), std::string{*name}, false};
        file_.emplace(std::move(*file));
        return {};
    }

    [[nodiscard]] std::expected<void, Error> data(std::span<const std::byte> bytes) override {
        return file_->append(bytes);
    }

    [[nodiscard]] std::expected<void, Error> end_file(const IndexEntry &entry) override {
        const auto committed = file_->commit(entry.size, stop_);
        pending_.size = entry.size;
        pending_.crc = file_->checksum(); // CRC32C of the verified bytes that were appended
        file_.reset();
        if (committed) {
            created_.push_back(std::move(pending_)); // only committed files need rollback
        }
        return committed;
    }

    // Closes every handle below the staging directory, re-verifies the whole staged tree and
    // renames it to the target name.
    //
    // Protection between writing and publication (ADR-0002 section 8.4): the staging directory
    // carries a protected DACL from its creation, so only this user, SYSTEM and Administrators can
    // change it. This final pass then requires every staged directory to contain exactly the
    // entries this run created and every file to keep its identity, a single link, no named
    // streams and its verified size and CRC32C. A trusted principal that modifies the tree after
    // this pass and before the rename is outside the guarantee.
    [[nodiscard]] std::expected<void, Error> commit() {
        if (stack_.empty()) {
            return std::unexpected(Error{ErrorCode::io_error, "extraction never started"});
        }
        close_children();
        if (auto verified = verify_staged_tree(); !verified) {
            return verified;
        }
        if (stop_.stop_requested()) {
            return std::unexpected(cancelled());
        }
        return stack_.front().commit_staged();
    }

    // Removes what this run created, newest first, each object only if it still has the identity
    // recorded when it was created. Returns a description of anything that could not be removed.
    [[nodiscard]] std::string rollback() {
        file_.reset(); // an uncommitted file deletes its own temporary
        if (stack_.empty()) {
            return {};
        }
        close_children();
        std::size_t failures = 0;
        std::string first;
        const auto note = [&failures, &first](const Error &error) {
            if (failures++ == 0) {
                first = error.message;
            }
        };
        std::vector<std::size_t> chain;
        for (auto k = created_.size(); k-- > 0;) {
            chain.clear();
            for (auto p = created_[k].parent; p != no_parent; p = created_[p].parent) {
                chain.push_back(p);
            }
            std::expected<SafeOutputDirectory, Error> directory{stack_.front()};
            for (auto it = chain.rbegin(); directory && it != chain.rend(); ++it) {
                directory = directory->open_owned_child(created_[*it].name, created_[*it].id);
            }
            if (!directory) {
                note(directory.error());
                continue;
            }
            if (const auto removed = directory->remove_owned_child(created_[k].name, created_[k].id,
                                                                   created_[k].directory);
                !removed) {
                note(removed.error());
            }
        }
        if (const auto discarded = stack_.front().discard_empty(); !discarded) {
            note(discarded.error());
        }
        stack_.clear();
        created_.clear();
        if (failures == 0) {
            return {};
        }
        return "cleanup incomplete (" + std::to_string(failures) + " objects, first: " + first +
               "); a .seedarc-*.tmp directory may remain next to the target";
    }

  private:
    static constexpr auto no_parent = static_cast<std::size_t>(-1);
    struct Created {
        ObjectId id;
        std::size_t parent = no_parent; // index into created_
        std::string name;
        bool directory = false;
        std::uint64_t size = 0; // files only
        std::uint32_t crc = 0;  // files only: CRC32C of the whole content
    };

    // Walks created_ in its depth-first pre-order (the archive entry order) with at most one open
    // handle per depth level, as during writing.
    [[nodiscard]] std::expected<void, Error> verify_staged_tree() const {
        // children[0] lists the staging directory itself, children[k + 1] the directory
        // created_[k].
        std::vector<std::vector<std::string>> children(created_.size() + 1);
        for (const auto &item : created_) {
            children[item.parent == no_parent ? 0 : item.parent + 1].push_back(item.name);
        }
        if (auto exact = stack_.front().require_exact_children(children[0]); !exact) {
            return exact;
        }
        std::vector<std::pair<std::size_t, SafeOutputDirectory>> open; // created_ index, handle
        for (std::size_t k = 0; k < created_.size(); ++k) {
            if (stop_.stop_requested()) {
                return std::unexpected(cancelled());
            }
            const auto &item = created_[k];
            while (!open.empty() && open.back().first != item.parent) {
                open.pop_back();
            }
            if (item.parent != no_parent && open.empty()) {
                return std::unexpected(
                    Error{ErrorCode::io_error, "internal error: staged tree order"});
            }
            const auto &directory = open.empty() ? stack_.front() : open.back().second;
            if (!item.directory) {
                if (auto verified =
                        directory.verify_owned_file(item.name, item.id, item.size, item.crc, stop_);
                    !verified) {
                    return verified;
                }
                continue;
            }
            auto child = directory.open_owned_child(item.name, item.id);
            if (!child) {
                return std::unexpected(child.error());
            }
            if (auto exact = child->require_exact_children(children[k + 1]); !exact) {
                return exact;
            }
            open.emplace_back(k, std::move(*child));
        }
        return {};
    }

    // Trims the directory stack to the entry's parent and returns the entry's basename.
    [[nodiscard]] std::expected<std::string_view, Error> enter(const IndexEntry &entry) {
        const std::string_view path{entry.path};
        const auto level = static_cast<std::size_t>(std::ranges::count(path, '/'));
        if (stack_.empty() || level >= stack_.size()) {
            return malformed("archive entry has no open parent directory");
        }
        while (stack_.size() > level + 1) {
            stack_.pop_back();
            owners_.pop_back();
        }
        const auto slash = path.rfind('/');
        return slash == std::string_view::npos ? path : path.substr(slash + 1);
    }

    [[nodiscard]] std::size_t parent_entry() const noexcept {
        return owners_.empty() ? no_parent : owners_.back();
    }

    void close_children() {
        while (stack_.size() > 1) {
            stack_.pop_back();
        }
        owners_.clear();
    }

    const SafeOutputDirectory &parent_;
    std::string name_;
    std::stop_token stop_;
    std::vector<SafeOutputDirectory> stack_; // [0] is the staging directory
    std::vector<std::size_t> owners_;        // created_ index of stack_[i + 1]
    std::vector<Created> created_;
    Created pending_;
    std::optional<SafeOutputFile> file_;
};
} // namespace

std::expected<ArchiveSummary, Error>
verify_archive(const fs::path &archive, const ArchiveLimits &limits, std::stop_token stop) {
    FileSource source;
    if (auto opened = source.open(archive); !opened) {
        return std::unexpected(opened.error());
    }
    Sink discard;
    return run_pipeline(source, limits, discard, stop);
}

std::expected<ArchiveSummary, Error> verify_archive_bytes(std::span<const std::byte> archive,
                                                          const ArchiveLimits &limits) {
    MemorySource source{archive};
    Sink discard;
    return run_pipeline(source, limits, discard, {});
}

std::expected<ArchiveSummary, Error> extract_archive(const fs::path &archive,
                                                     const fs::path &new_directory,
                                                     const ArchiveLimits &limits,
                                                     std::stop_token stop) {
    const auto target = split_target(new_directory);
    if (!target) {
        return std::unexpected(target.error());
    }
    FileSource source;
    if (auto opened = source.open(archive); !opened) {
        return std::unexpected(opened.error());
    }
    const auto parent = SafeOutputDirectory::open_existing(target->parent);
    if (!parent) {
        return std::unexpected(parent.error());
    }
    ExtractSink sink{*parent, target->name, stop};
    auto result = run_pipeline(source, limits, sink, stop);
    if (result) {
        if (const auto committed = sink.commit(); !committed) {
            result = std::unexpected(committed.error());
        }
    }
    if (!result) {
        auto error = result.error();
        if (const auto leftover = sink.rollback(); !leftover.empty()) {
            error.message += "; " + leftover;
        }
        return std::unexpected(std::move(error));
    }
    return result;
}

std::expected<ArchiveSummary, Error> compress_store(const fs::path &input,
                                                    const fs::path &output_archive,
                                                    const CompressOptions &options,
                                                    std::stop_token stop) {
    const auto &limits = options.limits;
    if (options.chunk_size_bytes == 0) {
        return std::unexpected(Error{ErrorCode::invalid_argument, "chunk size must not be zero"});
    }
    if (options.chunk_size_bytes > limits.max_chunk_bytes) {
        return std::unexpected(Error{ErrorCode::resource_limit, "chunk size exceeds the limit"});
    }
    const auto target = split_target(output_archive);
    if (!target) {
        return std::unexpected(target.error());
    }
    const auto tree = internal::InputTree::open(input);
    if (!tree) {
        return std::unexpected(tree.error());
    }
    const auto parent = SafeOutputDirectory::open_existing(target->parent);
    if (!parent) {
        return std::unexpected(parent.error());
    }
    // The archive's temporary exists before enumeration so that it can be excluded by identity
    // when the output lies inside the input tree. Every early return below deletes it.
    auto output = parent->begin_file(target->name);
    if (!output) {
        return std::unexpected(output.error());
    }
    const auto entries = tree->enumerate(limits, &output->identity(), stop);
    if (!entries) {
        return std::unexpected(entries.error());
    }

    // Sizes are known from enumeration, so the header and the index layout are fixed before any
    // content is read; a file that turns out to differ fails the run instead of changing them.
    ArchiveIndex index;
    index.entries.reserve(entries->size());
    std::uint64_t block_count = 0;
    std::uint64_t total = 0;
    for (const auto &entry : *entries) {
        const auto blocks = entry.size / options.chunk_size_bytes +
                            (entry.size % options.chunk_size_bytes != 0 ? 1U : 0U);
        index.entries.push_back({entry.path, entry.directory, entry.size, block_count, blocks});
        const auto next = checked_add(block_count, blocks);
        if (!next || *next > limits.max_blocks) {
            return std::unexpected(
                Error{ErrorCode::resource_limit, "input needs more blocks than the limit"});
        }
        block_count = *next;
        total += entry.size; // bounded by max_total_output during enumeration
    }
    index.blocks.resize(static_cast<std::size_t>(block_count));
    const auto index_size = encoded_index_bytes(index);
    if (!index_size) {
        return std::unexpected(index_size.error());
    }
    ArchiveHeader header;
    header.index_bytes = *index_size;
    header.entry_count = index.entries.size();
    header.block_count = block_count;
    header.data_bytes = total;
    header.total_output = total;
    header.chunk_size_bytes = options.chunk_size_bytes;
    const auto header_bytes = encode_header(header, limits);
    if (!header_bytes) {
        return std::unexpected(header_bytes.error());
    }
    if (auto written = output->append(*header_bytes); !written) {
        return std::unexpected(written.error());
    }

    std::vector<std::byte> buffer(static_cast<std::size_t>(
        (std::min)(std::uint64_t{options.chunk_size_bytes}, total))); // one chunk at most
    std::uint64_t offset = 0;
    std::size_t block = 0;
    for (std::size_t i = 0; i < entries->size(); ++i) {
        const auto &entry = (*entries)[i];
        if (stop.stop_requested()) {
            return std::unexpected(cancelled());
        }
        if (entry.directory) {
            continue;
        }
        auto file = tree->open_file(entry);
        if (!file) {
            return std::unexpected(file.error());
        }
        for (auto remaining = entry.size; remaining != 0;) {
            if (stop.stop_requested()) {
                return std::unexpected(cancelled());
            }
            const auto want = static_cast<std::size_t>(
                (std::min)(remaining, std::uint64_t{options.chunk_size_bytes}));
            const auto chunk = std::span{buffer}.first(want);
            const auto got = file->read(chunk);
            if (!got) {
                return std::unexpected(got.error());
            }
            if (*got != want) {
                return std::unexpected(
                    Error{ErrorCode::io_error,
                          "input changed while it was being compressed: " + entry.path});
            }
            index.blocks[block++] = {offset, want, want, 0, 0, crc32c(chunk)};
            if (auto written = output->append(chunk); !written) {
                return std::unexpected(written.error());
            }
            offset += want;
            remaining -= want;
        }
        if (auto unchanged = file->confirm_unchanged(); !unchanged) {
            return std::unexpected(unchanged.error());
        }
    }

    const auto index_bytes = encode_index(index);
    // Never commit an archive that this library's own reader would reject.
    if (index_bytes.size() != header.index_bytes || block != index.blocks.size() ||
        offset != total) {
        return std::unexpected(Error{ErrorCode::io_error, "internal error: archive layout"});
    }
    if (const auto reparsed = parse_index(header, index_bytes, limits); !reparsed) {
        auto error = reparsed.error();
        error.message = "internal error: produced index is invalid: " + error.message;
        return std::unexpected(std::move(error));
    }
    if (auto written = output->append(index_bytes); !written) {
        return std::unexpected(written.error());
    }
    const auto archive_bytes = archive_header_bytes + total + header.index_bytes;
    if (auto committed = output->commit(archive_bytes, stop); !committed) {
        return std::unexpected(committed.error());
    }
    return ArchiveSummary{header.entry_count, block_count, total, archive_bytes};
}
} // namespace seedarc::experimental
