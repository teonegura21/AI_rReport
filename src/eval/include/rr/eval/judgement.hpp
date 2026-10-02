// rr/eval/judgement.hpp — an article read by the eval's frozen judges: which
// of the gold's facts it tells, and which of its sentences the video supports.
//
// Why this exists
//   Code bounds what an article tells (facts_cited, rr/eval/score.hpp) and
//   checks what it keeps word for word; it cannot read. Whether a reader of
//   the article learns a fact, and whether a sentence says only what the
//   video says, the panel's judges read (rr/eval/panel.hpp), each item by
//   every judge, so that two families' readings stand side by side:
//     * A fact (eval_fact) is shown with the article's sentences most likely
//       to tell it: those citing an atom of the sentences that say it (as
//       facts_cited finds them), then the best three by BM25 on its claim
//       and its words, in the article's order, twelve at most. A fact with no
//       such sentence is not told, and no judge is asked.
//     * A paragraph (eval_support) is shown sentence by sentence, each with
//       the video's own sentences it rests on: the transcript's sentences of
//       the atoms it cites, never the atoms themselves, so an atom that
//       misread the video cannot vouch for a sentence that repeats the
//       mistake. A sentence citing nothing is shown its paragraph's.
//   Every answer quotes its evidence before its verdict, and a quote that is
//   not in what the judge was shown, word for word, is sent back once: a
//   verdict resting on words the judge made up is no reading. A judge that
//   still cannot answer leaves its item unread, a finding; the item's score
//   is then the other judges' alone.
//
// As a job
//   A judgement is a library job of its own kind (library::JobKind::judgement,
//   migrations/0007_judgement.sql) with one stage, this one. It reads what it
//   judges from the article job, not from stages before it, so its key holds
//   that: the panel (its name, procedure, prompts and judges), the hashes of
//   the transcript, the atoms, the article and the facts, and how the
//   judges' engines sample. So an article judged again, unchanged, by the same
//   panel, is found done and asks nothing; one interrupted is taken up by its
//   memo (ADR-0020); and every call is the judgement's, never the article's
//   costs.
//
// The scores (judged_scores(), scorer Panel::scorer())
//   Each a share, and each item's value the mean of its judges' readings
//   (PoLL's average pooling), an unread reading left out of its item:
//     facts_told           facts a reader learns from the article
//     facts_told_partly    facts it tells at least part of
//     core_facts_told      core facts told
//     facts_agreement      facts every judge read alike
//     sentences_supported  sentences the video supports ("none")
//     adds, softens, intensifies, misattributes, contradicts
//                          sentences read so
//     sentences_agreement  sentences every judge read alike
//   Low agreement is a panel to doubt, and the items its judges read unalike
//   are the labels a person settles first (plan P27). A share with nothing
//   to share is no score (as rr/eval/score.hpp says).
//
// Thread-safety: the stage is immutable, and the functions pure, but for
//   start_judgement() and run_judgement(), which read and write the library:
//   one judgement of an article at a time.
#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <rr/core/clock.hpp>
#include <rr/core/result.hpp>
#include <rr/db/database.hpp>
#include <rr/eval/gold.hpp>
#include <rr/eval/panel.hpp>
#include <rr/eval/run.hpp>
#include <rr/json/json.hpp>
#include <rr/knowledge/article.hpp>
#include <rr/knowledge/atoms.hpp>
#include <rr/knowledge/transcript.hpp>
#include <rr/library/jobs.hpp>
#include <rr/library/stage_store.hpp>
#include <rr/pipeline/job_run.hpp>
#include <rr/pipeline/judging.hpp>
#include <rr/pipeline/stage.hpp>

namespace rr::eval {

// What a judgement reads: an article job's artifacts, and the facts of its
// video's gold where a person reviewed it for the snapshot the job read.
struct ArticleToJudge {
    std::string title{};  // the video's, for the prompts' shared part
    std::string channel{};
    knowledge::Transcript transcript{};
    knowledge::AtomLedger ledger{};
    knowledge::Article article{};
    std::vector<Fact> facts{};  // none: only the sentences are read

    friend bool operator==(const ArticleToJudge&, const ArticleToJudge&) = default;
};

// One sentence of an article, as the judges are shown it.
struct ArticleSentence {
    std::size_t number = 0;                  // 1, 2, … in the article
    std::size_t paragraph = 0;               // 1, 2, … in the article
    std::string section{};                   // the heading it is under; empty before the first
    std::string text{};                      // as a reader reads it: without its markers
    std::vector<knowledge::AtomId> cited{};  // the atoms it cites that the ledger has

