// A video's silver annotation, drafted by models (rr/eval/annotate.hpp).
#include <rr/eval/annotate.hpp>

#include <algorithm>
#include <cstdint>
#include <format>
#include <iterator>
#include <memory>
#include <set>
#include <system_error>
#include <utility>

#include <rr/core/assert.hpp>
#include <rr/core/file_io.hpp>
#include <rr/eval/snapshot.hpp>
#include <rr/knowledge/artifacts.hpp>
#include <rr/library/stage_store.hpp>
#include <rr/llm/memo.hpp>
#include <rr/pipeline/asking.hpp>
#include <rr/pipeline/executor.hpp>
#include <rr/pipeline/source.hpp>
#include <rr/pipeline/transcript_stage.hpp>
#include <rr/pipeline/windows.hpp>
#include <rr/prompt/tokens.hpp>
#include <rr/text/align.hpp>
#include <rr/text/unicode.hpp>

namespace rr::eval {

namespace {

using knowledge::SentenceId;
using knowledge::Transcript;
using pipeline::sentence_lines;
using pipeline::sentence_range;
using pipeline::SentenceWindow;

constexpr std::string_view kAnnotate = "annotate";
constexpr std::string_view kAnnotateMore = "annotate_more";
constexpr std::size_t kMostProblems = 8;  // sent back at once: enough to fix, few enough to read
constexpr std::size_t kRecentJobs = 500;  // where an unfinished annotation is looked for

// Sizing, in tokens as the army's estimator counts them by default: three
// bytes a token, which errs high (rr/prompt/tokens.hpp).
constexpr std::int64_t kBytesPerToken = 3;
constexpr std::int64_t kPromptTokens = 1'200;      // the rest of a prompt: system message, task, context
constexpr std::int64_t kMoreAnswerTokens = 1'024;  // what the other family adds is short
constexpr std::size_t kLeastWindow = 1'500;        // bytes: a smaller window says too little to annotate

bool serves(const agents::Engine& engine, std::string_view role) {
    return std::ranges::contains(engine.roles, role);
}

// The model serving `role`: what drafted_by names.
std::string model_for(const agents::Army& army, std::string_view role) {
    const agents::Engine* engine = pipeline::engine_serving(army, role);
    return engine == nullptr ? std::string{role} : engine->model;
}

// The commander's answer: up to twice the tokens it reads. A dense passage
// gives a fact a sentence, each a quote and a claim of its own, in JSON.
int annotate_answer_tokens(std::size_t window_bytes) {
    const auto read = static_cast<std::int64_t>(window_bytes) / kBytesPerToken;
    return static_cast<int>(std::clamp<std::int64_t>(read * 2, 1'024, 8'192));
}

std::size_t index_of(SentenceId id) {
    return id.number() - 1;
}

std::vector<SentenceId> ids_of(const json::Json& list) {
    std::vector<SentenceId> ids;
    for (const json::Json& id : list) {
        const std::optional<SentenceId> parsed = SentenceId::parse(id.get<std::string>());
        RR_ASSERT(parsed.has_value(), "the answer's schema allows S1, S2, \u2026 only");
        ids.push_back(*parsed);
    }
    std::ranges::sort(ids);
    const auto repeats = std::ranges::unique(ids);
    ids.erase(repeats.begin(), repeats.end());
    return ids;
}

// The sentences of `ids` outside `window`'s own.
std::vector<SentenceId> outside(const std::vector<SentenceId>& ids, const SentenceWindow& window) {
    std::vector<SentenceId> out;
    std::ranges::copy_if(ids, std::back_inserter(out), [&](SentenceId id) {
        return index_of(id) < window.begin || index_of(id) >= window.end;
    });
    return out;
}

// What the sentences `ids`, which are the transcript's, say, joined.
std::string said_by(const std::vector<SentenceId>& ids, const Transcript& transcript) {
    std::string said;
    for (const SentenceId id : ids) {
        RR_ASSERT(index_of(id) < transcript.sentences.size(), "an item cites the transcript's sentences");
        const std::string& text = transcript.sentences[index_of(id)].text;
        said += said.empty() ? text : " " + text;
    }
    return said;
}

// Whether `said` says `quote`, word for word (case, accents and punctuation
// aside). A quote with no word in it says nothing, and never is.
bool verbatim_in(std::string_view quote, std::string_view said) {
    return text::align(quote, said).verbatim();
}

// When the sentences `ids` were said, from the first's start to the last's end.
std::pair<Millis, Millis> span_of(const std::vector<SentenceId>& ids, const Transcript& transcript) {
    RR_ASSERT(!ids.empty(), "an item cites a sentence at least");
    return {transcript.sentences[index_of(ids.front())].span.start(),
            transcript.sentences[index_of(ids.back())].span.end()};
}

std::string listed(const std::vector<SentenceId>& ids) {
    std::string list;
    for (const SentenceId id : ids) {
        list += list.empty() ? id.str() : ", " + id.str();
    }
    return list;
}

// Whether two items are one: they cite a sentence in common, and one's quote
// holds the other's (punctuation and case aside).
bool same_item(const std::vector<SentenceId>& left_ids, std::string_view left_quote,
               const std::vector<SentenceId>& right_ids, std::string_view right_quote) {
    std::vector<SentenceId> common;
    std::ranges::set_intersection(left_ids, right_ids, std::back_inserter(common));
    if (common.empty()) {
        return false;
    }
    const bool left_shorter = left_quote.size() <= right_quote.size();
    return text::align(left_shorter ? left_quote : right_quote, left_shorter ? right_quote : left_quote)
        .verbatim();
}

std::vector<SentenceId> merged(const std::vector<SentenceId>& left, const std::vector<SentenceId>& right) {
    std::vector<SentenceId> both;
    std::ranges::set_union(left, right, std::back_inserter(both));
    return both;
}

std::string folded(std::string_view text) {
    return text::fold(text).value_or(std::string{text});
}

// Adds the terms not yet there, by their folded form; a meaning fills an empty one.
void add_terms(std::vector<Term>& terms, const std::vector<Term>& more) {
    for (const Term& term : more) {
        const auto found =
            std::ranges::find(terms, folded(term.term), [](const Term& kept) { return folded(kept.term); });
        if (found == terms.end()) {
            terms.push_back(term);
        } else if (found->meaning.empty()) {
            found->meaning = term.meaning;
        }
    }
}

void add_quotes(std::vector<SilverQuote>& quotes, const std::vector<DraftQuote>& more,
                const Transcript& transcript) {
    for (const DraftQuote& quote : more) {
        const auto same = std::ranges::find_if(quotes, [&](const SilverQuote& kept) {
            return same_item(kept.sentences, kept.quote, quote.sentences, quote.quote);
        });
        if (same == quotes.end()) {
            const auto [start, end] = span_of(quote.sentences, transcript);
            quotes.push_back(
                SilverQuote{.sentences = quote.sentences, .start = start, .end = end, .quote = quote.quote});
        }
    }
}

template <typename Item>
void in_order_said(std::vector<Item>& items) {
    std::ranges::stable_sort(items, [](const Item& left, const Item& right) {
        return std::pair{left.start, left.end} < std::pair{right.start, right.end};
    });
}

json::Json ids_json(const std::vector<SentenceId>& ids) {
    json::Json list = json::Json::array();
    for (const SentenceId id : ids) {
        list.push_back(id.str());
    }
    return list;
}

std::vector<SentenceId> ids_from_artifact(const json::Json& list) {
    std::vector<SentenceId> ids;
    for (const json::Json& id : list) {
        const std::optional<SentenceId> parsed = SentenceId::parse(id.get<std::string>());
        RR_ASSERT(parsed.has_value(), "silver's schema allows S1, S2, \u2026 only");
        ids.push_back(*parsed);
    }
    return ids;
}

// The facts found so far that rest on `window`'s sentences, as the other
// family is shown them: "- [S12, S13] claim".
std::string found_lines(const Silver& silver, const SentenceWindow& window) {
    std::string lines;
    for (const SilverFact& fact : silver.facts) {
        const bool here = std::ranges::any_of(fact.sentences, [&](SentenceId id) {
            return index_of(id) >= window.begin && index_of(id) < window.end;
        });
        if (here) {
            lines +=
                std::format("{}- [{}] {}", lines.empty() ? "" : "\n", listed(fact.sentences), fact.claim);
        }
    }
    return lines.empty() ? std::string{"(none)"} : lines;
}

json::Json order_for(const Transcript& transcript, const SentenceWindow& window) {
    return json::Json{{"before", sentence_lines(transcript, window.context_begin, window.begin)},
                      {"sentences", sentence_lines(transcript, window.begin, window.end)},
                      {"after", sentence_lines(transcript, window.end, window.context_end)}};
}

// Who reads a window, and how.
struct Reader {
    std::string_view role;
    bool thinking = false;
    int samples = 1;
    int answer_tokens = 0;
};

Result<agents::Mission> mission_for(const pipeline::StageContext& context,
                                    const pipeline::StagePrompt& prompt, const json::Json& shared,
                                    const json::Json& order, const Transcript& transcript,
                                    const SentenceWindow& window, std::string id, const Reader& reader) {
    RR_TRY_ASSIGN(const prompt::Rendered rendered, context.prompts->render(prompt.info->name, shared, order));
    const std::int64_t seed = llm::derived_seed(context.job->salt, AnnotateStage::kName, id, 0);
    return agents::Mission{
        .id = std::move(id),
        .kind = prompt.info->name,
        .prompt = prompt.info->label(),
        .role = std::string{reader.role},
        .request = llm::ChatRequest{.messages = {llm::Message::system(rendered.system),
                                                 llm::Message::user(rendered.user)},
                                    .seed = seed,
                                    .thinking = reader.thinking},
        .answer_tokens = reader.answer_tokens,
        .schema = prompt.schema,
        .check = [&transcript,
                  window](const json::Json& answer) { return draft_problems(answer, transcript, window); },
        .samples = reader.samples};
}

// The windows the readings left without a draft, and why: published as
// findings in the order said, one reading after the other.
class Unread {
public:
    Unread(const Transcript& transcript, const agents::Army& army) : transcript_(&transcript), army_(&army) {}

