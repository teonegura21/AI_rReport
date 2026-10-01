// What a video says that an article must not lose (rr/eval/gold.hpp).
#include <rr/eval/gold.hpp>

#include <algorithm>
#include <array>
#include <format>
#include <set>
#include <utility>

#include <rr/eval/set.hpp>
#include <rr/text/align.hpp>

#include "toml_io.hpp"

namespace rr::eval {

namespace {

constexpr std::array<std::string_view, 2> kImportance{"core", "detail"};
constexpr std::array<std::string_view, 2> kStatus{"silver", "gold"};
constexpr std::size_t kQuoted = 60;  // of a quote, in a message

// "F1", "F2", …: F and a number without a leading zero.
bool fact_id(std::string_view id) {
    return id.size() >= 2 && id.size() <= 10 && id[0] == 'F' && id[1] != '0' &&
           id.substr(1).find_first_not_of("0123456789") == std::string_view::npos;
}

// The start of a quote, for a message.
std::string shown(std::string_view quote) {
    if (quote.size() <= kQuoted) {
        return std::string{quote};
    }
    std::size_t cut = kQuoted;
    while (cut > 0 && (static_cast<unsigned char>(quote[cut]) & 0xC0U) == 0x80U) {
        --cut;  // never inside a character
    }
    return std::string{quote.substr(0, cut)} + "\u2026";
}

Result<Fact> read_fact(detail::Table& table) {
    Fact fact;
    RR_TRY_ASSIGN(fact.id, table.text("id"));
    if (!fact_id(fact.id)) {
        return fail(Errc::invalid_argument, std::format("{}: id '{}' must be F and a number: F1, F2, \u2026",
                                                        table.where(), fact.id));
    }
    RR_TRY_ASSIGN(fact.start, table.time("start"));
    RR_TRY_ASSIGN(fact.end, table.time("end"));
    RR_TRY_ASSIGN(const std::string importance, table.choice("importance", kImportance));
    fact.importance = importance == "core" ? Importance::core : Importance::detail;
    RR_TRY_ASSIGN(fact.quote, table.text("quote"));
    RR_TRY_ASSIGN(fact.claim, table.text("claim"));
    RR_TRY_ASSIGN(const std::optional<std::int64_t> found, table.optional_integer("found", 0, 99));
    fact.found = static_cast<int>(found.value_or(0));
    return fact;
}

Result<MustQuote> read_must_quote(detail::Table& table) {
    MustQuote quote;
    RR_TRY_ASSIGN(quote.start, table.time("start"));
    RR_TRY_ASSIGN(quote.end, table.time("end"));
    RR_TRY_ASSIGN(quote.quote, table.text("quote"));
    return quote;
}

Result<Term> read_term(detail::Table& table) {
    Term term;
    RR_TRY_ASSIGN(term.term, table.text("term"));
    RR_TRY_ASSIGN(const std::optional<std::string> meaning, table.optional_text("meaning"));
    term.meaning = meaning.value_or("");
    return term;
}

// Where an anchored item goes wrong, or nothing.
std::optional<std::string> anchoring_problem(std::string_view what, std::string_view quote, Millis start,
                                             Millis end, const knowledge::Transcript& transcript) {
    if (end < start) {
        return std::format("{} ends at {}, before it starts at {}: correct its times", what, clock_time(end),
                           clock_time(start));
    }
    if (!said_there(quote, start, end, transcript)) {
        return std::format("{}: \"{}\" is not said between {} and {}: correct its quote, or its times", what,
                           shown(quote), clock_time(start), clock_time(end));
    }
    return std::nullopt;
}

// The keys at the top of a gold file, into `gold`.
Result<void> read_top(detail::Table& top, std::string_view file, Gold& gold) {
    RR_TRY_ASSIGN(gold.video, top.text("video"));
    if (!valid_video_id(gold.video)) {
        return fail(Errc::invalid_argument,
                    std::format("{}: video '{}' is not a video id", file, gold.video));
    }
    RR_TRY_ASSIGN(gold.snapshot, top.text("snapshot"));
    RR_TRY_ASSIGN(const std::string status, top.choice("status", kStatus));
    gold.status = status == "gold" ? GoldStatus::gold : GoldStatus::silver;
    RR_TRY_ASSIGN(const std::optional<std::string> reviewer, top.optional_text("reviewer"));
    gold.reviewer = reviewer.value_or("");
    RR_TRY_ASSIGN(const std::optional<std::string> drafted_by, top.optional_text("drafted_by"));
    gold.drafted_by = drafted_by.value_or("");
    for (const std::string_view tables : {"fact", "must_quote", "term"}) {
        top.allow(tables);  // read_items() reads them, table by table
    }
    return top.nothing_else();
}

// A gold file's facts, must-quotes and terms, into `gold`.
Result<void> read_items(const toml::table& document, std::string_view file, Gold& gold) {
    std::set<std::string, std::less<>> ids;
    RR_TRY(detail::each_table(document, "fact", file, [&](detail::Table& table) -> Result<void> {
        RR_TRY_ASSIGN(Fact fact, read_fact(table));
        if (!ids.insert(fact.id).second) {
            return fail(Errc::invalid_argument, std::format("{}: a second fact {}", table.where(), fact.id));
        }
        gold.facts.push_back(std::move(fact));
        return ok();
    }));
    RR_TRY(detail::each_table(document, "must_quote", file, [&](detail::Table& table) -> Result<void> {
        RR_TRY_ASSIGN(MustQuote quote, read_must_quote(table));
        gold.must_quotes.push_back(std::move(quote));
        return ok();
    }));
    return detail::each_table(document, "term", file, [&](detail::Table& table) -> Result<void> {
        RR_TRY_ASSIGN(Term term, read_term(table));
        gold.terms.push_back(std::move(term));
        return ok();
    });
}

}  // namespace

std::string_view to_string(Importance importance) noexcept {
    return kImportance[static_cast<std::size_t>(importance)];
}

std::string_view to_string(GoldStatus status) noexcept {
    return kStatus[static_cast<std::size_t>(status)];
}

std::filesystem::path gold_file(const std::filesystem::path& eval_root, std::string_view id) {
    return eval_root / "gold" / std::format("{}.toml", id);
}

Result<Gold> parse_gold(std::string_view toml, std::string_view file) {
    RR_TRY_ASSIGN(const toml::table document, detail::parse_toml(toml, file));
    detail::Table top{document, std::string{file}};
    Gold gold;
    RR_TRY(read_top(top, file, gold));
    RR_TRY(read_items(document, file, gold));
    return gold;
}

std::string clock_time(Millis at) {
    const std::int64_t total = std::max<std::int64_t>(at.count(), 0);
    const std::int64_t seconds = total / 1000;
    std::string written =
        seconds >= 3600 ? std::format("{}:{:02}:{:02}", seconds / 3600, (seconds / 60) % 60, seconds % 60)
                        : std::format("{}:{:02}", seconds / 60, seconds % 60);
    if (const std::int64_t millis = total % 1000; millis != 0) {
        std::string fraction = std::format("{:03}", millis);
        while (fraction.back() == '0') {
            fraction.pop_back();
        }
        written += '.' + fraction;
    }
    return written;
}

std::string to_toml(const Gold& gold) {
    std::string written = std::format(
        "# The gold annotation of {}: what it says that an article must not lose\n"
        "# (rr/eval/gold.hpp). Models drafted it; a person corrects it.\n"
        "#   * A fact is a claim in one sentence, with the quote that says it, as\n"
        "#     said, and the stretch of the video that says it (start, end, as a\n"
        "#     player shows them). importance: core, when an article without it\n"
        "#     misses the point, or detail. found: how many of the drafts found it;\n"
        "#     doubt a fact only one found. Add, merge, split or strike facts\n"
        "#     freely; keep the ids of the facts you keep.\n"
        "#   * A must-quote is said words an article must quote, not paraphrase.\n"
        "#   * A term is the video's own word for something.\n"
        "# When it is right, set status = \"gold\" and reviewer to your name.\n\n"
        "video = {}\nsnapshot = {}\nstatus = {}\nreviewer = {}\ndrafted_by = {}\n",
        gold.video, detail::toml_string(gold.video), detail::toml_string(gold.snapshot),
        detail::toml_string(to_string(gold.status)), detail::toml_string(gold.reviewer),
        detail::toml_string(gold.drafted_by));
    for (const Fact& fact : gold.facts) {
        written += std::format(
            "\n[[fact]]\nid = {}\nstart = {}\nend = {}\nimportance = {}\nfound = {}\nquote = {}\nclaim = "
            "{}\n",
            detail::toml_string(fact.id), detail::toml_string(clock_time(fact.start)),
            detail::toml_string(clock_time(fact.end)), detail::toml_string(to_string(fact.importance)),
            fact.found, detail::toml_string(fact.quote), detail::toml_string(fact.claim));
    }
    for (const MustQuote& quote : gold.must_quotes) {
        written += std::format("\n[[must_quote]]\nstart = {}\nend = {}\nquote = {}\n",
                               detail::toml_string(clock_time(quote.start)),
                               detail::toml_string(clock_time(quote.end)), detail::toml_string(quote.quote));
    }
    for (const Term& term : gold.terms) {
        written += std::format("\n[[term]]\nterm = {}\nmeaning = {}\n", detail::toml_string(term.term),
                               detail::toml_string(term.meaning));
    }
    return written;
}

bool said_there(std::string_view quote, Millis start, Millis end, const knowledge::Transcript& transcript) {
    const Millis from = start - kAnchorSlack;
    const Millis to = end + kAnchorSlack;
    std::string said;
    for (const knowledge::Sentence& sentence : transcript.sentences) {
        if (sentence.span.start() <= to && sentence.span.end() >= from) {
            said += said.empty() ? sentence.text : " " + sentence.text;
        }
    }
    return text::align(quote, said).verbatim();
}

std::vector<std::string> gold_problems(const Gold& gold, const knowledge::Transcript& transcript) {
    std::vector<std::string> problems;
    for (const Fact& fact : gold.facts) {
        if (auto problem = anchoring_problem(fact.id, fact.quote, fact.start, fact.end, transcript)) {
            problems.push_back(*std::move(problem));
        }
    }
    for (std::size_t index = 0; index < gold.must_quotes.size(); ++index) {
        const MustQuote& quote = gold.must_quotes[index];
        if (auto problem = anchoring_problem(std::format("must-quote {}", index + 1), quote.quote,
                                             quote.start, quote.end, transcript)) {
            problems.push_back(*std::move(problem));
        }
    }
    if (gold.status == GoldStatus::gold && gold.reviewer.empty()) {
        problems.emplace_back("it is gold, and names no reviewer: set reviewer to who corrected it");
    }
    return problems;
}

}  // namespace rr::eval
