// rr/eval/annotate.hpp — a video's silver annotation: what it says, drafted by
// models for a person to correct into gold.
//
// Why this exists
//   Writing down everything a lecture says takes a person most of a day;
//   correcting a good draft takes an hour or two (runbook R2). So models
//   draft it (ADR-0019), in a pipeline stage after the transcript:
//     1. The transcript is cut into windows of whole sentences, each sent
//        with a few sentences of context on either side.
//     2. The commander reads each window, thinking, and writes down what it
//        says (prompts/annotate), three times: three samples of one reading
//        of the prompt. Every item cites the window's sentences that say it,
//        and quotes them. A quote that is not what they say, word for word,
//        and an item citing a sentence outside the window, are sent back.
//     3. The three lists are joined. Two facts are one when they cite a
//        sentence in common and one's quote holds the other's: kept once,
//        with how many of the drafts found it. The reviewer checks first a
//        fact only one draft found.
//     4. A model of the other family (the infantry) reads each stretch again
//        with the facts found so far, and adds what they miss
//        (prompts/annotate_more): two families see differently (ADR-0017).
//   Silver is never scored: a person corrects it into gold (rr/eval/gold.hpp).
//   As a stage, it is keyed, cached, accounted and resumable like any other:
//   an annotation interrupted halfway asks again none of the calls it made.
//
// Windows, and the pod
//   A window is as large as the engine that reads it holds. Each is filled
//   with whole sentences up to a size the engine suggests (window_sizes():
//   the commander's up to 12,000 bytes of lines, room left for its thinking
//   and an answer twice as long; the other family's up to 3,000, with the
//   facts found in them beside), then its mission is built and, while the
//   engine would not hold it (agents::Army::holds()), its last sentence goes
//   to the next window. So a prompt is never refused for its size, whatever
//   the title, the context or the facts found. A sentence too long for its
//   engine even alone is a finding. The sizes, and what each engine holds,
//   enter the stage's key.
//
// Thread-safety: the stage is immutable, and the functions pure, but for
//   start_annotation() and run_annotation(), which read and write the eval's
//   files and the library: one annotation of a video at a time.
#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <rr/agents/army.hpp>
#include <rr/core/clock.hpp>
#include <rr/core/result.hpp>
#include <rr/core/time_span.hpp>
#include <rr/db/database.hpp>
#include <rr/eval/gold.hpp>
#include <rr/eval/set.hpp>
#include <rr/json/json.hpp>
#include <rr/knowledge/ids.hpp>
#include <rr/knowledge/transcript.hpp>
#include <rr/library/jobs.hpp>
#include <rr/pipeline/job_run.hpp>
#include <rr/pipeline/stage.hpp>

namespace rr::eval {

// A window of the transcript: its own sentences [begin, end), and with its
// context [context_begin, context_end), as indices of transcript.sentences.
struct SentenceWindow {
    std::size_t begin = 0;
    std::size_t end = 0;
    std::size_t context_begin = 0;
    std::size_t context_end = 0;

    friend bool operator==(const SentenceWindow&, const SentenceWindow&) = default;
};

// A window's context, on either side: up to three sentences, as many of them
// as fit 1,000 bytes of lines. It lets a model read the window's first and
// last sentences as they were said.
inline constexpr std::size_t kContextSentences = 3;
inline constexpr std::size_t kContextBytes = 1'000;

// The window of the sentences [begin, end), which must be some of
// `transcript`'s, with its context: on either side, the nearest sentences,
// up to `context` of them, as many as fit kContextBytes of lines.
[[nodiscard]] SentenceWindow window_of(const knowledge::Transcript& transcript, std::size_t begin,
                                       std::size_t end, std::size_t context = kContextSentences);

// The window from sentence `begin`, which must be one of `transcript`'s: as
// many whole sentences as fit `bytes` of their lines (sentence_lines(), each
// with its newline), one at least, with its context (window_of()).
[[nodiscard]] SentenceWindow window_from(const knowledge::Transcript& transcript, std::size_t begin,
                                         std::size_t bytes, std::size_t context = kContextSentences);

// The window sizes, in bytes of lines, that the army's engines suggest (see
// the header comment): what a window is filled to before it is checked
// against its engine.
struct WindowSizes {
    std::size_t annotate = 12'000;  // the commander's windows, at most
    std::size_t more = 3'000;       // the other family's, at most

