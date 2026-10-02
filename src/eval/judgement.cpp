// An article read by the eval's frozen judges (rr/eval/judgement.hpp).
#include <rr/eval/judgement.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <format>
#include <functional>
#include <initializer_list>
#include <iterator>
#include <memory>
#include <numeric>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <rr/agents/army.hpp>
#include <rr/agents/mission.hpp>
#include <rr/core/error.hpp>
#include <rr/core/time_span.hpp>
#include <rr/eval/score.hpp>
#include <rr/knowledge/artifacts.hpp>
#include <rr/library/jobs.hpp>
#include <rr/library/stage_store.hpp>
#include <rr/llm/memo.hpp>
#include <rr/llm/sampling.hpp>
#include <rr/pipeline/asking.hpp>
#include <rr/pipeline/executor.hpp>
#include <rr/report/contents.hpp>
#include <rr/text/align.hpp>
#include <rr/text/bm25.hpp>

#include "markdown.hpp"

namespace rr::eval {

namespace {

using knowledge::AtomId;
using knowledge::SentenceId;

constexpr std::string_view kFactPrompt = "eval_fact";
constexpr std::string_view kSupportPrompt = "eval_support";

// The best sentences by BM25 a fact's judge is shown, past those that cite it.
constexpr std::size_t kBm25Candidates = 3;

// The most tokens an answer may take: a fact's, and a sentence's in a
// paragraph's, with some to spare.
constexpr int kFactAnswer = 400;
constexpr int kTokensPerSentence = 160;
constexpr int kSpareTokens = 200;

// A judge of the panel answers as ordered or not at all: no rung of the
// ladder hands its item to another model, nor asks it again thinking. A
// frozen judge is those weights, asked that way.
constexpr agents::Escalation kFrozen{.thinking = false, .commander = false, .other_family = false};

constexpr auto kFactVerdicts = std::to_array<std::pair<FactVerdict, std::string_view>>({
    {FactVerdict::told, "told"},
    {FactVerdict::partly, "partly"},
    {FactVerdict::not_told, "not_told"},
});

// The passages `text` quotes: between straight double quotes, or between
// curly ones. A quote never closed runs to the end.
std::vector<std::string_view> quoted_passages(std::string_view text) {
    constexpr std::string_view kCurlyOpen = "\u201C";
    constexpr std::string_view kCurlyClose = "\u201D";
    std::vector<std::string_view> passages;
    std::size_t at = 0;
    while (at < text.size()) {
        const std::size_t straight = text.find('"', at);
        const std::size_t curly = text.find(kCurlyOpen, at);
        if (straight == std::string_view::npos && curly == std::string_view::npos) {
            break;
        }
        const bool is_curly = curly < straight;
        const std::size_t begin = is_curly ? curly + kCurlyOpen.size() : straight + 1;
        const std::size_t close = is_curly ? text.find(kCurlyClose, begin) : text.find('"', begin);
        const std::size_t end = close == std::string_view::npos ? text.size() : close;
        passages.push_back(text.substr(begin, end - begin));
        at = close == std::string_view::npos ? text.size() : close + (is_curly ? kCurlyClose.size() : 1);
    }
    return passages;
}

// The line a judge is shown for a sentence of the video: "S12 [1:02, Ada] The words."
std::string video_line(const knowledge::Sentence& sentence) {
    return std::format("{} [{}{}] {}", sentence.id.str(), format_timestamp(sentence.span.start()),
                       sentence.speaker.has_value() ? std::format(", {}", *sentence.speaker) : std::string{},
                       sentence.text);
}

// What the judges' answers rest on: everything that changes them.
json::Json reading_fingerprint(const ArticleToJudge& article) {
    json::Json facts = json::Json::array();
    for (const Fact& fact : article.facts) {
        facts.push_back(json::Json{{"id", fact.id},
                                   {"start_ms", fact.start.count()},
                                   {"end_ms", fact.end.count()},
                                   {"core", fact.importance == Importance::core},
                                   {"quote", fact.quote},
                                   {"claim", fact.claim}});
    }
    return json::Json{{"video", json::Json{{"title", article.title}, {"channel", article.channel}}},
                      {"transcript", json::content_hash(knowledge::to_json(article.transcript)).hex()},
                      {"atoms", json::content_hash(knowledge::to_json(article.ledger)).hex()},
                      {"article", json::content_hash(knowledge::to_json(article.article)).hex()},
                      {"facts", json::content_hash(facts).hex()}};
}

// One mission of the judgement, and where its answer goes.
struct Order {
    agents::Mission mission{};
    std::size_t judge = 0;                 // its index in the panel
    std::size_t fact = 0;                  // a fact's mission: its index
    std::vector<std::size_t> sentences{};  // a paragraph's: its sentences' indices, in order
};

// Asks the panel's judges, item by item, and keeps what they answer.
class Reader {
public:
    Reader(const pipeline::StageContext& context, const Panel& panel, const ArticleToJudge& article,
           agents::Army& army)
        : context_(&context)
        , panel_(&panel)
        , article_(&article)
        , army_(&army)
        , sentences_(article_sentences(article.article, article.ledger))
        , shared_(json::Json{{"title", article.title}, {"channel", article.channel}}) {}

