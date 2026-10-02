// The panel's readings, as a person labels them and calibration compares
// them (rr/eval/calibrate.hpp).
#include <rr/eval/calibrate.hpp>

#include <algorithm>
#include <array>
#include <format>
#include <map>
#include <ranges>
#include <utility>

#include <rr/core/assert.hpp>
#include <rr/core/error.hpp>
#include <rr/core/time_span.hpp>
#include <rr/eval/gold.hpp>
#include <rr/library/jobs.hpp>
#include <rr/reason/pairwise.hpp>

#include "shown.hpp"

namespace rr::eval {

namespace {

constexpr std::array<std::string_view, 3> kFactClasses{"told", "partly", "not_told"};
constexpr std::array<std::string_view, 6> kSentenceClasses{"none",        "adds",          "softens",
                                                           "intensifies", "misattributes", "contradicts"};
constexpr std::array<std::string_view, 3> kPartClasses{"a", "b", "tie"};

// Where `name` is among `classes`: each kind's verdicts are named by its
// own to_string(), so that no enum's order is relied on.
std::size_t index_of(std::span<const std::string_view> classes, std::string_view name) {
    const auto found = std::ranges::find(classes, name);
    RR_ASSERT(found != classes.end(), "a verdict is one of its kind's classes");
    return static_cast<std::size_t>(found - classes.begin());
}

// The artifact of the done job `job`'s stage `stage`; none while the job is
// not done.
Result<std::optional<json::Json>> done_artifact(db::Database& database, const Clock& clock,
                                                std::string_view job_id, std::string_view stage) {
    const library::Jobs jobs{database, clock};
    RR_TRY_ASSIGN(const library::Job job, jobs.find(job_id));
    if (job.status != library::JobStatus::done) {
        return std::optional<json::Json>{};
    }
    const library::StageStore store{database, clock};
    RR_TRY_ASSIGN(const std::vector<library::StageRun> runs, store.runs(job_id));
    // The last done run of the stage: a run that failed, then was taken up,
    // has an earlier one that did not.
    for (const library::StageRun& run : runs | std::views::reverse) {
        if (run.stage == stage && run.status == library::StageStatus::done && run.artifact.has_value()) {
            RR_TRY_ASSIGN(json::Json artifact, store.artifact(*run.artifact));
            return std::optional<json::Json>{std::move(artifact)};
        }
    }
    return fail(Errc::internal, std::format("job {} is done, and has no {} run done", job_id, stage));
}

// "1:30 to 3:55": the stretch of the video a part tells.
std::string stretch_of(const knowledge::Transcript& transcript, const Part& part) {
    const knowledge::Sentence* first = transcript.find(part.first);
    const knowledge::Sentence* last = transcript.find(part.last);
    RR_ASSERT(first != nullptr, "a part's sentences are the transcript's");
    RR_ASSERT(last != nullptr, "a part's sentences are the transcript's");
    return std::format("{} to {}", format_timestamp(first->span.start()), format_timestamp(last->span.end()));
}

// A version of a part: its sections, headings and prose, as the judges read them.
std::string version_of(const std::vector<ArticleSection>& sections, std::span<const std::size_t> indices) {
    std::string text;
    for (const std::size_t index : indices) {
        text += std::format("{}{}", text.empty() ? "" : "\n\n", sections.at(index).text);
    }
    return text;
}

std::size_t words_in(std::string_view text) {
    std::size_t words = 0;
    bool in_word = false;
    for (const char character : text) {
        const bool space = character == ' ' || character == '\n' || character == '\t';
        if (!space && !in_word) {
            ++words;
        }
        in_word = !space;
    }
    return words;
}

bool read(const FactReading& reading) {
    return reading.verdict.has_value();
}
bool read(const SentenceReading& reading) {
    return reading.issue.has_value();
}
bool read(const PartReading& reading) {
    return reading.a_first.has_value() && reading.b_first.has_value();
}

template <typename Reading>
bool read_by_any(const std::vector<Reading>& readings) {
    return std::ranges::any_of(readings, [](const Reading& reading) { return read(reading); });
}

// What one judge said of an item, as a class of its kind; none when unread.
std::optional<std::size_t> judge_class(const FactReading& reading) {
    return reading.verdict.has_value() ? std::optional{index_of(kFactClasses, to_string(*reading.verdict))}
                                       : std::nullopt;
}
std::optional<std::size_t> judge_class(const SentenceReading& reading) {
    return reading.issue.has_value()
               ? std::optional{index_of(kSentenceClasses, pipeline::to_string(*reading.issue))}
               : std::nullopt;
}
std::optional<std::size_t> judge_class(const PartReading& reading) {
    if (!read(reading)) {
        return std::nullopt;
    }
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access): read() checked both orders
    return index_of(kPartClasses, reason::to_string(reason::swapped(*reading.a_first, *reading.b_first)));
}

// The verdicts one kind gathers, judge by judge.
class Tally {
public:
    Tally(std::string_view kind, const Panel& panel) : kind_(kind) {
        for (const PanelJudge& judge : panel.judges) {
            judges_.push_back(judge.role);
        }
        verdicts_.resize(judges_.size());
        unread_.resize(judges_.size(), 0);
    }

