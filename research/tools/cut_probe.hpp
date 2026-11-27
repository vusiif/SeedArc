// SPDX-License-Identifier: GPL-3.0-or-later
// cut-probe (DEV-037, research/ideas/frame-cut-r0.md), included inside append4_study.cpp's
// anonymous namespace after dup_probe.hpp. Compares rules that choose where bounded solid frames
// end. No archive is written and no format records the rule.
//
//   cut-probe DIR VARIANT <16MiB|32MiB|64MiB|128MiB|256MiB>
//
// Files are taken in path order as one group. "Boundary b" ends a frame before file b. Rules:
//   greedy             the g1-r0 cut of encode_pending: close when the next file does not fit
//   shift-1 .. shift-7 noise reference: the first frame holds at most k*L/8 bytes, then greedy
//   balanced           n = greedy frame count, t = ceil(total / n): close at t or when full
//   path-w25, path-w50 from frame start s, e = the furthest greedy boundary; if the rest fits,
//                      take it. Candidates: boundaries in (s, e] with at least (1 - P) * L bytes
//                      in the frame, and e. Pick the fewest common directory levels between the
//                      two neighbouring files; ties take the latest boundary
//   sim-w25, sim-w50   same candidates; pick the lowest estimated Jaccard similarity between the
//                      files right before and right after the boundary (each side at most 1 MiB
//                      and 64 files), from 128-bucket one-permutation MinHash sketches of 8-byte
//                      windows read little-endian; ties take the latest boundary
// Every distinct frame is compressed once (by `--jobs` workers) and round-tripped. Results do not
// depend on the worker count.

using FrameCuts = std::vector<std::size_t>; // frame end boundaries, increasing, last == file count
constexpr std::size_t sketch_buckets = 128;
using Sketch = std::array<std::uint64_t, sketch_buckets>;
constexpr std::uint64_t sketch_empty = std::numeric_limits<std::uint64_t>::max();

Sketch file_sketch(std::span<const std::byte> data) {
    Sketch s;
    s.fill(sketch_empty);
    if (data.empty()) {
        return s;
    }
    const auto add = [&](std::uint64_t window) {
        const auto h = seedarc::third_party::splitmix64_step(window);
        auto &slot = s[static_cast<std::size_t>(h >> 57)];
        slot = std::min(slot, h);
    };
    // Little-endian 8-byte window; files shorter than 8 bytes give one zero-padded window.
    std::uint64_t window = 0;
    const auto first = std::min<std::size_t>(data.size(), 8);
    for (std::size_t i = 0; i < first; ++i) {
        window |= std::uint64_t{std::to_integer<std::uint8_t>(data[i])} << (8 * i);
    }
    add(window);
    for (std::size_t i = 8; i < data.size(); ++i) {
        window = (window >> 8) | (std::uint64_t{std::to_integer<std::uint8_t>(data[i])} << 56);
        add(window);
    }
    return s;
}

// Directory levels two paths share (the file name never counts).
std::size_t common_dirs(std::string_view a, std::string_view b) {
    std::size_t levels = 0;
    for (std::size_t i = 0; i < a.size() && i < b.size() && a[i] == b[i]; ++i) {
        levels += a[i] == '/' ? 1 : 0;
    }
    return levels;
}

struct CutInput {
    const std::vector<InputFile> &files;
    std::vector<std::uint64_t> prefix; // prefix[i] = bytes of files [0, i)
    std::vector<Sketch> sketches;
    std::uint64_t limit = 0;
    std::uint64_t bytes(std::size_t s, std::size_t e) const { return prefix[e] - prefix[s]; }
    // Furthest boundary from s holding at most cap bytes; always at least one file.
    std::size_t fill(std::size_t s, std::uint64_t cap) const {
        auto e = s + 1;
        while (e < files.size() && bytes(s, e + 1) <= cap) {
            ++e;
        }
        return e;
    }
};

FrameCuts greedy_cuts(const CutInput &in, std::uint64_t first_cap) {
    FrameCuts cuts;
    for (std::size_t s = 0; s < in.files.size();) {
        s = in.fill(s, cuts.empty() ? first_cap : in.limit);
        cuts.push_back(s);
    }
    return cuts;
}

FrameCuts balanced_cuts(const CutInput &in, std::size_t greedy_frames) {
    FrameCuts cuts;
    if (greedy_frames == 0) {
        return cuts;
    }
    const auto total = in.prefix.back();
    const auto target = total / greedy_frames + (total % greedy_frames != 0 ? 1 : 0);
    for (std::size_t s = 0; s < in.files.size();) {
        auto e = s + 1;
        while (e < in.files.size() && in.bytes(s, e) < target && in.bytes(s, e + 1) <= in.limit) {
            ++e;
        }
        cuts.push_back(e);
        s = e;
    }
    return cuts;
}

