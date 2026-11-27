// SPDX-License-Identifier: GPL-3.0-or-later
// dup-probe (DEV-036), included inside append4_study.cpp's anonymous namespace after
// codec_probe.hpp. Measures exact whole-file duplicates and what removing them would save under
// bounded solid frames. No archive is written and no format records a duplicate.
//
//   dup-probe DIR VARIANT <whole|16MiB|32MiB|64MiB|128MiB|256MiB>
//
// Files are taken in path order as one group (the g1-r0 cut of encode_pending). A file is a
// duplicate when an earlier file has the same size, CRC32C and bytes; empty files never count.
// "baseline" compresses every file; "dedup" drops every duplicate and cuts frames again over the
// remaining files; "dedup_same_cut" keeps the baseline frames and only drops the duplicates from
// them, which separates the effect of removing copies from that of moving frame boundaries. All
// three round-trip every frame. A duplicate is "cross-frame" when its first copy
// lies in another baseline frame, where the codec cannot see it.

struct FramedSize {
    std::uint64_t frames = 0, compressed = 0;
    std::uint32_t crc = 0;
    double encode_wall_ms = 0;
};

using FrameList = std::vector<std::vector<const InputFile *>>;

// The encode_pending cut: path order, a frame is closed when the next file does not fit.
std::expected<FrameList, Error> cut_frames(const std::vector<const InputFile *> &members, std::uint64_t limit) {
    FrameList frames;
    std::uint64_t used = 0;
    for (const auto *f : members) {
        if (f->data.size() > limit) {
            return std::unexpected(Error{ErrorCode::resource_limit, "file exceeds the requested solid frame limit"});
        }
        if (frames.empty() || f->data.size() > limit - used) {
            frames.emplace_back();
            used = 0;
        }
        frames.back().push_back(f);
        used += f->data.size();
    }
    return frames;
}

// Compresses each non-empty frame (a frame of only empty files gets one byte, as encode_pending
// does) and round-trips it.
std::expected<FramedSize, Error> framed_size(const FrameList &frames, const research::SolidVariant &v) {
    FramedSize out;
    std::vector<std::byte> all;
    for (const auto &members : frames) {
        if (members.empty()) {
            continue;
        }
        std::vector<std::byte> joined;
        for (const auto *f : members) {
            joined.insert(joined.end(), f->data.begin(), f->data.end());
        }
        if (joined.empty()) {
            joined.push_back(std::byte{0});
        }
        const auto w = wall_ms();
        auto frame = research::compress_solid_variant(v, joined);
        if (!frame) {
            return std::unexpected(frame.error());
        }
        out.encode_wall_ms += wall_ms() - w;
        std::vector<std::byte> back(joined.size());
        if (auto r = research::decompress_solid_variant(v, *frame, back); !r) {
            return std::unexpected(r.error());
        }
        if (back != joined) {
            return std::unexpected(bad("dup probe round trip mismatch"));
        }
        all.insert(all.end(), frame->begin(), frame->end());
        ++out.frames;
    }
    out.compressed = all.size();
    out.crc = seedarc::experimental::crc32c(all);
    return out;
}

std::string framed_json(const FramedSize &s, std::size_t files, std::uint64_t raw) {
    return "{\"files\":" + std::to_string(files) + ",\"raw_bytes\":" + std::to_string(raw) +
           ",\"frames\":" + std::to_string(s.frames) + ",\"compressed_bytes\":" + std::to_string(s.compressed) +
           ",\"frame_crc32c\":" + std::to_string(s.crc) + ",\"encode_wall_ms\":" + std::to_string(s.encode_wall_ms) +
           "}";
}