    Result<pipeline::StageOutput> read() {
        RR_TRY_ASSIGN(fact_prompt_, pipeline::stage_prompt(*context_->prompts, kFactPrompt));
        RR_TRY_ASSIGN(support_prompt_, pipeline::stage_prompt(*context_->prompts, kSupportPrompt));
        judgement_.panel = panel_->name;
        for (const PanelJudge& judge : panel_->judges) {
            judgement_.judges.push_back(judge.role);
        }
        RR_TRY(order_facts());
        RR_TRY(order_sentences());
        std::vector<agents::Mission> missions;
        missions.reserve(orders_.size());
        for (Order& order : orders_) {
            missions.push_back(std::move(order.mission));
        }
        RR_TRY_ASSIGN(const std::vector<agents::Outcome> outcomes,
                      army_->squad(std::move(missions), context_->stop));
        for (std::size_t index = 0; index < outcomes.size(); ++index) {
            keep(orders_[index], outcomes[index]);
        }
        return pipeline::StageOutput{.artifact = to_json(judgement_), .findings = std::move(findings_)};
    }

private:
    Result<void> order_facts() {
        for (const Fact& fact : article_->facts) {
            JudgedFact judged{
                .fact = fact.id,
                .core = fact.importance == Importance::core,
                .candidates = fact_candidates(fact, sentences_, article_->transcript, article_->ledger)};
            const std::size_t index = judgement_.facts.size();
            if (!judged.candidates.empty()) {
                std::string lines;
                std::vector<std::string> shown;
                for (std::size_t shown_number = 1; const std::size_t number : judged.candidates) {
                    const std::string& text = sentences_.at(number - 1).text;
                    lines += std::format("{}{}. {}", lines.empty() ? "" : "\n", shown_number++, text);
                    shown.push_back(text);
                }
                const json::Json order{{"claim", fact.claim}, {"quote", fact.quote}, {"sentences", lines}};
                RR_TRY_ASSIGN(const prompt::Rendered rendered,
                              context_->prompts->render(kFactPrompt, shared_, order));
                for (std::size_t judge = 0; judge < panel_->judges.size(); ++judge) {
                    judged.readings.push_back(FactReading{.judge = panel_->judges[judge].role});
                    agents::Mission mission =
                        ask(std::format("{} by the {}", fact.id, panel_->judges[judge].role), kFactPrompt,
                            fact_prompt_, rendered, panel_->judges[judge].role, kFactAnswer);
                    mission.check = [shown](const json::Json& answer) {
                        const std::optional<FactVerdict> verdict =
                            fact_verdict_from_string(answer.at("verdict").get<std::string>());
                        return evidence_problems(answer.at("evidence").get<std::string>(), shown,
                                                 verdict != FactVerdict::not_told);
                    };
                    if (!army_->holds(mission)) {
                        judged.readings.back().unread = std::format(
                            "the {}'s engine holds no mission reading it", panel_->judges[judge].role);
                        findings_.push_back(std::format("{}: {}", fact.id, judged.readings.back().unread));
                        continue;
                    }
                    orders_.push_back(Order{.mission = std::move(mission), .judge = judge, .fact = index});
                }
            }
            judgement_.facts.push_back(std::move(judged));
        }
        return ok();
    }

    Result<void> order_sentences() {
        for (const ArticleSentence& sentence : sentences_) {
            judgement_.sentences.push_back(JudgedSentence{
                .number = sentence.number, .paragraph = sentence.paragraph, .text = sentence.text});
            for (const PanelJudge& judge : panel_->judges) {
                judgement_.sentences.back().readings.push_back(SentenceReading{.judge = judge.role});
            }
        }
        for (std::size_t first = 0; first < sentences_.size();) {
            std::size_t last = first;
            while (last < sentences_.size() && sentences_[last].paragraph == sentences_[first].paragraph) {
                ++last;
            }
            std::vector<std::size_t> paragraph(last - first);
            std::ranges::iota(paragraph, first);
            for (const std::size_t index : paragraph) {
                judgement_.sentences[index].evidence =
                    evidence_of(sentences_[index], paragraph_sentences(paragraph), article_->ledger);
            }
            for (std::size_t judge = 0; judge < panel_->judges.size(); ++judge) {
                RR_TRY(order_paragraph(paragraph, judge));
            }
            first = last;
        }
        return ok();
    }

    // What a judge is shown of `part` of a paragraph: each sentence, numbered,
    // with the video's sentences it rests on; and, sentence by sentence,
    // those sentences' words, which an answer's quotes are checked against.
    struct Shown {
        std::string lines{};
        std::vector<std::vector<std::string>> texts{};
    };

