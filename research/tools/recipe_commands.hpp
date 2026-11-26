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
        std::string recipe = "null", seed = "null", text, check;
        if (g.recipe) {
            const SolidRecipe r{g.plan, *g.recipe};
            recipe = json_string(recipe_text(r));
            seed = json_string(hex32_text(encode_recipe_seed(r)));
            text = recipe_text(r);
            check = "frame boundaries verified against the recipe; level declared, not verified "
                    "from data";
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
                ",\"check\":" + json_string(check) + "}";
    }
    std::cout << "{\"schema\":\"seedarc.solid-describe.r0\",\"archive_bytes\":" << bytes->size()
              << ",\"files\":" << m->entries.size() << ",\"generations\":[" << gens << "]}\n";
    return std::cout ? 0 : 1;
}
