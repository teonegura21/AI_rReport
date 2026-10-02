// rr/eval/gate.hpp — whether a candidate run may replace its baseline
// (ADR-0019): every invariant kept by every entry, and no gated metric worse
// than its margin at the worse end of its interval.
//
// Why this exists
//   A change to a prompt, a model or a strategy is kept only when the
//   numbers say so, and "the numbers" must not mean "the mean happened to
//   rise". A gate (eval/gates/<name>.toml), a file a person reviews, says
//   what they must say:
//     * Invariants: what every entry of the candidate, every video and
//       replicate, must be at least as good as. A faithful article accounts
//       for every sentence, writes no marker that names no atom, and keeps
//       every word it must quote; an average of 99% would hide the one
//       article that dropped a quote.
//     * Gated metrics, each with a margin δ: how much worse than the
//       baseline the candidate may be. A non-inferiority test: the metric
//       passes when the worse end of the 95% interval of candidate −
//       baseline (rr/eval/stats.hpp) is −δ or better, so a real regression
//       larger than δ passes at most 2.5% of the time, whatever the noise.
//       It fails as worse when the whole interval lies past zero on the
//       worse side, and as inconclusive when it spans both −δ and zero: the
//       remedy then is more videos or replicates, never a wider margin at
//       gate time.
//   An entry never scored, a metric neither run has, runs scored by
//   different scorers: each is inconclusive, and fails the gate too, saying
//   what would let it decide. A metric the frozen judges score counts only
//   once a person has calibrated their panel (P27): until then it is shown,
//   and ignored. Costs are never gated: quality comes first (ADR-0018).
//
// The margins themselves
//   Margins under the noise would make the gate a coin toss. So they are
//   checked against an A/A comparison, the baseline run again under other
//   salts (`rreport eval run --first-replicate`): read_gate_on_aa() asks how
//   often a pipeline no different from the baseline would pass every gated
//   metric at once (unchanged_pass_rate()), and the margins hold when it is
//   80% or more. It also lists the metrics whose A/A interval leaves out
//   zero: with ten metrics, chance does that to one now and then, so it is
//   no failure, but one that keeps doing it points at a draw the salt does
//   not reach.
//
// Thread-safety: pure functions, but for read_gate(), which reads a file.
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <rr/core/result.hpp>
#include <rr/eval/metrics.hpp>
#include <rr/eval/panel.hpp>
#include <rr/eval/run.hpp>
#include <rr/eval/stats.hpp>

namespace rr::eval {

// The share of A/A comparisons an unchanged pipeline must pass (ADR-0019).
inline constexpr double kUnchangedPasses = 0.8;

struct Invariant {
    std::string metric{};
    double every_entry = 0;  // each entry is at least as good: as high, or as low, as this

    friend bool operator==(const Invariant&, const Invariant&) = default;
};

struct Gated {
    std::string metric{};
    double margin = 0;  // δ: how much worse the candidate may be, in the metric's own units

    friend bool operator==(const Gated&, const Gated&) = default;
};

struct Gate {
    std::string name{};
    std::vector<Invariant> invariants{};
    std::vector<Gated> gated{};

    friend bool operator==(const Gate&, const Gate&) = default;
};

[[nodiscard]] std::filesystem::path gate_file(const std::filesystem::path& eval_root, std::string_view name);

// `text`, the gate `name`'s file, read strictly, as every file of the eval:
// a metric this rreport does not score, one better neither higher nor lower,
// a cost; a metric named twice; a negative margin; a gate with nothing to
// check: each Errc::invalid_argument saying where.
[[nodiscard]] Result<Gate> parse_gate(std::string_view name, std::string_view text);
[[nodiscard]] Result<Gate> read_gate(const std::filesystem::path& eval_root, std::string_view name);

enum class Outcome : std::uint8_t {
    passed,
    failed,        // an invariant broken; a gated metric worse
    inconclusive,  // the numbers cannot say: `why` says what would let them
    ignored        // judged by a panel no person has calibrated yet
};

[[nodiscard]] std::string_view to_string(Outcome outcome) noexcept;

struct InvariantReading {
    Invariant invariant{};
    Outcome outcome = Outcome::passed;
    std::size_t entries = 0;            // of the candidate, scored by the metric's family
    std::vector<std::string> broken{};  // "radium-talk, replicate 2: 1.000"
    std::string why{};

    friend bool operator==(const InvariantReading&, const InvariantReading&) = default;
};

struct GatedReading {
    Gated gated{};
    Outcome outcome = Outcome::passed;
    std::optional<Comparison> comparison{};  // none when the runs cannot be compared on it
    std::string why{};

    friend bool operator==(const GatedReading&, const GatedReading&) = default;
};

struct GateReading {
    std::vector<InvariantReading> invariants{};
    std::vector<GatedReading> gated{};

    // Passed when every item passed or is ignored; else failed, whether
    // what stopped it is a regression or a question the numbers cannot
    // answer yet.
    [[nodiscard]] bool passed() const;

    friend bool operator==(const GateReading&, const GateReading&) = default;
};

// `candidate` against `baseline`, as `gate` says, with `panel` the judges a
// judged metric must have been scored by.
[[nodiscard]] GateReading read_gate_on(const Gate& gate, std::span<const ScoredEntry> baseline,
                                       std::span<const ScoredEntry> candidate, const Panel& panel);

// As `rreport eval gate` prints a reading.
[[nodiscard]] std::string to_text(const Gate& gate, const GateReading& reading, std::string_view baseline,
                                  std::string_view candidate);

// A gated metric in an A/A comparison.
struct AaMetric {
    Gated gated{};
    Outcome outcome = Outcome::passed;  // ignored for a judged metric not counted yet, inconclusive when
                                        // the runs cannot be compared on it
    std::optional<Comparison> comparison{};
    std::optional<double> alone{};  // how often it passes alone, unchanged: alone_pass_rate()
    std::string why{};

    friend bool operator==(const AaMetric&, const AaMetric&) = default;
};

struct AaReading {
    std::vector<AaMetric> metrics{};
    std::optional<double> unchanged_passes{};  // every counted metric at once; none when it cannot be told
    std::vector<std::string> off_zero{};       // metrics whose A/A interval leaves out zero

    // The margins hold: an unchanged pipeline passes them kUnchangedPasses
    // of the time or more, and nothing was inconclusive.
    [[nodiscard]] bool holds() const;

    friend bool operator==(const AaReading&, const AaReading&) = default;
};

// Whether `aa` is `baseline` run again, as an A/A comparison needs: the same
// set, split and tier, and replicates that share no salt with the
// baseline's, so that no stage run is shared and the noise shows whole
// (ADR-0019). Errc::invalid_argument saying what differs.
[[nodiscard]] Result<void> check_aa(const EvalRun& baseline, const EvalRun& aa);

// `gate`'s margins against `aa`, the baseline run again under other salts.
[[nodiscard]] AaReading read_gate_on_aa(const Gate& gate, std::span<const ScoredEntry> baseline,
                                        std::span<const ScoredEntry> aa, const Panel& panel);

// As `rreport eval gate --aa` prints a reading.
[[nodiscard]] std::string to_text(const Gate& gate, const AaReading& reading, std::string_view baseline,
                                  std::string_view aa);

}  // namespace rr::eval
