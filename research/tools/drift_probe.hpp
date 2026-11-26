// SPDX-License-Identifier: GPL-3.0-or-later
// Private append-drift probe (research/ideas/append-drift-r0.md), included inside
// append4_study.cpp's anonymous namespace. Measurement only: nothing here is written into an
// archive or changes a format. Every variant is decoded again and compared byte for byte.

std::vector<std::byte> concat_files(const std::vector<const InputFile *> &files) {
    std::vector<std::byte> out;
    for (const auto *f : files) {
        out.insert(out.end(), f->data.begin(), f->data.end());
    }
    return out;
}

std::string_view parent_dir(std::string_view path) {
    const auto slash = path.rfind('/');
    return slash == std::string_view::npos ? std::string_view{} : path.substr(0, slash);
}

// Old files ranked by the deepest directory they share with any new file (that directory
// itself or an ancestor of it), then by path; taken until `cap` bytes, then put back in path
// order. Deterministic: depends only on paths and sizes.
std::vector<const InputFile *> related_old(const std::vector<InputFile> &old_files,
                                           const std::vector<InputFile> &new_files, std::uint64_t cap) {
    std::set<std::string_view> new_dirs;
    for (const auto &f : new_files) {
        for (auto d = parent_dir(f.path); !d.empty(); d = parent_dir(d)) {
            new_dirs.insert(d);
        }
    }
    std::vector<std::pair<std::size_t, const InputFile *>> ranked;
    for (const auto &f : old_files) {
        std::size_t depth = 0;
        for (auto d = parent_dir(f.path); !d.empty(); d = parent_dir(d)) {
            if (new_dirs.contains(d)) {
                depth = static_cast<std::size_t>(std::ranges::count(d, '/')) + 1;
                break;
            }
        }
        ranked.emplace_back(depth, &f);
    }
    std::ranges::stable_sort(ranked, [](const auto &a, const auto &b) { return a.first > b.first; });
    std::vector<const InputFile *> chosen;
    std::uint64_t total = 0;
    for (const auto &[depth, f] : ranked) {
        if (depth == 0 || total + f->data.size() > cap) {
            continue;
        }
        chosen.push_back(f);
        total += f->data.size();
    }
    std::ranges::sort(chosen, {}, &InputFile::path);
    return chosen;
}