// Score of a boundary as a fraction num / den (den 0 means 0); lower is a better cut.
struct CutScore {
    std::uint64_t num = 0, den = 0;
    bool less(const CutScore &o) const { // num/den < o.num/o.den, with x/0 read as 0
        const auto d1 = den == 0 ? 1 : den, d2 = o.den == 0 ? 1 : o.den;
        return (den == 0 ? 0 : num) * d2 < (o.den == 0 ? 0 : o.num) * d1;
    }
};

CutScore similarity(const CutInput &in, std::size_t b) {
    constexpr std::uint64_t side_bytes = 1 << 20;
    constexpr std::size_t side_files = 64;
    const auto side = [&](std::size_t from, int step) {
        Sketch u;
        u.fill(sketch_empty);
        std::uint64_t used = 0;
        for (std::size_t k = 0, i = from; k < side_files; ++k) {
            const auto size = in.files[i].data.size();
            if (k > 0 && used + size > side_bytes) {
                break;
            }
            used += size;
            for (std::size_t j = 0; j < sketch_buckets; ++j) {
                u[j] = std::min(u[j], in.sketches[i][j]);
            }
            if (step < 0 ? i == 0 : i + 1 == in.files.size()) {
                break;
            }
            i = step < 0 ? i - 1 : i + 1;
        }
        return u;
    };
    const auto left = side(b - 1, -1), right = side(b, 1);
    CutScore s;
    for (std::size_t j = 0; j < sketch_buckets; ++j) {
        if (left[j] == sketch_empty && right[j] == sketch_empty) {
            continue;
        }
        ++s.den;
        s.num += left[j] == right[j] ? 1 : 0;
    }
    return s;
}

FrameCuts window_cuts(const CutInput &in, std::uint64_t window_percent, bool by_content) {
    FrameCuts cuts;
    const auto floor = in.limit - in.limit / 100 * window_percent;
    for (std::size_t s = 0; s < in.files.size();) {
        const auto e = in.fill(s, in.limit);
        auto best = e;
        if (e < in.files.size()) {
            const auto score = [&](std::size_t b) {
                return by_content ? similarity(in, b)
                                  : CutScore{common_dirs(in.files[b - 1].path, in.files[b].path), 1};
            };
            auto best_score = score(e);
            for (auto b = e - 1; b > s && in.bytes(s, b) >= floor; --b) {
                const auto sc = score(b);
                if (sc.less(best_score)) {
                    best = b;
                    best_score = sc;
                }
            }
        }
        cuts.push_back(best);
        s = best;
    }
    return cuts;
}

struct CutFrame {
    std::uint64_t compressed = 0;
    std::uint32_t crc = 0;
    double encode_wall_ms = 0;
};