    void skip() { ++skipped_; }

    // An item the person labelled `person`, and the readings the judges gave
    // it (none found: every judge left it unread).
    template <typename Reading>
    void add(std::size_t person, const std::vector<Reading>* readings) {
        ++labelled_;
        for (std::size_t index = 0; index < judges_.size(); ++index) {
            const Reading* reading = nullptr;
            if (readings != nullptr) {
                const auto found = std::ranges::find(*readings, judges_[index], &Reading::judge);
                reading = found != readings->end() ? &*found : nullptr;
            }
            const std::optional<std::size_t> judged =
                reading != nullptr ? judge_class(*reading) : std::nullopt;
            if (judged.has_value()) {
                verdicts_[index].push_back(Verdicts{.person = person, .judge = *judged});
            } else {
                ++unread_[index];
            }
        }
    }

    [[nodiscard]] KindCalibration done(std::span<const std::string_view> classes) const {
        KindCalibration kind{.kind = std::string{kind_}, .labelled = labelled_, .skipped = skipped_};
        for (std::size_t index = 0; index < judges_.size(); ++index) {
            kind.judges.push_back(agree(judges_[index], classes, verdicts_[index], unread_[index]));
        }
        return kind;
    }

private:
    std::string_view kind_;
    std::vector<std::string> judges_;
    std::vector<std::vector<Verdicts>> verdicts_;
    std::vector<std::size_t> unread_;
    std::size_t labelled_ = 0;
    std::size_t skipped_ = 0;
};

// Judgements by article job, each read once.
class Judgements {
public:
    Judgements(db::Database& database, const Clock& clock, const Panel& panel)
        : database_(&database)
        , clock_(&clock)
        , panel_(&panel) {}