    [[nodiscard]] Shown shown_of(std::span<const std::size_t> part) const {
        Shown shown;
        for (std::size_t number = 1; const std::size_t index : part) {
            shown.lines +=
                std::format("{}{}. {}", shown.lines.empty() ? "" : "\n", number++, sentences_[index].text);
            std::vector<std::string>& texts = shown.texts.emplace_back();
            if (judgement_.sentences[index].evidence.empty()) {
                shown.lines += "\n   (no sentence of the video: neither it nor its paragraph cites one)";
            }
            for (const SentenceId id : judgement_.sentences[index].evidence) {
                const knowledge::Sentence* said = article_->transcript.find(id);
                RR_ASSERT(said != nullptr, "an atom's sentences are the transcript's");
                shown.lines += "\n   " + video_line(*said);
                texts.push_back(said->text);
            }
        }
        return shown;
    }

    // `part` of its paragraph, as the judge is told what it reads:
    // "paragraph 2, in \"The scales\" (its sentences 1 to 2)".
    [[nodiscard]] std::string part_named(std::span<const std::size_t> part) const {
        const ArticleSentence& opening = sentences_[part.front()];
        std::string what = opening.section.empty()
                               ? std::format("paragraph {}", opening.paragraph)
                               : std::format("paragraph {}, in \"{}\"", opening.paragraph, opening.section);
        if (part.size() < paragraph_length(opening.paragraph)) {
            const std::size_t from = part.front() - first_of(opening.paragraph) + 1;
            const std::size_t to = part.back() - first_of(opening.paragraph) + 1;
            what += from == to ? std::format(" (its sentence {})", from)
                               : std::format(" (its sentences {} to {})", from, to);
        }
        return what;
    }

    // The missions reading `part` of a paragraph by the judge `judge`: one, or,
    // when its engine holds no mission so long, the part halved until each
    // fits. A sentence too long even alone is left unread.
    Result<void> order_paragraph(std::span<const std::size_t> part, std::size_t judge) {
        const std::string& role = panel_->judges[judge].role;
        const ArticleSentence& opening = sentences_[part.front()];
        Shown shown = shown_of(part);
        const json::Json order{{"what", part_named(part)}, {"sentences", shown.lines}};
        RR_TRY_ASSIGN(const prompt::Rendered rendered,
                      context_->prompts->render(kSupportPrompt, shared_, order));
        agents::Mission mission =
            ask(std::format("S{} by the {}", opening.number, role), kSupportPrompt, support_prompt_, rendered,
                role, (static_cast<int>(part.size()) * kTokensPerSentence) + kSpareTokens);
        mission.check = [shown = std::move(shown.texts)](const json::Json& answer) {
            std::vector<std::string> problems = pipeline::judgment_problems(answer, shown.size());
            if (!problems.empty()) {
                return problems;
            }
            for (const json::Json& judged : answer.at("sentences")) {
                const std::size_t number = judged.at("sentence").get<std::size_t>();
                for (std::string& problem : evidence_problems(judged.at("evidence").get<std::string>(),
                                                              shown.at(number - 1), false)) {
                    problems.push_back(std::format("sentence {}: {}", number, std::move(problem)));
                }
            }
            return problems;
        };
        if (army_->holds(mission)) {
            orders_.push_back(Order{.mission = std::move(mission),
                                    .judge = judge,
                                    .sentences = std::vector<std::size_t>(part.begin(), part.end())});
            return ok();
        }
        if (part.size() == 1) {
            judgement_.sentences[part.front()].readings[judge].unread =
                std::format("the {}'s engine holds no mission reading it, even alone", role);
            findings_.push_back(std::format("sentence {}: {}", opening.number,
                                            judgement_.sentences[part.front()].readings[judge].unread));
            return ok();
        }
        const std::size_t half = part.size() / 2;
        RR_TRY(order_paragraph(part.first(half), judge));
        return order_paragraph(part.subspan(half), judge);
    }

    [[nodiscard]] agents::Mission ask(std::string id, std::string_view kind,
                                      const pipeline::StagePrompt& prompt, const prompt::Rendered& rendered,
                                      const std::string& role, int answer_tokens) const {
        const std::int64_t seed = llm::derived_seed(context_->job->salt, JudgementStage::kName, id, 0);
        return agents::Mission{
            .id = std::move(id),
            .kind = std::string{kind},
            .prompt = prompt.info->label(),
            .role = role,
            .request = llm::ChatRequest{.messages = {llm::Message::system(rendered.system),
                                                     llm::Message::user(rendered.user)},
                                        .temperature = 0.0,  // greedy: a frozen judge reads alike every time
                                        .seed = seed,
                                        .thinking = false},
            .answer_tokens = answer_tokens,
            .schema = prompt.schema,
            .escalation = kFrozen};
    }

