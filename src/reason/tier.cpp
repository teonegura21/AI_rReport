#include <rr/reason/tier.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <format>
#include <limits>
#include <memory>
#include <set>
#include <span>
#include <string>
#include <system_error>
#include <utility>

#include <toml++/toml.hpp>

#include <rr/core/assert.hpp>
#include <rr/core/strings.hpp>
#include <rr/embedded/tiers.hpp>
#include <rr/json/json.hpp>

namespace rr::reason {

namespace {

constexpr std::array<std::string_view, 3> kTiers{"fast", "thorough", "max"};
constexpr std::array<std::string_view, 5> kStages{"outline", "atomize", "plan", "write", "seams"};
constexpr std::array<std::string_view, 2> kPlanModes{"group", "assign"};
constexpr std::array<std::string_view, 3> kSeamsModes{"none", "rewrite", "recheck"};

Error invalid(std::string_view where, std::string_view why) {
    return Error{Errc::invalid_argument, std::format("{}: {}", where, why)};
}

// One section of a tier, read strictly: a key left out takes its default,
// a key not read is unknown.
class Section {
public:
    Section(const toml::table* table, std::string where) : table_(table), where_(std::move(where)) {}

    [[nodiscard]] Result<bool> flag(std::string_view key, bool fallback) {
        const toml::node* value = node(key);
        if (value == nullptr) {
            return fallback;
        }
        if (const auto* flag = value->as_boolean()) {
            return flag->get();
        }
        return wrong(key, "true or false");
    }

    [[nodiscard]] Result<int> count(std::string_view key, int fallback, int at_least) {
        const toml::node* value = node(key);
        if (value == nullptr) {
            return fallback;
        }
        const auto* number = value->as_integer();
        if (number == nullptr) {
            return wrong(key, "a whole number");
        }
        if (number->get() < at_least || number->get() > std::numeric_limits<int>::max()) {
            return wrong(key, std::format("a whole number of at least {}", at_least));
        }
        return static_cast<int>(number->get());
    }

    // A number, written as a decimal or as a whole number; `fallback` when
    // absent. `what` names what it must be.
    [[nodiscard]] Result<double> number(std::string_view key, double fallback, std::string_view what) {
        const toml::node* value = node(key);
        if (value == nullptr) {
            return fallback;
        }
        if (const auto* real = value->as_floating_point()) {
            return real->get();
        }
        if (const auto* whole = value->as_integer()) {
            return static_cast<double>(whole->get());
        }
        return wrong(key, what);
    }

    // A share of votes or outlines: none is no share.
    [[nodiscard]] Result<double> share(std::string_view key, double fallback) {
        RR_TRY_ASSIGN(const double share, number(key, fallback, "a share in (0, 1]"));
        if (std::isnan(share) || share <= 0.0 || share > 1.0) {
            return wrong(key, "a share in (0, 1]");
        }
        return share;
    }

    // A probability: unlike a share, 0 is one.
    [[nodiscard]] Result<double> probability(std::string_view key, double fallback) {
        RR_TRY_ASSIGN(const double probability, number(key, fallback, "a probability in [0, 1]"));
        if (std::isnan(probability) || probability < 0.0 || probability > 1.0) {
            return wrong(key, "a probability in [0, 1]");
        }
        return probability;
    }

    [[nodiscard]] Result<std::string> choice(std::string_view key, std::string_view fallback,
                                             std::span<const std::string_view> allowed) {
        const toml::node* value = node(key);
        if (value == nullptr) {
            return std::string{fallback};
        }
        const auto* text = value->as_string();
        if (text == nullptr || std::ranges::find(allowed, std::string_view{text->get()}) == allowed.end()) {
            return wrong(key, std::format("one of: {}", join(allowed, ", ")));
        }
        return text->get();
    }

    // Errc::invalid_argument for a key no reader asked for.
    [[nodiscard]] Result<void> nothing_else() const {
        if (table_ == nullptr) {
            return ok();
        }
        for (const auto& [key, value] : *table_) {
            if (!read_.contains(key.str())) {
                return std::unexpected(invalid(where_, std::format("unknown key '{}'", key.str())));
            }
        }
        return ok();
    }

