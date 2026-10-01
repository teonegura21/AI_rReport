// The sheet a person reviews a gold file with (rr/eval/sheet.hpp).
#include <rr/eval/sheet.hpp>

#include <algorithm>
#include <format>
#include <iterator>

#include <rr/core/strings.hpp>
#include <rr/transcript/captions.hpp>
#include <rr/transcript/sentences.hpp>

namespace rr::eval {

namespace {

constexpr std::size_t kShortHash = 12;

// Text as a Markdown table cell: on one line, its bars escaped.
std::string cell(std::string_view text) {
    return replace_all(replace_all(replace_all(text, "\r", " "), "\n", " "), "|", "\\|");
}

std::string stretch(std::string_view url, Millis start, Millis end) {
    return std::format("[{}\u2013{}]({})", clock_time(start), clock_time(end), at_time(url, start));
}

constexpr std::string_view kSaid = "\u2713";
constexpr std::string_view kNotSaid = "\u2717";

std::string said(bool there) {
    return there ? std::string{kSaid} : std::format("{} not said there", kNotSaid);
}

}  // namespace

Result<knowledge::Transcript> caption_transcript(const pipeline::Source& source) {
    RR_TRY_ASSIGN(const transcript::CaptionTrack track,
                  transcript::read_captions(source.captions.content, source.captions.format));
    return transcript::transcript_of(track.words);
}

std::string at_time(std::string_view url, Millis at) {
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(at).count();
    return std::format("{}{}t={}s", url, url.contains('?') ? '&' : '?', seconds);
}

std::string review_sheet(const Gold& gold, const EvalVideo& video, const knowledge::Transcript& transcript,
                         std::string_view snapshot) {
    std::size_t core = 0;
    for (const Fact& fact : gold.facts) {
        core += fact.importance == Importance::core ? 1 : 0;
    }
    std::string sheet = std::format(
        "# Review: {}\n\n{} \u00B7 snapshot `{}` \u00B7 {}{}\n\n"
        "Correct `gold/{}.toml`, in the eval's directory, beside this sheet, then make the sheet again. {} "
        "marks a quote "
        "that is not said where its fact says; a fact only one draft found is marked with ?, the first to "
        "doubt. Found 0: added by the other family's reading, or by a person.\n\n"
        "## Facts: {} ({} core, {} detail)\n\n"
        "| Fact | When | Found | Importance | Claim | Quote | Said there |\n|---|---|---|---|---|---|---|\n",
        video.title, video.url, gold.snapshot.substr(0, kShortHash), to_string(gold.status),
        gold.drafted_by.empty() ? "" : std::format(", drafted by {}", gold.drafted_by), video.id, kNotSaid,
        gold.facts.size(), core, gold.facts.size() - core);
    for (const Fact& fact : gold.facts) {
        sheet += std::format("| {}{} | {} | {} | {} | {} | \"{}\" | {} |\n", fact.id,
                             fact.found == 1 ? " ?" : "", stretch(video.url, fact.start, fact.end),
                             fact.found, to_string(fact.importance), cell(fact.claim), cell(fact.quote),
                             said(said_there(fact.quote, fact.start, fact.end, transcript)));
    }
    sheet += std::format("\n## Must-quotes: {}\n\n| # | When | Quote | Said there |\n|---|---|---|---|\n",
                         gold.must_quotes.size());
    for (std::size_t index = 0; index < gold.must_quotes.size(); ++index) {
        const MustQuote& quote = gold.must_quotes[index];
        sheet +=
            std::format("| {} | {} | \"{}\" | {} |\n", index + 1, stretch(video.url, quote.start, quote.end),
                        cell(quote.quote), said(said_there(quote.quote, quote.start, quote.end, transcript)));
    }
    sheet += std::format("\n## Terms: {}\n\n| Term | Meaning |\n|---|---|\n", gold.terms.size());
    for (const Term& term : gold.terms) {
        sheet += std::format("| {} | {} |\n", cell(term.term),
                             term.meaning.empty() ? "\u2014" : cell(term.meaning));
    }
    std::vector<std::string> problems;
    if (gold.snapshot != snapshot) {
        problems.push_back(
            std::format("it was drafted on the snapshot {}, and the snapshot is now {}: check every "
                        "fact against the new captions, then set snapshot = \"{}\"",
                        gold.snapshot, snapshot, snapshot));
    }
    std::ranges::move(gold_problems(gold, transcript), std::back_inserter(problems));
    sheet += "\n## To correct\n\n";
    if (problems.empty()) {
        sheet +=
            "Nothing the sheet can check. Whether each claim says what its quote says, and whether the "
            "video says more, is the reviewer's to judge.\n";
    }
    for (const std::string& problem : problems) {
        sheet += std::format("- {}\n", problem);
    }
    return sheet;
}

}  // namespace rr::eval
