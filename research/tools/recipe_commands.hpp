// SPDX-License-Identifier: GPL-3.0-or-later
// Private solid-recipe-r1 commands (ADR-0006), included inside append4_study.cpp's anonymous
// namespace: recipe-pack, recipe-append and describe. The older bounded-pack / bounded-append
// keep writing mapping revision 2 byte for byte.

std::string hex32_text(std::uint32_t value) {
    constexpr char digits[] = "0123456789abcdef";
    std::string out = "0x";
    bool started = false;
    for (int shift = 28; shift >= 0; shift -= 4) {
        const auto d = (value >> shift) & 15;
        started = started || d != 0 || shift == 0;
        if (started) {
            out += digits[d];
        }
    }
    return out;
}

int cmd_recipe_pack(std::string_view representation, std::string_view recipe_arg,
                    const fs::path &input, const fs::path &out) {
    const auto recipe = parse_recipe(recipe_arg);
    if (!recipe || (representation != "seed" && representation != "explicit")) {
        std::cerr << "not a solid-recipe-r1 recipe (text form or 0x Seed)\n";
        return 2;
    }
    return cmd_pack(representation == "seed" ? "seed-solid" : "explicit-solid", "", input, out,
                    red_off, 0, recipe_frame_limit(recipe->fields), recipe);
}

// DEV-039: per-worker memory estimate for a recipe, from the codec library's own estimate for a
// full frame (the frame limit; 1 GiB for whole): joined frame + encoder state + output buffer.
int cmd_memory_estimate(std::string_view recipe_arg) {
    const auto recipe = parse_recipe(recipe_arg);
    if (!recipe) {
        std::cerr << "not a solid-recipe-r1 recipe (text form or 0x Seed)\n";
        return 2;
    }
    const auto frame = std::min<std::uint64_t>(recipe_frame_limit(recipe->fields), research::max_solid_bytes);
    const auto encoder = research::solid_encoder_memory(recipe->fields.codec, recipe->fields.level, frame,
                                                        recipe->fields.extreme, recipe_dict_size(recipe->fields));
    if (!encoder) {
        return fail(encoder.error());
    }
    std::cout << "{\"schema\":\"seedarc.memory-estimate.r0\",\"recipe\":" << json_string(recipe_text(*recipe))
              << ",\"frame_bytes\":" << frame << ",\"encoder_and_output_bytes\":" << *encoder
              << ",\"per_worker_bytes\":" << frame + *encoder << "}\n";
    return std::cout ? 0 : 1;
}

// Without RECIPE the new generation keeps the last generation's recipe (queue variant A); with
// it, the new generation gets its own grouping and recipe (variant B). Old data is untouched.
int cmd_recipe_append(const fs::path &old_path, const fs::path &new_dir, const fs::path &out,
                      std::optional<std::string_view> recipe_arg) {
    std::optional<SolidRecipe> recipe;
    if (recipe_arg) {
        recipe = parse_recipe(*recipe_arg);
        if (!recipe) {
            std::cerr << "not a solid-recipe-r1 recipe (text form or 0x Seed)\n";
            return 2;
        }
    }
    std::uint64_t limit = 0;
    if (recipe) {
        limit = recipe_frame_limit(recipe->fields);
    } else {
        auto old = read_file(old_path);
        if (!old) {
            return fail(old.error());
        }
        auto m = parse(*old);
        if (!m) {
            return fail(m.error());
        }
        if (!m->gens.back().recipe) {
            return fail(bad("the last generation records no recipe; give RECIPE"));
        }
        limit = recipe_frame_limit(*m->gens.back().recipe);
    }
    return cmd_append(old_path, new_dir, out, "segment",
                      recipe ? std::optional{recipe->plan} : std::nullopt, limit, true,
                      recipe ? std::optional{recipe->fields} : std::nullopt);
}

