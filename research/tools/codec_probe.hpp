// SPDX-License-Identifier: GPL-3.0-or-later
// codec-probe (research/ideas/codec-capabilities-r0.md), included inside append4_study.cpp's
// anonymous namespace. Compresses a tree as solid frames with encoder options the recipes do not
// use yet, round-trips every frame and reports sizes and times. No archive is written.
//
//   codec-probe DIR VARIANTS           one frame: every file in path order
//   codec-probe-classes DIR VARIANTS   one frame per file class (executable / text / binary)
//   codec-probe-framed DIR VARIANTS LIMIT   frames cut as recipe-pack cuts one group at a recipe
//                                      frame limit (16MiB ... 256MiB), sizes summed over frames
//
// VARIANTS is a comma list: zstd-<1..22>[-ldm][-mt<1..64>] or lzma2-<0..9>[e][-bcj].

std::optional<research::SolidVariant> parse_variant(std::string_view name) {
    research::SolidVariant v;
    const auto number = [](std::string_view text) -> std::optional<std::uint32_t> {
        if (text.empty() || text.size() > 2 || (text.size() == 2 && text[0] == '0')) {
            return std::nullopt;
        }
        std::uint32_t value = 0;
        for (const char c : text) {
            if (c < '0' || c > '9') {
                return std::nullopt;
            }
            value = value * 10 + static_cast<std::uint32_t>(c - '0');
        }
        return value;
    };
    std::vector<std::string_view> parts;
    for (std::size_t start = 0;;) {
        const auto dash = name.find('-', start);
        parts.push_back(name.substr(start, dash == std::string_view::npos ? name.npos : dash - start));
        if (dash == std::string_view::npos) {
            break;
        }
        start = dash + 1;
    }
    if (parts.size() < 2) {
        return std::nullopt;
    }
    if (parts[0] == "zstd") {
        const auto zstd_level = number(parts[1]);
        if (!zstd_level) {
            return std::nullopt;
        }
        v.codec = research::solid_zstd;
        v.level = *zstd_level;
        for (std::size_t i = 2; i < parts.size(); ++i) {
            if (parts[i] == "ldm" && !v.long_distance && v.workers == 0) {
                v.long_distance = true;
            } else if (parts[i].starts_with("mt") && v.workers == 0) {
                const auto workers = number(parts[i].substr(2));
                if (!workers || *workers == 0) {
                    return std::nullopt;
                }
                v.workers = *workers;
            } else {
                return std::nullopt;
            }
        }
    } else if (parts[0] == "lzma2") {
        auto level_text = parts[1];
        if (level_text.ends_with('e')) {
            v.extreme = true;
            level_text.remove_suffix(1);
        }
        const auto preset = number(level_text);
        if (!preset || level_text.size() != 1) {
            return std::nullopt;
        }
        v.codec = research::solid_lzma2;
        v.level = *preset;
        if (parts.size() == 3 && parts[2] == "bcj") {
            v.bcj_x86 = true;
        } else if (parts.size() != 2) {
            return std::nullopt;
        }
    } else {
        return std::nullopt;
    }
    if (!research::valid_solid_variant(v)) {
        return std::nullopt;
    }
    return v;
}

// executable: MZ / ELF / Mach-O magic; text: no NUL in the first 64 KiB and at least 95% of
// those bytes printable (TAB, LF, CR, 0x20-0x7E, or >= 0x80 for UTF-8); binary: the rest.
std::string_view file_class(std::span<const std::byte> data) {
    const auto at = [&](std::size_t i) { return std::to_integer<unsigned>(data[i]); };
    if (data.size() >= 2 && at(0) == 'M' && at(1) == 'Z') {
        return "executable";
    }
    if (data.size() >= 4) {
        const auto word = static_cast<std::uint32_t>(at(0)) | (static_cast<std::uint32_t>(at(1)) << 8) |
                           (static_cast<std::uint32_t>(at(2)) << 16) |
                           (static_cast<std::uint32_t>(at(3)) << 24);
        if (word == 0x464c457fU || word == 0xfeedfaceU || word == 0xfeedfacfU ||
            word == 0xcefaedfeU || word == 0xcffaedfeU) {
            return "executable";
        }
    }
    const auto head = data.first(std::min<std::size_t>(data.size(), 64 * 1024));
    std::size_t printable = 0;
    for (const auto b : head) {
        const auto c = std::to_integer<unsigned>(b);
        if (c == 0) {
            return "binary";
        }
        printable += (c == 9 || c == 10 || c == 13 || (c >= 0x20 && c < 0x7f) || c >= 0x80) ? 1 : 0;
    }
    return printable * 100 >= head.size() * 95 ? "text" : "binary";
}

enum class ProbeMode { whole, classes, framed };