    void keep(const Order& order, const agents::Outcome& outcome) {
        const std::string& role = panel_->judges[order.judge].role;
        if (outcome.verdict != agents::Verdict::accepted || outcome.answers.empty()) {
            const std::string why = pipeline::why_unanswered(outcome, *army_, "read it");
            if (order.sentences.empty()) {
                judgement_.facts[order.fact].readings[order.judge].unread = why;
                findings_.push_back(std::format("{}: the {} could not read it: {}",
                                                judgement_.facts[order.fact].fact, role, why));
                return;
            }
            for (const std::size_t index : order.sentences) {
                judgement_.sentences[index].readings[order.judge].unread = why;
            }
            findings_.push_back(std::format("sentences {} to {}: the {} could not read them: {}",
                                            sentences_[order.sentences.front()].number,
                                            sentences_[order.sentences.back()].number, role, why));
            return;
        }
        const json::Json& answer = outcome.answers.front().value;
        if (order.sentences.empty()) {
            FactReading& reading = judgement_.facts[order.fact].readings[order.judge];
            reading.verdict = fact_verdict_from_string(answer.at("verdict").get<std::string>());
            reading.evidence = answer.at("evidence").get<std::string>();
            return;
        }
        for (const pipeline::Judged& judged : pipeline::judgment_of(answer)) {
            SentenceReading& reading =
                judgement_.sentences[order.sentences.at(judged.sentence - 1)].readings[order.judge];
            reading.issue = judged.issue;
            reading.evidence = judged.evidence;
            reading.reading = judged.reading;
        }
    }

    [[nodiscard]] std::vector<ArticleSentence> paragraph_sentences(
        std::span<const std::size_t> indices) const {
        std::vector<ArticleSentence> paragraph;
        for (const std::size_t index : indices) {
            paragraph.push_back(sentences_[index]);
        }
        return paragraph;
    }

    [[nodiscard]] std::size_t first_of(std::size_t paragraph) const {
        return static_cast<std::size_t>(
            std::ranges::find(sentences_, paragraph, &ArticleSentence::paragraph) - sentences_.begin());
    }

    [[nodiscard]] std::size_t paragraph_length(std::size_t paragraph) const {
        return static_cast<std::size_t>(
            std::ranges::count(sentences_, paragraph, &ArticleSentence::paragraph));
    }

    const pipeline::StageContext* context_;
    const Panel* panel_;
    const ArticleToJudge* article_;
    agents::Army* army_;
    std::vector<ArticleSentence> sentences_;
    json::Json shared_;
    pipeline::StagePrompt fact_prompt_{};
    pipeline::StagePrompt support_prompt_{};
    Judgement judgement_{};
    std::vector<Order> orders_{};
    std::vector<std::string> findings_{};
};

// `part` of `whole`, for a share: none when there is nothing to share.
void mean_share(std::vector<Score>& scores, std::string_view metric, double part, std::size_t whole) {
    if (whole > 0) {
        scores.push_back(Score{.metric = std::string{metric}, .value = part / static_cast<double>(whole)});
    }
}

// The mean of `values` over the readings that have one; none when none has.
template <typename Reading, typename Value>
std::optional<double> mean_of(const std::vector<Reading>& readings, Value value) {
    double sum = 0;
    std::size_t read = 0;
    for (const Reading& reading : readings) {
        if (const std::optional<double> one = value(reading); one.has_value()) {
            sum += *one;
            ++read;
        }
    }
    return read == 0 ? std::nullopt : std::optional<double>{sum / static_cast<double>(read)};
}

// Whether every reading of an item read it alike, when two at least read it;
// none when fewer did.
template <typename Reading, typename Key>
std::optional<bool> agreed(const std::vector<Reading>& readings, Key key) {
    std::vector<std::string> keys;
    for (const Reading& reading : readings) {
        if (std::optional<std::string> one = key(reading); one.has_value()) {
            keys.push_back(*std::move(one));
        }
    }
    if (keys.size() < 2) {
        return std::nullopt;
    }
    return std::ranges::all_of(keys, [&](const std::string& one) { return one == keys.front(); });
}

// How often the judges read an item alike, over the items two of them at
// least read (agreed()).
struct Agreement {
    std::size_t compared = 0;
    std::size_t alike = 0;