    [[nodiscard]] const std::string& where() const noexcept { return where_; }

private:
    [[nodiscard]] const toml::node* node(std::string_view key) {
        read_.emplace(key);
        return table_ == nullptr ? nullptr : table_->get(key);
    }

    [[nodiscard]] std::unexpected<Error> wrong(std::string_view key, std::string_view expected) const {
        return std::unexpected(invalid(where_, std::format("'{}' must be {}", key, expected)));
    }

    const toml::table* table_;  // null: the section is left out, and every key defaults
    std::string where_;
    std::set<std::string, std::less<>> read_;
};

Result<OutlineCompute> read_outline(Section& section) {
    const OutlineCompute defaults;
    OutlineCompute outline;
    RR_TRY_ASSIGN(outline.chapters, section.flag("chapters", defaults.chapters));
    RR_TRY_ASSIGN(outline.samples, section.count("samples", defaults.samples, 1));
    RR_TRY_ASSIGN(outline.boundary_tolerance,
                  section.count("boundary_tolerance", defaults.boundary_tolerance, 0));
    RR_TRY_ASSIGN(outline.quorum, section.share("quorum", defaults.quorum));
    RR_TRY_ASSIGN(outline.thinking_budget, section.count("thinking_budget", defaults.thinking_budget, 1));
    RR_TRY(section.nothing_else());
    return outline;
}

Result<AtomizeCompute> read_atomize(Section& section) {
    const AtomizeCompute defaults;
    AtomizeCompute atomize;
    RR_TRY_ASSIGN(atomize.waves.first, section.count("first", defaults.waves.first, 1));
    RR_TRY_ASSIGN(atomize.waves.step, section.count("step", defaults.waves.step, 0));
    RR_TRY_ASSIGN(atomize.waves.max, section.count("max", defaults.waves.max, 1));
    RR_TRY_ASSIGN(atomize.waves.quorum, section.share("quorum", defaults.waves.quorum));
    RR_TRY_ASSIGN(atomize.waves.sure, section.probability("sure", defaults.waves.sure));
    RR_TRY_ASSIGN(atomize.thinking, section.flag("thinking", defaults.thinking));
    RR_TRY_ASSIGN(atomize.repairs, section.count("repairs", defaults.repairs, 0));
    RR_TRY_ASSIGN(atomize.recall_pass, section.flag("recall_pass", defaults.recall_pass));
    RR_TRY_ASSIGN(atomize.contested_to_commander,
                  section.flag("contested_to_commander", defaults.contested_to_commander));
    RR_TRY(section.nothing_else());
    if (const std::vector<std::string> problems = waves_problems(atomize.waves); !problems.empty()) {
        return std::unexpected(invalid(section.where(), join(problems, "; ")));
    }
    return atomize;
}

Result<PlanCompute> read_plan(Section& section) {
    const PlanCompute defaults;
    PlanCompute plan;
    RR_TRY_ASSIGN(const std::string mode, section.choice("mode", "group", kPlanModes));
    plan.mode = mode == "assign" ? PlanMode::assign : PlanMode::group;
    RR_TRY_ASSIGN(plan.thinking_budget, section.count("thinking_budget", defaults.thinking_budget, 1));
    RR_TRY(section.nothing_else());
    return plan;
}

Result<WriteCompute> read_write(Section& section) {
    const WriteCompute defaults;
    WriteCompute write;
    RR_TRY_ASSIGN(write.drafts, section.count("drafts", defaults.drafts, 1));
    RR_TRY_ASSIGN(write.fuse, section.flag("fuse", defaults.fuse));
    RR_TRY_ASSIGN(write.thinking, section.flag("thinking", defaults.thinking));
    RR_TRY_ASSIGN(write.judge_top, section.count("judge_top", defaults.judge_top, 0));
    RR_TRY_ASSIGN(write.repair_rounds, section.count("repair_rounds", defaults.repair_rounds, 0));
    RR_TRY(section.nothing_else());
    // A judge compares drafts: none, or two and more of those there are.
    if (write.judge_top == 1 || write.judge_top > write.drafts) {
        return std::unexpected(
            invalid(section.where(), std::format("'judge_top' is 0 or from 2 to the drafts ({}), not {}",
                                                 write.drafts, write.judge_top)));
    }
    // A fusion is written from the drafts that pass: one alone has nothing to fuse.
    if (write.fuse && write.drafts < 2) {
        return std::unexpected(invalid(
            section.where(), std::format("'fuse' needs 2 drafts or more to fuse, not {}", write.drafts)));
    }
    return write;
}

Result<SeamsCompute> read_seams(Section& section) {
    SeamsCompute seams;
    RR_TRY_ASSIGN(const std::string mode, section.choice("mode", "none", kSeamsModes));
    seams.mode = SeamsMode::none;
    if (mode == "rewrite") {
        seams.mode = SeamsMode::rewrite;
    } else if (mode == "recheck") {
        seams.mode = SeamsMode::recheck;
    }
    RR_TRY(section.nothing_else());
    return seams;
}

// A tier's sections read strictly, each message naming `name`.
Result<Tier> read_tier(std::string_view name, const toml::table& document) {
    const std::string where = std::format("tier '{}'", name);
    for (const auto& [key, value] : document) {
        if (std::ranges::find(kStages, key.str()) == kStages.end()) {
            return std::unexpected(invalid(where, std::format("unknown section [{}]", key.str())));
        }
        if (!value.is_table()) {
            return std::unexpected(
                invalid(where, std::format("'{}' must be a section, [{}]", key.str(), key.str())));
        }
    }
    const auto section = [&](std::string_view stage) {
        return Section{document[stage].as_table(), std::format("{}, [{}]", where, stage)};
    };

    Tier tier{.name = std::string{name}};
    Section outline = section("outline");
    RR_TRY_ASSIGN(tier.outline, read_outline(outline));
    Section atomize = section("atomize");
    RR_TRY_ASSIGN(tier.atomize, read_atomize(atomize));
    Section plan = section("plan");
    RR_TRY_ASSIGN(tier.plan, read_plan(plan));
    Section write = section("write");
    RR_TRY_ASSIGN(tier.write, read_write(write));
    Section seams = section("seams");
    RR_TRY_ASSIGN(tier.seams, read_seams(seams));
    return tier;
}

Result<toml::table> parsed(std::string_view name, std::string_view toml) {
    const std::string where = std::format("tier '{}'", name);
    try {
        return toml::parse(toml, where);
    } catch (const toml::parse_error& error) {
        return std::unexpected(
            invalid(where, std::format("line {}: {}", error.source().begin.line, error.description())));
    }
}

// A shipped tier's file, as compiled in.
Result<std::string_view> builtin_text(std::string_view name) {
    if (std::ranges::find(kTiers, name) == kTiers.end()) {
        return fail(Errc::not_found,
                    std::format("there is no tier '{}': the tiers are {}", name, join(kTiers, ", ")));
    }
    const std::string file = std::format("config/tiers/{}.toml", name);
    const std::optional<std::string_view> text = embedded::tiers::find(file);
    if (!text.has_value()) {
        return fail(Errc::internal, std::format("{} is not compiled in", file));
    }
    return *text;
}

// Sets `key` of `section` to `text` as TOML reads a value: true or false, a
// whole number, a number, or else a word.
void set_value(toml::table& section, std::string_view key, std::string_view text) {
    if (text == "true" || text == "false") {
        section.insert_or_assign(key, text == "true");
        return;
    }
    // from_chars reads the range [first, last), never past it.
    const char* const first = std::to_address(text.begin());
    const char* const last = std::to_address(text.end());
    std::int64_t whole = 0;
    if (const auto [stop, error] = std::from_chars(first, last, whole);
        error == std::errc{} && stop == last) {
        section.insert_or_assign(key, whole);
        return;
    }
    double number = 0;
    if (const auto [stop, error] = std::from_chars(first, last, number);
        error == std::errc{} && stop == last) {
        section.insert_or_assign(key, number);
        return;
    }
    section.insert_or_assign(key, std::string{text});
}

// One change of a variant's name, `stage.key=value`, set in `document`;
// `changed` keeps the keys changed so far.
Result<void> apply_change(std::string_view name, std::string_view change, toml::table& document,
                          std::set<std::string, std::less<>>& changed) {
    const std::string where = std::format("tier '{}'", name);
    const std::size_t equals = change.find('=');
    const std::size_t dot = change.find('.');
    if (equals == std::string_view::npos || dot == std::string_view::npos || dot == 0 || dot + 1 >= equals ||
        equals + 1 == change.size()) {
        return std::unexpected(invalid(where, std::format("'{}' is not stage.key=value", change)));
    }
    const std::string_view stage = change.substr(0, dot);
    if (std::ranges::find(kStages, stage) == kStages.end()) {
        return std::unexpected(invalid(where, std::format("unknown section [{}]", stage)));
    }
    const std::string_view key = change.substr(dot + 1, equals - dot - 1);
    if (!changed.emplace(change.substr(0, equals)).second) {
        return std::unexpected(
            invalid(where, std::format("'{}' is changed twice", change.substr(0, equals))));
    }
    toml::table* const section = document[stage].as_table();
    RR_ASSERT(section != nullptr, "a shipped tier writes out every section (tier_test)");
    set_value(*section, key, change.substr(equals + 1));
    return ok();
}

std::string_view to_string(PlanMode mode) noexcept {
    return mode == PlanMode::assign ? "assign" : "group";
}

std::string_view to_string(SeamsMode mode) noexcept {
    switch (mode) {
        case SeamsMode::none:
            return "none";
        case SeamsMode::rewrite:
            return "rewrite";
        case SeamsMode::recheck:
            return "recheck";
    }
    return "none";
}

}  // namespace

std::string_view to_string(Stage stage) noexcept {
    return kStages[static_cast<std::size_t>(stage)];
}

std::string Tier::canonical(Stage stage) const {
    json::Json settings = json::Json::object();
    switch (stage) {
        case Stage::outline:
            settings = {{"chapters", outline.chapters},
                        {"samples", outline.samples},
                        {"boundary_tolerance", outline.boundary_tolerance},
                        {"quorum", outline.quorum},
                        {"thinking_budget", outline.thinking_budget}};
            break;
        case Stage::atomize:
            settings = {{"first", atomize.waves.first},
                        {"step", atomize.waves.step},
                        {"max", atomize.waves.max},
                        {"quorum", atomize.waves.quorum},
                        {"sure", atomize.waves.sure},
                        {"thinking", atomize.thinking},
                        {"repairs", atomize.repairs},
                        {"recall_pass", atomize.recall_pass},
                        {"contested_to_commander", atomize.contested_to_commander}};
            break;
        case Stage::plan:
            settings = {{"mode", to_string(plan.mode)}, {"thinking_budget", plan.thinking_budget}};
            break;
        case Stage::write:
            settings = {{"drafts", write.drafts},
                        {"fuse", write.fuse},
                        {"thinking", write.thinking},
                        {"judge_top", write.judge_top},
                        {"repair_rounds", write.repair_rounds}};
            break;
        case Stage::seams:
            settings = {{"mode", to_string(seams.mode)}};
            break;
    }
    return json::dump(settings);
}

Result<Tier> parse_tier(std::string_view name, std::string_view toml) {
    RR_TRY_ASSIGN(const toml::table document, parsed(name, toml));
    return read_tier(name, document);
}

std::vector<std::string> builtin_tiers() {
    return {kTiers.begin(), kTiers.end()};
}

Result<Tier> builtin_tier(std::string_view name) {
    RR_TRY_ASSIGN(const std::string_view text, builtin_text(name));
    return parse_tier(name, text);
}

Result<Tier> find_tier(std::string_view name) {
    const std::size_t plus = name.find('+');
    if (plus == std::string_view::npos) {
        return builtin_tier(name);
    }
    RR_TRY_ASSIGN(const std::string_view text, builtin_text(name.substr(0, plus)));
    RR_TRY_ASSIGN(toml::table document, parsed(name.substr(0, plus), text));
    std::set<std::string, std::less<>> changed;
    for (const std::string_view change : split(name.substr(plus + 1), '+')) {
        RR_TRY(apply_change(name, change, document, changed));
    }
    return read_tier(name, document);
}

}  // namespace rr::reason
