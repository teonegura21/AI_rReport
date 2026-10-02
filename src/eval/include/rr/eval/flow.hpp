// rr/eval/flow.hpp — which of two articles of one video reads better, part
// by part, by the eval's frozen judges, each part in both orders.
//
// Why this exists
//   How an article reads is ADR-0019's form metric, and it cannot be scored
//   alone: "reads well" means "reads better than that one". So it is judged
//   pairwise, between the articles of one video in two runs (`rreport eval
//   compare --flow`), by the panel's judges (rr/eval/panel.hpp) with their
//   own prompt, eval_flow, never the pipeline's flow, which the writer's
//   drafts are chosen by.
//
// Parts
//   The two articles retell one transcript, but may cut it differently: two
//   tiers outline the same video in other sections. So each article is cut
//   into its sections (its "## " headings), each section found on the video
//   by the sentences of the atoms it cites, and the two articles' sections
//   are joined wherever they overlap there. Each part then covers the same
//   stretch of the video in both articles: the same things told, to be
//   judged only on how they read. Two articles of one outline make a part a
//   section. A section citing nothing is read with the section before it
//   (the first, with the one after); a stretch one article tells and the
//   other does not is no part: what an article tells is coverage, measured
//   elsewhere.
//
// Pairs
//   Replicate k of A is read with replicate k of B, by their numbers' order
//   (pairs_to_read()): two runs starting at the same replicate pair articles
//   drawn under the same salts, which differ by the change, not the dice.
//
// Judging
//   Every judge reads every part twice: A's version first, then B's first,
//   greedily, without thinking. A side wins only when both orders prefer it
//   (reason::swapped()): a judge that prefers whatever comes first gives a
//   tie, and its disagreements are counted, its position bias; a reading one
//   of whose orders no answer settled is unread. A part is worth +1 when B
//   wins, −1 when A wins, 0 for a tie, averaged over the judges that read it
//   (PoLL); a pair is worth the mean over its parts read, and a video the
//   mean over its pairs (flow_comparison()). Its value is thus B's
//   preference, from −1 (A always) to +1 (B always): a difference already,
//   resampled over videos as any other. A part its judge's engine cannot
//   hold is left unread, a finding: a part is the smallest stretch both
//   articles tell alike, and cannot be cut.
//
// As a job
//   A pairing is a library job of its own kind (library::JobKind::pairing,
//   migrations/0009_pairing.sql) with one stage, this one, keyed by both
//   articles, their atoms, the transcript, the title, the panel and its
//   judges' engines (how much each holds decides which parts are read): a
//   pair judged again, unchanged, asks nothing, even through other jobs; one
//   interrupted is taken up by its memo; and its calls are never either
//   article's costs.
//
// Thread-safety: the stage is immutable, and the functions pure, but for
//   start_pairing() and run_pairing(), which read and write the library.
#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <rr/core/clock.hpp>
#include <rr/core/result.hpp>
#include <rr/db/database.hpp>
#include <rr/eval/panel.hpp>
#include <rr/eval/run.hpp>
#include <rr/eval/stats.hpp>
#include <rr/json/json.hpp>
#include <rr/knowledge/article.hpp>
#include <rr/knowledge/atoms.hpp>
#include <rr/knowledge/ids.hpp>
#include <rr/knowledge/transcript.hpp>
#include <rr/library/jobs.hpp>
#include <rr/library/stage_store.hpp>
#include <rr/pipeline/job_run.hpp>
#include <rr/pipeline/stage.hpp>
#include <rr/reason/pairwise.hpp>

namespace rr::eval {

// Its name in the registry (rr/eval/metrics.hpp) and in a comparison.
inline constexpr std::string_view kFlowMetric = "flow";

// A section of an article, and the stretch of its video it tells.
struct ArticleSection {
    std::string heading{};
    std::string text{};  // its heading and its prose, as a reader reads them: no marker
    std::optional<knowledge::SentenceId> first{};  // of the video's sentences its atoms say; none:
    std::optional<knowledge::SentenceId> last{};   // it cites nothing

    friend bool operator==(const ArticleSection&, const ArticleSection&) = default;
};

// `article`'s sections, in order. Prose before the first heading is a
// section without one.
[[nodiscard]] std::vector<ArticleSection> article_sections(const knowledge::Article& article,
                                                           const knowledge::AtomLedger& ledger);

// A stretch of the video both articles tell, and their sections over it.
struct Part {
    knowledge::SentenceId first;
    knowledge::SentenceId last;
    std::vector<std::size_t> a{};  // indices of A's sections, in order
    std::vector<std::size_t> b{};

    friend bool operator==(const Part&, const Part&) = default;
};

// The sections of `a` and `b` joined where they overlap on the video, in
// its order; a section citing nothing goes with the one before it (the
// first, with the one after). A stretch only one article tells is left out.
[[nodiscard]] std::vector<Part> parts_of(const std::vector<ArticleSection>& a,
                                         const std::vector<ArticleSection>& b);

// One judge's two readings of a part: A's version shown first, then B's.
struct PartReading {
    std::string judge{};                          // its role
    std::optional<reason::Preference> a_first{};  // as it was shown; none: unread
    std::optional<reason::Preference> b_first{};
    std::string reasons{};  // what it said with A first, then with B first, for a person to check
    std::string unread{};   // why not read, when it was not

