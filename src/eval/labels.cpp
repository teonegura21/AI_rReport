// A person's verdicts on what the eval's judges read (rr/eval/labels.hpp).
#include <rr/eval/labels.hpp>

#include <algorithm>
#include <array>
#include <format>
#include <numeric>
#include <random>
#include <utility>

#include <rr/core/error.hpp>
#include <rr/core/file_io.hpp>
#include <rr/eval/set.hpp>
#include <rr/eval/stats.hpp>

#include "toml_io.hpp"

namespace rr::eval {

namespace {

constexpr std::array<std::string_view, 3> kFactVerdicts{"told", "partly", "not_told"};
constexpr std::array<std::string_view, 6> kIssues{"none",        "adds",          "softens",
                                                  "intensifies", "misattributes", "contradicts"};
constexpr std::array<std::string_view, 3> kPreferences{"first", "second", "tie"};
constexpr std::array<std::string_view, 2> kOrders{"ab", "ba"};

std::string_view to_string(reason::Preference preference) noexcept {
    switch (preference) {
        case reason::Preference::first:
            return "first";
        case reason::Preference::second:
            return "second";
        case reason::Preference::tie:
            break;
    }
    return "tie";
}

// A list of texts, each on its own line, as a person reads it.
std::string texts(const std::vector<std::string>& lines) {
    if (lines.empty()) {
        return "[]";
    }
    std::string written = "[\n";
    for (const std::string& line : lines) {
        written += std::format("    {},\n", detail::toml_string(line));
    }
    return written + "]";
}

// An optional verdict as the file has it: empty when none was given.
std::string verdict_text(std::optional<std::string_view> verdict) {
    return detail::toml_string(verdict.value_or(std::string_view{}));
}

std::optional<reason::Preference> preference_from_string(std::string_view name) noexcept {
    if (name == "first") {
        return reason::Preference::first;
    }
    if (name == "second") {
        return reason::Preference::second;
    }
    if (name == "tie") {
        return reason::Preference::tie;
    }
    return std::nullopt;
}

// The verdict of `table`, read by `from_string`: none when left empty, and
// Errc::invalid_argument, naming `allowed`, for anything else.
template <typename Answer>
Result<std::optional<Answer>> verdict_of(detail::Table& table, std::span<const std::string_view> allowed,
                                         std::optional<Answer> (*from_string)(std::string_view) noexcept) {
    RR_TRY_ASSIGN(const std::optional<std::string> verdict, table.optional_text("verdict"));
    if (!verdict.has_value()) {
        return std::optional<Answer>{};
    }
    const std::optional<Answer> answer = from_string(*verdict);
    if (!answer.has_value()) {
        return fail(Errc::invalid_argument,
                    std::format(R"({}: 'verdict' must be empty or one of: {}, not "{}")", table.where(),
                                join(allowed, ", "), *verdict));
    }
    return answer;
}

Result<int> replicate_of(detail::Table& table) {
    RR_TRY_ASSIGN(const std::int64_t replicate, table.integer("replicate", 0, 1'000));
    return static_cast<int>(replicate);
}

Result<FactLabel> parse_fact(detail::Table& table) {
    FactLabel label;
    RR_TRY_ASSIGN(label.job, table.text("job"));
    RR_TRY_ASSIGN(label.video, table.text("video"));
    RR_TRY_ASSIGN(label.replicate, replicate_of(table));
    RR_TRY_ASSIGN(label.fact, table.text("fact"));
    RR_TRY_ASSIGN(label.core, table.boolean("core"));
    RR_TRY_ASSIGN(label.said, table.text("said"));
    RR_TRY_ASSIGN(label.claim, table.text("claim"));
    RR_TRY_ASSIGN(label.quote, table.text("quote"));
    RR_TRY_ASSIGN(label.shown, table.texts("shown"));
    RR_TRY_ASSIGN(label.verdict, verdict_of(table, kFactVerdicts, &fact_verdict_from_string));
    return label;
}

Result<SentenceLabel> parse_sentence(detail::Table& table) {
    SentenceLabel label;
    RR_TRY_ASSIGN(label.job, table.text("job"));
    RR_TRY_ASSIGN(label.video, table.text("video"));
    RR_TRY_ASSIGN(label.replicate, replicate_of(table));
    RR_TRY_ASSIGN(const std::int64_t number, table.integer("number", 1, 1'000'000));
    label.number = static_cast<std::size_t>(number);
    RR_TRY_ASSIGN(label.text, table.text("text"));
    RR_TRY_ASSIGN(label.evidence, table.texts("evidence"));
    RR_TRY_ASSIGN(label.verdict, verdict_of(table, kIssues, &pipeline::issue_from_string));
    return label;
}

Result<PartLabel> parse_part(detail::Table& table) {
    PartLabel label;
    RR_TRY_ASSIGN(label.job, table.text("job"));
    RR_TRY_ASSIGN(label.video, table.text("video"));
    RR_TRY_ASSIGN(label.replicate, replicate_of(table));
    RR_TRY_ASSIGN(const std::int64_t part, table.integer("part", 0, 100'000));
    label.part = static_cast<std::size_t>(part);
    RR_TRY_ASSIGN(label.stretch, table.text("stretch"));
    RR_TRY_ASSIGN(const std::string order, table.choice("order", kOrders));
    label.b_first = order == "ba";
    RR_TRY_ASSIGN(label.first, table.text("first"));
    RR_TRY_ASSIGN(label.second, table.text("second"));
    RR_TRY_ASSIGN(label.verdict, verdict_of(table, kPreferences, &preference_from_string));
    return label;
}

}  // namespace

std::optional<reason::Winner> PartLabel::winner() const noexcept {
    if (!verdict.has_value()) {
        return std::nullopt;
    }
    if (*verdict == reason::Preference::tie) {
        return reason::Winner::tie;
    }
    // The version shown first is A's, unless B's was.
    const bool first_preferred = *verdict == reason::Preference::first;
    return first_preferred != b_first ? reason::Winner::a : reason::Winner::b;
}

bool Labels::any_verdict() const noexcept {
    return std::ranges::any_of(facts, [](const FactLabel& label) { return label.verdict.has_value(); }) ||
           std::ranges::any_of(sentences,
                               [](const SentenceLabel& label) { return label.verdict.has_value(); }) ||
           std::ranges::any_of(parts, [](const PartLabel& label) { return label.verdict.has_value(); });
}

std::filesystem::path labels_file(const std::filesystem::path& eval_root, std::string_view name) {
    return eval_root / "labels" / std::format("{}.toml", name);
}

std::string to_toml(const Labels& labels) {
    std::string written = std::format(
        "# Labels that calibrate the panel {} (rr/eval/labels.hpp, runbook R5): the\n"
        "# panel's own questions, asked of you, about what it read of run {}.\n"
        "# Read what each item shows, the same as the judges read, and set its verdict:\n"
        "#   * a fact: is it told by the article's sentences shown? told, partly or\n"
        "#     not_told;\n"
        "#   * a sentence: does the video's evidence shown support it? none (it says\n"
        "#     what the evidence says, and only that), adds, softens, intensifies,\n"
        "#     misattributes or contradicts;\n"
        "#   * a part: which version of the stretch reads better, as a piece of\n"
        "#     writing, both telling the same things? first, second or tie.\n"
        "# An empty verdict skips its item. When you are done, set labeller to your\n"
        "# name, and run: rreport eval calibrate {}\n\n"
        "name = {}\npanel = {}\nrun = {}\nagainst = {}\nlabeller = {}\n",
        labels.panel, labels.run, labels.name, detail::toml_string(labels.name),
        detail::toml_string(labels.panel), detail::toml_string(labels.run),
        detail::toml_string(labels.against), detail::toml_string(labels.labeller));
    for (const FactLabel& label : labels.facts) {
        written += std::format(
            "\n[[fact]]\njob = {}\nvideo = {}\nreplicate = {}\nfact = {}\ncore = {}\nsaid = {}\nclaim = {}\n"
            "quote = {}\nshown = {}\nverdict = {}  # told | partly | not_told\n",
            detail::toml_string(label.job), detail::toml_string(label.video), label.replicate,
            detail::toml_string(label.fact), label.core, detail::toml_string(label.said),
            detail::toml_string(label.claim), detail::toml_string(label.quote), texts(label.shown),
            verdict_text(label.verdict.has_value() ? std::optional{to_string(*label.verdict)}
                                                   : std::nullopt));
    }
    for (const SentenceLabel& label : labels.sentences) {
        written += std::format(
            "\n[[sentence]]\njob = {}\nvideo = {}\nreplicate = {}\nnumber = {}\ntext = {}\nevidence = {}\n"
            "verdict = {}  # none | adds | softens | intensifies | misattributes | contradicts\n",
            detail::toml_string(label.job), detail::toml_string(label.video), label.replicate, label.number,
            detail::toml_string(label.text), texts(label.evidence),
            verdict_text(label.verdict.has_value() ? std::optional{pipeline::to_string(*label.verdict)}
                                                   : std::nullopt));
    }
    for (const PartLabel& label : labels.parts) {
        written += std::format(
            "\n[[part]]\njob = {}\nvideo = {}\nreplicate = {}\npart = {}\nstretch = {}\norder = {}\n"
            "first = {}\nsecond = {}\nverdict = {}  # first | second | tie\n",
            detail::toml_string(label.job), detail::toml_string(label.video), label.replicate, label.part,
            detail::toml_string(label.stretch), detail::toml_string(label.b_first ? "ba" : "ab"),
            detail::toml_string(label.first), detail::toml_string(label.second),
            verdict_text(label.verdict.has_value() ? std::optional{to_string(*label.verdict)}
                                                   : std::nullopt));
    }
    return written;
}

Result<Labels> parse_labels(std::string_view name, std::string_view text) {
    const std::string file = std::format("eval/labels/{}.toml", name);
    RR_TRY_ASSIGN(const toml::table document, detail::parse_toml(text, file));
    detail::Table top{document, file};
    Labels labels;
    RR_TRY_ASSIGN(labels.name, top.text("name"));
    if (labels.name != name) {
        return fail(Errc::invalid_argument,
                    std::format(R"({}: 'name' must be the file's, "{}", not "{}")", file, name, labels.name));
    }
    RR_TRY_ASSIGN(labels.panel, top.text("panel"));
    RR_TRY_ASSIGN(labels.run, top.text("run"));
    RR_TRY_ASSIGN(const std::optional<std::string> against, top.optional_text("against"));
    labels.against = against.value_or(std::string{});
    RR_TRY_ASSIGN(const std::optional<std::string> labeller, top.optional_text("labeller"));
    labels.labeller = labeller.value_or(std::string{});
    top.allow("fact");  // read table by table below
    top.allow("sentence");
    top.allow("part");
    RR_TRY(top.nothing_else());
    RR_TRY(detail::each_table(document, "fact", file, [&](detail::Table& table) -> Result<void> {
        RR_TRY_ASSIGN(FactLabel label, parse_fact(table));
        labels.facts.push_back(std::move(label));
        return ok();
    }));
    RR_TRY(detail::each_table(document, "sentence", file, [&](detail::Table& table) -> Result<void> {
        RR_TRY_ASSIGN(SentenceLabel label, parse_sentence(table));
        labels.sentences.push_back(std::move(label));
        return ok();
    }));
    RR_TRY(detail::each_table(document, "part", file, [&](detail::Table& table) -> Result<void> {
        RR_TRY_ASSIGN(PartLabel label, parse_part(table));
        labels.parts.push_back(std::move(label));
        return ok();
    }));
    return labels;
}

Result<Labels> read_labels(const std::filesystem::path& eval_root, std::string_view name) {
    if (!valid_video_id(name)) {
        return fail(Errc::invalid_argument,
                    std::format("'{}' is no labels' name: lowercase letters, digits and hyphens", name));
    }
    RR_TRY_ASSIGN(const std::string text, read_file(labels_file(eval_root, name)));
    return parse_labels(name, text);
}

std::vector<std::size_t> draw(std::size_t population, std::size_t count, std::string_view name) {
    std::vector<std::size_t> indices(population);
    std::ranges::iota(indices, std::size_t{0});
    if (count >= population) {
        return indices;
    }
    // A partial Fisher–Yates shuffle: the first `count` places drawn from
    // those left, each by the generator's own number modulo their count, as
    // the bootstrap draws, so that every machine draws the same.
    std::mt19937_64 generator{seed_of(name)};
    for (std::size_t place = 0; place < count; ++place) {
        const std::size_t left = population - place;
        std::swap(indices[place], indices[place + static_cast<std::size_t>(generator() % left)]);
    }
    indices.resize(count);
    std::ranges::sort(indices);
    return indices;
}

bool b_first(std::string_view name, std::string_view video, int replicate, std::size_t part) {
    std::mt19937_64 generator{seed_of(std::format("{}/{}/{}/{}", name, video, replicate, part))};
    return (generator() >> 63U) == 1;  // the top bit: FNV-1a's lowest bits are its least mixed
}

}  // namespace rr::eval
