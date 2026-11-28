// SPDX-License-Identifier: GPL-3.0-or-later
// Private indexed single-file extraction, included inside append4_study.cpp's anonymous namespace.
//
// Unlike access-profile (whole archive in memory), this reads only what one file needs: the
// header, the footer, the trailer, every GEN record (checked against its trailer copy) and the
// target file's frame. The frame is decoded only through the end of the target file, the file's
// CRC32C is checked, and the file is written through SafeOutputDirectory (exclusive temporary,
// size and CRC re-read at commit, rename without replacement). The full-container cross-check
// against a scan of every record is NOT done here: only the trailer CRC, the header CRC, the GEN
// copies, the trailer's own bounds checks and the file CRC protect the result. Revision-5 solid
// archives only; anything else is refused.

// Positioned reads from one open archive; every byte read is counted.
class ArchiveReader {
  public:
    static std::expected<ArchiveReader, Error> open(const fs::path &path) {
        std::error_code ec;
        const auto size = fs::file_size(path, ec);
        if (ec) {
            return std::unexpected(Error{ErrorCode::io_error, "cannot stat " + utf8(path)});
        }
        ArchiveReader r;
        r.stream_.open(path, std::ios::binary);
        if (!r.stream_) {
            return std::unexpected(Error{ErrorCode::io_error, "cannot open " + utf8(path)});
        }
        r.size_ = size;
        return r;
    }
    std::uint64_t size() const { return size_; }
    std::uint64_t bytes_read() const { return read_; }
    std::uint32_t reads() const { return reads_; }
    std::expected<std::vector<std::byte>, Error> read(std::uint64_t offset, std::uint64_t length) {
        if (offset > size_ || size_ - offset < length || length > max_read) {
            return std::unexpected(bad("read outside the archive or beyond the read limit"));
        }
        std::vector<std::byte> out(static_cast<std::size_t>(length));
        stream_.clear();
        stream_.seekg(static_cast<std::streamoff>(offset));
        if (!stream_ || !stream_.read(reinterpret_cast<char *>(out.data()),
                                      static_cast<std::streamsize>(out.size()))) {
            return std::unexpected(Error{ErrorCode::io_error, "short archive read"});
        }
        read_ += length;
        ++reads_;
        return out;
    }

  private:
    static constexpr std::uint64_t max_read = research::max_solid_bytes;
    std::ifstream stream_;
    std::uint64_t size_ = 0, read_ = 0;
    std::uint32_t reads_ = 0;
};

constexpr std::uint64_t max_trailer_read = 256ULL << 20;