int cmd_drift_probe(const fs::path &old_dir, const fs::path &new_dir, std::uint32_t zstd_level) {
    auto old_files = read_tree(old_dir);
    auto new_files = read_tree(new_dir);
    if (!old_files || !new_files || old_files->empty() || new_files->empty()) {
        return fail(!old_files ? old_files.error() : !new_files ? new_files.error() : bad("empty input"));
    }
    std::vector<const InputFile *> old_view, new_view, all_view;
    for (const auto &f : *old_files) {
        old_view.push_back(&f);
        all_view.push_back(&f);
    }
    for (const auto &f : *new_files) {
        new_view.push_back(&f);
        all_view.push_back(&f);
    }
    std::ranges::sort(all_view, {}, &InputFile::path);
    const auto old_raw = concat_files(old_view), new_raw = concat_files(new_view);
    const auto all_raw = concat_files(all_view);
    if (all_raw.size() > research::max_solid_bytes) {
        return fail(Error{ErrorCode::resource_limit, "probe input exceeds one solid frame"});
    }
    std::string variants;
    std::uint64_t baseline = 0;
    const auto add = [&](std::string_view name, std::span<const std::byte> reference, bool raw_prefix,
                         std::uint64_t stored_extra, bool long_distance = false) -> bool {
        const auto t0 = wall_ms();
        auto frame = research::compress_solid_referenced(new_raw, zstd_level, reference, raw_prefix,
                                                         long_distance);
        const auto encode_ms = wall_ms() - t0;
        if (!frame) {
            std::cerr << name << ": " << frame.error().message << '\n';
            return false;
        }
        std::vector<std::byte> back(new_raw.size());
        if (!research::decompress_solid_referenced(*frame, reference, raw_prefix, back) || back != new_raw) {
            std::cerr << name << ": round trip failed\n";
            return false;
        }
        if (baseline == 0) {
            baseline = frame->size();
        }
        variants += (variants.empty() ? "" : ",") + std::string{"{\"name\":"} + json_string(name) +
                    ",\"compressed_bytes\":" + std::to_string(frame->size()) +
                    ",\"stored_extra_bytes\":" + std::to_string(stored_extra) +
                    ",\"total_bytes\":" + std::to_string(frame->size() + stored_extra) +
                    ",\"dependency_raw_bytes\":" + std::to_string(raw_prefix ? reference.size() : 0) +
                    ",\"encode_ms\":" + std::to_string(encode_ms) + "}";
        return true;
    };
    if (!add("none", {}, true, 0)) {
        return 1;
    }
    // Trained dictionaries: old files in path order as samples, each cut to 128 KiB.
    std::vector<std::byte> samples;
    std::vector<std::size_t> sizes;
    for (const auto *f : old_view) {
        const auto n = std::min<std::size_t>(f->data.size(), 128 * 1024);
        if (n == 0 || samples.size() + n > research::max_training_bytes || sizes.size() >= research::max_training_samples) {
            continue;
        }
        samples.insert(samples.end(), f->data.begin(), f->data.begin() + static_cast<std::ptrdiff_t>(n));
        sizes.push_back(n);
    }
    for (const std::size_t capacity : {std::size_t{112} * 1024, std::size_t{1024} * 1024}) {
        const auto t0 = wall_ms();
        auto dict = research::train_dictionary(samples, sizes, capacity);
        const auto train_ms = wall_ms() - t0;
        if (!dict) {
            std::cerr << "dictionary " << capacity << ": " << dict.error().message << '\n';
            continue;
        }
        const auto name = "dict-" + std::to_string(capacity / 1024) + "KiB";
        if (!add(name, *dict, false, dict->size())) {
            return 1;
        }
        variants.insert(variants.size() - 1, ",\"train_ms\":" + std::to_string(train_ms));
    }
    for (const std::uint64_t cap : {8ULL << 20, 32ULL << 20}) {
        const auto take = std::min<std::size_t>(old_raw.size(), static_cast<std::size_t>(cap));
        if (!add("tail-" + std::to_string(cap >> 20) + "MiB",
                 std::span{old_raw}.last(take), true, 0)) {
            return 1;
        }
        const auto related = concat_files(related_old(*old_files, *new_files, cap));
        const auto tag = "related-" + std::to_string(cap >> 20) + "MiB";
        if (!related.empty() && (!add(tag, related, true, 0) || !add(tag + "-ldm", related, true, 0, true))) {
            return 1;
        }
    }
    if (!add("none-ldm", {}, true, 0, true)) {
        return 1;
    }
    // Per directory group (first `depth` path components): each group of new files is its own
    // frame and refers only to the old files of the same group, in path order, up to `cap`
    // bytes. Cap 0 is the no-reference control that isolates the cost of splitting the batch.
    // dependency_raw_bytes reports the largest single-frame reference (what one extraction
    // needs); dependency_total_bytes all referenced old bytes. Exploration sweep, not tuned.
    const auto group_of_path = [](std::string_view p, int depth) {
        std::size_t at = 0;
        for (int i = 0; i < depth; ++i) {
            const auto next = p.find('/', at);
            if (next == std::string_view::npos) {
                return parent_dir(p);
            }
            at = next + 1;
        }
        return p.substr(0, at - 1);
    };
    for (const int depth : {2, 3, 4}) {
        std::map<std::string_view, std::vector<const InputFile *>> new_groups, old_groups;
        for (const auto &f : *new_files) {
            new_groups[group_of_path(f.path, depth)].push_back(&f);
        }
        for (const auto &f : *old_files) {
            old_groups[group_of_path(f.path, depth)].push_back(&f);
        }
        for (const std::uint64_t cap : {0ULL, 8ULL << 20, 32ULL << 20}) {
            std::uint64_t total = 0, dependency = 0, dependency_total = 0;
            const auto t0 = wall_ms();
            for (const auto &[key, members] : new_groups) {
                const auto raw = concat_files(members);
                std::vector<std::byte> reference;
                if (const auto it = old_groups.find(key); cap != 0 && it != old_groups.end()) {
                    for (const auto *f : it->second) {
                        if (reference.size() + f->data.size() <= cap) {
                            reference.insert(reference.end(), f->data.begin(), f->data.end());
                        }
                    }
                }
                auto frame = research::compress_solid_referenced(raw, zstd_level, reference, true);
                std::vector<std::byte> back(raw.size());
                if (!frame || !research::decompress_solid_referenced(*frame, reference, true, back) ||
                    back != raw) {
                    std::cerr << "per-group round trip failed\n";
                    return 1;
                }
                total += frame->size();
                dependency = std::max<std::uint64_t>(dependency, reference.size());
                dependency_total += reference.size();
            }
            variants += ",{\"name\":" +
                        json_string("per-group-d" + std::to_string(depth) +
                                    (cap == 0 ? std::string{"-none"} : "-" + std::to_string(cap >> 20) + "MiB")) +
                        ",\"compressed_bytes\":" + std::to_string(total) + ",\"stored_extra_bytes\":0" +
                        ",\"total_bytes\":" + std::to_string(total) +
                        ",\"dependency_raw_bytes\":" + std::to_string(dependency) +
                        ",\"dependency_total_bytes\":" + std::to_string(dependency_total) +
                        ",\"groups\":" + std::to_string(new_groups.size()) +
                        ",\"encode_ms\":" + std::to_string(wall_ms() - t0) + "}";
        }
    }
    // Ideal: the new files' marginal cost inside one full rebuild of old + new.
    const auto t0 = wall_ms();
    auto whole = research::compress_solid(all_raw, zstd_level);
    auto old_only = research::compress_solid(old_raw, zstd_level);
    if (!whole || !old_only) {
        return fail(!whole ? whole.error() : old_only.error());
    }
    const auto ideal = whole->size() > old_only->size() ? whole->size() - old_only->size() : 0;
    std::cout << "{\"schema\":\"seedarc.drift-probe.r0\",\"level\":" << zstd_level
              << ",\"old_files\":" << old_files->size() << ",\"old_bytes\":" << old_raw.size()
              << ",\"new_files\":" << new_files->size() << ",\"new_bytes\":" << new_raw.size()
              << ",\"baseline_bytes\":" << baseline << ",\"rebuild_all_bytes\":" << whole->size()
              << ",\"old_only_bytes\":" << old_only->size() << ",\"ideal_marginal_bytes\":" << ideal
              << ",\"ideal_ms\":" << wall_ms() - t0 << ",\"variants\":[" << variants << "],"
              << peak_memory_json() << "}\n";
    return std::cout ? 0 : 1;
}