    // Each window of `plan` too long for the engine serving `role`.
    void too_long(const pipeline::WindowPlan& plan, std::string_view role) {
        for (const SentenceWindow& window : plan.too_long) {
            missed_.emplace_back(window, std::format("even alone, with its context and an answer, it is more "
                                                     "than {} holds at once",
                                                     model_for(*army_, role)));
        }
    }

    // `window`, whose mission ended with `outcome`: noted unless accepted.
    void ended(const SentenceWindow& window, const agents::Outcome& outcome) {
        if (outcome.verdict == agents::Verdict::accepted) {
            return;
        }
        missed_.emplace_back(window, pipeline::why_unanswered(outcome, *army_, "annotate them"));
        ceiling_ = ceiling_ || outcome.verdict == agents::Verdict::ceiling;
    }

    // The windows noted since the last call, as `output`'s findings. After a
    // ceiling, the result is its job's alone: a job allowed more would have
    // drafted them.
    void publish(pipeline::StageOutput& output) {
        std::ranges::stable_sort(missed_, {}, [](const auto& entry) { return entry.first.begin; });
        for (const auto& [window, why] : missed_) {
            output.findings.push_back(std::format(
                "{} no draft: {}", pipeline::sentences_have(*transcript_, window.begin, window.end), why));
        }
        missed_.clear();
        if (ceiling_) {
            output.reusable = false;
        }
    }

private:
    const Transcript* transcript_;
    const agents::Army* army_;
    std::vector<std::pair<SentenceWindow, std::string>> missed_;
    bool ceiling_ = false;
};

// Runs `plan`'s missions; notes in `unread` the windows they left without a
// draft; returns every accepted answer as a draft, window by window.
Result<std::vector<Draft>> drafts_of(pipeline::WindowPlan plan, agents::Army& army,
                                     const std::stop_token& stop, Unread& unread) {
    RR_TRY_ASSIGN(const std::vector<agents::Outcome> outcomes, army.squad(std::move(plan.missions), stop));
    std::vector<Draft> drafts;
    for (std::size_t index = 0; index < plan.windows.size(); ++index) {
        unread.ended(plan.windows[index], outcomes[index]);
        for (const agents::Answer& answer : outcomes[index].answers) {
            drafts.push_back(draft_from_json(answer.value));
        }
    }
    return drafts;
}

}  // namespace

WindowSizes window_sizes(const agents::Army& army) {
    const auto bytes = [](std::int64_t tokens, std::size_t most) {
        return std::clamp(static_cast<std::size_t>(std::max<std::int64_t>(tokens, 0) * kBytesPerToken),
                          kLeastWindow, most);
    };
    WindowSizes sizes;
    for (const agents::Engine& engine : army.engines()) {
        const std::int64_t room = engine.ctx_per_slot - kPromptTokens;
        if (serves(engine, "commander")) {
            // After its thinking, a third of the room for the window and two
            // for its answer (annotate_answer_tokens()).
            const std::optional<llm::ThinkingBudget>& budget = engine.thinking.thinking_budget;
            const std::int64_t thinking =
                budget.has_value() ? budget->tokens + prompt::TokenEstimator{}.estimate(budget->closing) : 0;
            sizes.annotate = bytes((room - thinking) / 3, sizes.annotate);
        }
        if (serves(engine, "infantry")) {
            // After its answer, half the room for the window and half for the
            // facts found in it.
            sizes.more = bytes((room - kMoreAnswerTokens) / 2, sizes.more);
        }
    }
    return sizes;
}

Draft draft_from_json(const json::Json& answer) {
    Draft draft;
    for (const json::Json& fact : answer.at("facts")) {
        draft.facts.push_back(DraftFact{.sentences = ids_of(fact.at("sentences")),
                                        .quote = fact.at("quote").get<std::string>(),
                                        .claim = fact.at("claim").get<std::string>(),
                                        .importance = fact.at("importance").get<std::string>() == "core"
                                                          ? Importance::core
                                                          : Importance::detail});
    }
    for (const json::Json& quote : answer.at("must_quotes")) {
        draft.must_quotes.push_back(DraftQuote{.sentences = ids_of(quote.at("sentences")),
                                               .quote = quote.at("quote").get<std::string>()});
    }
    for (const json::Json& term : answer.at("terms")) {
        draft.terms.push_back(Term{.term = term.at("term").get<std::string>(),
                                   .meaning = term.at("meaning").get<std::string>()});
    }
    return draft;
}

std::vector<std::string> draft_problems(const json::Json& answer, const Transcript& transcript,
                                        const SentenceWindow& window) {
    std::vector<std::string> problems;
    std::size_t more = 0;
    const auto note = [&](std::string problem) {
        if (problems.size() < kMostProblems) {
            problems.push_back(std::move(problem));
        } else {
            ++more;
        }
    };
    const std::string own = sentence_range(window.begin, window.end);
    const auto check = [&](std::string_view what, std::size_t number, const json::Json& item) {
        const std::vector<SentenceId> ids = ids_of(item.at("sentences"));
        const std::string quote = item.at("quote").get<std::string>();
        if (const std::vector<SentenceId> stray = outside(ids, window); !stray.empty()) {
            note(std::format("{} {}: cite only the sentences to annotate, {}, not {}", what, number, own,
                             listed(stray)));
        } else if (!verbatim_in(quote, said_by(ids, transcript))) {
            const bool one = ids.size() == 1;
            note(std::format("{} {}: \"{}\" is not what {} {}, word for word: copy the quote from {}", what,
                             number, quote, listed(ids), one ? "says" : "say", one ? "it" : "them"));
        }
    };
    std::size_t number = 0;
    for (const json::Json& fact : answer.at("facts")) {
        check("fact", ++number, fact);
    }
    number = 0;
    for (const json::Json& quote : answer.at("must_quotes")) {
        check("must-quote", ++number, quote);
    }
    std::string window_text;
    for (std::size_t index = window.begin; index < window.end; ++index) {
        window_text += " " + transcript.sentences[index].text;
    }
    for (const json::Json& term : answer.at("terms")) {
        if (const std::string written = term.at("term").get<std::string>();
            !verbatim_in(written, window_text)) {
            note(std::format("term \"{}\" is not said in {}: list only the video's own words", written, own));
        }
    }
    if (more > 0) {
        problems.push_back(std::format("and {} more like them", more));
    }
    return problems;
}

Silver join_drafts(const std::vector<Draft>& drafts, const Transcript& transcript) {
    struct Joined {
        SilverFact fact;
        std::set<std::size_t> drafts;
        int core = 0;  // the drafts that call it core
    };
    std::vector<Joined> joined;
    Silver silver;
    for (std::size_t index = 0; index < drafts.size(); ++index) {
        for (const DraftFact& fact : drafts[index].facts) {
            const auto same = std::ranges::find_if(joined, [&](const Joined& kept) {
                return same_item(kept.fact.sentences, kept.fact.quote, fact.sentences, fact.quote);
            });
            if (same == joined.end()) {
                const auto [start, end] = span_of(fact.sentences, transcript);
                joined.push_back(Joined{.fact = SilverFact{.sentences = fact.sentences,
                                                           .start = start,
                                                           .end = end,
                                                           .quote = fact.quote,
                                                           .claim = fact.claim,
                                                           .importance = fact.importance},
                                        .drafts = {index},
                                        .core = fact.importance == Importance::core ? 1 : 0});
                continue;
            }
            same->fact.sentences = merged(same->fact.sentences, fact.sentences);
            std::tie(same->fact.start, same->fact.end) = span_of(same->fact.sentences, transcript);
            if (same->drafts.insert(index).second && fact.importance == Importance::core) {
                ++same->core;
            }
        }
        add_quotes(silver.must_quotes, drafts[index].must_quotes, transcript);
        add_terms(silver.terms, drafts[index].terms);
    }
    for (Joined& kept : joined) {
        kept.fact.found = static_cast<int>(kept.drafts.size());
        kept.fact.importance = 2 * kept.core >= kept.fact.found ? Importance::core : Importance::detail;
        silver.facts.push_back(std::move(kept.fact));
    }
    in_order_said(silver.facts);
    in_order_said(silver.must_quotes);
    return silver;
}

void add_missed(Silver& silver, const Draft& more, const Transcript& transcript) {
    for (const DraftFact& fact : more.facts) {
        const bool known = std::ranges::any_of(silver.facts, [&](const SilverFact& kept) {
            return same_item(kept.sentences, kept.quote, fact.sentences, fact.quote);
        });
        if (!known) {
            const auto [start, end] = span_of(fact.sentences, transcript);
            silver.facts.push_back(SilverFact{.sentences = fact.sentences,
                                              .start = start,
                                              .end = end,
                                              .quote = fact.quote,
                                              .claim = fact.claim,
                                              .importance = fact.importance,
                                              .found = 0});
        }
    }
    add_quotes(silver.must_quotes, more.must_quotes, transcript);
    add_terms(silver.terms, more.terms);
    in_order_said(silver.facts);
    in_order_said(silver.must_quotes);
}

json::Json to_json(const Silver& silver) {
    json::Json facts = json::Json::array();
    for (const SilverFact& fact : silver.facts) {
        facts.push_back(json::Json{{"sentences", ids_json(fact.sentences)},
                                   {"start_ms", fact.start.count()},
                                   {"end_ms", fact.end.count()},
                                   {"quote", fact.quote},
                                   {"claim", fact.claim},
                                   {"importance", std::string{to_string(fact.importance)}},
                                   {"found", fact.found}});
    }
    json::Json quotes = json::Json::array();
    for (const SilverQuote& quote : silver.must_quotes) {
        quotes.push_back(json::Json{{"sentences", ids_json(quote.sentences)},
                                    {"start_ms", quote.start.count()},
                                    {"end_ms", quote.end.count()},
                                    {"quote", quote.quote}});
    }
    json::Json terms = json::Json::array();
    for (const Term& term : silver.terms) {
        terms.push_back(json::Json{{"term", term.term}, {"meaning", term.meaning}});
    }
    return json::Json{{"facts", std::move(facts)},
                      {"must_quotes", std::move(quotes)},
                      {"terms", std::move(terms)},
                      {"drafted_by", silver.drafted_by}};
}

Result<Silver> silver_from_json(const json::Json& document) {
    if (Result<void> valid = knowledge::artifact_schemas().validate("artifacts/silver", document);
        !valid.has_value()) {
        return fail(Errc::invalid_argument,
                    std::format("not a silver annotation: {}", valid.error().message()));
    }
    Silver silver{.drafted_by = document.at("drafted_by").get<std::string>()};
    for (const json::Json& fact : document.at("facts")) {
        silver.facts.push_back(SilverFact{.sentences = ids_from_artifact(fact.at("sentences")),
                                          .start = Millis{fact.at("start_ms").get<std::int64_t>()},
                                          .end = Millis{fact.at("end_ms").get<std::int64_t>()},
                                          .quote = fact.at("quote").get<std::string>(),
                                          .claim = fact.at("claim").get<std::string>(),
                                          .importance = fact.at("importance").get<std::string>() == "core"
                                                            ? Importance::core
                                                            : Importance::detail,
                                          .found = fact.at("found").get<int>()});
    }
    for (const json::Json& quote : document.at("must_quotes")) {
        silver.must_quotes.push_back(SilverQuote{.sentences = ids_from_artifact(quote.at("sentences")),
                                                 .start = Millis{quote.at("start_ms").get<std::int64_t>()},
                                                 .end = Millis{quote.at("end_ms").get<std::int64_t>()},
                                                 .quote = quote.at("quote").get<std::string>()});
    }
    for (const json::Json& term : document.at("terms")) {
        silver.terms.push_back(Term{.term = term.at("term").get<std::string>(),
                                    .meaning = term.at("meaning").get<std::string>()});
    }
    return silver;
}

Gold silver_to_gold(const Silver& silver, std::string video, std::string snapshot) {
    Gold gold{.video = std::move(video),
              .snapshot = std::move(snapshot),
              .status = GoldStatus::silver,
              .drafted_by = silver.drafted_by,
              .terms = silver.terms};
    for (std::size_t index = 0; index < silver.facts.size(); ++index) {
        const SilverFact& fact = silver.facts[index];
        gold.facts.push_back(Fact{.id = std::format("F{}", index + 1),
                                  .start = fact.start,
                                  .end = fact.end,
                                  .importance = fact.importance,
                                  .quote = fact.quote,
                                  .claim = fact.claim,
                                  .found = fact.found});
    }
    for (const SilverQuote& quote : silver.must_quotes) {
        gold.must_quotes.push_back(MustQuote{.start = quote.start, .end = quote.end, .quote = quote.quote});
    }
    return gold;
}

Result<json::Json> AnnotateStage::fingerprint(const pipeline::StageContext& context) const {
    if (!context.army) {
        return fail(Errc::needs_user,
                    "drafting an annotation needs models, and no pod is configured: rreport doctor says how");
    }
    RR_ASSERT(context.prompts != nullptr, "a stage that asks models is given the prompts");
    RR_TRY_ASSIGN(json::Json prompts,
                  pipeline::prompts_fingerprint(*context.prompts, {kAnnotate, kAnnotateMore}));
    RR_TRY_ASSIGN(agents::Army* const army, context.army());
    const WindowSizes sizes = window_sizes(*army);
    const auto reading = [&](std::size_t bytes, std::string_view role) {
        const agents::Engine* engine = pipeline::engine_serving(*army, role);
        return json::Json{{"bytes", bytes}, {"holds", engine == nullptr ? 0 : engine->ctx_per_slot}};
    };
    return json::Json{
        {"prompts", std::move(prompts)},
        {"army", pipeline::army_fingerprint(*army)},
        {"windows", json::Json{{"annotate", reading(sizes.annotate, "commander")},
                               {"more", reading(sizes.more, "infantry")},
                               {"context", json::Json{{"sentences", pipeline::kContextSentences},
                                                      {"bytes", pipeline::kContextBytes}}}}},
        {"samples", kSamples}};
}

Result<pipeline::StageOutput> AnnotateStage::run(const pipeline::StageContext& context) const {
    RR_TRY_ASSIGN(const pipeline::Source source, pipeline::source_from_json(context.inputs.at("source")));
    RR_TRY_ASSIGN(const Transcript transcript,
                  knowledge::transcript_from_json(context.inputs.at("transcript")));
    RR_TRY_ASSIGN(agents::Army* const army, context.army());
    RR_TRY_ASSIGN(const pipeline::StagePrompt annotate, pipeline::stage_prompt(*context.prompts, kAnnotate));
    RR_TRY_ASSIGN(const pipeline::StagePrompt more, pipeline::stage_prompt(*context.prompts, kAnnotateMore));
    const json::Json& windows = context.fingerprint.at("windows");
    const json::Json shared{{"title", source.video.title}, {"channel", source.video.channel}};
    pipeline::StageOutput output;
    Unread unread{transcript, *army};

    // The commander's three drafts of each window, joined.
    RR_TRY_ASSIGN(pipeline::WindowPlan drafted,
                  pipeline::plan_windows(transcript, windows.at("annotate").at("bytes").get<std::size_t>(),
                                         *army, [&](const SentenceWindow& window, std::size_t number) {
                                             return mission_for(
                                                 context, annotate, shared, order_for(transcript, window),
                                                 transcript, window, std::format("annotate-{}", number),
                                                 Reader{.role = "commander",
                                                        .thinking = true,
                                                        .samples = kSamples,
                                                        .answer_tokens = annotate_answer_tokens(
                                                            pipeline::lines_bytes(transcript, window))});
                                         }));
    unread.too_long(drafted, "commander");
    if (context.progress) {
        context.progress(std::format("annotate: the commander drafts {} windows, {} times each",
                                     drafted.missions.size(), kSamples));
    }
    RR_TRY_ASSIGN(const std::vector<Draft> drafts,
                  drafts_of(std::move(drafted), *army, context.stop, unread));
    unread.publish(output);
    Silver silver = join_drafts(drafts, transcript);

    // The other family's reading, with the facts found so far.
    RR_TRY_ASSIGN(
        pipeline::WindowPlan reread,
        pipeline::plan_windows(
            transcript, windows.at("more").at("bytes").get<std::size_t>(), *army,
            [&](const SentenceWindow& window, std::size_t number) {
                json::Json order = order_for(transcript, window);
                order["found"] = found_lines(silver, window);
                return mission_for(
                    context, more, shared, order, transcript, window, std::format("annotate-more-{}", number),
                    Reader{.role = "infantry", .answer_tokens = static_cast<int>(kMoreAnswerTokens)});
            }));
    unread.too_long(reread, "infantry");
    if (context.progress) {
        context.progress(
            std::format("annotate: the other family reads {} windows again, with the {} facts found",
                        reread.missions.size(), silver.facts.size()));
    }
    RR_TRY_ASSIGN(const std::vector<Draft> additions,
                  drafts_of(std::move(reread), *army, context.stop, unread));
    for (const Draft& addition : additions) {
        add_missed(silver, addition, transcript);
    }
    unread.publish(output);
    silver.drafted_by =
        std::format("{} \u00D7{} on {}; {} on {}", annotate.info->label(), kSamples,
                    model_for(*army, "commander"), more.info->label(), model_for(*army, "infantry"));
    output.artifact = to_json(silver);
    return output;
}

Result<library::Job> start_annotation(const std::filesystem::path& eval_root, const EvalVideo& video,
                                      db::Database& database, const Clock& clock, bool replace) {
    RR_TRY_ASSIGN(const std::optional<Snapshot> snapshot, read_snapshot(eval_root, video));
    if (!snapshot.has_value()) {
        return fail(Errc::not_found,
                    std::format("no snapshot yet: `rreport eval fetch {}` reads it first", video.id));
    }
    const std::filesystem::path file = gold_file(eval_root, video.id);
    if (Result<std::string> text = read_file(file); text.has_value()) {
        RR_TRY_ASSIGN(const Gold existing, parse_gold(*text, file.string()));
        if (existing.status == GoldStatus::gold) {
            return fail(Errc::conflict,
                        std::format("{} is reviewed gold: a draft never replaces it", file.string()));
        }
        if (!replace) {
            return fail(Errc::conflict,
                        std::format("{} exists: --replace drafts it again, and loses what was "
                                    "corrected in it",
                                    file.string()));
        }
    } else if (text.error().code() != Errc::not_found) {
        return std::unexpected(std::move(text).error());
    }
    const std::string source = std::filesystem::absolute(snapshot_file(eval_root, video.id)).string();
    library::Jobs jobs{database, clock};
    RR_TRY_ASSIGN(const std::vector<library::Job> recent, jobs.recent(kRecentJobs));
    for (const library::Job& job : recent) {
        const bool unfinished =
            job.status != library::JobStatus::done && job.status != library::JobStatus::failed;
        if (job.kind == library::JobKind::annotation && job.source == source && unfinished) {
            return job;  // taken up again: its memo answers every call it made
        }
    }
    return jobs.create(source, "max", 0, library::JobKind::annotation);
}

Result<Annotated> run_annotation(const std::filesystem::path& eval_root, const EvalVideo& video,
                                 library::Job job, db::Database& database, const Clock& clock,
                                 pipeline::StageContext context) {
    RR_TRY_ASSIGN(const std::optional<Snapshot> snapshot, read_snapshot(eval_root, video));
    if (!snapshot.has_value()) {
        return fail(Errc::not_found, std::format("{} has no snapshot", video.id));
    }
    std::vector<std::unique_ptr<pipeline::Stage>> stages;
    stages.push_back(std::make_unique<pipeline::SourceStage>());
    stages.push_back(std::make_unique<pipeline::TranscriptStage>());
    stages.push_back(std::make_unique<AnnotateStage>());
    RR_TRY_ASSIGN(const pipeline::Executor executor, pipeline::Executor::create(std::move(stages), clock));
    context.work_directory /= job.id;
    RR_TRY_ASSIGN(pipeline::JobRun run,
                  pipeline::run_job(std::move(job), executor, database, clock, std::move(context)));
    Annotated annotated{.run = std::move(run)};
    if (annotated.run.job.status != library::JobStatus::done) {
        return annotated;
    }
    const auto drafted = std::ranges::find(annotated.run.report.stages, std::string{AnnotateStage::kName},
                                           &pipeline::StageReport::stage);
    RR_ASSERT(drafted != annotated.run.report.stages.end(), "a job done has done its every stage");
    const library::StageStore store{database, clock};
    RR_TRY_ASSIGN(const json::Json artifact, store.artifact(drafted->artifact));
    RR_TRY_ASSIGN(const Silver silver, silver_from_json(artifact));
    Gold gold = silver_to_gold(silver, video.id, snapshot->hash);
    const std::filesystem::path file = gold_file(eval_root, video.id);
    if (std::error_code error; !std::filesystem::create_directories(file.parent_path(), error) && error) {
        return fail(Errc::io,
                    std::format("cannot make '{}': {}", file.parent_path().string(), error.message()));
    }
    RR_TRY(write_file_atomically(file, to_toml(gold)));
    annotated.gold = std::move(gold);
    return annotated;
}

}  // namespace rr::eval