    // The judgement of `article`; nullptr when it has none done.
    Result<const Judgement*> of(const std::string& article) {
        auto found = judgements_.find(article);
        if (found == judgements_.end()) {
            RR_TRY_ASSIGN(std::optional<Judgement> judgement,
                          find_judgement(*database_, *clock_, article, *panel_));
            found = judgements_.emplace(article, std::move(judgement)).first;
        }
        return found->second.has_value() ? &*found->second : nullptr;
    }

private:
    db::Database* database_;
    const Clock* clock_;
    const Panel* panel_;
    std::map<std::string, std::optional<Judgement>, std::less<>> judgements_;
};

// A pair of entries read side by side: its pairing job, the pairing, done,
// and the two articles with their sections, as the judges read them.
struct SideBySide {
    std::string job{};
    Pairing pairing{};
    ArticlesToCompare articles{};
    std::vector<ArticleSection> a_sections{};
    std::vector<ArticleSection> b_sections{};
};

// The entries `a` and `b`, as `panel` read them side by side; none until
// their pairing is done.
Result<std::optional<SideBySide>> side_by_side(db::Database& database, const Clock& clock, const RunEntry& a,
                                               const RunEntry& b, const Panel& panel) {
    RR_TRY_ASSIGN(std::optional<std::string> job, find_pairing_job(database, a.job, b.job, panel));
    if (!job.has_value()) {
        return std::optional<SideBySide>{};
    }
    RR_TRY_ASSIGN(std::optional<Pairing> pairing, find_pairing(database, clock, *job));
    if (!pairing.has_value()) {
        return std::optional<SideBySide>{};
    }
    const library::Jobs jobs{database, clock};
    const library::StageStore store{database, clock};
    RR_TRY_ASSIGN(const library::Job a_job, jobs.find(a.job));
    RR_TRY_ASSIGN(const library::Job b_job, jobs.find(b.job));
    RR_TRY_ASSIGN(ArticlesToCompare articles, articles_to_compare(a_job, b_job, store));
    SideBySide both{.job = std::move(*job), .pairing = std::move(*pairing)};
    both.a_sections = article_sections(articles.a, articles.a_ledger);
    both.b_sections = article_sections(articles.b, articles.b_ledger);
    both.articles = std::move(articles);
    return std::optional<SideBySide>{std::move(both)};
}

// The facts of `judgement` some judge read, as labels showing what the
// judges were shown: each fact by `article`'s gold, and the article's
// sentences it was shown, numbered as the judges saw them.
void add_fact_labels(LabelPool& pool, const RunEntry& entry, const Judgement& judgement,
                     const ArticleToJudge& article) {
    const std::vector<ArticleSentence> sentences = article_sentences(article.article, article.ledger);
    for (const JudgedFact& judged : judgement.facts) {
        const auto fact = std::ranges::find(article.facts, judged.fact, &Fact::id);
        if (!read_by_any(judged.readings) || fact == article.facts.end()) {
            continue;  // unread; or a fact the gold no longer has, which no label can show
        }
        FactLabel label{.job = entry.job,
                        .video = entry.video,
                        .replicate = entry.replicate,
                        .fact = judged.fact,
                        .core = judged.core,
                        .said = std::format("{} to {}", clock_time(fact->start), clock_time(fact->end)),
                        .claim = fact->claim,
                        .quote = fact->quote};
        for (std::size_t shown = 1; const std::size_t number : judged.candidates) {
            label.shown.push_back(std::format("{}. {}", shown++, sentences.at(number - 1).text));
        }
        pool.facts.push_back(std::move(label));
    }
}

// The sentences of `judgement` some judge read, as labels showing the
// video's lines the judges were shown.
void add_sentence_labels(LabelPool& pool, const RunEntry& entry, const Judgement& judgement,
                         const knowledge::Transcript& transcript) {
    for (const JudgedSentence& judged : judgement.sentences) {
        if (!read_by_any(judged.readings)) {
            continue;
        }
        SentenceLabel label{.job = entry.job,
                            .video = entry.video,
                            .replicate = entry.replicate,
                            .number = judged.number,
                            .text = judged.text};
        for (const knowledge::SentenceId id : judged.evidence) {
            const knowledge::Sentence* said = transcript.find(id);
            RR_ASSERT(said != nullptr, "an atom's sentences are the transcript's");
            label.evidence.push_back(video_line(*said));
        }
        pool.sentences.push_back(std::move(label));
    }
}

// What the panel read of `entry`, as labels: its facts, by its video's
// reviewed gold in `facts`, and its sentences; or a note saying why not.
Result<void> add_entry_labels(LabelPool& pool, db::Database& database, const Clock& clock,
                              const FactsByVideo& facts, const RunEntry& entry, const Panel& panel) {
    RR_TRY_ASSIGN(const std::optional<Judgement> judgement,
                  find_judgement(database, clock, entry.job, panel));
    if (!judgement.has_value()) {
        pool.notes.push_back(std::format("{} replicate {}: not judged by {} yet: rreport eval judge",
                                         entry.video, entry.replicate, panel.name));
        return ok();
    }
    const auto found = facts.find(entry.video);
    if (found == facts.end() && !judgement->facts.empty()) {
        pool.notes.push_back(
            std::format("{} replicate {}: no reviewed gold to show its facts by: they are left out",
                        entry.video, entry.replicate));
    }
    const library::Jobs jobs{database, clock};
    const library::StageStore store{database, clock};
    RR_TRY_ASSIGN(const library::Job article_job, jobs.find(entry.job));
    RR_TRY_ASSIGN(
        const ArticleToJudge article,
        article_to_judge(article_job, store, found != facts.end() ? found->second : std::vector<Fact>{}));
    add_fact_labels(pool, entry, *judgement, article);
    add_sentence_labels(pool, entry, *judgement, article.transcript);
    return ok();
}

// The parts of `both` some judge read in both orders, as labels showing
// the two versions, each part in an order drawn for it (b_first()).
void add_part_labels(LabelPool& pool, std::string_view name, std::string_view video, int replicate,
                     const SideBySide& both) {
    for (std::size_t index = 0; index < both.pairing.parts.size(); ++index) {
        const PairedPart& paired = both.pairing.parts[index];
        if (!read_by_any(paired.readings)) {
            continue;
        }
        const std::string a_version = version_of(both.a_sections, paired.part.a);
        const std::string b_version = version_of(both.b_sections, paired.part.b);
        const bool b_shown_first = b_first(name, video, replicate, index);
        pool.parts.push_back(PartLabel{.job = both.job,
                                       .video = std::string{video},
                                       .replicate = replicate,
                                       .part = index,
                                       .stretch = stretch_of(both.articles.transcript, paired.part),
                                       .b_first = b_shown_first,
                                       .first = b_shown_first ? b_version : a_version,
                                       .second = b_shown_first ? a_version : b_version});
    }
}

// The facts labelled, each set against the judges' readings of it; a fact
// no judgement holds is said in `notes`.
Result<KindCalibration> calibrate_facts(Judgements& judgements, std::span<const FactLabel> labels,
                                        const Panel& panel, std::vector<std::string>& notes) {
    Tally tally{kFactsKind, panel};
    for (const FactLabel& label : labels) {
        if (!label.verdict.has_value()) {
            tally.skip();
            continue;
        }
        RR_TRY_ASSIGN(const Judgement* judgement, judgements.of(label.job));
        const std::vector<FactReading>* readings = nullptr;
        if (judgement != nullptr) {
            const auto found = std::ranges::find(judgement->facts, label.fact, &JudgedFact::fact);
            readings = found != judgement->facts.end() ? &found->readings : nullptr;
        }
        if (readings == nullptr) {
            notes.push_back(std::format("fact {} of {} replicate {}: no judgement of it found", label.fact,
                                        label.video, label.replicate));
        }
        tally.add(index_of(kFactClasses, to_string(*label.verdict)), readings);
    }
    return tally.done(kFactClasses);
}

// The sentences labelled, each set against the judges' readings of it.
Result<KindCalibration> calibrate_sentences(Judgements& judgements, std::span<const SentenceLabel> labels,
                                            const Panel& panel, std::vector<std::string>& notes) {
    Tally tally{kSentencesKind, panel};
    for (const SentenceLabel& label : labels) {
        if (!label.verdict.has_value()) {
            tally.skip();
            continue;
        }
        RR_TRY_ASSIGN(const Judgement* judgement, judgements.of(label.job));
        const std::vector<SentenceReading>* readings = nullptr;
        if (judgement != nullptr) {
            const auto found = std::ranges::find(judgement->sentences, label.number, &JudgedSentence::number);
            readings = found != judgement->sentences.end() ? &found->readings : nullptr;
        }
        if (readings == nullptr) {
            notes.push_back(std::format("sentence {} of {} replicate {}: no judgement of it found",
                                        label.number, label.video, label.replicate));
        }
        tally.add(index_of(kSentenceClasses, pipeline::to_string(*label.verdict)), readings);
    }
    return tally.done(kSentenceClasses);
}

// The parts labelled, each set against the judges' readings of it in both
// orders: the side the person's verdict prefers, against the side each
// judge's two orders settle on.
Result<KindCalibration> calibrate_parts(db::Database& database, const Clock& clock,
                                        std::span<const PartLabel> labels, const Panel& panel,
                                        std::vector<std::string>& notes) {
    Tally tally{kPartsKind, panel};
    std::map<std::string, std::optional<Pairing>, std::less<>> pairings;  // by job, each read once
    for (const PartLabel& label : labels) {
        const std::optional<reason::Winner> winner = label.winner();
        if (!winner.has_value()) {
            tally.skip();
            continue;
        }
        auto found = pairings.find(label.job);
        if (found == pairings.end()) {
            RR_TRY_ASSIGN(std::optional<Pairing> pairing, find_pairing(database, clock, label.job));
            found = pairings.emplace(label.job, std::move(pairing)).first;
        }
        const std::vector<PartReading>* readings = nullptr;
        if (found->second.has_value() && label.part < found->second->parts.size()) {
            readings = &found->second->parts[label.part].readings;
        } else {
            notes.push_back(std::format("part {} of {} replicate {}: no pairing of it found", label.part,
                                        label.video, label.replicate));
        }
        tally.add(index_of(kPartClasses, reason::to_string(*winner)), readings);
    }
    return tally.done(kPartClasses);
}

// Each judge's reading of each part of `both`, in both orders, added to its
// own (`ordered`, by the panel's judges): whether its orders flipped, and
// whether a consistent preference went to the longer version.
void add_ordered(std::vector<std::vector<OrderedReading>>& ordered, const SideBySide& both,
                 const Panel& panel) {
    for (const PairedPart& paired : both.pairing.parts) {
        const std::size_t a_words = words_in(version_of(both.a_sections, paired.part.a));
        const std::size_t b_words = words_in(version_of(both.b_sections, paired.part.b));
        for (std::size_t index = 0; index < panel.judges.size(); ++index) {
            const auto reading =
                std::ranges::find(paired.readings, panel.judges[index].role, &PartReading::judge);
            if (reading == paired.readings.end() || !read(*reading)) {
                continue;
            }
            // The two orders, tallied as a comparison tallies them: a flip is
            // two orders that disagree, and a consistent preference is the
            // side both orders chose.
            reason::PairwiseTally tally;
            // NOLINTNEXTLINE(bugprone-unchecked-optional-access): read() checked both orders
            tally.add(*reading->a_first, *reading->b_first);
            OrderedReading one{.flipped = tally.inconsistent > 0};
            if (!one.flipped && tally.ties == 0 && lengths_differ(a_words, b_words)) {
                one.longer = (tally.a > 0) == (a_words > b_words);
            }
            ordered[index].push_back(one);
        }
    }
}

// Each judge's biases, over every part of every pairing of `pairs` done.
Result<std::vector<JudgeBiases>> judge_biases(db::Database& database, const Clock& clock, const Panel& panel,
                                              std::span<const VideoPairs> pairs) {
    std::vector<std::vector<OrderedReading>> ordered(panel.judges.size());
    for (const VideoPairs& video : pairs) {
        for (const auto& [a, b] : video.pairs) {
            RR_TRY_ASSIGN(const std::optional<SideBySide> both, side_by_side(database, clock, a, b, panel));
            if (both.has_value()) {
                add_ordered(ordered, *both, panel);
            }
        }
    }
    std::vector<JudgeBiases> found;
    found.reserve(panel.judges.size());
    for (std::size_t index = 0; index < panel.judges.size(); ++index) {
        found.push_back(JudgeBiases{.judge = panel.judges[index].role, .biases = biases(ordered[index])});
    }
    return found;
}

}  // namespace