int cmd_cut_probe(const fs::path &dir, std::string_view variant_name, std::string_view limit_text) {
    const auto v = parse_variant(variant_name);
    if (!v) {
        std::cerr << "unknown codec variant: " << variant_name << "\n";
        return 2;
    }
    std::optional<std::uint64_t> limit;
    for (std::size_t i = 1; i < recipe_frame_limits.size(); ++i) {
        if (limit_text == std::to_string(recipe_frame_limits[i] >> 20) + "MiB") {
            limit = recipe_frame_limits[i];
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
    CutInput in{*files, {0}, {}, *limit};
    for (const auto &f : *files) {
        if (f.data.size() > *limit) {
            return fail(Error{ErrorCode::resource_limit, "file exceeds the requested solid frame limit"});
        }
        in.prefix.push_back(in.prefix.back() + f.data.size());
    }
    const auto sw = wall_ms();
    in.sketches.reserve(files->size());
    for (const auto &f : *files) {
        in.sketches.push_back(file_sketch(f.data));
    }
    const auto sketch_ms = wall_ms() - sw;
    const auto cw = wall_ms();
    std::vector<std::pair<std::string, FrameCuts>> rules;
    rules.emplace_back("greedy", greedy_cuts(in, *limit));
    for (std::uint64_t k = 1; k <= 7; ++k) {
        rules.emplace_back("shift-" + std::to_string(k), greedy_cuts(in, *limit / 8 * k));
    }
    rules.emplace_back("balanced", balanced_cuts(in, rules[0].second.size()));
    for (const std::uint64_t w : {25, 50}) {
        rules.emplace_back("path-w" + std::to_string(w), window_cuts(in, w, false));
    }
    for (const std::uint64_t w : {25, 50}) {
        rules.emplace_back("sim-w" + std::to_string(w), window_cuts(in, w, true));
    }
    const auto cut_ms = wall_ms() - cw;
    // Every distinct frame once, in first-seen order.
    std::map<std::pair<std::size_t, std::size_t>, std::size_t> index;
    std::vector<std::pair<std::size_t, std::size_t>> jobs;
    for (const auto &[name, cuts] : rules) {
        for (std::size_t f = 0, s = 0; f < cuts.size(); s = cuts[f++]) {
            if (index.emplace(std::pair{s, cuts[f]}, jobs.size()).second) {
                jobs.emplace_back(s, cuts[f]);
            }
        }
    }
    std::vector<std::expected<CutFrame, Error>> done(jobs.size(),
                                                     std::unexpected(Error{ErrorCode::io_error, "frame not encoded"}));
    const auto encode_one = [&](std::size_t j) {
        try {
            std::vector<std::byte> joined;
            for (auto i = jobs[j].first; i < jobs[j].second; ++i) {
                joined.insert(joined.end(), (*files)[i].data.begin(), (*files)[i].data.end());
            }
            if (joined.empty()) {
                joined.push_back(std::byte{0}); // as encode_pending: a frame of empty files
            }
            const auto w = wall_ms();
            auto frame = research::compress_solid_variant(*v, joined);
            if (!frame) {
                done[j] = std::unexpected(frame.error());
                return;
            }
            const auto ms = wall_ms() - w;
            std::vector<std::byte> back(joined.size());
            if (auto r = research::decompress_solid_variant(*v, *frame, back); !r) {
                done[j] = std::unexpected(r.error());
                return;
            }
            if (back != joined) {
                done[j] = std::unexpected(bad("cut probe round trip mismatch"));
                return;
            }
            done[j] = CutFrame{frame->size(), seedarc::experimental::crc32c(*frame), ms};
        } catch (const std::bad_alloc &) {
            done[j] = std::unexpected(Error{ErrorCode::resource_limit, "out of memory encoding a frame"});
        }
    };
    const auto ew = wall_ms();
    {
        std::atomic<std::size_t> next{0};
        std::vector<std::jthread> pool;
        for (std::size_t w = 0; w < std::min(encode_jobs, jobs.size()); ++w) {
            pool.emplace_back([&] {
                for (auto j = next.fetch_add(1); j < jobs.size(); j = next.fetch_add(1)) {
                    encode_one(j);
                }
            });
        }
    }
    const auto encode_ms = wall_ms() - ew;
    for (const auto &d : done) {
        if (!d) {
            return fail(d.error());
        }
    }
    std::cout << "{\"schema\":\"seedarc.cut-probe.r0\",\"variant\":" << json_string(variant_name)
              << ",\"frame_limit_bytes\":" << *limit << ",\"files\":" << files->size()
              << ",\"input_bytes\":" << in.prefix.back() << ",\"distinct_frames\":" << jobs.size()
              << ",\"sketch_wall_ms\":" << sketch_ms << ",\"cut_wall_ms\":" << cut_ms
              << ",\"encode_wall_ms\":" << encode_ms << ",\"rules\":[";
    for (std::size_t r = 0; r < rules.size(); ++r) {
        const auto &[name, cuts] = rules[r];
        std::uint64_t total = 0;
        std::vector<std::byte> chain;
        std::string raw_list, byte_list;
        for (std::size_t f = 0, s = 0; f < cuts.size(); s = cuts[f++]) {
            const auto &d = *done[index.at({s, cuts[f]})];
            total += d.compressed;
            for (int k = 0; k < 4; ++k) {
                chain.push_back(static_cast<std::byte>(d.crc >> (8 * k)));
            }
            raw_list += (f ? "," : "") + std::to_string(in.bytes(s, cuts[f]));
            byte_list += (f ? "," : "") + std::to_string(d.compressed);
        }
        std::cout << (r ? "," : "") << "{\"rule\":" << json_string(name) << ",\"frames\":" << cuts.size()
                  << ",\"compressed_bytes\":" << total
                  << ",\"frame_crc_chain\":" << seedarc::experimental::crc32c(chain) << ",\"frame_raw\":["
                  << raw_list << "],\"frame_bytes\":[" << byte_list << "]}";
    }
    std::cout << "]," << peak_memory_json() << "}\n";
    return std::cout ? 0 : 1;
}