int cmd_dup_probe(const fs::path &dir, std::string_view variant_name, std::string_view limit_text) {
    const auto v = parse_variant(variant_name);
    if (!v) {
        std::cerr << "unknown codec variant: " << variant_name << "\n";
        return 2;
    }
    std::optional<std::uint64_t> limit;
    for (std::size_t i = 0; i < recipe_frame_limits.size(); ++i) {
        const auto name = i == 0 ? std::string{"whole"} : std::to_string(recipe_frame_limits[i] >> 20) + "MiB";
        if (limit_text == name) {
            limit = i == 0 ? research::max_solid_bytes : recipe_frame_limits[i];
        }
    }
    if (!limit) {
        std::cerr << "unknown frame limit: " << limit_text << "\n";
        return 2;
    }
    auto files = read_tree(dir);
    if (!files) {
        return fail(files.error());
    }
    std::vector<const InputFile *> all;
    for (const auto &f : *files) {
        all.push_back(&f);
    }
    const auto base_cut = cut_frames(all, *limit);
    if (!base_cut) {
        return fail(base_cut.error());
    }
    std::vector<std::uint64_t> frame_of(files->size());
    for (std::size_t frame = 0, i = 0; frame < base_cut->size(); ++frame) {
        for (std::size_t k = 0; k < (*base_cut)[frame].size(); ++k, ++i) {
            frame_of[i] = frame;
        }
    }
    // First occurrence of each content: bucket by (size, CRC32C), then compare bytes.
    std::map<std::pair<std::uint64_t, std::uint32_t>, std::vector<std::size_t>> firsts;
    std::vector<const InputFile *> kept;
    std::vector<bool> is_dup(files->size(), false);
    std::uint64_t raw = 0, kept_raw = 0, dup_files = 0, dup_bytes = 0, cross_files = 0, cross_bytes = 0,
                  contents_with_copies = 0;
    std::set<std::size_t> copied;
    for (std::size_t i = 0; i < files->size(); ++i) {
        const auto &f = (*files)[i];
        raw += f.data.size();
        std::optional<std::size_t> first;
        if (!f.data.empty()) {
            auto &bucket = firsts[{f.data.size(), seedarc::experimental::crc32c(f.data)}];
            for (const auto j : bucket) {
                if ((*files)[j].data == f.data) {
                    first = j;
                    break;
                }
            }
            if (!first) {
                bucket.push_back(i);
            }
        }
        if (first) {
            is_dup[i] = true;
            ++dup_files;
            dup_bytes += f.data.size();
            if (frame_of[*first] != frame_of[i]) {
                ++cross_files;
                cross_bytes += f.data.size();
            }
            contents_with_copies += copied.insert(*first).second ? 1 : 0;
        } else {
            kept.push_back(&f);
            kept_raw += f.data.size();
        }
    }
    const auto baseline = framed_size(*base_cut, *v);
    if (!baseline) {
        return fail(baseline.error());
    }
    const auto dedup_cut = cut_frames(kept, *limit);
    if (!dedup_cut) {
        return fail(dedup_cut.error());
    }
    const auto dedup = framed_size(*dedup_cut, *v);
    if (!dedup) {
        return fail(dedup.error());
    }
    FrameList same_cut;
    for (std::size_t frame = 0, i = 0; frame < base_cut->size(); ++frame) {
        same_cut.emplace_back();
        for (const auto *f : (*base_cut)[frame]) {
            if (!is_dup[i++]) {
                same_cut.back().push_back(f);
            }
        }
    }
    const auto same = framed_size(same_cut, *v);
    if (!same) {
        return fail(same.error());
    }
    std::cout << "{\"schema\":\"seedarc.dup-probe.r1\",\"variant\":" << json_string(variant_name)
              << ",\"frame_limit_bytes\":" << *limit << ",\"files\":" << files->size()
              << ",\"input_bytes\":" << raw << ",\"duplicates\":{\"files\":" << dup_files
              << ",\"bytes\":" << dup_bytes << ",\"cross_frame_files\":" << cross_files
              << ",\"cross_frame_bytes\":" << cross_bytes << ",\"distinct_contents_with_copies\":"
              << contents_with_copies << "},\"baseline\":" << framed_json(*baseline, all.size(), raw)
              << ",\"dedup\":" << framed_json(*dedup, kept.size(), kept_raw)
              << ",\"dedup_same_cut\":" << framed_json(*same, kept.size(), kept_raw) << "," << peak_memory_json()
              << "}\n";
    return std::cout ? 0 : 1;
}