struct IndexTiming {
    double read_ms = 0, manifest_ms = 0; // the rest of index_ms is trailer interpretation
};
std::expected<Model, Error> parse_indexed(ArchiveReader &in, IndexTiming *timing = nullptr) {
    const auto read_start = wall_ms();
    double reading = 0;
    auto head = in.read(0, header_bytes);
    if (!head) {
        return std::unexpected(head.error());
    }
    auto header = parse_header(*head);
    if (!header) {
        return std::unexpected(header.error());
    }
    if (!header->solid() || header->revision != 5) {
        return std::unexpected(bad("indexed extraction supports revision-5 solid archives only"));
    }
    if (in.size() < header_bytes + footer_bytes) {
        return std::unexpected(bad("bad footer magic"));
    }
    auto tail = in.read(in.size() - footer_bytes, footer_bytes);
    if (!tail) {
        return std::unexpected(tail.error());
    }
    const auto footer = parse_footer(*tail, in.size());
    if (!footer) {
        return std::unexpected(footer.error());
    }
    if (footer->trailer_size > max_trailer_read) {
        return std::unexpected(Error{ErrorCode::resource_limit, "trailer too large"});
    }
    auto trailer = in.read(footer->trailer_offset, footer->trailer_size);
    if (!trailer) {
        return std::unexpected(trailer.error());
    }
    if (seedarc::experimental::crc32c(*trailer) != footer->trailer_crc) {
        return std::unexpected(bad("trailer checksum mismatch"));
    }
    const auto view = trailer_view(*header, *trailer); // ADR-0010 (bounded raw length)
    if (!view) {
        return std::unexpected(view.error());
    }
    reading = wall_ms() - read_start; // header, footer, trailer, its CRC and decoding
    const auto limit = footer->trailer_offset;
    const GenFetch fetch = [&](std::uint64_t off) -> std::optional<GenRecord> {
        if (off > limit || limit - off < record_overhead) {
            return std::nullopt;
        }
        auto front = in.read(off, 12);
        if (!front) {
            return std::nullopt;
        }
        Reader r{*front, 8};
        const auto len = r.get(4);
        if (!len || *len > max_record_body || limit - off - record_overhead < *len) {
            return std::nullopt;
        }
        auto rest = in.read(off + 12, *len + 4); // body and CRC; the first 12 bytes are read
        if (!rest) {
            return std::nullopt;
        }
        auto whole = std::move(*front);
        whole.insert(whole.end(), rest->begin(), rest->end());
        const auto rec = read_record(whole, 0, off);
        if (!rec || rec->type != rec_gen) {
            return std::nullopt;
        }
        return GenRecord{{rec->body.begin(), rec->body.end()}, off + rec->end};
    };
    double manifest = 0;
    auto model = parse_index(*header, seedarc::experimental::crc32c(*head), view->bytes, limit, fetch,
                             {}, &manifest);
    if (model) {
        model->trailer_raw_bytes = view->bytes.size();
    }
    if (timing != nullptr) {
        *timing = {reading, manifest};
    }
    return model;
}

struct ExtractStats {
    std::uint64_t size = 0, frame_offset = 0, frame_stored = 0, frame_raw = 0, decoded = 0;
    std::uint32_t frame = 0;
    double read_ms = 0, decode_ms = 0, write_ms = 0;
};

// Reads, decodes and writes one file; nothing is published unless every check passes.
std::expected<ExtractStats, Error> extract_one(ArchiveReader &in, const Model &m,
                                               std::string_view path,
                                               const seedarc::experimental::SafeOutputDirectory &dir,
                                               std::string_view basename) {
    const auto e = std::ranges::find(m.entries, path, &Entry::path);
    if (e == m.entries.end()) {
        return std::unexpected(bad("requested file is missing"));
    }
    ExtractStats s;
    s.size = e->size;
    s.frame = e->frame;
    std::vector<std::byte> decoded;
    std::span<const std::byte> view;
    if (e->size != 0) {
        const auto &f = m.frames[e->frame];
        s.frame_offset = f.offset;
        s.frame_stored = f.stored;
        s.frame_raw = f.raw;
        auto t0 = wall_ms();
        auto stored = in.read(f.offset, f.stored);
        if (!stored) {
            return std::unexpected(stored.error());
        }
        s.read_ms = wall_ms() - t0;
        t0 = wall_ms();
        decoded.resize(static_cast<std::size_t>(e->frame_offset + e->size)); // <= f.raw (parse)
        const auto produced =
            research::decompress_solid_prefix_codec(frame_codec(m, f), *stored, decoded);
        if (!produced) {
            return std::unexpected(produced.error());
        }
        if (*produced != decoded.size()) {
            return std::unexpected(bad("frame ends before the requested file"));
        }
        s.decoded = decoded.size();
        s.decode_ms = wall_ms() - t0;
        view = std::span{decoded}.subspan(static_cast<std::size_t>(e->frame_offset));
    }
    if (seedarc::experimental::crc32c(view) != e->file_crc) {
        return std::unexpected(bad("file checksum mismatch"));
    }
    const auto t0 = wall_ms();
    auto file = dir.begin_file(basename);
    if (!file) {
        return std::unexpected(file.error());
    }
    if (auto a = file->append(view); !a) {
        return std::unexpected(a.error());
    }
    if (auto c = file->commit(e->size); !c) {
        return std::unexpected(c.error());
    }
    s.write_ms = wall_ms() - t0;
    return s;
}

