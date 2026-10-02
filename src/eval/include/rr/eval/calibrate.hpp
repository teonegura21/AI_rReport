// rr/eval/calibrate.hpp — the panel's readings, as a person labels them and
// as calibration compares them: `rreport eval label` and `rreport eval
// calibrate` (ADR-0019, runbook R5).
//
// Why this exists
//   Labels (rr/eval/labels.hpp) are drawn from what the panel read of a run,
//   and calibration (rr/eval/calibration.hpp) needs what each judge said of
//   every item labelled. Both read the library: each entry's judgement (the
//   judgement job of its article under the panel, done), each pair's pairing
//   (the pairing job of its two articles, done), and the articles, the
//   transcript and the gold they read, so that a label shows a person what
//   the judges were shown. Nothing here asks a model: a judgement or pairing
//   not done is left out, and said.
//
// Thread-safety: none; one call at a time on a database.
#pragma once

#include <cstddef>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <rr/core/clock.hpp>
#include <rr/core/result.hpp>
#include <rr/db/database.hpp>
#include <rr/eval/calibration.hpp>
#include <rr/eval/flow.hpp>
#include <rr/eval/gold.hpp>
#include <rr/eval/judgement.hpp>
#include <rr/eval/labels.hpp>
#include <rr/eval/panel.hpp>
#include <rr/eval/run.hpp>
#include <rr/library/stage_store.hpp>

namespace rr::eval {

// The judgement of the article job `article` under `panel`, once its job is
// done; none before, or when it was never judged.
[[nodiscard]] Result<std::optional<Judgement>> find_judgement(db::Database& database, const Clock& clock,
                                                              std::string_view article, const Panel& panel);

// The pairing job of `a` and `b` (in that order) under `panel`; none when
// they were never paired.
[[nodiscard]] Result<std::optional<std::string>> find_pairing_job(db::Database& database, std::string_view a,
                                                                  std::string_view b, const Panel& panel);

// The pairing of the pairing job `job`, once it is done; none before.
[[nodiscard]] Result<std::optional<Pairing>> find_pairing(db::Database& database, const Clock& clock,
                                                          std::string_view job);

// Every item the panel read of a run, as a label shows it, its verdict
// empty, in the run's order (video, replicate, then the item's place).
struct LabelPool {
    std::vector<FactLabel> facts{};
    std::vector<SentenceLabel> sentences{};
    std::vector<PartLabel> parts{};
    std::vector<std::string> notes{};  // what could not be read, and why: a judgement not done, no gold
};

// Each video's gold facts, as its reviewed gold has them (reviewed_gold()).
using FactsByVideo = std::map<std::string, std::vector<Fact>, std::less<>>;

// The facts and sentences the panel read of `entries` (a run's), and the
// parts it read of `pairs` (the run's against another, pairs_to_read();
// none for no part). `facts` gives each fact's words: a video it has none
// of has no fact drawn, and says so. `name` draws each part's order
// (b_first()).
[[nodiscard]] Result<LabelPool> label_pool(db::Database& database, const Clock& clock,
                                           const FactsByVideo& facts, std::span<const RunEntry> entries,
                                           std::span<const VideoPairs> pairs, const Panel& panel,
                                           std::string_view name);

struct LabelCounts {
    std::size_t facts = 15;
    std::size_t sentences = 15;
    std::size_t parts = 10;  // 40 in all: about an hour (ADR-0019)
};

// `counts` of each kind of `pool`, drawn (draw(), under "<name>/facts" and
// so on), as the file `name` holds them.
[[nodiscard]] Labels draw_labels(std::string name, std::string panel, std::string run, std::string against,
                                 const LabelPool& pool, const LabelCounts& counts);

// The three kinds of reading, as calibration reports them.
inline constexpr std::string_view kFactsKind = "facts";
inline constexpr std::string_view kSentencesKind = "sentences";
inline constexpr std::string_view kPartsKind = "parts";

struct KindCalibration {
    std::string kind{};
    std::size_t labelled = 0;              // items with a verdict
    std::size_t skipped = 0;               // items left without one
    std::vector<JudgeAgreement> judges{};  // in the panel's order

    // Every judge passes, on one item at least.
    [[nodiscard]] bool passes() const noexcept;
};

struct JudgeBiases {
    std::string judge{};
    Biases biases{};
};

struct Calibration {
    std::vector<KindCalibration> kinds{};  // facts, sentences, parts
    std::vector<JudgeBiases> biases{};     // over every part of the two runs' pairings
    std::vector<std::string> notes{};      // items whose readings could not be found, and why

    // Every kind passes: the panel reads as the person does.
    [[nodiscard]] bool passes() const noexcept;
};

// The panel's readings of each item of `labels` with a verdict, against it;
// and its biases over every part of the pairings of `pairs`.
[[nodiscard]] Result<Calibration> calibrate(db::Database& database, const Clock& clock, const Labels& labels,
                                            const Panel& panel, std::span<const VideoPairs> pairs);

// As `rreport eval calibrate` prints it.
[[nodiscard]] std::string to_text(const Calibration& calibration, const Labels& labels, const Panel& panel);

}  // namespace rr::eval