    void count(std::optional<bool> same) {
        if (same.has_value()) {
            ++compared;
            if (*same) {
                ++alike;
            }
        }
    }
};

// The facts' scores: the share a reader learns, wholly or in part; the share
// of the core facts learnt; and the share the judges read alike.
void add_fact_scores(const std::vector<JudgedFact>& facts, std::vector<Score>& scores) {
    const auto verdict_is = [](std::initializer_list<FactVerdict> counted) {
        return [counted](const FactReading& reading) -> std::optional<double> {
            if (!reading.verdict.has_value()) {
                return std::nullopt;
            }
            return std::ranges::contains(counted, *reading.verdict) ? 1.0 : 0.0;
        };
    };
    double told = 0;
    double partly = 0;
    double core_told = 0;
    std::size_t read = 0;
    std::size_t core = 0;
    Agreement agreement;
    for (const JudgedFact& fact : facts) {
        // A fact shown to no judge has no sentence that might tell it: not told.
        const std::optional<double> is_told = fact.candidates.empty()
                                                  ? std::optional{0.0}
                                                  : mean_of(fact.readings, verdict_is({FactVerdict::told}));
        const std::optional<double> is_partly =
            fact.candidates.empty()
                ? std::optional{0.0}
                : mean_of(fact.readings, verdict_is({FactVerdict::told, FactVerdict::partly}));
        if (!is_told.has_value() || !is_partly.has_value()) {
            continue;  // every judge left it unread
        }
        ++read;
        told += *is_told;
        partly += *is_partly;
        if (fact.core) {
            ++core;
            core_told += *is_told;
        }
        agreement.count(agreed(fact.readings, [](const FactReading& reading) {
            return reading.verdict.has_value() ? std::optional{std::string{to_string(*reading.verdict)}}
                                               : std::nullopt;
        }));
    }
    mean_share(scores, "facts_told", told, read);
    mean_share(scores, "facts_told_partly", partly, read);
    mean_share(scores, "core_facts_told", core_told, core);
    mean_share(scores, "facts_agreement", static_cast<double>(agreement.alike), agreement.compared);
}

// Each issue a sentence may be read to have, and its score: the share of the
// sentences read so. A sentence with none is one the video supports.
struct IssueScore {
    pipeline::Issue issue = pipeline::Issue::none;
    std::string_view metric{};
};
constexpr std::array kIssueScores{
    IssueScore{.issue = pipeline::Issue::none, .metric = "sentences_supported"},
    IssueScore{.issue = pipeline::Issue::adds, .metric = "adds"},
    IssueScore{.issue = pipeline::Issue::softens, .metric = "softens"},
    IssueScore{.issue = pipeline::Issue::intensifies, .metric = "intensifies"},
    IssueScore{.issue = pipeline::Issue::misattributes, .metric = "misattributes"},
    IssueScore{.issue = pipeline::Issue::contradicts, .metric = "contradicts"},
};

// The sentences' scores: the share read as each issue (none: supported), each
// sentence's share of an issue the share of its readings that say it; and the
// share the judges read alike.
void add_sentence_scores(const std::vector<JudgedSentence>& sentences, std::vector<Score>& scores) {
    std::array<double, kIssueScores.size()> issued{};
    std::size_t read = 0;
    Agreement agreement;
    for (const JudgedSentence& sentence : sentences) {
        std::array<std::size_t, kIssueScores.size()> said{};
        std::size_t readings = 0;
        for (const SentenceReading& reading : sentence.readings) {
            if (!reading.issue.has_value()) {
                continue;  // unread by this judge
            }
            const auto kind = static_cast<std::size_t>(std::ranges::distance(
                kIssueScores.begin(), std::ranges::find(kIssueScores, *reading.issue, &IssueScore::issue)));
            RR_ASSERT(kind < kIssueScores.size(), "kIssueScores names every issue");
            ++said[kind];
            ++readings;
        }
        if (readings == 0) {
            continue;  // every judge left it unread
        }
        ++read;
        for (std::size_t kind = 0; kind < kIssueScores.size(); ++kind) {
            issued[kind] += static_cast<double>(said[kind]) / static_cast<double>(readings);
        }
        agreement.count(agreed(sentence.readings, [](const SentenceReading& reading) {
            return reading.issue.has_value() ? std::optional{std::string{pipeline::to_string(*reading.issue)}}
                                             : std::nullopt;
        }));
    }
    for (std::size_t kind = 0; kind < kIssueScores.size(); ++kind) {
        mean_share(scores, kIssueScores[kind].metric, issued[kind], read);
    }
    mean_share(scores, "sentences_agreement", static_cast<double>(agreement.alike), agreement.compared);
}

// One fact of a judgement artifact, its schema checked already.
Result<JudgedFact> fact_from_json(const json::Json& fact) {
    JudgedFact read{.fact = fact.at("fact").get<std::string>(),
                    .core = fact.at("core").get<bool>(),
                    .candidates = fact.at("candidates").get<std::vector<std::size_t>>()};
    for (const json::Json& reading : fact.at("readings")) {
        FactReading kept{.judge = reading.at("judge").get<std::string>(),
                         .evidence = reading.at("evidence").get<std::string>()};
        if (reading.contains("verdict")) {
            kept.verdict = fact_verdict_from_string(reading.at("verdict").get<std::string>());
        } else if (reading.contains("unread")) {
            kept.unread = reading.at("unread").get<std::string>();
        } else {
            return fail(
                Errc::invalid_argument,
                std::format("not a judgement: a reading of {} has no verdict and says not why", read.fact));
        }
        read.readings.push_back(std::move(kept));
    }
    return read;
}

// One sentence of a judgement artifact, its schema checked already.
Result<JudgedSentence> sentence_from_json(const json::Json& sentence) {
    JudgedSentence read{.number = sentence.at("sentence").get<std::size_t>(),
                        .paragraph = sentence.at("paragraph").get<std::size_t>(),
                        .text = sentence.at("text").get<std::string>()};
    for (const json::Json& id : sentence.at("evidence")) {
        const std::optional<SentenceId> parsed = SentenceId::parse(id.get<std::string>());
        RR_ASSERT(parsed.has_value(), "a judgement's schema allows S1, S2, \u2026 only");
        read.evidence.push_back(*parsed);
    }
    for (const json::Json& reading : sentence.at("readings")) {
        SentenceReading kept{.judge = reading.at("judge").get<std::string>(),
                             .evidence = reading.at("evidence").get<std::string>()};
        if (reading.contains("issue")) {
            kept.issue = pipeline::issue_from_string(reading.at("issue").get<std::string>());
            if (const auto said = reading.find("reading"); said != reading.end() && !said->is_null()) {
                kept.reading = said->get<std::string>();
            }
        } else if (reading.contains("unread")) {
            kept.unread = reading.at("unread").get<std::string>();
        } else {
            return fail(Errc::invalid_argument,
                        std::format("not a judgement: a reading of sentence {} has no issue and says not why",
                                    read.number));
        }
        read.readings.push_back(std::move(kept));
    }
    return read;
}

}  // namespace

std::vector<ArticleSentence> article_sentences(const knowledge::Article& article,
                                               const knowledge::AtomLedger& ledger) {
    const knowledge::ParsedArticle parsed = knowledge::parse_article(article.markdown);
    std::vector<ArticleSentence> sentences;
    std::string section;
    std::size_t paragraph = 0;
    for (const knowledge::Block& block : parsed.blocks) {
        if (block.kind == knowledge::BlockKind::heading) {
            section = detail::heading_text(
                std::string_view{article.markdown}.substr(block.span.begin, block.span.size()));
            continue;
        }
        if (block.kind == knowledge::BlockKind::code || block.sentences.empty()) {
            continue;
        }
        ++paragraph;
        for (const knowledge::CitingSentence& sentence : block.sentences) {
            ArticleSentence read{.number = sentences.size() + 1,
                                 .paragraph = paragraph,
                                 .section = section,
                                 .text = knowledge::without_markers(article.markdown, parsed, sentence.span)};
            for (const knowledge::Marker& marker : sentence.markers) {
                if (ledger.find(marker.atom) != nullptr && !std::ranges::contains(read.cited, marker.atom)) {
                    read.cited.push_back(marker.atom);
                }
            }
            sentences.push_back(std::move(read));
        }
    }
    return sentences;
}

std::vector<std::size_t> fact_candidates(const Fact& fact, const std::vector<ArticleSentence>& sentences,
                                         const knowledge::Transcript& transcript,
                                         const knowledge::AtomLedger& ledger) {
    const std::vector<SentenceId> saying = sentences_saying(fact.quote, fact.start, fact.end, transcript);
    std::set<AtomId> atoms;  // the atoms of the sentences that say it
    for (const knowledge::Atom& atom : ledger.atoms) {
        if (std::ranges::any_of(atom.sentences,
                                [&](SentenceId id) { return std::ranges::contains(saying, id); })) {
            atoms.insert(atom.id);
        }
    }
    std::vector<std::size_t> chosen;
    for (const ArticleSentence& sentence : sentences) {
        if (chosen.size() < kMostCandidates &&
            std::ranges::any_of(sentence.cited, [&](AtomId id) { return atoms.contains(id); })) {
            chosen.push_back(sentence.number);
        }
    }
    text::Bm25 index;
    for (const ArticleSentence& sentence : sentences) {
        index.add(sentence.text);
    }
    for (const text::Hit& hit : index.search(std::format("{} {}", fact.claim, fact.quote), kBm25Candidates)) {
        const std::size_t number = hit.passage + 1;
        if (chosen.size() < kMostCandidates && !std::ranges::contains(chosen, number)) {
            chosen.push_back(number);
        }
    }
    std::ranges::sort(chosen);
    return chosen;
}

std::vector<SentenceId> evidence_of(const ArticleSentence& sentence,
                                    const std::vector<ArticleSentence>& paragraph,
                                    const knowledge::AtomLedger& ledger) {
    std::vector<AtomId> cited = sentence.cited;
    if (cited.empty()) {
        for (const ArticleSentence& other : paragraph) {
            cited.insert(cited.end(), other.cited.begin(), other.cited.end());
        }
    }
    std::vector<SentenceId> said;
    for (const AtomId id : cited) {
        const knowledge::Atom* atom = ledger.find(id);
        RR_ASSERT(atom != nullptr, "a sentence cites only the ledger's atoms (article_sentences())");
        said.insert(said.end(), atom->sentences.begin(), atom->sentences.end());
    }
    std::ranges::sort(said);
    const auto repeated = std::ranges::unique(said);
    said.erase(repeated.begin(), repeated.end());
    return said;
}

std::vector<std::string> evidence_problems(std::string_view evidence, const std::vector<std::string>& shown,
                                           bool required) {
    pipeline::Problems problems;
    const std::vector<std::string_view> passages = quoted_passages(evidence);
    if (required && passages.empty()) {
        problems.note(
            "quote, between double quotes, the words of the sentences that tell it, exactly as they "
            "are written: the evidence quotes none");
    }
    for (const std::string_view passage : passages) {
        const bool found = std::ranges::any_of(
            shown, [&](const std::string& text) { return text::align(passage, text).verbatim(); });
        if (!found) {
            problems.note(
                std::format("\"{}\" is not in what you were shown: quote its words exactly as they are "
                            "written, or quote nothing",
                            passage));
        }
    }
    return std::move(problems).list();
}

std::string_view to_string(FactVerdict verdict) noexcept {
    for (const auto& [known, name] : kFactVerdicts) {
        if (known == verdict) {
            return name;
        }
    }
    return "not_told";
}

std::optional<FactVerdict> fact_verdict_from_string(std::string_view name) noexcept {
    for (const auto& [verdict, known] : kFactVerdicts) {
        if (known == name) {
            return verdict;
        }
    }
    return std::nullopt;
}

json::Json to_json(const Judgement& judgement) {
    json::Json facts = json::Json::array();
    for (const JudgedFact& fact : judgement.facts) {
        json::Json readings = json::Json::array();
        for (const FactReading& reading : fact.readings) {
            json::Json written{{"judge", reading.judge}, {"evidence", reading.evidence}};
            if (reading.verdict.has_value()) {
                written["verdict"] = to_string(*reading.verdict);
            } else {
                written["unread"] = reading.unread;
            }
            readings.push_back(std::move(written));
        }
        facts.push_back(json::Json{{"fact", fact.fact},
                                   {"core", fact.core},
                                   {"candidates", fact.candidates},
                                   {"readings", std::move(readings)}});
    }
    json::Json sentences = json::Json::array();
    for (const JudgedSentence& sentence : judgement.sentences) {
        json::Json evidence = json::Json::array();
        for (const SentenceId id : sentence.evidence) {
            evidence.push_back(id.str());
        }
        json::Json readings = json::Json::array();
        for (const SentenceReading& reading : sentence.readings) {
            json::Json written{{"judge", reading.judge}, {"evidence", reading.evidence}};
            if (reading.issue.has_value()) {
                written["issue"] = pipeline::to_string(*reading.issue);
                written["reading"] =
                    reading.reading.has_value() ? json::Json(*reading.reading) : json::Json(nullptr);
            } else {
                written["unread"] = reading.unread;
            }
            readings.push_back(std::move(written));
        }
        sentences.push_back(json::Json{{"sentence", sentence.number},
                                       {"paragraph", sentence.paragraph},
                                       {"text", sentence.text},
                                       {"evidence", std::move(evidence)},
                                       {"readings", std::move(readings)}});
    }
    return json::Json{{"panel", judgement.panel},
                      {"judges", judgement.judges},
                      {"facts", std::move(facts)},
                      {"sentences", std::move(sentences)}};
}

Result<Judgement> judgement_from_json(const json::Json& document) {
    if (Result<void> valid = knowledge::artifact_schemas().validate("artifacts/judgement", document);
        !valid.has_value()) {
        return fail(Errc::invalid_argument, std::format("not a judgement: {}", valid.error().message()));
    }
    Judgement judgement{.panel = document.at("panel").get<std::string>(),
                        .judges = document.at("judges").get<std::vector<std::string>>()};
    for (const json::Json& fact : document.at("facts")) {
        RR_TRY_ASSIGN(JudgedFact read, fact_from_json(fact));
        judgement.facts.push_back(std::move(read));
    }
    for (const json::Json& sentence : document.at("sentences")) {
        RR_TRY_ASSIGN(JudgedSentence read, sentence_from_json(sentence));
        judgement.sentences.push_back(std::move(read));
    }
    return judgement;
}

std::vector<Score> judged_scores(const Judgement& judgement) {
    std::vector<Score> scores;
    add_fact_scores(judgement.facts, scores);
    add_sentence_scores(judgement.sentences, scores);
    return scores;
}

Result<library::Job> start_judgement(db::Database& database, const Clock& clock, const library::Job& article,
                                     const Panel& panel) {
    library::Jobs jobs{database, clock};
    RR_TRY_ASSIGN(const std::optional<std::string> found, database.read([&](db::ReadTx& tx) {
        return tx.query_optional_text(
            "SELECT job_id FROM eval_judgements WHERE article_job_id = ?1 AND panel = ?2",
            {article.id, panel.name});
    }));
    if (found.has_value()) {
        return jobs.find(*found);
    }
    RR_TRY_ASSIGN(library::Job job, jobs.create(article.id, panel.name, 0, library::JobKind::judgement));
    job.title = article.title;  // what `rreport jobs` lists it by: the video judged
    RR_TRY_ASSIGN(job, jobs.save(std::move(job)));
    RR_TRY(database.write([&](db::WriteTx& tx) -> Result<void> {
        RR_TRY(tx.execute("INSERT INTO eval_judgements (article_job_id, panel, job_id) VALUES (?1, ?2, ?3)",
                          {article.id, panel.name, job.id}));
        return ok();
    }));
    return job;
}

Result<ArticleToJudge> article_to_judge(const library::Job& article, const library::StageStore& store,
                                        std::vector<Fact> facts) {
    RR_TRY_ASSIGN(report::Contents contents, report::contents_of(article, store));
    if (!contents.transcript.has_value() || !contents.ledger.has_value() || !contents.article.has_value()) {
        return fail(Errc::invalid_argument, std::format("job {} has no article to judge", article.id));
    }
    return ArticleToJudge{
        .title = contents.video.has_value() ? contents.video->title : article.title.value_or(article.source),
        .channel = contents.video.has_value() ? contents.video->channel : std::string{},
        .transcript = *std::move(contents.transcript),
        .ledger = *std::move(contents.ledger),
        .article = *std::move(contents.article),
        .facts = std::move(facts)};
}

Result<JudgementRun> run_judgement(library::Job job, const Panel& panel, ArticleToJudge article,
                                   db::Database& database, const Clock& clock,
                                   pipeline::StageContext context) {
    std::vector<std::unique_ptr<pipeline::Stage>> stages;
    stages.push_back(std::make_unique<JudgementStage>(panel, std::move(article)));
    RR_TRY_ASSIGN(const pipeline::Executor executor, pipeline::Executor::create(std::move(stages), clock));
    context.work_directory /= job.id;
    RR_TRY_ASSIGN(pipeline::JobRun run,
                  pipeline::run_job(std::move(job), executor, database, clock, std::move(context)));
    JudgementRun judged{.run = std::move(run)};
    if (judged.run.job.status != library::JobStatus::done) {
        return judged;
    }
    const auto stage = std::ranges::find(judged.run.report.stages, std::string{JudgementStage::kName},
                                         &pipeline::StageReport::stage);
    RR_ASSERT(stage != judged.run.report.stages.end(), "a job done has done its every stage");
    const library::StageStore store{database, clock};
    RR_TRY_ASSIGN(const json::Json artifact, store.artifact(stage->artifact));
    RR_TRY_ASSIGN(const Judgement judgement, judgement_from_json(artifact));
    judged.scoring = Scoring{.scorer = panel.scorer(), .scores = judged_scores(judgement)};
    return judged;
}

JudgementStage::JudgementStage(Panel panel, ArticleToJudge article)
    : panel_(std::move(panel))
    , article_(std::move(article)) {}

Result<json::Json> JudgementStage::fingerprint(const pipeline::StageContext& context) const {
    if (!context.army) {
        return fail(
            Errc::needs_user,
            "judging an article needs the panel's models, and no pod is configured: rreport doctor says how");
    }
    RR_ASSERT(context.prompts != nullptr, "a stage that asks models is given the prompts");
    RR_TRY(check_panel(panel_, *context.prompts));
    RR_TRY_ASSIGN(agents::Army* const army, context.army());
    RR_TRY_ASSIGN(const std::vector<const agents::Engine*> engines, judges_in(panel_, *army));
    json::Json prompts = json::Json::array();
    for (const PanelPrompt& prompt : panel_.prompts) {
        prompts.push_back(
            json::Json{{"name", prompt.name}, {"version", prompt.version}, {"hash", prompt.hash}});
    }
    json::Json judges = json::Json::array();
    for (std::size_t index = 0; index < engines.size(); ++index) {
        judges.push_back(json::Json{{"role", panel_.judges[index].role},
                                    {"checksum", engines[index]->checksum},
                                    {"holds", engines[index]->ctx_per_slot},  // how paragraphs are cut
                                    {"direct", llm::to_json(engines[index]->direct)}});
    }
    return json::Json{{"panel", panel_.name},
                      {"prompts", std::move(prompts)},
                      {"judges", std::move(judges)},
                      {"read", reading_fingerprint(article_)}};
}

Result<pipeline::StageOutput> JudgementStage::run(const pipeline::StageContext& context) const {
    RR_TRY_ASSIGN(agents::Army* const army, context.army());
    Reader reader{context, panel_, article_, *army};
    return reader.read();
}

}  // namespace rr::eval