    friend bool operator==(const ArticleSentence&, const ArticleSentence&) = default;
};

// The sentences of `article`'s paragraphs, list items and quotes, in order;
// headings and code are no sentences.
[[nodiscard]] std::vector<ArticleSentence> article_sentences(const knowledge::Article& article,
                                                             const knowledge::AtomLedger& ledger);

// The line a judge is shown for a sentence of the video: "S12 [1:02, Ada]
// The words." A label shows a person the same (rr/eval/labels.hpp).
[[nodiscard]] std::string video_line(const knowledge::Sentence& sentence);

// The most sentences a fact's judge is shown.
inline constexpr std::size_t kMostCandidates = 12;

// The sentences of `sentences` most likely to tell `fact` (see the header
// comment), by their numbers, in the article's order.
[[nodiscard]] std::vector<std::size_t> fact_candidates(const Fact& fact,
                                                       const std::vector<ArticleSentence>& sentences,
                                                       const knowledge::Transcript& transcript,
                                                       const knowledge::AtomLedger& ledger);

// The video's sentences `sentence` rests on: those of the atoms it cites, in
// the order said, each once; `paragraph`'s when it cites none.
[[nodiscard]] std::vector<knowledge::SentenceId> evidence_of(const ArticleSentence& sentence,
                                                             const std::vector<ArticleSentence>& paragraph,
                                                             const knowledge::AtomLedger& ledger);

// What is wrong with `evidence`, an answer's evidence, given what the judge was
// shown: each passage it quotes between double quotes (straight or curly) must
// be in `shown`, word for word, case and punctuation aside. `required` when
// the verdict needs evidence (a fact told, or told in part).
[[nodiscard]] std::vector<std::string> evidence_problems(std::string_view evidence,
                                                         const std::vector<std::string>& shown,
                                                         bool required);

enum class FactVerdict : std::uint8_t {
    told,
    partly,
    not_told
};

[[nodiscard]] std::string_view to_string(FactVerdict verdict) noexcept;
// The inverse of to_string(); none for anything else.
[[nodiscard]] std::optional<FactVerdict> fact_verdict_from_string(std::string_view name) noexcept;

// One judge's reading of one item: what it answered, or why it could not.
struct FactReading {
    std::string judge{};                   // its role
    std::optional<FactVerdict> verdict{};  // none: unread
    std::string evidence{};
    std::string unread{};  // why, when unread

    friend bool operator==(const FactReading&, const FactReading&) = default;
};

struct JudgedFact {
    std::string fact{};  // its id: "F3"
    bool core = false;
    std::vector<std::size_t> candidates{};  // the sentences shown
    std::vector<FactReading> readings{};    // a judge each, in the panel's order; none: no candidate

    friend bool operator==(const JudgedFact&, const JudgedFact&) = default;
};

struct SentenceReading {
    std::string judge{};
    std::optional<pipeline::Issue> issue{};  // none: unread
    std::string evidence{};
    std::optional<std::string> reading{};
    std::string unread{};

    friend bool operator==(const SentenceReading&, const SentenceReading&) = default;
};

struct JudgedSentence {
    std::size_t number = 0;
    std::size_t paragraph = 0;
    std::string text{};
    std::vector<knowledge::SentenceId> evidence{};  // the video's sentences shown
    std::vector<SentenceReading> readings{};

    friend bool operator==(const JudgedSentence&, const JudgedSentence&) = default;
};

// The judgement stage's artifact (schemas/artifacts/judgement.json).
struct Judgement {
    std::string panel{};
    std::vector<std::string> judges{};  // their roles, in the panel's order
    std::vector<JudgedFact> facts{};
    std::vector<JudgedSentence> sentences{};

    friend bool operator==(const Judgement&, const Judgement&) = default;
};

[[nodiscard]] json::Json to_json(const Judgement& judgement);
// Errc::invalid_argument when `document` breaks the schema.
[[nodiscard]] Result<Judgement> judgement_from_json(const json::Json& document);

// The scores of `judgement` (the header comment), in that order.
[[nodiscard]] std::vector<Score> judged_scores(const Judgement& judgement);

// The judgement job of `article`, an article job, under `panel`: the one it
// has (migrations/0007_judgement.sql), done or to be taken up, or a new one,
// recorded as its. Its source is the article job's id, its tier the panel's
// name, its salt 0: greedy judges draw nothing.
[[nodiscard]] Result<library::Job> start_judgement(db::Database& database, const Clock& clock,
                                                   const library::Job& article, const Panel& panel);

// What a judgement of `article` (a job done) reads: its artifacts as its page
// shows them (report::contents_of()), and `facts`. Errc::invalid_argument for
// a job with no article.
[[nodiscard]] Result<ArticleToJudge> article_to_judge(const library::Job& article,
                                                      const library::StageStore& store,
                                                      std::vector<Fact> facts);

// A judgement's job, as it stands after its run.
struct JudgementRun {
    pipeline::JobRun run{};
    std::optional<Scoring> scoring{};  // the panel's scores, once the job is done
};

// Runs `job` (start_judgement()) through its stage, and, once it is done,
// scores its judgement (judged_scores(), scorer panel.scorer()). `context`
// gives the army, the prompts and the stop; the work directory is the job's
// own, under its work_directory.
[[nodiscard]] Result<JudgementRun> run_judgement(library::Job job, const Panel& panel, ArticleToJudge article,
                                                 db::Database& database, const Clock& clock,
                                                 pipeline::StageContext context);

class JudgementStage final : public pipeline::Stage {
public:
    static constexpr std::string_view kName = "judgement";

    // `panel` must have passed check_panel() against the prompts the context
    // will give.
    JudgementStage(Panel panel, ArticleToJudge article);

    [[nodiscard]] std::string_view name() const override { return kName; }
    [[nodiscard]] int version() const override { return kJudgingProcedure; }
    [[nodiscard]] std::vector<std::string> inputs() const override { return {}; }
    [[nodiscard]] std::string_view schema() const override { return "artifacts/judgement"; }

    // The panel, the hashes of what it reads, and its judges' engines as
    // they sample. Errc::needs_user without a pod, or one that serves other
    // judges than the panel's (judges_in()).
    [[nodiscard]] Result<json::Json> fingerprint(const pipeline::StageContext& context) const override;

    // Errors: the army's. An item a judge could not read is a finding.
    [[nodiscard]] Result<pipeline::StageOutput> run(const pipeline::StageContext& context) const override;

private:
    Panel panel_;
    ArticleToJudge article_;
};

}  // namespace rr::eval
