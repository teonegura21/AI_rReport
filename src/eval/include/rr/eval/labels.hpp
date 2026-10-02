// rr/eval/labels.hpp — a person's verdicts on what the eval's judges read:
// the file `rreport eval label` writes, a person fills in, and `rreport eval
// calibrate` reads (ADR-0019, runbook R5).
//
// Why this exists
//   The panel's scores count once a person's labels show that it reads as
//   they do (rr/eval/calibration.hpp). A label is the panel's own question,
//   asked of a person, about an item the panel read:
//     * a fact: is this gold fact told by these sentences of the article?
//       (told, partly, not_told: eval_fact's answer);
//     * a sentence: does the video support it? (none, adds, softens,
//       intensifies, misattributes, contradicts: eval_support's);
//     * a part: which of two versions of a stretch of the video reads
//       better? (first, second, tie: eval_flow's).
//   The file holds what the judges were shown, so that a person answers on
//   the same evidence, and never what they answered: labels are made
//   blind. A part's two versions are shown in a drawn order, recorded, so
//   that the person's own taste for whatever comes first averages out.
//
// Which items
//   Drawn uniformly, without replacement, from every item of a run the
//   panel read (a fact or sentence one judge read at least; a part one
//   judge read in both orders), by `mt19937_64` seeded with the FNV-1a of
//   the file's name, as the bootstrap draws (rr/eval/stats.hpp): the same
//   on every machine. Uniform, and never enriched with the items the
//   judges disagree on: a sample of hard items would bias κ and the
//   precisions low, and they must say how the panel reads the run. A rare
//   class may go undrawn: draw more under another name.
//
// Every kind
//   The panel has one `calibrated` flag (rr/eval/panel.hpp), and the gate
//   counts every metric it judges or none: so calibration asks for labels
//   of all three kinds, and a kind without one cannot pass.
//
// The file (eval/labels/<name>.toml), read strictly:
//     name = "r5"
//     panel = "eval-v1"
//     run = "r4-max"          # whose judgements the facts and sentences are
//     against = "r4-fast"     # the run its articles were read against, for flow
//     labeller = ""           # who gave the verdicts
//     [[fact]] job, video, replicate, fact, core, said, claim, quote, shown, verdict
//     [[sentence]] job, video, replicate, number, text, evidence, verdict
//     [[part]] job, video, replicate, part, stretch, order, first, second, verdict
//   An empty verdict skips its item; a verdict filled must be one of its
//   kind's answers.
//
// Thread-safety: plain values and pure functions, but for read_labels(),
// which reads a file.
#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <rr/core/result.hpp>
#include <rr/eval/judgement.hpp>
#include <rr/pipeline/judging.hpp>
#include <rr/reason/pairwise.hpp>

namespace rr::eval {

struct FactLabel {
    std::string job{};  // the article job the judgement read
    std::string video{};
    int replicate = 0;
    std::string fact{};  // its id in the video's gold: "F3"
    bool core = false;
    std::string said{};                    // when the video says it: "12:03 to 12:20"
    std::string claim{};                   // what it says, as the judges read it
    std::string quote{};                   // the words that say it
    std::vector<std::string> shown{};      // the article's sentences the judges read: "14. The speaker …"
    std::optional<FactVerdict> verdict{};  // the person's; none: skipped

    friend bool operator==(const FactLabel&, const FactLabel&) = default;
};

struct SentenceLabel {
    std::string job{};
    std::string video{};
    int replicate = 0;
    std::size_t number = 0;  // in the article, from 1
    std::string text{};
    std::vector<std::string> evidence{};       // the video's sentences the judges read: "1:31 So we …"
    std::optional<pipeline::Issue> verdict{};  // the person's; none: skipped

    friend bool operator==(const SentenceLabel&, const SentenceLabel&) = default;
};

struct PartLabel {
    std::string job{};  // the pairing job
    std::string video{};
    int replicate = 0;
    std::size_t part = 0;   // its place among the pairing's parts, from 0
    std::string stretch{};  // "1:30 to 3:55"
    bool b_first = false;   // the order shown: the run read against first ("ba"), or not ("ab")
    std::string first{};    // the version shown first: its headings and prose
    std::string second{};
    std::optional<reason::Preference> verdict{};  // the person's, as shown; none: skipped

    // The verdict, as the side it prefers: A is the run, B the run read
    // against it. None when skipped.
    [[nodiscard]] std::optional<reason::Winner> winner() const noexcept;

    friend bool operator==(const PartLabel&, const PartLabel&) = default;
};

struct Labels {
    std::string name{};
    std::string panel{};
    std::string run{};       // its label
    std::string against{};   // empty: no part drawn
    std::string labeller{};  // empty until a person gives the verdicts
    std::vector<FactLabel> facts{};
    std::vector<SentenceLabel> sentences{};
    std::vector<PartLabel> parts{};

    // Whether a person has given any verdict yet: such a file is never
    // written over.
    [[nodiscard]] bool any_verdict() const noexcept;

    friend bool operator==(const Labels&, const Labels&) = default;
};

[[nodiscard]] std::filesystem::path labels_file(const std::filesystem::path& eval_root,
                                                std::string_view name);

// As a person reads and fills it: a comment that says how, then each item
// with what the judges were shown and an empty verdict, or the one given.
[[nodiscard]] std::string to_toml(const Labels& labels);

// Errc::invalid_argument, saying where, for a name not the file's, a key
// unknown or missing, a verdict that is not its kind's, a part's order
// that is neither "ab" nor "ba".
[[nodiscard]] Result<Labels> parse_labels(std::string_view name, std::string_view text);
// The labels `name` of `eval_root`, as parse_labels() reads them.
// Errc::invalid_argument for a name that is no id (never a path);
// Errc::not_found for no such file.
[[nodiscard]] Result<Labels> read_labels(const std::filesystem::path& eval_root, std::string_view name);

// `count` of the indices 0 … `population` − 1, drawn uniformly without
// replacement under the seed of `name` (see the header comment), in
// increasing order; all of them when `count` is not less.
[[nodiscard]] std::vector<std::size_t> draw(std::size_t population, std::size_t count, std::string_view name);

// Whether a part's order is drawn B first, for the file `name`: a coin per
// part, under its own seed (the file's name, the video, the replicate, the
// part's place), so that adding parts changes no other's, and the same
// labels draw the same orders on every machine.
[[nodiscard]] bool b_first(std::string_view name, std::string_view video, int replicate, std::size_t part);

}  // namespace rr::eval
