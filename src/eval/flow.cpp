// Which of two articles of one video reads better (rr/eval/flow.hpp).
#include <rr/eval/flow.hpp>

#include <algorithm>
#include <format>
#include <memory>
#include <numeric>
#include <optional>
#include <span>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include <rr/agents/army.hpp>
#include <rr/agents/mission.hpp>
#include <rr/core/error.hpp>
#include <rr/core/time_span.hpp>
#include <rr/knowledge/artifacts.hpp>
#include <rr/library/jobs.hpp>
#include <rr/llm/memo.hpp>
#include <rr/pipeline/asking.hpp>
#include <rr/pipeline/executor.hpp>
#include <rr/report/contents.hpp>

#include "markdown.hpp"

namespace rr::eval {

namespace {

using knowledge::SentenceId;

constexpr std::string_view kFlowPrompt = "eval_flow";

// A judge's reasons, then its verdict: a few sentences.
constexpr int kFlowAnswer = 600;

// The judges answer as the panel froze them: their own engine, never
// thinking, never escalated (as rr/eval/judgement.cpp asks them).
constexpr agents::Escalation kFrozen{.thinking = false, .commander = false, .other_family = false};

std::string_view preference_name(reason::Preference preference) noexcept {
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

std::optional<reason::Preference> preference_named(std::string_view name) noexcept {
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

// Where `sentence` lies on the video: its atoms' sentences, from the first
// to the last; none for a sentence citing nothing the ledger has.
void stretch_by(const knowledge::CitingSentence& sentence, const knowledge::AtomLedger& ledger,
                ArticleSection& section) {
    for (const knowledge::Marker& marker : sentence.markers) {
        const knowledge::Atom* atom = ledger.find(marker.atom);
        if (atom == nullptr) {
            continue;  // a marker of no atom: the ledger's check names it
        }
        for (const SentenceId said : atom->sentences) {
            section.first = section.first.has_value() ? std::min(*section.first, said) : said;
            section.last = section.last.has_value() ? std::max(*section.last, said) : said;
        }
    }
}

// Where each section lies, for the joining: its own stretch; the last
// sentence before it when it cites nothing; the first after it for one
// before any that cites.
struct Placed {
    SentenceId first;
    SentenceId last;
    bool in_a = false;
    std::size_t index = 0;
};

void place(const std::vector<ArticleSection>& sections, bool in_a, std::vector<Placed>& placed) {
    std::optional<SentenceId> before;
    std::vector<std::size_t> leading;  // sections citing nothing, before any that cites
    for (std::size_t index = 0; index < sections.size(); ++index) {
        const ArticleSection& section = sections[index];
        if (section.first.has_value() && section.last.has_value()) {
            for (const std::size_t waiting : leading) {
                placed.push_back(
                    Placed{.first = *section.first, .last = *section.first, .in_a = in_a, .index = waiting});
            }
            leading.clear();
            placed.push_back(
                Placed{.first = *section.first, .last = *section.last, .in_a = in_a, .index = index});
            before = section.last;
        } else if (before.has_value()) {
            placed.push_back(Placed{.first = *before, .last = *before, .in_a = in_a, .index = index});
        } else {
            leading.push_back(index);
        }
    }
}

// The text a judge is shown of `indices` of `sections`.
std::string text_of(const std::vector<ArticleSection>& sections, std::span<const std::size_t> indices) {
    std::string text;
    for (const std::size_t index : indices) {
        text += std::format("{}{}", text.empty() ? "" : "\n\n", sections[index].text);
    }
    return text;
}

// A reading with one order answered and the other not is no reading, and
// says why: a preference counts only when it survives the swap.
json::Json reading_to_json(const PartReading& reading) {
    json::Json read{{"judge", reading.judge}};
    if (reading.a_first.has_value() && reading.b_first.has_value()) {
        read["a_first"] = preference_name(*reading.a_first);
        read["b_first"] = preference_name(*reading.b_first);
        read["reasons"] = reading.reasons;
    } else {
        read["unread"] = reading.unread;
    }
    return read;
}

Result<PartReading> reading_from_json(const json::Json& read) {
    PartReading reading{.judge = read.at("judge").get<std::string>()};
    if (read.contains("a_first")) {
        reading.a_first = preference_named(read.at("a_first").get<std::string>());
        reading.b_first = preference_named(read.at("b_first").get<std::string>());
        reading.reasons = read.at("reasons").get<std::string>();
    } else if (read.contains("unread")) {
        reading.unread = read.at("unread").get<std::string>();
    } else {
        return fail(Errc::invalid_argument,
                    std::format("not a pairing: a reading by the {} has no preference and says not why",
                                reading.judge));
    }
    return reading;
}

// A run's entries of each video, by replicate.
std::map<std::string, std::vector<RunEntry>, std::less<>> entries_by_video(
    std::span<const ScoredEntry> scored) {
    std::map<std::string, std::vector<RunEntry>, std::less<>> videos;
    for (const ScoredEntry& one : scored) {
        videos[one.entry.video].push_back(one.entry);
    }
    for (auto& [video, entries] : videos) {
        std::ranges::sort(entries, {}, &RunEntry::replicate);
    }
    return videos;
}

json::Json reading_fingerprint(const ArticlesToCompare& articles) {
    return json::Json{
        {"video", json::Json{{"title", articles.title}, {"channel", articles.channel}}},
        {"transcript", json::content_hash(knowledge::to_json(articles.transcript)).hex()},
        {"a", json::Json{{"atoms", json::content_hash(knowledge::to_json(articles.a_ledger)).hex()},
                         {"article", json::content_hash(knowledge::to_json(articles.a)).hex()}}},
        {"b", json::Json{{"atoms", json::content_hash(knowledge::to_json(articles.b_ledger)).hex()},
                         {"article", json::content_hash(knowledge::to_json(articles.b)).hex()}}}};
}

// One mission of the pairing, and where its answer goes.
struct Order {
    agents::Mission mission{};
    std::size_t part = 0;   // its index in the pairing
    std::size_t judge = 0;  // its index in the panel
    bool a_first = true;    // A's version shown first
};

// Asks the panel's judges, part by part, in both orders, and keeps what
// they answer.
class Comparer {
public:
    Comparer(const pipeline::StageContext& context, const Panel& panel, const ArticlesToCompare& articles,
             agents::Army& army)
        : context_(&context)
        , panel_(&panel)
        , articles_(&articles)
        , army_(&army)
        , a_(article_sections(articles.a, articles.a_ledger))
        , b_(article_sections(articles.b, articles.b_ledger))
        , shared_(json::Json{{"title", articles.title}, {"channel", articles.channel}}) {}

    Result<pipeline::StageOutput> read() {
        RR_TRY_ASSIGN(prompt_, pipeline::stage_prompt(*context_->prompts, kFlowPrompt));
        pairing_.panel = panel_->name;
        for (const PanelJudge& judge : panel_->judges) {
            pairing_.judges.push_back(judge.role);
        }
        for (const Part& part : parts_of(a_, b_)) {
            RR_TRY(order_part(part));
        }
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
        return pipeline::StageOutput{.artifact = to_json(pairing_), .findings = std::move(findings_)};
    }

private:
    // "the stretch of the video from 1:30 to 4:10"
    [[nodiscard]] std::string stretch_named(const Part& part) const {
        const knowledge::Sentence* first = articles_->transcript.find(part.first);
        const knowledge::Sentence* last = articles_->transcript.find(part.last);
        RR_ASSERT(first != nullptr, "an atom's sentences are the transcript's");
        RR_ASSERT(last != nullptr, "an atom's sentences are the transcript's");
        return std::format("the stretch of the video from {} to {}", format_timestamp(first->span.start()),
                           format_timestamp(last->span.end()));
    }

    Result<void> order_part(const Part& part) {
        const std::size_t index = pairing_.parts.size();
        pairing_.parts.push_back(PairedPart{.part = part});
        const std::string what = stretch_named(part);
        const std::string a_text = text_of(a_, part.a);
        const std::string b_text = text_of(b_, part.b);
        for (std::size_t judge = 0; judge < panel_->judges.size(); ++judge) {
            const std::string& role = panel_->judges[judge].role;
            pairing_.parts.back().readings.push_back(PartReading{.judge = role});
            std::vector<Order> both;
            for (const bool a_first : {true, false}) {
                const json::Json order{{"what", what},
                                       {"first", a_first ? a_text : b_text},
                                       {"second", a_first ? b_text : a_text}};
                RR_TRY_ASSIGN(const prompt::Rendered rendered,
                              context_->prompts->render(kFlowPrompt, shared_, order));
                both.push_back(
                    Order{.mission = ask(std::format("{}-{}, {} first, by the {}", part.first.str(),
                                                     part.last.str(), a_first ? "A" : "B", role),
                                         rendered, role),
                          .part = index,
                          .judge = judge,
                          .a_first = a_first});
            }
            if (!std::ranges::all_of(both, [&](const Order& order) { return army_->holds(order.mission); })) {
                pairing_.parts.back().readings.back().unread =
                    std::format("the {}'s engine holds no mission reading it", role);
                findings_.push_back(std::format("{} to {}: {}", part.first.str(), part.last.str(),
                                                pairing_.parts.back().readings.back().unread));
                continue;
            }
            for (Order& order : both) {
                orders_.push_back(std::move(order));
            }
        }
        return ok();
    }

    [[nodiscard]] agents::Mission ask(std::string id, const prompt::Rendered& rendered,
                                      const std::string& role) const {
        const std::int64_t seed = llm::derived_seed(context_->job->salt, FlowStage::kName, id, 0);
        return agents::Mission{.id = std::move(id),
                               .kind = std::string{kFlowPrompt},
                               .prompt = prompt_.info->label(),
                               .role = role,
                               .request = llm::ChatRequest{.messages = {llm::Message::system(rendered.system),
                                                                        llm::Message::user(rendered.user)},
                                                           .temperature = 0.0,  // greedy, as a frozen judge
                                                           .seed = seed,
                                                           .thinking = false},
                               .answer_tokens = kFlowAnswer,
                               .schema = prompt_.schema,
                               .escalation = kFrozen};
    }

    void keep(const Order& order, const agents::Outcome& outcome) {
        PartReading& reading = pairing_.parts[order.part].readings[order.judge];
        if (outcome.verdict != agents::Verdict::accepted || outcome.answers.empty()) {
            if (reading.unread.empty()) {
                reading.unread = pipeline::why_unanswered(outcome, *army_, "read it");
                const Part& part = pairing_.parts[order.part].part;
                findings_.push_back(std::format("{} to {}: the {} could not read it: {}", part.first.str(),
                                                part.last.str(), reading.judge, reading.unread));
            }
            return;
        }
        const json::Json& answer = outcome.answers.front().value;
        const std::optional<reason::Preference> better =
            preference_named(answer.at("better").get<std::string>());
        RR_ASSERT(better.has_value(), "eval_flow's schema allows first, second and tie only");
        (order.a_first ? reading.a_first : reading.b_first) = better;
        reading.reasons += std::format("{}{} first: {}", reading.reasons.empty() ? "" : "\n",
                                       order.a_first ? "A" : "B", answer.at("reasons").get<std::string>());
    }

    const pipeline::StageContext* context_;
    const Panel* panel_;
    const ArticlesToCompare* articles_;
    agents::Army* army_;
    std::vector<ArticleSection> a_;
    std::vector<ArticleSection> b_;
    json::Json shared_;
    pipeline::StagePrompt prompt_{};
    Pairing pairing_{};
    std::vector<Order> orders_{};
    std::vector<std::string> findings_{};
};

}  // namespace

std::vector<ArticleSection> article_sections(const knowledge::Article& article,
                                             const knowledge::AtomLedger& ledger) {
    const knowledge::ParsedArticle parsed = knowledge::parse_article(article.markdown);
    std::vector<ArticleSection> sections;
    for (const knowledge::Block& block : parsed.blocks) {
        const std::string_view lines =
            std::string_view{article.markdown}.substr(block.span.begin, block.span.size());
        if (block.kind == knowledge::BlockKind::heading) {
            sections.push_back(
                ArticleSection{.heading = detail::heading_text(lines), .text = std::string{lines}});
            while (!sections.back().text.empty() && sections.back().text.back() == '\n') {
                sections.back().text.pop_back();
            }
            continue;
        }
        if (sections.empty()) {
            sections.emplace_back();  // prose before any heading: a section without one
        }
        ArticleSection& section = sections.back();
        std::string said = knowledge::without_markers(article.markdown, parsed, block.span);
        while (!said.empty() && (said.back() == '\n' || said.back() == ' ')) {
            said.pop_back();
        }
        section.text += std::format("{}{}", section.text.empty() ? "" : "\n\n", said);
        for (const knowledge::CitingSentence& sentence : block.sentences) {
            stretch_by(sentence, ledger, section);
        }
    }
    return sections;
}

std::vector<Part> parts_of(const std::vector<ArticleSection>& a, const std::vector<ArticleSection>& b) {
    std::vector<Placed> placed;
    place(a, true, placed);
    place(b, false, placed);
    std::ranges::sort(placed, [](const Placed& left, const Placed& right) {
        return std::tuple{left.first, !left.in_a, left.index} <
               std::tuple{right.first, !right.in_a, right.index};
    });
    std::vector<Part> parts;
    for (const Placed& one : placed) {
        if (!parts.empty() && one.first <= parts.back().last) {
            parts.back().last = std::max(parts.back().last, one.last);
        } else {
            parts.push_back(Part{.first = one.first, .last = one.last});
        }
        (one.in_a ? parts.back().a : parts.back().b).push_back(one.index);
    }
    std::erase_if(parts, [](const Part& part) { return part.a.empty() || part.b.empty(); });
    for (Part& part : parts) {
        std::ranges::sort(part.a);
        std::ranges::sort(part.b);
    }
    return parts;
}

json::Json to_json(const Pairing& pairing) {
    json::Json parts = json::Json::array();
    for (const PairedPart& paired : pairing.parts) {
        json::Json readings = json::Json::array();
        for (const PartReading& reading : paired.readings) {
            readings.push_back(reading_to_json(reading));
        }
        parts.push_back(json::Json{{"first", paired.part.first.str()},
                                   {"last", paired.part.last.str()},
                                   {"a", paired.part.a},
                                   {"b", paired.part.b},
                                   {"readings", std::move(readings)}});
    }
    return json::Json{{"panel", pairing.panel}, {"judges", pairing.judges}, {"parts", std::move(parts)}};
}

Result<Pairing> pairing_from_json(const json::Json& document) {
    if (Result<void> valid = knowledge::artifact_schemas().validate("artifacts/pairing", document);
        !valid.has_value()) {
        return fail(Errc::invalid_argument, std::format("not a pairing: {}", valid.error().message()));
    }
    Pairing pairing{.panel = document.at("panel").get<std::string>(),
                    .judges = document.at("judges").get<std::vector<std::string>>()};
    for (const json::Json& paired : document.at("parts")) {
        const std::optional<SentenceId> first = SentenceId::parse(paired.at("first").get<std::string>());
        const std::optional<SentenceId> last = SentenceId::parse(paired.at("last").get<std::string>());
        RR_ASSERT(first.has_value() && last.has_value(), "a pairing's schema allows S1, S2, … only");
        PairedPart part{.part = Part{.first = *first,
                                     .last = *last,
                                     .a = paired.at("a").get<std::vector<std::size_t>>(),
                                     .b = paired.at("b").get<std::vector<std::size_t>>()}};
        for (const json::Json& read : paired.at("readings")) {
            RR_TRY_ASSIGN(PartReading reading, reading_from_json(read));
            part.readings.push_back(std::move(reading));
        }
        pairing.parts.push_back(std::move(part));
    }
    return pairing;
}

FlowScore flow_score(const Pairing& pairing) {
    FlowScore score;
    double sum = 0;
    for (const PairedPart& paired : pairing.parts) {
        reason::PairwiseTally tally;
        for (const PartReading& reading : paired.readings) {
            if (reading.a_first.has_value() && reading.b_first.has_value()) {
                tally.add(*reading.a_first, *reading.b_first);
            }
        }
        score.inconsistent += static_cast<std::size_t>(tally.inconsistent);
        if (tally.pairs() == 0) {
            continue;  // no judge read it
        }
        // B's share less A's: +1 when every judge prefers B, -1 when every one A.
        sum += static_cast<double>(tally.b - tally.a) / static_cast<double>(tally.pairs());
        ++score.parts;
    }
    if (score.parts > 0) {
        score.b_preferred = sum / static_cast<double>(score.parts);
    }
    return score;
}

std::vector<VideoPairs> pairs_to_read(std::span<const ScoredEntry> a, std::span<const ScoredEntry> b) {
    const auto in_b = entries_by_video(b);
    std::vector<VideoPairs> videos;
    for (const auto& [video, a_entries] : entries_by_video(a)) {
        const auto found = in_b.find(video);
        if (found == in_b.end()) {
            continue;  // A's alone: the comparison lists it
        }
        const std::vector<RunEntry>& b_entries = found->second;
        VideoPairs paired{.video = video, .a_replicates = a_entries.size(), .b_replicates = b_entries.size()};
        for (std::size_t index = 0; index < std::min(a_entries.size(), b_entries.size()); ++index) {
            paired.pairs.emplace_back(a_entries[index], b_entries[index]);
        }
        videos.push_back(std::move(paired));
    }
    return videos;
}

Comparison flow_comparison(const Panel& panel,
                           const std::map<std::string, std::vector<double>, std::less<>>& read) {
    std::map<std::string, double, std::less<>> preferences;
    for (const auto& [video, values] : read) {
        if (!values.empty()) {
            preferences.emplace(video, std::accumulate(values.begin(), values.end(), 0.0) /
                                           static_cast<double>(values.size()));
        }
    }
    return compare_pairwise(kFlowMetric, panel.scorer(), std::move(preferences));
}

Result<ArticlesToCompare> articles_to_compare(const library::Job& a, const library::Job& b,
                                              const library::StageStore& store) {
    RR_TRY_ASSIGN(report::Contents first, report::contents_of(a, store));
    RR_TRY_ASSIGN(report::Contents second, report::contents_of(b, store));
    const auto no_article = [](const library::Job& job) {
        return fail(Errc::invalid_argument, std::format("job {} has no article to compare", job.id));
    };
    if (!first.transcript.has_value() || !first.ledger.has_value() || !first.article.has_value()) {
        return no_article(a);
    }
    if (!second.transcript.has_value() || !second.ledger.has_value() || !second.article.has_value()) {
        return no_article(b);
    }
    if (json::content_hash(knowledge::to_json(*first.transcript)) !=
        json::content_hash(knowledge::to_json(*second.transcript))) {
        return fail(
            Errc::invalid_argument,
            std::format("jobs {} and {} read other transcripts: their articles are not of one video's "
                        "sentences",
                        a.id, b.id));
    }
    return ArticlesToCompare{
        .title = first.video.has_value() ? first.video->title : a.title.value_or(a.source),
        .channel = first.video.has_value() ? first.video->channel : std::string{},
        .transcript = *std::move(first.transcript),
        .a_ledger = *std::move(first.ledger),
        .a = *std::move(first.article),
        .b_ledger = *std::move(second.ledger),
        .b = *std::move(second.article)};
}

Result<library::Job> start_pairing(db::Database& database, const Clock& clock, const library::Job& a,
                                   const library::Job& b, const Panel& panel) {
    library::Jobs jobs{database, clock};
    RR_TRY_ASSIGN(const std::optional<std::string> found, database.read([&](db::ReadTx& tx) {
        return tx.query_optional_text(
            "SELECT job_id FROM eval_pairings WHERE first_job_id = ?1 AND second_job_id = ?2 AND panel = ?3",
            {a.id, b.id, panel.name});
    }));
    if (found.has_value()) {
        return jobs.find(*found);
    }
    RR_TRY_ASSIGN(library::Job job,
                  jobs.create(std::format("{} {}", a.id, b.id), panel.name, 0, library::JobKind::pairing));
    job.title = a.title;  // what `rreport jobs` lists it by: the video
    RR_TRY_ASSIGN(job, jobs.save(std::move(job)));
    RR_TRY(database.write([&](db::WriteTx& tx) -> Result<void> {
        RR_TRY(tx.execute(
            "INSERT INTO eval_pairings (first_job_id, second_job_id, panel, job_id) VALUES (?1, ?2, ?3, ?4)",
            {a.id, b.id, panel.name, job.id}));
        return ok();
    }));
    return job;
}

Result<PairingRun> run_pairing(library::Job job, const Panel& panel, ArticlesToCompare articles,
                               db::Database& database, const Clock& clock, pipeline::StageContext context) {
    std::vector<std::unique_ptr<pipeline::Stage>> stages;
    stages.push_back(std::make_unique<FlowStage>(panel, std::move(articles)));
    RR_TRY_ASSIGN(const pipeline::Executor executor, pipeline::Executor::create(std::move(stages), clock));
    context.work_directory /= job.id;
    RR_TRY_ASSIGN(pipeline::JobRun run,
                  pipeline::run_job(std::move(job), executor, database, clock, std::move(context)));
    PairingRun paired{.run = std::move(run)};
    if (paired.run.job.status != library::JobStatus::done) {
        return paired;
    }
    const auto stage = std::ranges::find(paired.run.report.stages, std::string{FlowStage::kName},
                                         &pipeline::StageReport::stage);
    RR_ASSERT(stage != paired.run.report.stages.end(), "a job done has done its every stage");
    const library::StageStore store{database, clock};
    RR_TRY_ASSIGN(const json::Json artifact, store.artifact(stage->artifact));
    RR_TRY_ASSIGN(const Pairing pairing, pairing_from_json(artifact));
    paired.score = flow_score(pairing);
    return paired;
}

FlowStage::FlowStage(Panel panel, ArticlesToCompare articles)
    : panel_(std::move(panel))
    , articles_(std::move(articles)) {}

Result<json::Json> FlowStage::fingerprint(const pipeline::StageContext& context) const {
    if (!context.army) {
        return fail(
            Errc::needs_user,
            "comparing two articles needs the panel's models, and no pod is configured: rreport doctor "
            "says how");
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
                                    {"holds", engines[index]->ctx_per_slot},  // which parts are read
                                    {"direct", llm::to_json(engines[index]->direct)}});
    }
    return json::Json{{"panel", panel_.name},
                      {"prompts", std::move(prompts)},
                      {"judges", std::move(judges)},
                      {"read", reading_fingerprint(articles_)}};
}

Result<pipeline::StageOutput> FlowStage::run(const pipeline::StageContext& context) const {
    RR_TRY_ASSIGN(agents::Army* const army, context.army());
    Comparer comparer{context, panel_, articles_, *army};
    return comparer.read();
}

}  // namespace rr::eval