// What each generation was written with. A full strict parse runs first, so a recipe that
// disagrees with the frames never gets described.
int cmd_describe(const fs::path &archive) {
    auto bytes = read_file(archive);
    if (!bytes) {
        return fail(bytes.error());
    }
    auto m = parse(*bytes);
    if (!m) {
        return fail(m.error());
    }
    if (m->header.revision != 5 || !m->header.solid()) {
        return fail(bad("describe covers revision-5 solid archives"));
    }
    std::string gens;
    for (const auto &g : m->gens) {
        std::uint64_t frames = 0, largest = 0;
        for (const auto &f : m->frames) {
            if (f.gen == g.index) {
                ++frames;
                largest = std::max(largest, f.raw);
            }
        }
        // ADR-0008 aliases: entries that repeat an earlier entry's frame and offset (size > 0).
        std::uint64_t alias_files = 0, alias_bytes = 0;
        std::set<std::pair<std::uint32_t, std::uint64_t>> seen;
        for (std::uint32_t i = g.first_file; i < g.files_after; ++i) {
            const auto &e = m->entries[i];
            if (e.size > 0 && !seen.emplace(e.frame, e.frame_offset).second) {
                ++alias_files;
                alias_bytes += e.size;
            }
        }
        std::string recipe = "null", seed = "null", text, check;
        if (g.recipe) {
            const SolidRecipe r{g.plan, *g.recipe};
            recipe = json_string(recipe_text(r));
            seed = json_string(hex32_text(encode_recipe_seed(r)));
            text = recipe_text(r);
            check = std::string{"frame boundaries verified against the recipe"} +
                    (g.recipe->dedup ? "; aliases verified against earlier entries" : "") + "; level" +
                    (g.recipe->extreme ? " and extreme" : "") +
                    (g.recipe->dict_code != 0 ? " and dictionary" : "") +
                    (g.recipe->dedup ? " and dedup completeness" : "") + " declared, not verified from data";
        } else {
            text = "solid/zstd-9/g" + std::to_string(g.plan.k) + "-r" +
                   std::to_string(g.plan.rule) + " (frame limit not recorded)";
            check = "mapping revision 2: Zstd 9 fixed by the revision; frame limit not recorded";
        }
        gens += (gens.empty() ? "" : ",") + std::string{"{\"generation\":"} +
                std::to_string(g.index) + ",\"files\":" + std::to_string(g.files_after - g.first_file) +
                ",\"mapping_revision\":" + std::to_string(g.mapping_revision) +
                ",\"representation\":" + (g.has_seed ? "\"seed\"" : "\"explicit\"") +
                ",\"recipe\":" + recipe + ",\"recipe_seed\":" + seed +
                ",\"summary\":" + json_string(text) + ",\"frames\":" + std::to_string(frames) +
                ",\"max_frame_raw_bytes\":" + std::to_string(largest) +
                ",\"alias_files\":" + std::to_string(alias_files) +
                ",\"alias_bytes\":" + std::to_string(alias_bytes) +
                ",\"check\":" + json_string(check) + "}";
    }
    // Facts only: how much of the plaintext arrived after generation 0. The size cost of that
    // depends on the data (DEV-027 / DEV-028), so the advice is to measure, not a guessed figure.
    std::uint64_t raw_total = 0, raw_appended = 0;
    for (const auto &f : m->frames) {
        raw_total += f.raw;
        raw_appended += f.gen == 0 ? 0 : f.raw;
    }
    const double share =
        raw_total == 0 ? 0.0 : static_cast<double>(raw_appended) / static_cast<double>(raw_total);
    std::cout << "{\"schema\":\"seedarc.solid-describe.r1\",\"archive_bytes\":" << bytes->size()
              << ",\"files\":" << m->entries.size() << ",\"generations\":[" << gens
              << "],\"consolidation\":{\"generation_count\":" << m->gens.size()
              << ",\"raw_bytes\":" << raw_total << ",\"appended_raw_bytes\":" << raw_appended
              << ",\"appended_raw_share\":" << share << ",\"hint\":"
              << json_string(m->gens.size() == 1
                                 ? "one generation: nothing to consolidate"
                                 : "appended generations cannot reference older data; measure "
                                   "the saving with consolidate-check")
              << "}}\n";
    return std::cout ? 0 : 1;
}

