// SPDX-License-Identifier: GPL-3.0-or-later
// DEV-016 research tool: G5, budgeted search over grouping and dictionary allocation
// (research/ideas/seed-g5-r1.md). Writes EXPERIMENTAL research container v2 (not the SeedArc
// archive format; nothing here is frozen) in which every byte counts: header, optional Seed,
// group table, dictionaries, per-file group table, index and data.
//
// A plan = grouping rule, group count k and per-group dictionary size D and training sample T.
// All search methods share one evaluator with a per-group cache; the budget counts the input
// bytes actually read by training and trial encoding (cache hits are free), the same for every
// method. The final plan is rebuilt, its size must equal the evaluated size, and it is decoded in
// memory and compared byte for byte with the input before anything is reported.
#include "codec/codec.hpp"
#include "codec/dictionary.hpp"
#include "seedarc/archive.hpp"
#include "splitmix64/splitmix64.hpp"
#include "study_common.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace codec = seedarc::experimental::codec;
namespace research = seedarc::experimental::codec::research;

namespace {
using namespace seedarc::research_tools;

constexpr std::string_view usage =
    "Research-only G5 study (container v2); not the SeedArc archive format.\n"
    "  g5_study search METHOD INPUT_DIR BUDGET_BYTES SEED OUT [STALL]\n"
    "      METHOD: random | coord | seed; SEED: search PRNG seed (decimal); STALL default 20\n"
    "  g5_study pack PLAN_HEX INPUT_DIR OUT       (fixed plan, e.g. the G-dict-r1 start plan)\n"
    "  g5_study verify CONTAINER INPUT_DIR\n"
    "  g5_study selftest INPUT_DIR                 (damage rejection and Seed round trip)\n";
constexpr std::array<char, 8> magic{'S', 'A', 'D', 'X', 'R', '0', '0', '2'};
constexpr std::uint32_t version = 2;
constexpr std::uint32_t chunk = 65536;
constexpr std::uint32_t level = 9; // DEV-013 selection
constexpr std::size_t header_bytes = 40;
constexpr std::size_t max_groups = 8;
constexpr std::uint32_t seed_field_bytes = 4;
constexpr std::size_t group_entry_bytes = 8; // dictionary size + CRC32C of the dictionary
constexpr std::array<std::size_t, 4> dict_sizes{0, 16384, 65536, 114688};
constexpr std::array<std::size_t, 2> train_prefix{chunk, 4096}; // per-file sample prefix
constexpr std::array<const char *, 4> rule_names{"dir1", "dir2", "ext", "size"};
enum : std::uint8_t { kind_store = 0, kind_zstd = 1, kind_zstd_dict = 2 };

Error bad(const char *what) {
    return {ErrorCode::integrity_error, std::string{"research container v2: "} + what};
}
int fail(const Error &e) {
    std::cerr << e.message << '\n';
    return 1;
}

// ---- plans and the Seed encoding --------------------------------------------------------------
struct Plan {
    std::uint8_t rule = 0;
    std::uint8_t k = 1;
    std::array<std::uint8_t, max_groups> d{};
    std::array<std::uint8_t, max_groups> t{};
    auto operator<=>(const Plan &) const = default;
};

// One canonical form per behaviour: the rule is irrelevant for one group, T is irrelevant
// without a dictionary, parameters of groups >= k are unused.
Plan canonical(Plan p) {
    if (p.k == 1) {
        p.rule = 0;
    }
    for (std::size_t i = 0; i < max_groups; ++i) {
        if (i >= p.k) {
            p.d[i] = 0;
            p.t[i] = 0;
        } else if (p.d[i] == 0) {
            p.t[i] = 0;
        }
    }
    return p;
}

// G-dict-r1: one group, 64 KiB dictionary, trained on every file's first block.
Plan start_plan() {
    Plan p;
    p.d[0] = 2;
    return canonical(p);
}

// Seed layout (29 bits, serialized as 4 bytes; not frozen): rule:2 | k-1:3 | 8 x (D:2 | T:1).
std::uint32_t encode_seed(const Plan &p) {
    std::uint32_t s = p.rule | (static_cast<std::uint32_t>(p.k - 1) << 2);
    for (std::size_t i = 0; i < max_groups; ++i) {
        s |= static_cast<std::uint32_t>(p.d[i] | (p.t[i] << 2)) << (5 + 3 * i);
    }
    return s;
}
std::optional<Plan> decode_seed(std::uint32_t s) {
    if (s >> 29) {
        return std::nullopt;
    }
    Plan p;
    p.rule = static_cast<std::uint8_t>(s & 3);
    p.k = static_cast<std::uint8_t>(((s >> 2) & 7) + 1);
    for (std::size_t i = 0; i < max_groups; ++i) {
        const auto f = (s >> (5 + 3 * i)) & 7;
        p.d[i] = static_cast<std::uint8_t>(f & 3);
        p.t[i] = static_cast<std::uint8_t>(f >> 2);
    }
    return p;
}
std::string plan_json(const Plan &p) {
    std::string out = "{\"seed\":" + std::to_string(encode_seed(p)) + ",\"rule\":\"" +
                      rule_names[p.rule] + "\",\"k\":" + std::to_string(p.k) + ",\"groups\":[";
    for (std::size_t i = 0; i < p.k; ++i) {
        out += (i ? "," : "") + std::string{"["} + std::to_string(dict_sizes[p.d[i]]) + "," +
               std::to_string(train_prefix[p.t[i]]) + "]";
    }
    return out + "]}";
}

// Fields a search changes: 0 rule, 1 k, then (D_i, T_i) for each active group i.
struct Field {
    int id;
    int values;
};
std::vector<Field> active_fields(const Plan &p) {
    std::vector<Field> f{{0, 4}, {1, static_cast<int>(max_groups)}};
    for (int i = 0; i < p.k; ++i) {
        f.push_back({2 + 2 * i, 4});
        f.push_back({3 + 2 * i, 2});
    }
    return f;
}
int field_value(const Plan &p, int id) {
    if (id == 0) {
        return p.rule;
    }
    if (id == 1) {
        return p.k - 1;
    }
    const auto g = static_cast<std::size_t>((id - 2) / 2);
    return (id % 2 == 0) ? p.d[g] : p.t[g];
}
Plan with_field(Plan p, int id, int value) {
    if (id == 0) {
        p.rule = static_cast<std::uint8_t>(value);
    } else if (id == 1) {
        p.k = static_cast<std::uint8_t>(value + 1);
    } else {
        const auto g = static_cast<std::size_t>((id - 2) / 2);
        (id % 2 == 0 ? p.d[g] : p.t[g]) = static_cast<std::uint8_t>(value);
    }
    return canonical(p);
}

struct Rng {
    std::uint64_t state;
    std::uint64_t next() {
        const auto out = seedarc::third_party::splitmix64_step(state);
        state += 0x9e3779b97f4a7c15ULL;
        return out;
    }
    // Uniform in [0, n) without modulo bias.
    std::uint64_t below(std::uint64_t n) {
        const auto limit = UINT64_MAX - UINT64_MAX % n;
        for (;;) {
            const auto v = next();
            if (v < limit) {
                return v % n;
            }
        }
    }
};
Plan random_plan(Rng &rng) {
    Plan p;
    p.rule = static_cast<std::uint8_t>(rng.below(4));
    p.k = static_cast<std::uint8_t>(rng.below(max_groups) + 1);
    for (std::size_t i = 0; i < max_groups; ++i) {
        p.d[i] = static_cast<std::uint8_t>(rng.below(4));
        p.t[i] = static_cast<std::uint8_t>(rng.below(2));
    }
    return canonical(p);
}

// ---- grouping ---------------------------------------------------------------------------------
// Grouping depends only on each file's path and size, so a decoder can recompute it from the
// index and check a stored Seed against the per-file group table.
struct Named {
    std::string_view path;
    std::uint64_t size;
};
std::string group_key(int rule, const Named &f) {
    const auto path = std::string{f.path};
    switch (rule) {
    case 0: {
        const auto slash = path.find('/');
        return slash == std::string::npos ? std::string{} : path.substr(0, slash);
    }
    case 1: {
        const auto first = path.find('/');
        if (first == std::string::npos) {
            return {};
        }
        const auto second = path.find('/', first + 1);
        return second == std::string::npos ? path.substr(0, first) : path.substr(0, second);
    }
    case 2: {
        const auto base = path.substr(path.rfind('/') + 1); // npos + 1 == 0
        const auto dot = base.rfind('.');
        std::string ext =
            dot == std::string::npos || dot == 0 ? std::string{} : base.substr(dot + 1);
        for (auto &c : ext) {
            if (c >= 'A' && c <= 'Z') {
                c = static_cast<char>(c - 'A' + 'a');
            }
        }
        return ext;
    }
    default:
        return f.size <= 1024 ? "s" : f.size <= 16384 ? "m" : "l";
    }
}

struct Grouping {
    std::vector<std::uint8_t> group_of; // per file
    std::vector<std::vector<std::uint32_t>> members;
};
// Keys ordered by total bytes (descending, then key); the k-1 largest get their own group and
// the rest share the last one. Groups without files are dropped.
Grouping make_grouping(const Plan &p, const std::vector<Named> &files) {
    std::map<std::string, std::uint64_t> bytes;
    std::vector<std::string> keys(files.size());
    for (std::size_t i = 0; i < files.size(); ++i) {
        keys[i] = group_key(p.rule, files[i]);
        bytes[keys[i]] += files[i].size;
    }
    std::vector<std::pair<std::uint64_t, std::string>> order;
    for (const auto &[key, n] : bytes) {
        order.emplace_back(n, key);
    }
    std::ranges::sort(order, [](const auto &a, const auto &b) {
        return a.first != b.first ? a.first > b.first : a.second < b.second;
    });
    std::map<std::string, std::size_t> rank;
    for (std::size_t r = 0; r < order.size(); ++r) {
        rank[order[r].second] = std::min<std::size_t>(r, p.k - 1);
    }
    const auto used = std::min<std::size_t>(p.k, order.size());
    Grouping g;
    g.members.resize(used);
    g.group_of.resize(files.size());
    for (std::size_t i = 0; i < files.size(); ++i) {
        const auto r = rank[keys[i]];
        g.group_of[i] = static_cast<std::uint8_t>(r);
        g.members[r].push_back(static_cast<std::uint32_t>(i));
    }
    return g;
}
std::vector<Named> named(const std::vector<InputFile> &files) {
    std::vector<Named> out;
    out.reserve(files.size());
    for (const auto &f : files) {
        out.push_back({f.path, f.data.size()});
    }
    return out;
}

// ---- encoding one group -----------------------------------------------------------------------
struct GroupResult {
    std::vector<std::byte> dictionary;
    std::uint64_t data_bytes = 0;
    std::uint64_t training_bytes = 0;
    bool training_failed = false;
};
struct EncodedGroup {
    GroupResult result;
    // per member: per block (stored bytes, kind)
    std::vector<std::vector<std::pair<std::vector<std::byte>, std::uint8_t>>> blocks;
};

// Training samples: each member's first T bytes, in a fixed path-hash order (as dict_study).
std::expected<EncodedGroup, Error> encode_group(const std::vector<InputFile> &files,
                                                const std::vector<std::uint32_t> &members,
                                                std::uint8_t d, std::uint8_t t, bool keep_blocks) {
    EncodedGroup out;
    if (dict_sizes[d] != 0) {
        std::vector<std::uint32_t> order;
        for (const auto m : members) {
            if (!files[m].data.empty()) {
                order.push_back(m);
            }
        }
        std::ranges::sort(order, {}, [&](std::uint32_t m) { return fnv1a(files[m].path); });
        std::vector<std::byte> samples;
        std::vector<std::size_t> sizes;
        for (const auto m : order) {
            const auto n = std::min<std::size_t>(files[m].data.size(), train_prefix[t]);
            if (samples.size() + n > research::max_training_bytes) {
                break;
            }
            samples.insert(samples.end(), files[m].data.begin(),
                           files[m].data.begin() + static_cast<std::ptrdiff_t>(n));
            sizes.push_back(n);
        }
        out.result.training_bytes = samples.size();
        if (auto trained = research::train_dictionary(samples, sizes, dict_sizes[d])) {
            out.result.dictionary = std::move(*trained);
        } else {
            out.result.training_failed = true; // falls back to no dictionary, recorded
        }
    }
    auto plain = codec::Encoder::create(codec::zstd_id, level);
    if (!plain) {
        return std::unexpected(plain.error());
    }
    std::optional<research::DictionaryEncoder> dict;
    if (!out.result.dictionary.empty()) {
        auto created = research::DictionaryEncoder::create(level, out.result.dictionary);
        if (!created) {
            return std::unexpected(created.error());
        }
        dict.emplace(std::move(*created));
    }
    std::vector<std::byte> packed(chunk);
    for (const auto m : members) {
        const auto &data = files[m].data;
        std::vector<std::pair<std::vector<std::byte>, std::uint8_t>> file_blocks;
        for (std::size_t at = 0; at < data.size(); at += chunk) {
            const auto raw =
                std::span{data}.subspan(at, std::min<std::size_t>(chunk, data.size() - at));
            const auto room = std::span{packed}.first(raw.size() - 1);
            auto size = dict ? dict->encode(raw, room) : plain->encode(raw, room);
            if (!size) {
                return std::unexpected(size.error());
            }
            const auto kind = *size != 0 ? (dict ? kind_zstd_dict : kind_zstd) : kind_store;
            const auto stored = *size != 0 ? std::span<const std::byte>{packed}.first(*size) : raw;
            out.result.data_bytes += stored.size();
            if (keep_blocks) {
                file_blocks.emplace_back(std::vector<std::byte>{stored.begin(), stored.end()},
                                         kind);
            }
        }
        if (keep_blocks) {
            out.blocks.push_back(std::move(file_blocks));
        }
    }
    return out;
}

// ---- evaluator --------------------------------------------------------------------------------
struct Evaluation {
    std::uint64_t bytes; // full container bytes (without the optional Seed field)
    std::uint64_t dictionary_bytes;
    std::uint64_t data_bytes;
    std::size_t groups;
    std::size_t training_failures;
};

class Evaluator {
  public:
    Evaluator(const std::vector<InputFile> &files, std::uint64_t budget)
        : files_(files), named_(named(files)), budget_(budget) {
        for (const auto &f : files) {
            index_bytes_ += 2 + f.path.size() + 8 + 4 + 9 * ((f.data.size() + chunk - 1) / chunk);
        }
    }
    // nullopt: the uncached part would exceed the remaining budget (nothing is spent).
    std::expected<std::optional<Evaluation>, Error> evaluate(const Plan &plan) {
        if (const auto it = plans_.find(plan); it != plans_.end()) {
            ++plan_cache_hits_;
            return it->second;
        }
        const auto grouping = make_grouping(plan, named_);
        std::uint64_t cost = 0;
        for (std::size_t g = 0; g < grouping.members.size(); ++g) {
            if (!groups_.contains(key(grouping.members[g], plan.d[g], plan.t[g]))) {
                cost += group_cost(grouping.members[g], plan.d[g], plan.t[g]);
            }
        }
        if (spent_ + cost > budget_) {
            return std::optional<Evaluation>{};
        }
        Evaluation e{header_bytes + files_.size() + index_bytes_, 0, 0, grouping.members.size(), 0};
        for (std::size_t g = 0; g < grouping.members.size(); ++g) {
            const auto k = key(grouping.members[g], plan.d[g], plan.t[g]);
            auto it = groups_.find(k);
            if (it == groups_.end()) {
                auto encoded =
                    encode_group(files_, grouping.members[g], plan.d[g], plan.t[g], false);
                if (!encoded) {
                    return std::unexpected(encoded.error());
                }
                ++group_evaluations_;
                it = groups_
                         .emplace(k, Summary{encoded->result.dictionary.size(),
                                             encoded->result.data_bytes,
                                             encoded->result.training_failed})
                         .first;
            } else {
                ++group_cache_hits_;
            }
            e.dictionary_bytes += it->second.dictionary;
            e.data_bytes += it->second.data;
            e.training_failures += it->second.training_failed ? 1 : 0;
        }
        e.bytes += group_entry_bytes * e.groups + e.dictionary_bytes + e.data_bytes;
        spent_ += cost;
        ++plan_evaluations_;
        plans_.emplace(plan, e);
        return std::optional<Evaluation>{e};
    }
    std::uint64_t spent() const { return spent_; }
    std::uint64_t budget() const { return budget_; }
    std::string stats_json() const {
        return "\"plan_evaluations\":" + std::to_string(plan_evaluations_) +
               ",\"plan_cache_hits\":" + std::to_string(plan_cache_hits_) +
               ",\"group_evaluations\":" + std::to_string(group_evaluations_) +
               ",\"group_cache_hits\":" + std::to_string(group_cache_hits_) +
               ",\"budget_bytes\":" + std::to_string(budget_) +
               ",\"budget_spent_bytes\":" + std::to_string(spent_);
    }