Result<std::optional<Judgement>> find_judgement(db::Database& database, const Clock& clock,
                                                std::string_view article, const Panel& panel) {
    RR_TRY_ASSIGN(const std::optional<std::string> job, database.read([&](db::ReadTx& tx) {
        return tx.query_optional_text(
            "SELECT job_id FROM eval_judgements WHERE article_job_id = ?1 AND panel = ?2",
            {std::string{article}, panel.name});
    }));
    if (!job.has_value()) {
        return std::optional<Judgement>{};
    }
    RR_TRY_ASSIGN(const std::optional<json::Json> artifact,
                  done_artifact(database, clock, *job, JudgementStage::kName));
    if (!artifact.has_value()) {
        return std::optional<Judgement>{};
    }
    RR_TRY_ASSIGN(Judgement judgement, judgement_from_json(*artifact));
    return std::optional<Judgement>{std::move(judgement)};
}

Result<std::optional<std::string>> find_pairing_job(db::Database& database, std::string_view a,
                                                    std::string_view b, const Panel& panel) {
    return database.read([&](db::ReadTx& tx) {
        return tx.query_optional_text(
            "SELECT job_id FROM eval_pairings WHERE first_job_id = ?1 AND second_job_id = ?2 AND panel = ?3",
            {std::string{a}, std::string{b}, panel.name});
    });
}