    friend bool operator==(const WindowSizes&, const WindowSizes&) = default;
};

[[nodiscard]] WindowSizes window_sizes(const agents::Army& army);

// Sentences [begin, end) as the prompts show them, a line each:
// "S12 [3:04] What was said."
[[nodiscard]] std::string sentence_lines(const knowledge::Transcript& transcript, std::size_t begin,
                                         std::size_t end);

// One answer, read (schemas/missions/annotate.json).
struct DraftFact {
    std::vector<knowledge::SentenceId> sentences{};
    std::string quote{};
    std::string claim{};
    Importance importance = Importance::core;
};

struct DraftQuote {
    std::vector<knowledge::SentenceId> sentences{};
    std::string quote{};
};

struct Draft {
    std::vector<DraftFact> facts{};
    std::vector<DraftQuote> must_quotes{};
    std::vector<Term> terms{};
};

// `answer`, which follows its schema, read.
[[nodiscard]] Draft draft_from_json(const json::Json& answer);

// What is wrong with `answer` as a draft of `window` (step 2 of the header
// comment), each said as what to do instead, at most a few; empty when
// nothing is.
[[nodiscard]] std::vector<std::string> draft_problems(const json::Json& answer,
                                                      const knowledge::Transcript& transcript,
                                                      const SentenceWindow& window);

// The silver annotation.
struct SilverFact {
    std::vector<knowledge::SentenceId> sentences{};  // in order, without repeats
    Millis start{0};
    Millis end{0};
    std::string quote{};
    std::string claim{};
    Importance importance = Importance::core;
    int found = 0;  // the drafts that found it; 0: added by the other family

    friend bool operator==(const SilverFact&, const SilverFact&) = default;
};

struct SilverQuote {
    std::vector<knowledge::SentenceId> sentences{};
    Millis start{0};
    Millis end{0};
    std::string quote{};

    friend bool operator==(const SilverQuote&, const SilverQuote&) = default;
};

struct Silver {
    std::vector<SilverFact> facts{};  // in the order said
    std::vector<SilverQuote> must_quotes{};
    std::vector<Term> terms{};
    std::string drafted_by{};

    friend bool operator==(const Silver&, const Silver&) = default;
};

// The drafts of one stretch, joined (step 3 of the header comment). A fact
// most drafts call detail is detail; otherwise, core: an account that keeps
// too much is corrected faster than one that misses the point.
[[nodiscard]] Silver join_drafts(const std::vector<Draft>& drafts, const knowledge::Transcript& transcript);

// Adds what `more` has that `silver` misses (step 4), found by none of the
// drafts. Silver stays in the order said.
void add_missed(Silver& silver, const Draft& more, const knowledge::Transcript& transcript);

[[nodiscard]] json::Json to_json(const Silver& silver);

// Errc::invalid_argument when `document` breaks the schema
// (schemas/artifacts/silver.json).
[[nodiscard]] Result<Silver> silver_from_json(const json::Json& document);

// The first gold file of `video`: silver, unreviewed, its facts numbered
// F1, F2, … in the order said.
[[nodiscard]] Gold silver_to_gold(const Silver& silver, std::string video, std::string snapshot);

// The job that drafts `video`'s annotation, from its snapshot: an annotation
// job (library::JobKind::annotation) taken up again when an earlier one did
// not finish (its memo answers every call it made), a new one otherwise.
// Errors:
//   Errc::not_found  no snapshot: `rreport eval fetch` reads it first;
//   Errc::conflict   a gold file exists. A reviewed one is never replaced
//                    by a draft; a silver one only when `replace` says so.
[[nodiscard]] Result<library::Job> start_annotation(const std::filesystem::path& eval_root,
                                                    const EvalVideo& video, db::Database& database,
                                                    const Clock& clock, bool replace);

// An annotation's job, as it stands after its run.
struct Annotated {
    pipeline::JobRun run{};
    std::optional<Gold> gold{};  // the silver written to the gold file, once the job is done
};

// Runs `job` (start_annotation()) through the stages source (the snapshot),
// transcript and annotate, and once it is done, writes the silver to the
// video's gold file (to_toml()). `context` gives the army, the prompts and
// the stop; the work directory is the job's own, under its
// work_directory.
[[nodiscard]] Result<Annotated> run_annotation(const std::filesystem::path& eval_root, const EvalVideo& video,
                                               library::Job job, db::Database& database, const Clock& clock,
                                               pipeline::StageContext context);

class AnnotateStage final : public pipeline::Stage {
public:
    static constexpr std::string_view kName = "annotate";
    static constexpr int kSamples = 3;

    [[nodiscard]] std::string_view name() const override { return kName; }
    [[nodiscard]] int version() const override { return 1; }
    [[nodiscard]] std::vector<std::string> inputs() const override { return {"source", "transcript"}; }
    [[nodiscard]] std::string_view schema() const override { return "artifacts/silver"; }
    // The commander's three drafts sample, from seeds the job's salt derives.
    [[nodiscard]] bool samples() const override { return true; }

    // The prompts by version and hash, the army (which it raises), the
    // window sizes and what each engine holds, and the samples.
    // Errc::needs_user without a pod: drafting needs models.
    [[nodiscard]] Result<json::Json> fingerprint(const pipeline::StageContext& context) const override;

    // Errors: the army's. A window no draft passed its checks for, and a
    // sentence too long for its engine, are findings naming the sentences
    // and their minutes.
    [[nodiscard]] Result<pipeline::StageOutput> run(const pipeline::StageContext& context) const override;
};

}  // namespace rr::eval