  private:
    struct Summary {
        std::uint64_t dictionary;
        std::uint64_t data;
        bool training_failed;
    };
    using Key = std::tuple<std::vector<std::uint32_t>, std::uint8_t, std::uint8_t>;
    static Key key(const std::vector<std::uint32_t> &members, std::uint8_t d, std::uint8_t t) {
        return {members, d, t};
    }
    std::uint64_t group_cost(const std::vector<std::uint32_t> &members, std::uint8_t d,
                             std::uint8_t t) const {
        std::uint64_t cost = 0;
        for (const auto m : members) {
            cost += files_[m].data.size();
            if (dict_sizes[d] != 0) {
                cost += std::min<std::uint64_t>(files_[m].data.size(), train_prefix[t]);
            }
        }
        return cost;
    }
    const std::vector<InputFile> &files_;
    std::vector<Named> named_;
    std::uint64_t budget_;
    std::uint64_t spent_ = 0;
    std::uint64_t index_bytes_ = 0;
    std::map<Plan, Evaluation> plans_;
    std::map<Key, Summary> groups_;
    std::uint64_t plan_evaluations_ = 0;
    std::uint64_t plan_cache_hits_ = 0;
    std::uint64_t group_evaluations_ = 0;
    std::uint64_t group_cache_hits_ = 0;
};

// ---- searches ---------------------------------------------------------------------------------
struct SearchState {
    Plan best = start_plan();
    std::uint64_t best_bytes = UINT64_MAX;
    std::uint64_t proposals = 0;
    std::uint64_t restarts = 0;
    std::string trajectory; // [proposal, spent, best_bytes] at each improvement
    bool exhausted = false;
};
constexpr std::uint64_t max_proposals = 20000;

// Evaluates a candidate and updates the global best; returns its size, or nullopt once the
// budget is exhausted (or the proposal limit is reached).
std::expected<std::optional<std::uint64_t>, Error> try_plan(Evaluator &ev, SearchState &s,
                                                            const Plan &p) {
    if (s.proposals >= max_proposals) {
        s.exhausted = true;
        return std::optional<std::uint64_t>{};
    }
    ++s.proposals;
    auto r = ev.evaluate(p);
    if (!r) {
        return std::unexpected(r.error());
    }
    if (!*r) {
        s.exhausted = true;
        return std::optional<std::uint64_t>{};
    }
    const auto bytes = (*r)->bytes;
    if (bytes < s.best_bytes) {
        s.best_bytes = bytes;
        s.best = p;
        s.trajectory += (s.trajectory.empty() ? "[" : ",[") + std::to_string(s.proposals) + "," +
                        std::to_string(ev.spent()) + "," + std::to_string(bytes) + "]";
    }
    return std::optional<std::uint64_t>{bytes};
}

std::expected<void, Error> search_random(Evaluator &ev, SearchState &s, Rng &rng) {
    if (auto r = try_plan(ev, s, start_plan()); !r || !*r) {
        return r ? std::expected<void, Error>{} : std::unexpected(r.error());
    }
    while (!s.exhausted) {
        if (auto r = try_plan(ev, s, random_plan(rng)); !r) {
            return std::unexpected(r.error());
        }
    }
    return {};
}

// Coordinate search: in a shuffled field order, try every other value of the field and move to
// the best improvement; a full pass without improvement restarts from a random plan.
std::expected<void, Error> search_coord(Evaluator &ev, SearchState &s, Rng &rng) {
    Plan current = start_plan();
    auto first = try_plan(ev, s, current);
    if (!first || !*first) {
        return first ? std::expected<void, Error>{} : std::unexpected(first.error());
    }
    std::uint64_t current_bytes = **first;
    while (!s.exhausted) {
        auto fields = active_fields(current);
        for (std::size_t i = fields.size(); i > 1; --i) {
            std::swap(fields[i - 1], fields[rng.below(i)]);
        }
        bool improved = false;
        for (const auto field : fields) {
            if (field.id >= 2 + 2 * current.k) {
                continue; // k shrank earlier in this pass
            }
            Plan best_here = current;
            std::uint64_t best_here_bytes = current_bytes;
            for (int v = 0; v < field.values && !s.exhausted; ++v) {
                if (v == field_value(current, field.id)) {
                    continue;
                }
                const auto candidate = with_field(current, field.id, v);
                if (candidate == current) {
                    continue;
                }
                auto r = try_plan(ev, s, candidate);
                if (!r) {
                    return std::unexpected(r.error());
                }
                if (*r && **r < best_here_bytes) {
                    best_here = candidate;
                    best_here_bytes = **r;
                }
            }
            if (best_here_bytes < current_bytes) {
                current = best_here;
                current_bytes = best_here_bytes;
                improved = true;
            }
            if (s.exhausted) {
                return {};
            }
        }
        if (!improved) {
            ++s.restarts;
            current = random_plan(rng);
            auto r = try_plan(ev, s, current);
            if (!r) {
                return std::unexpected(r.error());
            }
            if (!*r) {
                return {};
            }
            current_bytes = **r;
        }
    }
    return {};
}

// Structured Seed: the plan is its Seed fields; a neighbour changes one active field to another
// random value (first-improvement hill climbing); `stall` proposals without improvement restart
// from a random Seed.
std::expected<void, Error> search_seed(Evaluator &ev, SearchState &s, Rng &rng, int stall) {
    Plan current = start_plan();
    auto first = try_plan(ev, s, current);
    if (!first || !*first) {
        return first ? std::expected<void, Error>{} : std::unexpected(first.error());
    }
    std::uint64_t current_bytes = **first;
    int without = 0;
    while (!s.exhausted) {
        const auto fields = active_fields(current);
        const auto field = fields[rng.below(fields.size())];
        auto v = static_cast<int>(rng.below(static_cast<std::uint64_t>(field.values - 1)));
        if (v >= field_value(current, field.id)) {
            ++v; // a different value
        }
        const auto candidate = with_field(current, field.id, v);
        if (candidate != current) {
            auto r = try_plan(ev, s, candidate);
            if (!r) {
                return std::unexpected(r.error());
            }
            if (!*r) {
                return {};
            }
            if (**r < current_bytes) {
                current = candidate;
                current_bytes = **r;
                without = 0;
                continue;
            }
        }
        if (++without >= stall) {
            ++s.restarts;
            without = 0;
            current = random_plan(rng);
            auto r = try_plan(ev, s, current);
            if (!r) {
                return std::unexpected(r.error());
            }
            if (!*r) {
                return {};
            }
            current_bytes = **r;
        }
    }
    return {};
}

// ---- container v2 -----------------------------------------------------------------------------
struct Built {
    std::vector<std::byte> bytes;
    std::uint64_t dictionary_bytes = 0, group_table = 0, index = 0, data = 0;
    std::size_t groups = 0;
};
std::expected<Built, Error> build(const std::vector<InputFile> &files, const Plan &plan,
                                  bool store_seed) {
    const auto grouping = make_grouping(plan, named(files));
    std::vector<EncodedGroup> encoded;
    for (std::size_t g = 0; g < grouping.members.size(); ++g) {
        auto e = encode_group(files, grouping.members[g], plan.d[g], plan.t[g], true);
        if (!e) {
            return std::unexpected(e.error());
        }
        encoded.push_back(std::move(*e));
    }
    // Per file: where its blocks are inside its group's encoding.
    std::vector<std::size_t> position(files.size());
    for (const auto &members : grouping.members) {
        for (std::size_t j = 0; j < members.size(); ++j) {
            position[members[j]] = j;
        }
    }
    std::vector<std::byte> index, data;
    for (std::size_t i = 0; i < files.size(); ++i) {
        const auto &f = files[i];
        if (f.path.empty() || f.path.size() > max_path) {
            return std::unexpected(Error{ErrorCode::invalid_argument, "bad path length"});
        }
        put(index, f.path.size(), 2);
        for (const auto c : f.path) {
            index.push_back(static_cast<std::byte>(c));
        }
        put(index, f.data.size(), 8);
        const auto &blocks = encoded[grouping.group_of[i]].blocks[position[i]];
        put(index, blocks.size(), 4);
        for (std::size_t b = 0; b < blocks.size(); ++b) {
            const auto raw = std::span{f.data}.subspan(
                b * chunk, std::min<std::size_t>(chunk, f.data.size() - b * chunk));
            put(index, blocks[b].first.size(), 4);
            index.push_back(static_cast<std::byte>(blocks[b].second));
            put(index, seedarc::experimental::crc32c(raw), 4);
            data.insert(data.end(), blocks[b].first.begin(), blocks[b].first.end());
        }
    }
    Built out;
    out.groups = encoded.size();
    out.bytes.resize(magic.size());
    std::memcpy(out.bytes.data(), magic.data(), magic.size());
    put(out.bytes, version, 4);
    put(out.bytes, chunk, 4);
    put(out.bytes, level, 4);
    put(out.bytes, encoded.size(), 4);
    put(out.bytes, files.size(), 4);
    put(out.bytes, index.size(), 4);
    put(out.bytes, store_seed ? seed_field_bytes : 0, 4);
    out.bytes.resize(header_bytes);
    if (store_seed) {
        put(out.bytes, encode_seed(plan), 4);
    }
    for (const auto &e : encoded) {
        put(out.bytes, e.result.dictionary.size(), 4);
        put(out.bytes, seedarc::experimental::crc32c(e.result.dictionary), 4);
    }
    for (const auto &e : encoded) {
        out.bytes.insert(out.bytes.end(), e.result.dictionary.begin(), e.result.dictionary.end());
        out.dictionary_bytes += e.result.dictionary.size();
    }
    for (const auto g : grouping.group_of) {
        out.bytes.push_back(static_cast<std::byte>(g));
    }
    out.bytes.insert(out.bytes.end(), index.begin(), index.end());
    out.bytes.insert(out.bytes.end(), data.begin(), data.end());
    out.group_table = group_entry_bytes * encoded.size() + files.size();
    out.index = index.size();
    out.data = data.size();
    return out;
}

struct Block {
    std::uint32_t raw, stored;
    std::uint8_t kind;
    std::uint32_t crc;
    std::uint64_t offset;
};
struct Entry {
    std::string path;
    std::uint64_t size;
    std::uint8_t group;
    std::vector<Block> blocks;
};
struct Container {
    std::vector<std::vector<std::byte>> dictionaries;
    std::optional<std::uint32_t> seed;
    std::vector<Entry> entries;
    std::size_t data_start = 0;
};

// Bounded, strict parse of container v2.
std::expected<Container, Error> parse(std::span<const std::byte> bytes) {
    if (bytes.size() < header_bytes || std::memcmp(bytes.data(), magic.data(), magic.size()) != 0) {
        return std::unexpected(bad("bad magic"));
    }
    Reader r{bytes, magic.size()};
    const auto ver = r.get(4), ck = r.get(4), lv = r.get(4), groups = r.get(4), count = r.get(4),
               index = r.get(4), seed_bytes = r.get(4), reserved = r.get(4);
    if (!reserved || *ver != version || *ck != chunk || *lv != level || *groups == 0 ||
        *groups > max_groups || *count > max_files || *index > bytes.size() ||
        (*seed_bytes != 0 && *seed_bytes != seed_field_bytes) || *reserved != 0) {
        return std::unexpected(bad("bad header"));
    }
    Container c;
    r.at = header_bytes;
    if (*seed_bytes != 0) {
        const auto s = r.get(4);
        if (!s || !decode_seed(static_cast<std::uint32_t>(*s))) {
            return std::unexpected(bad("bad seed"));
        }
        c.seed = static_cast<std::uint32_t>(*s);
    }
    std::vector<std::pair<std::uint64_t, std::uint64_t>> table; // (size, crc32c)
    for (std::uint64_t g = 0; g < *groups; ++g) {
        const auto n = r.get(4), crc = r.get(4);
        if (!crc || *n > research::max_dictionary_bytes) {
            return std::unexpected(bad("bad group table"));
        }
        table.emplace_back(*n, *crc);
    }
    for (const auto &[n, crc] : table) {
        if (bytes.size() - r.at < n) {
            return std::unexpected(bad("truncated dictionary"));
        }
        c.dictionaries.emplace_back(bytes.begin() + static_cast<std::ptrdiff_t>(r.at),
                                    bytes.begin() + static_cast<std::ptrdiff_t>(r.at + n));
        if (seedarc::experimental::crc32c(c.dictionaries.back()) != crc) {
            return std::unexpected(bad("dictionary checksum mismatch"));
        }
        r.at += static_cast<std::size_t>(n);
    }
    if (bytes.size() - r.at < *count || bytes.size() - r.at - *count < *index) {
        return std::unexpected(bad("truncated tables"));
    }
    std::vector<std::uint8_t> group_of(static_cast<std::size_t>(*count));
    for (auto &g : group_of) {
        g = static_cast<std::uint8_t>(*r.get(1));
        if (g >= *groups) {
            return std::unexpected(bad("bad file group"));
        }
    }
    const auto index_end = r.at + static_cast<std::size_t>(*index);
    Reader ix{bytes.first(index_end), r.at};
    std::uint64_t offset = 0;
    for (std::uint32_t i = 0; i < *count; ++i) {
        const auto length = ix.get(2);
        if (!length || *length == 0 || *length > max_path || index_end - ix.at < *length) {
            return std::unexpected(bad("bad path"));
        }
        Entry e;
        e.path.assign(reinterpret_cast<const char *>(bytes.data() + ix.at),
                      static_cast<std::size_t>(*length));
        ix.at += static_cast<std::size_t>(*length);
        const auto size = ix.get(8), blocks = ix.get(4);
        if (!blocks || *size > max_input || *blocks != (*size + chunk - 1) / chunk) {
            return std::unexpected(bad("bad entry"));
        }
        e.size = *size;
        e.group = group_of[i];
        const bool has_dict = !c.dictionaries[e.group].empty();
        for (std::uint64_t b = 0; b < *blocks; ++b) {
            const auto stored = ix.get(4), kind = ix.get(1), crc = ix.get(4);
            const auto raw =
                static_cast<std::uint32_t>(std::min<std::uint64_t>(chunk, *size - b * chunk));
            if (!crc || *kind > kind_zstd_dict || *stored == 0 || *stored > raw ||
                (*kind == kind_store) != (*stored == raw) ||
                (*kind == kind_zstd_dict && !has_dict) || (*kind == kind_zstd && has_dict)) {
                return std::unexpected(bad("bad block"));
            }
            e.blocks.push_back({raw, static_cast<std::uint32_t>(*stored),
                                static_cast<std::uint8_t>(*kind), static_cast<std::uint32_t>(*crc),
                                offset});
            offset += *stored;
        }
        c.entries.push_back(std::move(e));
    }
    if (ix.at != index_end || bytes.size() - index_end != offset) {
        return std::unexpected(bad("index / data length mismatch"));
    }
    // A stored Seed must describe exactly this grouping (recomputed from paths and sizes) and
    // may give a dictionary only to groups whose D is non-zero.
    if (c.seed) {
        const auto plan = decode_seed(*c.seed);
        if (!plan || canonical(*plan) != *plan) {
            return std::unexpected(bad("non-canonical seed"));
        }
        std::vector<Named> names;
        for (const auto &e : c.entries) {
            names.push_back({e.path, e.size});
        }
        const auto grouping = make_grouping(*plan, names);
        if (grouping.members.size() != c.dictionaries.size()) {
            return std::unexpected(bad("seed group count differs"));
        }
        for (std::size_t i = 0; i < c.entries.size(); ++i) {
            if (grouping.group_of[i] != c.entries[i].group) {
                return std::unexpected(bad("seed grouping differs from the group table"));
            }
        }
        for (std::size_t g = 0; g < c.dictionaries.size(); ++g) {
            if (dict_sizes[plan->d[g]] == 0 && !c.dictionaries[g].empty()) {
                return std::unexpected(bad("dictionary in a group the seed gives none"));
            }
        }
    }
    c.data_start = index_end;
    return c;
}

// Decodes every entry in memory; returns the restored files in container order.
std::expected<std::vector<std::vector<std::byte>>, Error>
decode_all(const Container &c, std::span<const std::byte> bytes) {
    std::vector<std::optional<research::DictionaryDecoder>> dicts;
    for (const auto &d : c.dictionaries) {
        if (d.empty()) {
            dicts.emplace_back();
            continue;
        }
        auto created = research::DictionaryDecoder::create(d);
        if (!created) {
            return std::unexpected(created.error());
        }
        dicts.emplace_back(std::move(*created));
    }
    codec::Decoder plain;
    std::vector<std::vector<std::byte>> out;
    for (const auto &e : c.entries) {
        std::vector<std::byte> file(static_cast<std::size_t>(e.size));
        std::size_t at = 0;
        for (const auto &b : e.blocks) {
            const auto in =
                bytes.subspan(c.data_start + static_cast<std::size_t>(b.offset), b.stored);
            const auto dst = std::span{file}.subspan(at, b.raw);
            if (b.kind == kind_store) {
                std::ranges::copy(in, dst.begin());
            } else if (b.kind == kind_zstd) {
                if (auto r = plain.decode(codec::zstd_id, in, dst); !r) {
                    return std::unexpected(r.error());
                }
            } else if (auto r = dicts[e.group]->decode(in, dst); !r) {
                return std::unexpected(r.error());
            }
            if (seedarc::experimental::crc32c(dst) != b.crc) {
                return std::unexpected(bad("crc mismatch"));
            }
            at += b.raw;
        }
        out.push_back(std::move(file));
    }
    return out;
}

std::expected<void, Error> byte_exact(const std::vector<InputFile> &files, const Container &c,
                                      std::span<const std::byte> bytes) {
    auto decoded = decode_all(c, bytes);
    if (!decoded) {
        return std::unexpected(decoded.error());
    }
    if (decoded->size() != files.size()) {
        return std::unexpected(bad("file count differs from input"));
    }
    for (std::size_t i = 0; i < files.size(); ++i) {
        if (c.entries[i].path != files[i].path || (*decoded)[i] != files[i].data) {
            return std::unexpected(bad("round trip mismatch"));
        }
    }
    return {};
}

std::expected<void, Error> write_new(const fs::path &path, std::span<const std::byte> bytes) {
    if (fs::exists(path)) {
        return std::unexpected(Error{ErrorCode::output_conflict, "output exists"});
    }
    std::ofstream stream(path, std::ios::binary);
    if (!stream.write(reinterpret_cast<const char *>(bytes.data()),
                      static_cast<std::streamsize>(bytes.size())) ||
        !stream.flush()) {
        return std::unexpected(Error{ErrorCode::io_error, "write failed"});
    }
    return {};
}

// Builds, cross-checks against the evaluated size, writes and round-trips the final container.
std::expected<std::string, Error> finish(const std::vector<InputFile> &files, const Plan &plan,
                                         bool store_seed, std::optional<std::uint64_t> expected,
                                         const fs::path &out) {
    const auto w = wall_ms(), cp = cpu_ms();
    auto built = build(files, plan, store_seed);
    if (!built) {
        return std::unexpected(built.error());
    }
    const auto build_wall = wall_ms() - w, build_cpu = cpu_ms() - cp;
    const auto seed_extra = store_seed ? seed_field_bytes : 0;
    if (expected && built->bytes.size() != *expected + seed_extra) {
        return std::unexpected(bad("rebuilt size differs from the evaluated size"));
    }
    if (auto r = write_new(out, built->bytes); !r) {
        return std::unexpected(r.error());
    }
    const auto dw = wall_ms(), dc = cpu_ms();
    auto parsed = parse(built->bytes);
    if (!parsed) {
        return std::unexpected(parsed.error());
    }
    if (auto r = byte_exact(files, *parsed, built->bytes); !r) {
        return std::unexpected(r.error());
    }
    const auto decode_wall = wall_ms() - dw, decode_cpu = cpu_ms() - dc;
    std::uint64_t input = 0;
    for (const auto &f : files) {
        input += f.data.size();
    }
    return "\"plan\":" + plan_json(plan) + ",\"files\":" + std::to_string(files.size()) +
           ",\"input_bytes\":" + std::to_string(input) +
           ",\"container_bytes\":" + std::to_string(built->bytes.size()) +
           ",\"container_bytes_seed16\":" +
           std::to_string(built->bytes.size() + (store_seed ? 16 - seed_field_bytes : 0)) +
           ",\"parts\":{\"header\":" + std::to_string(header_bytes) +
           ",\"seed\":" + std::to_string(seed_extra) +
           ",\"group_table\":" + std::to_string(built->group_table) +
           ",\"dictionaries\":" + std::to_string(built->dictionary_bytes) +
           ",\"index\":" + std::to_string(built->index) +
           ",\"data\":" + std::to_string(built->data) +
           "},\"groups\":" + std::to_string(built->groups) +
           ",\"build_wall_ms\":" + std::to_string(build_wall) +
           ",\"build_cpu_ms\":" + std::to_string(build_cpu) +
           ",\"decode_wall_ms\":" + std::to_string(decode_wall) +
           ",\"decode_cpu_ms\":" + std::to_string(decode_cpu) + ",\"byte_exact\":true";
}

int search(std::string_view method, const fs::path &input, std::uint64_t budget, std::uint64_t seed,
           const fs::path &out, int stall) {
    const auto call_wall = wall_ms(), call_cpu = cpu_ms();
    auto files = read_tree(input);
    if (!files) {
        return fail(files.error());
    }
    Evaluator ev(*files, budget);
    SearchState s;
    Rng rng{seed};
    const auto sw = wall_ms(), sc = cpu_ms();
    std::expected<void, Error> done;
    if (method == "random") {
        done = search_random(ev, s, rng);
    } else if (method == "coord") {
        done = search_coord(ev, s, rng);
    } else {
        done = search_seed(ev, s, rng, stall);
    }
    if (!done) {
        return fail(done.error());
    }
    const auto search_wall = wall_ms() - sw, search_cpu = cpu_ms() - sc;
    if (s.best_bytes == UINT64_MAX) {
        return fail(Error{ErrorCode::resource_limit, "budget too small for the start plan"});
    }
    auto report = finish(*files, s.best, method == "seed", s.best_bytes, out);
    if (!report) {
        return fail(report.error());
    }
    std::cout << "{\"schema\":\"seedarc.g5-study.r1\",\"mode\":\"search\",\"method\":"
              << json_string(method) << ",\"search_seed\":" << seed << ",\"stall\":" << stall
              << ",\"proposals\":" << s.proposals << ",\"restarts\":" << s.restarts << ","
              << ev.stats_json() << ",\"trajectory\":[" << s.trajectory << "]"
              << ",\"search_wall_ms\":" << search_wall << ",\"search_cpu_ms\":" << search_cpu << ","
              << *report << ",\"call_wall_ms\":" << wall_ms() - call_wall
              << ",\"call_cpu_ms\":" << cpu_ms() - call_cpu << "," << peak_memory_json() << "}\n";
    return std::cout ? 0 : 1;
}

int pack(std::uint32_t seed_value, const fs::path &input, const fs::path &out) {
    const auto plan = decode_seed(seed_value);
    if (!plan || canonical(*plan) != *plan) {
        std::cerr << "not a canonical plan\n";
        return 2;
    }
    const auto call_wall = wall_ms(), call_cpu = cpu_ms();
    auto files = read_tree(input);
    if (!files) {
        return fail(files.error());
    }
    auto report = finish(*files, *plan, false, std::nullopt, out);
    if (!report) {
        return fail(report.error());
    }
    std::cout << "{\"schema\":\"seedarc.g5-study.r1\",\"mode\":\"pack\"," << *report
              << ",\"call_wall_ms\":" << wall_ms() - call_wall
              << ",\"call_cpu_ms\":" << cpu_ms() - call_cpu << "," << peak_memory_json() << "}\n";
    return std::cout ? 0 : 1;
}

int verify(const fs::path &container, const fs::path &input) {
    const auto w = wall_ms(), cp = cpu_ms();
    auto bytes = read_file(container);
    if (!bytes) {
        return fail(bytes.error());
    }
    auto c = parse(*bytes);
    if (!c) {
        return fail(c.error());
    }
    auto decoded = decode_all(*c, *bytes);
    if (!decoded) {
        return fail(decoded.error());
    }
    const auto decode_wall = wall_ms() - w, decode_cpu = cpu_ms() - cp;
    auto files = read_tree(input);
    if (!files) {
        return fail(files.error());
    }
    if (auto r = byte_exact(*files, *c, *bytes); !r) {
        return fail(r.error());
    }
    std::cout << "{\"schema\":\"seedarc.g5-study.r1\",\"mode\":\"verify\",\"byte_exact\":true,"
              << "\"files\":" << c->entries.size() << ",\"groups\":" << c->dictionaries.size()
              << ",\"seed\":" << (c->seed ? std::to_string(*c->seed) : "null")
              << ",\"decode_wall_ms\":" << decode_wall << ",\"decode_cpu_ms\":" << decode_cpu << ","
              << peak_memory_json() << "}\n";
    return std::cout ? 0 : 1;
}

// Damage and Seed self-test (used by check_g5.cmake): every flipped byte and truncation of a
// valid container must fail to parse or to restore byte for byte; Seeds round-trip.
int selftest(const fs::path &input) {
    auto files = read_tree(input);
    if (!files) {
        return fail(files.error());
    }
    Rng rng{20261114};
    std::uint64_t seeds = 0;
    for (int i = 0; i < 10000; ++i) {
        const auto p = random_plan(rng);
        const auto back = decode_seed(encode_seed(p));
        if (!back || *back != p || encode_seed(p) >> 29) {
            return fail(bad("seed round trip"));
        }
        ++seeds;
    }
    Plan three = start_plan();
    three.rule = 0;
    three.k = 3;
    three.d = {2, 0, 1};
    three.t = {1, 0, 0};
    std::uint64_t tried = 0, rejected = 0;
    for (const auto &[plan, with_seed] :
         {std::pair{start_plan(), false}, std::pair{canonical(three), true}}) {
        auto built = build(*files, plan, with_seed);
        if (!built) {
            return fail(built.error());
        }
        const auto &good = built->bytes;
        auto parsed = parse(good);
        if (!parsed || !byte_exact(*files, *parsed, good)) {
            return fail(bad("selftest: valid container rejected"));
        }
        std::vector<std::size_t> offsets{0,
                                         8,
                                         12,
                                         16,
                                         20,
                                         24,
                                         28,
                                         32,
                                         36,
                                         header_bytes,
                                         header_bytes + 1,
                                         good.size() / 2,
                                         good.size() - 1};
        for (std::size_t at = 41; at < good.size(); at += good.size() / 40 + 1) {
            offsets.push_back(at);
        }
        const auto damaged_ok = [&](std::span<const std::byte> bytes) {
            auto c = parse(bytes);
            return c && byte_exact(*files, *c, bytes).has_value();
        };
        for (const auto at : offsets) {
            auto copy = good;
            copy[at] ^= std::byte{0x5a};
            ++tried;
            rejected += damaged_ok(copy) ? 0 : 1;
        }
        for (const auto keep :
             {std::size_t{0}, std::size_t{10}, header_bytes, good.size() / 3, good.size() - 1}) {
            ++tried;
            rejected += damaged_ok(std::span{good}.first(keep)) ? 0 : 1;
        }
    }
    std::cout
        << "{\"schema\":\"seedarc.g5-study.r1\",\"mode\":\"selftest\",\"seeds_round_tripped\":"
        << seeds << ",\"damaged_tried\":" << tried << ",\"damaged_rejected\":" << rejected << "}\n";
    return rejected == tried ? 0 : 1;
}

std::optional<std::uint32_t> hex32(std::string_view text) {
    if (text.starts_with("0x")) {
        text.remove_prefix(2);
    }
    if (text.empty() || text.size() > 8) {
        return std::nullopt;
    }
    std::uint32_t v = 0;
    for (const auto c : text) {
        const int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
        if (d < 0) {
            return std::nullopt;
        }
        v = (v << 4) | static_cast<std::uint32_t>(d);
    }
    return v;
}
} // namespace

int main(int argc, char *argv[]) {
    const std::vector<std::string_view> a(argv, argv + argc);
    if ((a.size() == 7 || a.size() == 8) && a[1] == "search" &&
        (a[2] == "random" || a[2] == "coord" || a[2] == "seed")) {
        const auto budget = number(a[4]), seed = number(a[5]);
        const auto stall = a.size() == 8 ? number(a[7]) : std::optional<std::uint64_t>{20};
        if (!budget || !seed || !stall || *stall == 0 || *stall > 10000) {
            std::cerr << usage;
            return 2;
        }
        return search(a[2], native(a[3]), *budget, *seed, native(a[6]), static_cast<int>(*stall));
    }
    if (a.size() == 5 && a[1] == "pack") {
        const auto seed = hex32(a[2]);
        if (!seed) {
            std::cerr << usage;
            return 2;
        }
        return pack(*seed, native(a[3]), native(a[4]));
    }
    if (a.size() == 4 && a[1] == "verify") {
        return verify(native(a[2]), native(a[3]));
    }
    if (a.size() == 3 && a[1] == "selftest") {
        return selftest(native(a[2]));
    }
    std::cerr << usage;
    return 2;
}