// framed: the cut recipe-pack makes for one group (encode_pending): files in path order, a
// frame is closed when the next file would not fit `frame_limit`. Compressed bytes are the sum
// over frames, frame_crc32c covers the concatenated frames.
int cmd_codec_probe(const fs::path &dir, std::string_view variant_list, ProbeMode mode,
                    std::uint64_t frame_limit = research::max_solid_bytes) {
    std::vector<std::pair<std::string, research::SolidVariant>> variants;
    for (std::size_t start = 0; start <= variant_list.size();) {
        const auto comma = variant_list.find(',', start);
        const auto name = variant_list.substr(start, comma == std::string_view::npos ? variant_list.npos
                                                                                     : comma - start);
        const auto v = parse_variant(name);
        if (!v) {
            std::cerr << "unknown codec variant: " << name << "\n";
            return 2;
        }
        variants.emplace_back(std::string{name}, *v);
        if (comma == std::string_view::npos) {
            break;
        }
        start = comma + 1;
    }
    auto files = read_tree(dir);
    if (!files) {
        return fail(files.error());
    }
    if (frame_limit == 0 || frame_limit > research::max_solid_bytes) {
        return fail(bad("invalid probe frame limit"));
    }
    std::map<std::string, std::vector<const InputFile *>> groups;
    for (const auto &f : *files) {
        groups[mode == ProbeMode::classes ? std::string{file_class(f.data)} : std::string{"all"}]
            .push_back(&f);
    }
    std::string out;
    for (const auto &[name, members] : groups) {
        // Same cut as encode_pending; whole and classes use one frame per group.
        std::vector<std::vector<std::byte>> frames;
        std::vector<std::byte> joined;
        bool any = false;
        const auto flush = [&] {
            if (any) {
                if (joined.empty()) {
                    joined.push_back(std::byte{0}); // as encode_pending: a frame for empty files
                }
                frames.push_back(std::move(joined));
                joined.clear();
                any = false;
            }
        };
        std::uint64_t raw = 0;
        for (const auto *f : members) {
            if (f->data.size() > frame_limit) {
                return fail(Error{ErrorCode::resource_limit, "file exceeds the requested solid frame limit"});
            }
            if (f->data.size() > frame_limit - joined.size()) {
                flush();
            }
            any = true;
            joined.insert(joined.end(), f->data.begin(), f->data.end());
            raw += f->data.size();
        }
        flush();
        std::string results;
        for (const auto &[label, v] : variants) {
            std::vector<std::byte> all_frames;
            double encode_wall = 0, encode_cpu = 0, decode_wall = 0;
            for (const auto &input : frames) {
                const auto w = wall_ms(), c = cpu_ms();
                auto frame = research::compress_solid_variant(v, input);
                if (!frame) {
                    return fail(frame.error());
                }
                encode_wall += wall_ms() - w;
                encode_cpu += cpu_ms() - c;
                std::vector<std::byte> back(input.size());
                const auto dw = wall_ms();
                if (auto r = research::decompress_solid_variant(v, *frame, back); !r) {
                    return fail(r.error());
                }
                decode_wall += wall_ms() - dw;
                if (back != input) {
                    return fail(bad("codec probe round trip mismatch"));
                }
                all_frames.insert(all_frames.end(), frame->begin(), frame->end());
            }
            results += (results.empty() ? "" : ",") + std::string{"{\"variant\":"} + json_string(label) +
                       ",\"compressed_bytes\":" + std::to_string(all_frames.size()) +
                       ",\"frame_crc32c\":" + std::to_string(seedarc::experimental::crc32c(all_frames)) +
                       ",\"encode_wall_ms\":" + std::to_string(encode_wall) +
                       ",\"encode_cpu_ms\":" + std::to_string(encode_cpu) +
                       ",\"decode_wall_ms\":" + std::to_string(decode_wall) + "}";
        }
        out += (out.empty() ? "" : ",") + std::string{"{\"group\":"} + json_string(name) +
               ",\"files\":" + std::to_string(members.size()) + ",\"raw_bytes\":" + std::to_string(raw) +
               ",\"frames\":" + std::to_string(frames.size()) + ",\"variants\":[" + results + "]}";
    }
    std::cout << "{\"schema\":\"seedarc.codec-probe.r0\",\"by_class\":"
              << (mode == ProbeMode::classes ? "true" : "false") << ",\"frame_limit_bytes\":"
              << (mode == ProbeMode::framed ? std::to_string(frame_limit) : std::string{"null"})
              << ",\"files\":" << files->size() << ",\"input_bytes\":" << input_bytes_of(*files)
              << ",\"groups\":[" << out << "]," << peak_memory_json() << "}\n";
    return std::cout ? 0 : 1;
}