Result<std::optional<Pairing>> find_pairing(db::Database& database, const Clock& clock,
                                            std::string_view job) {
    RR_TRY_ASSIGN(const std::optional<json::Json> artifact,
                  done_artifact(database, clock, job, FlowStage::kName));
    if (!artifact.has_value()) {
        return std::optional<Pairing>{};
    }
    RR_TRY_ASSIGN(Pairing pairing, pairing_from_json(*artifact));
    return std::optional<Pairing>{std::move(pairing)};
}

Result<LabelPool> label_pool(db::Database& database, const Clock& clock, const FactsByVideo& facts,
                             std::span<const RunEntry> entries, std::span<const VideoPairs> pairs,
                             const Panel& panel, std::string_view name) {
    LabelPool pool;
    for (const RunEntry& entry : entries) {
        RR_TRY(add_entry_labels(pool, database, clock, facts, entry, panel));
    }
    for (const VideoPairs& video : pairs) {
        for (const auto& [a, b] : video.pairs) {
            RR_TRY_ASSIGN(const std::optional<SideBySide> both, side_by_side(database, clock, a, b, panel));
            if (!both.has_value()) {
                pool.notes.push_back(
                    std::format("{} replicate {}: its two articles not read side by side yet: rreport eval "
                                "compare --flow",
                                video.video, a.replicate));
                continue;
            }
            add_part_labels(pool, name, video.video, a.replicate, *both);
        }
    }
    return pool;
}

