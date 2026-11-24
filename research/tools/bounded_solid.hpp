// SPDX-License-Identifier: GPL-3.0-or-later
// Private command helpers, included inside append4_study.cpp's anonymous namespace.

int cmd_access_profile(const fs::path &archive, const fs::path &input, std::size_t count) {
    auto files = read_tree(input);
    if (!files || files->empty()) {
        return fail(files ? bad("access profile needs files") : files.error());
    }
    count = std::min(count, files->size());
    std::vector<std::size_t> indices;
    for (std::size_t sample = 0; sample < count; ++sample) {
        indices.push_back(count == 1 ? 0 : sample * (files->size() - 1) / (count - 1));
    }
    std::size_t largest = 0;
    for (std::size_t i = 1; i < files->size(); ++i) {
        if ((*files)[i].data.size() > (*files)[largest].data.size()) {
            largest = i;
        }
    }
    if (std::ranges::find(indices, largest) == indices.end()) {
        indices.push_back(largest);
    }
    std::ranges::sort(indices);
    std::string samples;
    std::vector<double> times;
    std::uint64_t decoded_total = 0, requested_total = 0, archive_total = 0;
    for (const auto index : indices) {
        const auto &want = (*files)[index];
        const auto start = wall_ms();
        auto bytes = read_file(archive);
        if (!bytes) {
            return fail(bytes.error());
        }
        auto model = parse(*bytes);
        if (!model || !model->header.solid()) {
            return fail(model ? bad("access profile requires solid frames") : model.error());
        }
        const auto entry = std::ranges::find(model->entries, want.path, &Entry::path);
        if (entry == model->entries.end()) {
            return fail(bad("requested file is missing"));
        }
        const auto &frame = model->frames[entry->frame];
        auto raw = decode_frame(frame, *bytes);
        if (!raw) {
            return fail(raw.error());
        }
        const auto view = std::span{*raw}.subspan(static_cast<std::size_t>(entry->frame_offset),
                                                  static_cast<std::size_t>(entry->size));
        if (seedarc::experimental::crc32c(view) != entry->file_crc ||
            !std::ranges::equal(view, want.data)) {
            return fail(bad("single-file profile failed byte-exact validation"));
        }
        const auto elapsed = wall_ms() - start;
        times.push_back(elapsed);
        decoded_total += frame.raw;
        requested_total += entry->size;
        archive_total += bytes->size();
        samples += (samples.empty() ? "" : ",") + std::string{"{\"path\":"} +
                   json_string(want.path) + ",\"requested_bytes\":" + std::to_string(entry->size) +
                   ",\"decoded_bytes\":" + std::to_string(frame.raw) +
                   ",\"archive_bytes_read\":" + std::to_string(bytes->size()) +
                   ",\"frame\":" + std::to_string(entry->frame) +
                   ",\"wall_ms\":" + std::to_string(elapsed) + "}";
    }
    std::ranges::sort(times);
    const auto quantile = [&](std::size_t numerator) {
        return times[(numerator * (times.size() - 1) + 50) / 100];
    };
    std::cout << "{\"schema\":\"seedarc.bounded-access.r0\",\"byte_exact\":true,\"samples\":["
              << samples << "],\"sample_count\":" << indices.size()
              << ",\"selection\":\"uniform paths plus largest file, deduplicated\""
              << ",\"median_ms\":" << quantile(50) << ",\"p95_ms\":" << quantile(95)
              << ",\"max_ms\":" << times.back() << ",\"requested_bytes_total\":" << requested_total
              << ",\"decoded_bytes_total\":" << decoded_total
              << ",\"archive_bytes_read_total\":" << archive_total
              << ",\"io_mode\":\"whole archive per sample; no decoder reuse\","
              << peak_memory_json() << "}\n";
    return std::cout ? 0 : 1;
}

int cmd_payload_fault_profile(const fs::path &archive, const fs::path &input) {
    auto files = read_tree(input);
    auto bytes = read_file(archive);
    if (!files || !bytes) {
        return fail(!files ? files.error() : bytes.error());
    }
    auto original = parse(*bytes);
    if (!original || !original->header.solid() || original->frames.empty()) {
        return fail(original ? bad("fault profile requires nonempty solid archive")
                             : original.error());
    }
    if (auto checked = byte_exact(*files, *original, *bytes); !checked) {
        return fail(checked.error());
    }
    const auto by_path = index_files(*files);
    std::uint64_t payload = 0, total_raw = input_bytes_of(*files);
    for (const auto &f : original->frames) {
        payload += f.stored;
    }
    std::string cases;
    for (const auto percent : {10U, 50U, 90U}) {
        auto target = payload * percent / 100;
        std::size_t target_frame = 0;
        while (target >= original->frames[target_frame].stored) {
            target -= original->frames[target_frame++].stored;
        }
        const auto position = original->frames[target_frame].offset + target;
        auto damaged = *bytes;
        damaged[static_cast<std::size_t>(position)] ^= std::byte{1};
        auto model = parse(damaged); // never use an oracle index if damaged input is not parseable
        std::uint64_t good_files = 0, good_bytes = 0, rejected_files = 0, coverage = 0;
        const auto start = wall_ms();
        for (const auto &e : original->entries) {
            if (e.frame == target_frame) {
                coverage += e.size;
            }
        }
        if (model) {
            std::vector<std::vector<const Entry *>> members(model->frames.size());
            for (const auto &e : model->entries) {
                members[e.frame].push_back(&e);
            }
            for (std::size_t fi = 0; fi < model->frames.size(); ++fi) {
                auto raw = decode_frame(model->frames[fi], damaged);
                for (const auto *member : members[fi]) {
                    const auto &e = *member;
                    if (!raw) {
                        ++rejected_files;
                        continue;
                    }
                    const auto view = std::span{*raw}.subspan(
                        static_cast<std::size_t>(e.frame_offset), static_cast<std::size_t>(e.size));
                    if (seedarc::experimental::crc32c(view) != e.file_crc) {
                        ++rejected_files;
                        continue;
                    }
                    const auto expected = by_path.find(e.path);
                    if (expected == by_path.end() ||
                        !std::ranges::equal(view, expected->second->data)) {
                        return fail(bad("corrupt data passed the file CRC"));
                    }
                    ++good_files;
                    good_bytes += e.size;
                }
            }
        } else {
            rejected_files = original->entries.size();
        }
        cases += (cases.empty() ? "" : ",") + std::string{"{\"payload_percent\":"} +
                 std::to_string(percent) + ",\"damage_offset\":" + std::to_string(position) +
                 ",\"frame\":" + std::to_string(target_frame) +
                 ",\"index_usable\":" + (model ? "true" : "false") +
                 ",\"frame_file_bytes_at_risk\":" + std::to_string(coverage) +
                 ",\"verified_files\":" + std::to_string(good_files) +
                 ",\"verified_bytes\":" + std::to_string(good_bytes) +
                 ",\"unavailable_bytes\":" + std::to_string(total_raw - good_bytes) +
                 ",\"rejected_files\":" + std::to_string(rejected_files) +
                 ",\"decode_validate_wall_ms\":" + std::to_string(wall_ms() - start) + "}";
    }
    std::cout << "{\"schema\":\"seedarc.bounded-payload-fault.r0\",\"verified_source\":true,"
                 "\"fault\":\"one-bit payload flips; intact metadata\",\"cases\":["
              << cases << "],\"input_bytes\":" << total_raw << ",\"files\":" << files->size()
              << ",\"wrong_bytes_accepted\":0,\"outputs_written\":0," << peak_memory_json()
              << "}\n";
    return std::cout ? 0 : 1;
}