    friend bool operator==(const PartReading&, const PartReading&) = default;
};

struct PairedPart {
    Part part;
    std::vector<PartReading> readings{};  // one a judge, in the panel's order

    friend bool operator==(const PairedPart&, const PairedPart&) = default;
};

// The flow stage's artifact (schemas/artifacts/pairing.json).
struct Pairing {
    std::string panel{};
    std::vector<std::string> judges{};
    std::vector<PairedPart> parts{};

    friend bool operator==(const Pairing&, const Pairing&) = default;
};

[[nodiscard]] json::Json to_json(const Pairing& pairing);
// Errc::invalid_argument when `document` breaks the schema.
[[nodiscard]] Result<Pairing> pairing_from_json(const json::Json& document);

// What a pairing says.
struct FlowScore {
    std::optional<double> b_preferred{};  // from −1 (A always) to +1 (B always); none: no part read
    std::size_t parts = 0;                // read by a judge at least
    std::size_t inconsistent = 0;         // readings whose two orders disagreed: position bias

    friend bool operator==(const FlowScore&, const FlowScore&) = default;
};

[[nodiscard]] FlowScore flow_score(const Pairing& pairing);

// One video's entries `eval compare --flow` pairs.
struct VideoPairs {
    std::string video{};
    std::vector<std::pair<RunEntry, RunEntry>> pairs{};  // A's entry, B's
    std::size_t a_replicates = 0;  // each run's of it: when they differ, the first of each are paired
    std::size_t b_replicates = 0;

    friend bool operator==(const VideoPairs&, const VideoPairs&) = default;
};

// Each video both runs have, in order: replicate k of A paired with
// replicate k of B, by their numbers' order, so that two runs starting at
// the same replicate pair entries drawn under the same salts (ADR-0020).
[[nodiscard]] std::vector<VideoPairs> pairs_to_read(std::span<const ScoredEntry> a,
                                                    std::span<const ScoredEntry> b);

// Flow, as `eval compare --flow` shows it: each video's B preference, the
// mean over its pairs read (`read`: each pair's b_preferred), compared as
// differences (compare_pairwise()) under `panel`'s scorer. A video none of
// whose pairs was read is left out.
[[nodiscard]] Comparison flow_comparison(const Panel& panel,
                                         const std::map<std::string, std::vector<double>, std::less<>>& read);

// What a pairing reads: the video's title and channel, its transcript, and
// each article with the atoms it cites.
struct ArticlesToCompare {
    std::string title{};
    std::string channel{};
    knowledge::Transcript transcript{};
    knowledge::AtomLedger a_ledger{};
    knowledge::Article a{};
    knowledge::AtomLedger b_ledger{};
    knowledge::Article b{};
};

// What a pairing of `a` and `b`, article jobs done of one snapshot, reads.
// Errc::invalid_argument for a job with no article, or two jobs of other
// transcripts: their parts would not be the same video's.
[[nodiscard]] Result<ArticlesToCompare> articles_to_compare(const library::Job& a, const library::Job& b,
                                                            const library::StageStore& store);

// The pairing job of `a` and `b` under `panel`: the one they have
// (migrations/0009_pairing.sql), or a new one, recorded as theirs. Its
// source names both jobs, its tier the panel, its salt 0.
[[nodiscard]] Result<library::Job> start_pairing(db::Database& database, const Clock& clock,
                                                 const library::Job& a, const library::Job& b,
                                                 const Panel& panel);

struct PairingRun {
    pipeline::JobRun run{};
    std::optional<FlowScore> score{};  // once the job is done
};

// Runs `job` (start_pairing()) through its stage, and scores it once done.
[[nodiscard]] Result<PairingRun> run_pairing(library::Job job, const Panel& panel, ArticlesToCompare articles,
                                             db::Database& database, const Clock& clock,
                                             pipeline::StageContext context);

class FlowStage final : public pipeline::Stage {
public:
    static constexpr std::string_view kName = "flow";

    FlowStage(Panel panel, ArticlesToCompare articles);

    [[nodiscard]] std::string_view name() const override { return kName; }
    [[nodiscard]] int version() const override { return kJudgingProcedure; }
    [[nodiscard]] std::vector<std::string> inputs() const override { return {}; }
    [[nodiscard]] std::string_view schema() const override { return "artifacts/pairing"; }

    // The panel, the hashes of what it reads, and its judges' engines.
    // Errc::needs_user without a pod, or one serving other judges.
    [[nodiscard]] Result<json::Json> fingerprint(const pipeline::StageContext& context) const override;

    [[nodiscard]] Result<pipeline::StageOutput> run(const pipeline::StageContext& context) const override;

private:
    Panel panel_;
    ArticlesToCompare articles_;
};

}  // namespace rr::eval