std::expected<seedarc::experimental::SafeOutputDirectory, Error> open_output(const fs::path &p) {
    std::error_code ec;
    const auto absolute = fs::absolute(p, ec);
    if (ec) {
        return std::unexpected(Error{ErrorCode::io_error, "cannot resolve the output directory"});
    }
    return seedarc::experimental::SafeOutputDirectory::open_existing(absolute);
}

std::string extract_json(const ExtractStats &s) {
    return ",\"requested_bytes\":" + std::to_string(s.size) + ",\"frame\":" +
           std::to_string(s.frame) + ",\"frame_archive_offset\":" + std::to_string(s.frame_offset) +
           ",\"frame_stored_bytes\":" + std::to_string(s.frame_stored) +
           ",\"frame_raw_bytes\":" + std::to_string(s.frame_raw) +
           ",\"decoded_bytes\":" + std::to_string(s.decoded) +
           ",\"read_frame_ms\":" + std::to_string(s.read_ms) +
           ",\"decode_ms\":" + std::to_string(s.decode_ms) +
           ",\"write_commit_ms\":" + std::to_string(s.write_ms);
}

constexpr std::string_view extract_io_mode =
    "positioned reads (header, footer, trailer, GEN records, target frame); decode through the "
    "end of the target file; SafeOutputDirectory commit (size and CRC re-read, no replacement); "
    "no full-container record scan; no fsync; OS cache not dropped";

int cmd_extract(const fs::path &archive, std::string_view path, const fs::path &out_dir,
                std::string_view basename) {
    const auto start = wall_ms();
    auto in = ArchiveReader::open(archive);
    if (!in) {
        return fail(in.error());
    }
    auto model = parse_indexed(*in);
    if (!model) {
        return fail(model.error());
    }
    const auto index_bytes = in->bytes_read();
    const auto index_ms = wall_ms() - start;
    auto dir = open_output(out_dir);
    if (!dir) {
        return fail(dir.error());
    }
    auto s = extract_one(*in, *model, path, *dir, basename);
    if (!s) {
        return fail(s.error());
    }
    std::cout << "{\"schema\":\"seedarc.bounded-extract.r0\",\"path\":" << json_string(path)
              << ",\"output\":" << json_string(basename) << extract_json(*s)
              << ",\"archive_bytes\":" << in->size() << ",\"index_bytes_read\":" << index_bytes
              << ",\"archive_bytes_read\":" << in->bytes_read() << ",\"reads\":" << in->reads()
              << ",\"index_ms\":" << index_ms << ",\"wall_ms\":" << wall_ms() - start
              << ",\"io_mode\":" << json_string(extract_io_mode) << "," << peak_memory_json()
              << "}\n";
    return std::cout ? 0 : 1;
}

// The access-profile sample rule: uniform positions in the sorted paths plus the largest file.
std::vector<std::size_t> uniform_samples(const std::vector<InputFile> &files, std::size_t count) {
    count = std::min(count, files.size());
    std::vector<std::size_t> indices;
    for (std::size_t sample = 0; sample < count; ++sample) {
        indices.push_back(count == 1 ? 0 : sample * (files.size() - 1) / (count - 1));
    }
    std::size_t largest = 0;
    for (std::size_t i = 1; i < files.size(); ++i) {
        if (files[i].data.size() > files[largest].data.size()) {
            largest = i;
        }
    }
    if (std::ranges::find(indices, largest) == indices.end()) {
        indices.push_back(largest);
    }
    std::ranges::sort(indices);
    indices.erase(std::ranges::unique(indices).begin(), indices.end());
    return indices;
}