Labels draw_labels(std::string name, std::string panel, std::string run, std::string against,
                   const LabelPool& pool, const LabelCounts& counts) {
    Labels labels{.name = std::move(name),
                  .panel = std::move(panel),
                  .run = std::move(run),
                  .against = std::move(against)};
    for (const std::size_t index : draw(pool.facts.size(), counts.facts, labels.name + "/facts")) {
        labels.facts.push_back(pool.facts[index]);
    }
    for (const std::size_t index :
         draw(pool.sentences.size(), counts.sentences, labels.name + "/sentences")) {
        labels.sentences.push_back(pool.sentences[index]);
    }
    for (const std::size_t index : draw(pool.parts.size(), counts.parts, labels.name + "/parts")) {
        labels.parts.push_back(pool.parts[index]);
    }
    return labels;
}

bool KindCalibration::passes() const noexcept {
    return !judges.empty() && std::ranges::all_of(judges, [](const JudgeAgreement& judge) {
        return judge.items > 0 && judge.passes();
    });
}

bool Calibration::passes() const noexcept {
    return !kinds.empty() &&
           std::ranges::all_of(kinds, [](const KindCalibration& kind) { return kind.passes(); });
}

Result<Calibration> calibrate(db::Database& database, const Clock& clock, const Labels& labels,
                              const Panel& panel, std::span<const VideoPairs> pairs) {
    Calibration calibration;
    Judgements judgements{database, clock, panel};
    RR_TRY_ASSIGN(KindCalibration facts, calibrate_facts(judgements, labels.facts, panel, calibration.notes));
    calibration.kinds.push_back(std::move(facts));
    RR_TRY_ASSIGN(KindCalibration sentences,
                  calibrate_sentences(judgements, labels.sentences, panel, calibration.notes));
    calibration.kinds.push_back(std::move(sentences));
    RR_TRY_ASSIGN(KindCalibration parts,
                  calibrate_parts(database, clock, labels.parts, panel, calibration.notes));
    calibration.kinds.push_back(std::move(parts));
    RR_TRY_ASSIGN(calibration.biases, judge_biases(database, clock, panel, pairs));
    return calibration;
}

}  // namespace rr::eval

