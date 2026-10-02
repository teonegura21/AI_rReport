// An article measured by code alone (rr/eval/score.hpp).
#include <rr/eval/score.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <format>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <rr/core/error.hpp>
#include <rr/core/file_io.hpp>
#include <rr/eval/snapshot.hpp>
#include <rr/fidelity/finding.hpp>
#include <rr/knowledge/article.hpp>
#include <rr/knowledge/atoms.hpp>
#include <rr/library/stage_store.hpp>
#include <rr/report/contents.hpp>
#include <rr/text/align.hpp>
#include <rr/text/span.hpp>
#include <rr/text/tokens.hpp>

namespace rr::eval {

namespace {

using knowledge::AtomId;
using knowledge::SentenceId;

// `part` of `whole` as a score; none when there is nothing to share.
void share(std::vector<Score>& scores, std::string_view metric, std::size_t part, std::size_t whole) {
    if (whole > 0) {
        scores.push_back(Score{.metric = std::string{metric},
                               .value = static_cast<double>(part) / static_cast<double>(whole)});
    }
}

void add(std::vector<Score>& scores, std::string_view metric, double value) {
    scores.push_back(Score{.metric = std::string{metric}, .value = value});
}

// What a reader reads of the article: its prose, without its markers, a
// block a line. Headings name sections; code is no prose.
std::string reader_text(const std::string& markdown, const knowledge::ParsedArticle& parsed) {
    std::string text;
    for (const knowledge::Block& block : parsed.blocks) {
        if (block.kind == knowledge::BlockKind::heading || block.kind == knowledge::BlockKind::code) {
            continue;
        }
        text += knowledge::without_markers(markdown, parsed, block.span);
        text += '\n';
    }
    return text;
}

// The video's length: its own, else where its transcript ends.
Millis video_length(const report::Contents& contents) {
    if (contents.video.has_value() && contents.video->duration.has_value()) {
        return *contents.video->duration;
    }
    if (contents.transcript.has_value() && !contents.transcript->sentences.empty()) {
        return contents.transcript->sentences.back().span.end();
    }
    return Millis{0};
}

// The words of the transcript: what the article's length is measured against.
std::size_t transcript_words(const knowledge::Transcript& transcript) {
    std::size_t words = 0;
    for (const knowledge::Sentence& sentence : transcript.sentences) {
        words += text::terms(sentence.text).size();
    }
    return words;
}

// What a reviewed gold says that the article keeps: the article as a reader
// reads it (`reader`), as parsed, against the transcript and Ledger A's atoms.
void against_gold(std::vector<Score>& scores, const Gold& gold, const knowledge::Transcript& transcript,
                  std::span<const knowledge::Atom> atoms, const std::string& reader,
                  const knowledge::ParsedArticle& parsed) {
    const std::vector<text::Term> said = text::terms(reader);
    const auto said_in_article = [&](std::string_view words) {
        return text::align(text::terms(words), said).verbatim();
    };
    share(scores, "gold_quotes_kept",
          static_cast<std::size_t>(std::ranges::count_if(
              gold.must_quotes, [&](const MustQuote& quote) { return said_in_article(quote.quote); })),
          gold.must_quotes.size());
    share(scores, "gold_terms_used",
          static_cast<std::size_t>(std::ranges::count_if(
              gold.terms, [&](const Term& term) { return said_in_article(term.term); })),
          gold.terms.size());

    // A fact is cited when the article cites an atom of a sentence saying it.
    const std::vector<AtomId> cited_list = parsed.cited();
    const std::set<AtomId> cited(cited_list.begin(), cited_list.end());
    std::size_t facts = 0;
    std::size_t facts_cited = 0;
    std::size_t core = 0;
    std::size_t core_cited = 0;
    for (const Fact& fact : gold.facts) {
        const std::vector<SentenceId> sentences =
            sentences_saying(fact.quote, fact.start, fact.end, transcript);
        const bool is_cited = std::ranges::any_of(atoms, [&](const knowledge::Atom& atom) {
            return cited.contains(atom.id) && std::ranges::any_of(atom.sentences, [&](SentenceId sentence) {
                       return std::ranges::find(sentences, sentence) != sentences.end();
                   });
        });
        ++facts;
        facts_cited += is_cited ? 1 : 0;
        if (fact.importance == Importance::core) {
            ++core;
            core_cited += is_cited ? 1 : 0;
        }
    }
    share(scores, "facts_cited", facts_cited, facts);
    share(scores, "core_facts_cited", core_cited, core);
}

}  // namespace

Result<std::optional<Gold>> reviewed_gold(const std::filesystem::path& eval_root, const EvalVideo& video,
                                          std::vector<std::string>& notes) {
    Result<std::string> text = read_file(gold_file(eval_root, video.id));
    if (!text.has_value()) {
        if (text.error().code() != Errc::not_found) {
            return std::unexpected(std::move(text).error());
        }
        notes.push_back(std::format("{}: no gold yet: scored without it", video.id));
        return std::nullopt;
    }
    RR_TRY_ASSIGN(Gold gold, parse_gold(*text, gold_file(eval_root, video.id).string()));
    if (gold.status != GoldStatus::gold) {
        notes.push_back(std::format("{}: its gold is silver, not reviewed yet: scored without it", video.id));
        return std::nullopt;
    }
    RR_TRY_ASSIGN(const std::optional<Snapshot> snapshot, read_snapshot(eval_root, video));
    if (!snapshot.has_value() || snapshot->hash != gold.snapshot) {
        notes.push_back(
            std::format("{}: its gold was written for another snapshot than the one read: "
                        "scored without it",
                        video.id));
        return std::nullopt;
    }
    return gold;
}

Result<Costs> costs_of(db::Database& database, const library::Job& job) {
    Costs costs{.elapsed = job.elapsed};
    RR_TRY(database.read([&](db::ReadTx& tx) -> Result<void> {
        RR_TRY(
            tx.query("SELECT SUM(status = 'ok'), SUM(prompt_tokens), SUM(completion_tokens) FROM llm_calls "
                     "WHERE job_id = ?1",
                     {job.id}, [&](const db::Row& row) -> Result<void> {
                         costs.calls = row.get_int(0);
                         costs.prompt_tokens = row.get_int(1);
                         costs.completion_tokens = row.get_int(2);
                         return ok();
                     }));
        RR_TRY_ASSIGN(costs.escalations,
                      tx.query_int("SELECT COUNT(*) FROM missions WHERE job_id = ?1 AND step IS NOT NULL "
                                   "AND step <> 'as_ordered'",
                                   {job.id}));
        return ok();
    }));
    return costs;
}

std::vector<SentenceId> sentences_saying(std::string_view quote, Millis start, Millis end,
                                         const knowledge::Transcript& transcript) {
    const Millis from = start - kAnchorSlack;
    const Millis to = end + kAnchorSlack;
    std::string said;
    std::vector<std::pair<SentenceId, text::Span>> places;  // each sentence's bytes in `said`
    for (const knowledge::Sentence& sentence : transcript.sentences) {
        if (sentence.span.start() <= to && sentence.span.end() >= from) {
            if (!said.empty()) {
                said += ' ';
            }
            places.emplace_back(sentence.id,
                                text::Span{.begin = said.size(), .end = said.size() + sentence.text.size()});
            said += sentence.text;
        }
    }
    const text::Alignment alignment = text::align(quote, said);
    if (!alignment.verbatim()) {
        return {};
    }
    std::vector<SentenceId> saying;
    for (const auto& [sentence, place] : places) {
        if (place.begin < alignment.text.end && alignment.text.begin < place.end) {
            saying.push_back(sentence);
        }
    }
    return saying;
}

std::vector<Score> scores_of(const report::Contents& contents, const Gold* gold, const Costs& costs) {
    const std::optional<fidelity::Report> report = report::checked(contents);
    // A report is made only of a transcript and Ledger A; an article is scored
    // once written. Said again here, where they are read.
    if (!report.has_value() || !contents.transcript.has_value() || !contents.ledger.has_value() ||
        !contents.article.has_value()) {
        return {};
    }
    const knowledge::Transcript& transcript = *contents.transcript;
    std::vector<Score> scores;
    const fidelity::Coverage& kept = report->coverage;
    share(scores, "sentences_accounted", kept.sentences_accounted, kept.sentences);
    share(scores, "atoms_told", kept.atoms_told, kept.atoms);
    add(scores, "invalid_markers", static_cast<double>(kept.markers_invalid));
    share(scores, "quotes_kept", kept.must_quotes_kept, kept.must_quotes);
    share(scores, "numbers_kept", kept.numbers_kept, kept.numbers);

    const std::string& markdown = contents.article->markdown;
    const knowledge::ParsedArticle parsed = knowledge::parse_article(markdown);
    const std::string reader = reader_text(markdown, parsed);
    if (gold != nullptr) {
        against_gold(scores, *gold, transcript, contents.ledger->atoms, reader, parsed);
    }
    share(scores, "length_ratio", text::terms(reader).size(), transcript_words(transcript));

    const double minutes = std::chrono::duration<double, std::ratio<60>>(costs.elapsed).count();
    add(scores, "minutes", minutes);
    if (const Millis length = video_length(contents); length > Millis{0}) {
        add(scores, "minutes_per_video_hour",
            minutes / std::chrono::duration<double, std::ratio<3600>>(length).count());
    }
    add(scores, "calls", static_cast<double>(costs.calls));
    add(scores, "prompt_tokens", static_cast<double>(costs.prompt_tokens));
    add(scores, "completion_tokens", static_cast<double>(costs.completion_tokens));
    add(scores, "escalations", static_cast<double>(costs.escalations));
    return scores;
}

Result<RunScoring> score_run(const std::filesystem::path& eval_root, const EvalSet& set, const EvalRun& run,
                             Runs& runs, db::Database& database, const Clock& clock) {
    RR_TRY_ASSIGN(const std::vector<RunEntry> entries, runs.entries(run.id));
    const library::Jobs jobs{database, clock};
    const library::StageStore store{database, clock};
    RunScoring scoring;
    std::map<std::string, std::optional<Gold>, std::less<>> golds;  // a video's, read once
    for (const RunEntry& entry : entries) {
        RR_TRY_ASSIGN(const library::Job job, jobs.find(entry.job));
        if (job.status != library::JobStatus::done) {
            scoring.waiting.push_back(std::format("{}, replicate {}: job {} is {}", entry.video,
                                                  entry.replicate, job.id, library::to_string(job.status)));
            continue;
        }
        auto gold = golds.find(entry.video);
        if (gold == golds.end()) {
            std::optional<Gold> read;
            if (const EvalVideo* video = set.find(entry.video); video != nullptr) {
                RR_TRY_ASSIGN(read, reviewed_gold(eval_root, *video, scoring.notes));
            } else {
                scoring.notes.push_back(
                    std::format("{}: the set has it no more: scored without gold", entry.video));
            }
            gold = golds.emplace(entry.video, std::move(read)).first;
        }
        RR_TRY_ASSIGN(const report::Contents contents, report::contents_of(job, store));
        RR_TRY_ASSIGN(const Costs costs, costs_of(database, job));
        const Scoring computed{
            .scorer = std::string{kCodeScorer},
            .scores = scores_of(contents, gold->second.has_value() ? &*gold->second : nullptr, costs)};
        RR_TRY(runs.keep(run.id, entry, computed));
        ++scoring.scored;
    }
    return scoring;
}

}  // namespace rr::eval