// Each sample: open the archive, read its index, extract one file into OUT_DIR (must exist and
// be empty), all timed; then, untimed, compare the written file with the reference input and
// remove it again.
int cmd_extract_profile(const fs::path &archive, const fs::path &input, std::size_t count,
                        const fs::path &out_dir) {
    auto files = read_tree(input);
    if (!files || files->empty()) {
        return fail(files ? bad("extract profile needs files") : files.error());
    }
    std::error_code ec;
    if (!fs::is_directory(out_dir, ec) || fs::is_symlink(out_dir, ec) ||
        fs::directory_iterator(out_dir, ec) != fs::directory_iterator{} || ec) {
        return fail(bad("extract profile needs an existing empty output directory"));
    }
    auto dir = open_output(out_dir);
    if (!dir) {
        return fail(dir.error());
    }
    std::string samples;
    std::vector<double> times;
    std::uint64_t requested = 0, decoded = 0, read_total = 0, index_total = 0, written = 0;
    std::size_t n = 0;
    for (const auto index : uniform_samples(*files, count)) {
        const auto &want = (*files)[index];
        const auto name = "sample-" + std::to_string(n++);
        const auto start = wall_ms();
        auto in = ArchiveReader::open(archive);
        if (!in) {
            return fail(in.error());
        }
        IndexTiming timing;
        auto model = parse_indexed(*in, &timing);
        if (!model) {
            return fail(model.error());
        }
        const auto index_bytes = in->bytes_read();
        const auto index_ms = wall_ms() - start;
        auto s = extract_one(*in, *model, want.path, *dir, name);
        if (!s) {
            return fail(s.error());
        }
        const auto elapsed = wall_ms() - start;
        const auto target = out_dir / name;
        auto back = read_file(target);
        if (!back || !std::ranges::equal(*back, want.data)) {
            return fail(bad("extracted file differs from the reference input"));
        }
        if (!fs::remove(target, ec) || ec) {
            return fail(Error{ErrorCode::io_error, "cannot remove a profiled output"});
        }
        times.push_back(elapsed);
        requested += s->size;
        decoded += s->decoded;
        read_total += in->bytes_read();
        index_total += index_bytes;
        ++written;
        samples += (samples.empty() ? "" : ",") + std::string{"{\"path\":"} +
                   json_string(want.path) + extract_json(*s) +
                   ",\"index_bytes_read\":" + std::to_string(index_bytes) +
                   ",\"archive_bytes_read\":" + std::to_string(in->bytes_read()) +
                   ",\"reads\":" + std::to_string(in->reads()) +
                   ",\"index_ms\":" + std::to_string(index_ms) +
                   ",\"index_read_ms\":" + std::to_string(timing.read_ms) +
                   ",\"index_manifest_ms\":" + std::to_string(timing.manifest_ms) +
                   ",\"wall_ms\":" + std::to_string(elapsed) + "}";
    }
    auto sorted = times;
    std::ranges::sort(sorted);
    const auto quantile = [&](std::size_t numerator) {
        return sorted[(numerator * (sorted.size() - 1) + 50) / 100];
    };
    std::uint64_t archive_size = 0;
    if (auto in = ArchiveReader::open(archive)) {
        archive_size = in->size();
    }
    std::cout << "{\"schema\":\"seedarc.bounded-extract-profile.r0\",\"byte_exact\":true,"
                 "\"samples\":["
              << samples << "],\"sample_count\":" << times.size()
              << ",\"selection\":\"uniform paths plus largest file, deduplicated\""
              << ",\"median_ms\":" << quantile(50) << ",\"p95_ms\":" << quantile(95)
              << ",\"max_ms\":" << sorted.back() << ",\"requested_bytes_total\":" << requested
              << ",\"decoded_bytes_total\":" << decoded
              << ",\"archive_bytes_read_total\":" << read_total
              << ",\"index_bytes_read_total\":" << index_total << ",\"archive_bytes\":" << archive_size
              << ",\"outputs_written\":" << written << ",\"outputs_removed\":" << written
              << ",\"io_mode\":" << json_string(extract_io_mode) << "," << peak_memory_json()
              << "}\n";
    return std::cout ? 0 : 1;
}