namespace rr::eval {

namespace {

// "0.778 (0.453 to 0.937)", or "-" for no interval.
std::string rate_shown(std::size_t successes, std::size_t trials, const std::optional<Interval>& interval) {
    if (!interval.has_value() || trials == 0) {
        return "-";
    }
    return std::format("{} ({} to {})",
                       detail::shown(static_cast<double>(successes) / static_cast<double>(trials)),
                       detail::shown(interval->low), detail::shown(interval->high));
}

std::string optional_shown(const std::optional<double>& value) {
    return value.has_value() ? detail::shown(*value) : std::string{"-"};
}

std::string kind_text(const KindCalibration& kind) {
    std::string text = std::format("\n{}: {} labelled, {} skipped\n", kind.kind, kind.labelled, kind.skipped);
    text += std::format("  {:<12}{:>7}{:>8}{:>9}{:>9}{:>9}   {}\n", "judge", "items", "unread", "agree",
                        "chance", "kappa", "passes");
    for (const JudgeAgreement& judge : kind.judges) {
        text += std::format("  {:<12}{:>7}{:>8}{:>9}{:>9}{:>9}   {}\n", judge.judge, judge.items,
                            judge.unread, optional_shown(judge.observed), optional_shown(judge.chance),
                            optional_shown(judge.kappa), judge.items > 0 && judge.passes() ? "yes" : "no");
        for (const ClassAgreement& one : judge.classes) {
            text += std::format(
                "    {:<14} person {:>3}, judge {:>3}, both {:>3}   "
                "precision {:<24} bar {}   recall {:<24} bar {}{}\n",
                one.name, one.person, one.judge, one.both, rate_shown(one.both, one.judge, one.precision),
                detail::shown(one.precision_bar), rate_shown(one.both, one.person, one.recall),
                detail::shown(one.recall_bar), one.clears() ? "" : "   below a bar");
        }
    }
    return text;
}

// Why `calibration` does not pass, a line a reason.
std::vector<std::string> why_not(const Calibration& calibration) {
    std::vector<std::string> reasons;
    for (const KindCalibration& kind : calibration.kinds) {
        if (kind.labelled == 0) {
            reasons.push_back(std::format("{}: no item labelled: every kind needs labels", kind.kind));
            continue;
        }
        for (const JudgeAgreement& judge : kind.judges) {
            if (judge.items == 0) {
                reasons.push_back(
                    std::format("{}, {}: it read none of the items labelled", kind.kind, judge.judge));
            } else if (!judge.kappa.has_value()) {
                reasons.push_back(
                    std::format("{}, {}: one class only, where κ says nothing: label items of "
                                "other classes",
                                kind.kind, judge.judge));
            } else if (!judge.kappa_clears()) {
                reasons.push_back(std::format("{}, {}: κ {} under {}", kind.kind, judge.judge,
                                              detail::shown(*judge.kappa), detail::shown(kKappaBar)));
            }
            for (const ClassAgreement& one : judge.classes) {
                if (!one.clears()) {
                    reasons.push_back(std::format("{}, {}: {} below a random judge's bar", kind.kind,
                                                  judge.judge, one.name));
                }
            }
        }
    }
    return reasons;
}

}  // namespace

std::string to_text(const Calibration& calibration, const Labels& labels, const Panel& panel) {
    std::string text = std::format(
        "labels {}, by {}: run {}{}, panel {}\n"
        "Each judge's verdicts against the person's: κ, agreement beyond chance (at least {}); each class's\n"
        "precision and recall with their 95% intervals (Wilson), whose lower bounds must clear a random\n"
        "judge's: the class's share of the person's verdicts, and the judge's own rate of saying it.\n",
        labels.name, labels.labeller, labels.run, labels.against.empty() ? "" : " against " + labels.against,
        panel.name, detail::shown(kKappaBar));
    for (const KindCalibration& kind : calibration.kinds) {
        text += kind_text(kind);
    }
    if (!calibration.biases.empty()) {
        text += "\nbiases, over every part of the two runs' pairings (reported: no bar):\n";
        text += std::format("  {:<12}{:>7}{:>9}   {:<26}{:>9}{:>8}   {}\n", "judge", "read", "flipped",
                            "flip rate (95%)", "unequal", "longer", "longer (95%)");
        for (const JudgeBiases& judge : calibration.biases) {
            const Biases& biases = judge.biases;
            text += std::format("  {:<12}{:>7}{:>9}   {:<26}{:>9}{:>8}   {}\n", judge.judge, biases.read,
                                biases.flipped, rate_shown(biases.flipped, biases.read, biases.flip_rate),
                                biases.unequal, biases.longer,
                                rate_shown(biases.longer, biases.unequal, biases.longer_rate));
        }
    }
    for (const std::string& note : calibration.notes) {
        text += std::format("note: {}\n", note);
    }
    if (calibration.passes()) {
        return text + std::format(
                          "\nthe panel {} reads as the labels do: set calibrated = true in "
                          "eval/judges/{}.toml, in a commit someone reviews\n",
                          panel.name, panel.name);
    }
    text += std::format("\nthe panel {} does not pass yet:\n", panel.name);
    for (const std::string& reason : why_not(calibration)) {
        text += std::format("  {}\n", reason);
    }
    return text +
           "an interval too wide needs more labels (another file: rreport eval label NAME2 …);\n"
           "a judge that reads otherwise than you do needs another panel.\n";
}

}  // namespace rr::eval