// Rebuild a revision-5 solid archive as one generation: decode every file, re-encode the whole
// tree with one recipe (default: the last generation's), check the result byte for byte, and
// report the saving. Without OUT nothing is written. The rebuild of a tree equals recipe-pack
// of that tree, so consolidation never invents a layout of its own.
int cmd_consolidate(const fs::path &archive, const std::optional<fs::path> &out,
                    std::optional<std::string_view> recipe_arg) {
    const auto w = wall_ms(), c = cpu_ms();
    auto bytes = read_file(archive);
    if (!bytes) {
        return fail(bytes.error());
    }
    auto m = parse(*bytes);
    if (!m) {
        return fail(m.error());
    }
    if (m->header.revision != 5 || !m->header.solid()) {
        return fail(bad("consolidate covers revision-5 solid archives"));
    }
    const Gen &last = m->gens.back();
    std::optional<SolidRecipe> recipe;
    if (recipe_arg) {
        recipe = parse_recipe(*recipe_arg);
        if (!recipe) {
            std::cerr << "not a solid-recipe-r1 recipe (text form or 0x Seed)\n";
            return 2;
        }
    } else if (last.recipe) {
        recipe = SolidRecipe{last.plan, *last.recipe};
    } else {
        return fail(bad("the last generation records no recipe; give RECIPE"));
    }
    if (out && fs::exists(*out)) {
        return fail(Error{ErrorCode::output_conflict, "output exists"});
    }
    const auto dw = wall_ms();
    auto decoded = decode_all(*m, *bytes);
    if (!decoded) {
        return fail(decoded.error());
    }
    if (decoded->size() != m->entries.size()) {
        return fail(bad("decoded file count differs from the index"));
    }
    std::vector<InputFile> files; // std::map order is read_tree's path order
    files.reserve(decoded->size());
    for (auto &[path, data] : *decoded) {
        files.push_back({path, std::move(data)});
    }
    decoded->clear();
    const auto decode_wall = wall_ms() - dw;
    const auto pw = wall_ms();
    auto packed = pack_bytes(last.has_seed ? "seed-solid" : "explicit-solid", recipe->plan, files,
                             red_off, 0, recipe_frame_limit(recipe->fields), recipe->fields);
    if (!packed) {
        return fail(packed.error());
    }
    const auto pack_wall = wall_ms() - pw;
    auto rebuilt = parse(packed->bytes);
    if (!rebuilt) {
        return fail(rebuilt.error());
    }
    if (auto checked = byte_exact(files, *rebuilt, packed->bytes); !checked) {
        return fail(checked.error());
    }
    if (out) {
        if (auto r = write_new(*out, packed->bytes); !r) {
            return fail(r.error());
        }
    }
    const auto before = bytes->size(), after = packed->bytes.size();
    const auto saved = static_cast<std::int64_t>(before) - static_cast<std::int64_t>(after);
    std::cout << "{\"schema\":\"seedarc.solid-consolidate.r0\",\"written\":"
              << (out ? "true" : "false") << ",\"files\":" << files.size()
              << ",\"input_bytes\":" << input_bytes_of(files)
              << ",\"generations_before\":" << m->gens.size()
              << ",\"recipe\":" << json_string(recipe_text(*recipe))
              << ",\"recipe_from\":" << (recipe_arg ? "\"argument\"" : "\"last generation\"")
              << ",\"archive_bytes_before\":" << before << ",\"archive_bytes_after\":" << after
              << ",\"saved_bytes\":" << saved << ",\"saved_share\":"
              << static_cast<double>(saved) / static_cast<double>(before)
              << ",\"decode_wall_ms\":" << decode_wall << ",\"pack_wall_ms\":" << pack_wall
              << ",\"call_wall_ms\":" << wall_ms() - w << ",\"call_cpu_ms\":" << cpu_ms() - c
              << ",\"byte_exact\":true," << peak_memory_json() << "}\n";
    return std::cout ? 0 : 1;
}
