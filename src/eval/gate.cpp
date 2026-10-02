// Whether a candidate run may replace its baseline (rr/eval/gate.hpp).
#include <rr/eval/gate.hpp>

#include <algorithm>
#include <format>
#include <string>
#include <utility>

#include <rr/core/error.hpp>
#include <rr/core/file_io.hpp>
#include <rr/core/strings.hpp>

#include "shown.hpp"
#include "toml_io.hpp"

namespace rr::eval {

namespace {

constexpr std::string_view kJudges = "judges";

// Why a gate may not hold `metric`; none when it may.
std::optional<std::string> not_gateable(const Metric& metric) {
    if (metric.better == Better::neither) {
        return std::format("'{}' is better neither higher nor lower: it is shown, never gated", metric.name);
    }
    if (metric.cost) {
        return std::format("'{}' is a cost: costs are shown, never gated, as quality comes first (ADR-0019)",
                           metric.name);
    }
    return std::nullopt;
}

// The metric `table` names, if a gate may hold it.
Result<const Metric*> gateable(detail::Table& table) {
    RR_TRY_ASSIGN(const std::string name, table.text("metric"));
    const Metric* metric = find_metric(name);
    if (metric == nullptr) {
        return fail(
            Errc::invalid_argument,
            std::format("{}: '{}' is not a metric this rreport scores (docs/design/eval.md lists them)",
                        table.where(), name));
    }
    if (const std::optional<std::string> why = not_gateable(*metric); why.has_value()) {
        return fail(Errc::invalid_argument, std::format("{}: {}", table.where(), *why));
    }
    return metric;
}

// The metric a gate names, when a gate may hold it. parse_gate() refuses the
// others; a gate made in code may still name one, and is told why not.
Result<const Metric*> held(std::string_view name) {
    const Metric* metric = find_metric(name);
    if (metric == nullptr) {
        return fail(Errc::invalid_argument, std::format("'{}' is not a metric this rreport scores", name));
    }
    if (const std::optional<std::string> why = not_gateable(*metric); why.has_value()) {
        return fail(Errc::invalid_argument, *why);
    }
    return metric;
}

// Whether `value` is worse than `bound`, the way `better` says.
bool worse_than(Better better, double value, double bound) {
    switch (better) {
        case Better::higher:
            return value < bound;
        case Better::lower:
            return value > bound;
        case Better::neither:
            break;
    }
    RR_ASSERT(false, "a gate holds only metrics better one way (held())");
    return false;
}

// "radium-talk, replicate 2", as the export numbers replicates.
std::string entry_named(const RunEntry& entry) {
    return std::format("{}, replicate {}", entry.video, entry.replicate);
}

// The scoring of `entry` that would hold `metric`: its family's and, for a
// judged one, the panel's own.
const Scoring* scoring_for(const ScoredEntry& entry, const Metric& metric, const Panel& panel) {
    const auto found = std::ranges::find_if(entry.scorings, [&](const Scoring& scoring) {
        return metric.family == kJudges ? scoring.scorer == panel.scorer()
                                        : scorer_family(scoring.scorer) == metric.family;
    });
    return found == entry.scorings.end() ? nullptr : &*found;
}

// Whether the gate counts `metric`: the code's always; the judges' once a
// person has calibrated their panel.
bool counted(const Metric& metric, const Panel& panel) {
    return metric.family != kJudges || panel.calibrated;
}

std::string uncalibrated(const Panel& panel) {
    return std::format("the panel {} judges it, and no person has calibrated the panel yet: not counted",
                       panel.name);
}

// What a gate reads of a comparison.
struct Fitted {
    Interval interval{};
    double standard_error = 0;
};

// The interval and standard error of `comparison`, when a gate can read
// them; why not otherwise (Errc::invalid_argument, or the comparison's own
// error).
Result<Fitted> fitted(const Result<Comparison>& comparison, const Metric& metric, const Panel& panel) {
    if (!comparison.has_value()) {
        return std::unexpected(comparison.error());
    }
    const bool judged = metric.family == kJudges;
    if (comparison->scorer.empty()) {
        return fail(
            Errc::invalid_argument,
            judged ? std::format("neither run was judged by the panel {}: rreport eval judge", panel.name)
                   : std::string{"neither run was scored: rreport eval score"});
    }
    if (judged && comparison->scorer != panel.scorer()) {
        return fail(Errc::invalid_argument,
                    std::format("judged by {}, not by the gate's panel, {}", comparison->scorer, panel.name));
    }
    if (!comparison->interval.has_value() || !comparison->standard_error.has_value()) {
        return fail(Errc::invalid_argument,
                    std::format("{} video{} scored by both runs: an interval needs two at least",
                                comparison->videos, comparison->videos == 1 ? "" : "s"));
    }
    return Fitted{.interval = *comparison->interval, .standard_error = *comparison->standard_error};
}

// `interval` turned so that higher is better: as it is for a metric better
// higher, negated and swapped for one better lower.
Interval oriented(const Interval& interval, Better better) {
    return better == Better::lower ? Interval{.low = -interval.high, .high = -interval.low} : interval;
}

// "-0.004 to +0.006"
std::string interval_shown(const Interval& interval) {
    return std::format("{} to {}", detail::signed_shown(interval.low), detail::signed_shown(interval.high));
}

// "87%"
std::string percent(double share) {
    return std::format("{:.0f}%", share * 100);
}

InvariantReading read_invariant(const Invariant& invariant, std::span<const ScoredEntry> candidate,
                                const Panel& panel) {
    InvariantReading reading{.invariant = invariant};
    const Result<const Metric*> metric = held(invariant.metric);
    if (!metric.has_value()) {
        reading.outcome = Outcome::inconclusive;
        reading.why = metric.error().message();
        return reading;
    }
    std::vector<std::string> unscored;
    for (const ScoredEntry& entry : candidate) {
        const Scoring* scoring = scoring_for(entry, **metric, panel);
        if (scoring == nullptr) {
            unscored.push_back(entry_named(entry.entry));
            continue;
        }
        ++reading.entries;
        const auto score = std::ranges::find(scoring->scores, invariant.metric, &Score::metric);
        // An entry scored without it had nothing to share (no number to
        // keep): it has nothing to break.
        if (score != scoring->scores.end() &&
            worse_than((*metric)->better, score->value, invariant.every_entry)) {
            reading.broken.push_back(
                std::format("{}: {}", entry_named(entry.entry), detail::shown(score->value)));
        }
    }
    if (!counted(**metric, panel)) {
        reading.outcome = Outcome::ignored;
        reading.why = uncalibrated(panel);
    } else if (!reading.broken.empty()) {
        reading.outcome = Outcome::failed;
        reading.why = std::format("broken by {}", join(reading.broken, "; "));
    } else if (candidate.empty()) {
        reading.outcome = Outcome::inconclusive;
        reading.why = "the candidate has no entry";
    } else if (!unscored.empty()) {
        reading.outcome = Outcome::inconclusive;
        reading.why =
            std::format("not scored by {}: {}",
                        (*metric)->family == kJudges ? panel.scorer() : std::string{(*metric)->family},
                        join(unscored, "; "));
    } else {
        reading.why = std::format("kept by {} entr{}", reading.entries, reading.entries == 1 ? "y" : "ies");
    }
    return reading;
}

GatedReading read_gated(const Gated& gated, std::span<const ScoredEntry> baseline,
                        std::span<const ScoredEntry> candidate, const Panel& panel) {
    GatedReading reading{.gated = gated};
    const Result<const Metric*> metric = held(gated.metric);
    if (!metric.has_value()) {
        reading.outcome = Outcome::inconclusive;
        reading.why = metric.error().message();
        return reading;
    }
    const Result<Comparison> comparison = compare(baseline, candidate, gated.metric);
    if (comparison.has_value()) {
        reading.comparison = *comparison;
    }
    if (!counted(**metric, panel)) {
        reading.outcome = Outcome::ignored;
        reading.why = uncalibrated(panel);
        if (comparison.has_value() && comparison->interval.has_value()) {
            reading.why += std::format(" (candidate - baseline {})", interval_shown(*comparison->interval));
        }
        return reading;
    }
    const Result<Fitted> fit = fitted(comparison, **metric, panel);
    if (!fit.has_value()) {
        reading.outcome = Outcome::inconclusive;
        reading.why = fit.error().message();
        return reading;
    }
    const Interval& interval = fit->interval;
    const Interval turned = oriented(interval, (*metric)->better);
    const std::string said = std::format("candidate - baseline {}", interval_shown(interval));
    if (turned.low >= -gated.margin) {
        reading.why = std::format("{}: within its margin", said);
    } else if (turned.high < 0) {
        reading.outcome = Outcome::failed;
        reading.why = std::format("{}: worse, past its margin, and wholly on the worse side of zero", said);
    } else {
        reading.outcome = Outcome::inconclusive;
        reading.why = std::format("{}: it spans both its margin and zero: add videos or replicates", said);
    }
    return reading;
}

// "at least 1.000", "at most 0.000".
std::string bound_of(const Invariant& invariant) {
    const Metric* metric = find_metric(invariant.metric);
    const bool lower = metric != nullptr && metric->better == Better::lower;
    return std::format("{} {}", lower ? "at most" : "at least", detail::shown(invariant.every_entry));
}

// An [[invariant]] of a gate file, added to `gate`.
Result<void> parse_invariant(detail::Table& table, Gate& gate) {
    RR_TRY_ASSIGN(const Metric* metric, gateable(table));
    if (std::ranges::contains(gate.invariants, metric->name, &Invariant::metric)) {
        return fail(Errc::invalid_argument, std::format("{}: '{}' a second time: one invariant a metric",
                                                        table.where(), metric->name));
    }
    RR_TRY_ASSIGN(const double every_entry, table.number("every_entry"));
    gate.invariants.push_back(Invariant{.metric = std::string{metric->name}, .every_entry = every_entry});
    return ok();
}

// A [[gated]] metric of a gate file, added to `gate`.
Result<void> parse_gated(detail::Table& table, Gate& gate) {
    RR_TRY_ASSIGN(const Metric* metric, gateable(table));
    if (std::ranges::contains(gate.gated, metric->name, &Gated::metric)) {
        return fail(Errc::invalid_argument,
                    std::format("{}: '{}' a second time: one margin a metric", table.where(), metric->name));
    }
    RR_TRY_ASSIGN(const double margin, table.number("margin"));
    if (margin < 0) {
        return fail(Errc::invalid_argument,
                    std::format("{}: 'margin' must be 0 or more: how much worse the candidate may be",
                                table.where()));
    }
    gate.gated.push_back(Gated{.metric = std::string{metric->name}, .margin = margin});
    return ok();
}

}  // namespace

std::string_view to_string(Outcome outcome) noexcept {
    switch (outcome) {
        case Outcome::passed:
            return "passed";
        case Outcome::failed:
            return "failed";
        case Outcome::inconclusive:
            return "inconclusive";
        case Outcome::ignored:
            break;
    }
    return "ignored";
}

std::filesystem::path gate_file(const std::filesystem::path& eval_root, std::string_view name) {
    return eval_root / "gates" / std::format("{}.toml", name);
}

Result<Gate> parse_gate(std::string_view name, std::string_view text) {
    const std::string file = std::format("eval/gates/{}.toml", name);
    RR_TRY_ASSIGN(const toml::table document, detail::parse_toml(text, file));
    detail::Table top{document, file};
    Gate gate;
    RR_TRY_ASSIGN(gate.name, top.text("name"));
    if (gate.name != name) {
        return fail(Errc::invalid_argument,
                    std::format(R"({}: 'name' must be the file's, "{}", not "{}")", file, name, gate.name));
    }
    top.allow("invariant");  // read table by table below
    top.allow("gated");
    RR_TRY(top.nothing_else());

    RR_TRY(detail::each_table(document, "invariant", file,
                              [&](detail::Table& table) { return parse_invariant(table, gate); }));
    RR_TRY(detail::each_table(document, "gated", file,
                              [&](detail::Table& table) { return parse_gated(table, gate); }));
    if (gate.invariants.empty() && gate.gated.empty()) {
        return fail(Errc::invalid_argument,
                    std::format("{}: no [[invariant]] and no [[gated]]: a gate checks something", file));
    }
    return gate;
}

Result<Gate> read_gate(const std::filesystem::path& eval_root, std::string_view name) {
    RR_TRY_ASSIGN(const std::string text, read_file(gate_file(eval_root, name)));
    return parse_gate(name, text);
}

bool GateReading::passed() const {
    const auto fine = [](Outcome outcome) {
        return outcome == Outcome::passed || outcome == Outcome::ignored;
    };
    return std::ranges::all_of(invariants, fine, &InvariantReading::outcome) &&
           std::ranges::all_of(gated, fine, &GatedReading::outcome);
}

GateReading read_gate_on(const Gate& gate, std::span<const ScoredEntry> baseline,
                         std::span<const ScoredEntry> candidate, const Panel& panel) {
    GateReading reading;
    for (const Invariant& invariant : gate.invariants) {
        reading.invariants.push_back(read_invariant(invariant, candidate, panel));
    }
    for (const Gated& gated : gate.gated) {
        reading.gated.push_back(read_gated(gated, baseline, candidate, panel));
    }
    return reading;
}

std::string to_text(const Gate& gate, const GateReading& reading, std::string_view baseline,
                    std::string_view candidate) {
    std::string text =
        std::format("gate {}: candidate {} against baseline {}\n", gate.name, candidate, baseline);
    if (!reading.invariants.empty()) {
        text += "\ninvariants, on every entry of the candidate:\n";
        for (const InvariantReading& invariant : reading.invariants) {
            text += std::format("  {:<22} {:<16} {}: {}\n", invariant.invariant.metric,
                                bound_of(invariant.invariant), to_string(invariant.outcome), invariant.why);
        }
    }
    if (!reading.gated.empty()) {
        text += "\ngated: the 95% interval of candidate - baseline, against how much worse it may be:\n";
        for (const GatedReading& gated : reading.gated) {
            text += std::format("  {:<22} margin {:<9} {}: {}\n", gated.gated.metric,
                                detail::shown(gated.gated.margin), to_string(gated.outcome), gated.why);
        }
    }
    text += std::format("\ngate {}: {}\n", gate.name, reading.passed() ? "passed" : "failed");
    return text;
}

Result<void> check_aa(const EvalRun& baseline, const EvalRun& aa) {
    const RunSpec& once = baseline.spec;
    const RunSpec& again = aa.spec;
    if (once.eval_set != again.eval_set || once.split != again.split || once.tier != again.tier) {
        return fail(Errc::invalid_argument,
                    std::format("{} is not {} run again: an A/A run has its set, split and tier ({} {}, tier "
                                "{}), not {} {}, tier {}",
                                again.label, once.label, once.eval_set, to_string(once.split), once.tier,
                                again.eval_set, to_string(again.split), again.tier));
    }
    const int once_end = once.first_replicate + once.replicates;
    const int again_end = again.first_replicate + again.replicates;
    if (once.first_replicate < again_end && again.first_replicate < once_end) {
        return fail(
            Errc::invalid_argument,
            std::format("{} shares salts with {}: its replicates {} to {} and {} to {} overlap, so they "
                        "share stage runs and hide the noise; run it under other salts, with "
                        "--first-replicate {}",
                        again.label, once.label, again.first_replicate, again_end - 1, once.first_replicate,
                        once_end - 1, once_end));
    }
    return ok();
}

bool AaReading::holds() const {
    return unchanged_passes.has_value() && *unchanged_passes >= kUnchangedPasses &&
           !std::ranges::contains(metrics, Outcome::inconclusive, &AaMetric::outcome);
}

AaReading read_gate_on_aa(const Gate& gate, std::span<const ScoredEntry> baseline,
                          std::span<const ScoredEntry> aa, const Panel& panel) {
    AaReading reading;
    std::vector<NullMetric> nulls;
    for (const Gated& gated : gate.gated) {
        AaMetric one{.gated = gated};
        const Result<const Metric*> metric = held(gated.metric);
        if (!metric.has_value()) {
            one.outcome = Outcome::inconclusive;
            one.why = metric.error().message();
            reading.metrics.push_back(std::move(one));
            continue;
        }
        const Result<Comparison> comparison = compare(baseline, aa, gated.metric);
        if (comparison.has_value()) {
            one.comparison = *comparison;
        }
        const Result<Fitted> fit = fitted(comparison, **metric, panel);
        if (!counted(**metric, panel)) {
            one.outcome = Outcome::ignored;
            one.why = uncalibrated(panel);
        } else if (!fit.has_value()) {
            one.outcome = Outcome::inconclusive;
            one.why = fit.error().message();
        } else {
            if (fit->interval.low > 0 || fit->interval.high < 0) {
                reading.off_zero.push_back(gated.metric);
            }
            const double alone = alone_pass_rate(gated.margin, fit->standard_error);
            one.alone = alone;
            one.why = std::format(
                "A/A {}, standard error {}: alone, an unchanged pipeline passes it {}; 80% "
                "needs a margin of {}",
                interval_shown(fit->interval), detail::shown(fit->standard_error), percent(alone),
                detail::shown(kDetectable * fit->standard_error));
            NullMetric null{.metric = gated.metric, .margin = gated.margin};
            for (const auto& [video, difference] : comparison->differences) {
                null.differences.emplace(video,
                                         (*metric)->better == Better::lower ? -difference : difference);
            }
            nulls.push_back(std::move(null));
        }
        reading.metrics.push_back(std::move(one));
    }
    reading.unchanged_passes = unchanged_pass_rate(nulls, seed_of(gate.name));
    return reading;
}

std::string to_text(const Gate& gate, const AaReading& reading, std::string_view baseline,
                    std::string_view aa) {
    std::string text = std::format(
        "gate {}: its margins, on {} run again as {}, an A/A comparison of one pipeline with itself\n\n",
        gate.name, baseline, aa);
    for (const AaMetric& metric : reading.metrics) {
        const std::string said = metric.outcome == Outcome::passed
                                     ? metric.why
                                     : std::format("{}: {}", to_string(metric.outcome), metric.why);
        text += std::format("  {:<22} margin {:<9} {}\n", metric.gated.metric,
                            detail::shown(metric.gated.margin), said);
    }
    if (!reading.off_zero.empty()) {
        text += std::format(
            "\nA/A intervals that leave out zero: {}. Chance does that to one metric in twenty; one that "
            "keeps doing it points at a draw the salt does not reach.\n",
            join(reading.off_zero, ", "));
    }
    if (reading.unchanged_passes.has_value()) {
        text += std::format(
            "\nan unchanged pipeline passes every counted gated metric at once {} of the time ({} at least, "
            "ADR-0019)\n",
            percent(*reading.unchanged_passes), percent(kUnchangedPasses));
    } else {
        text += "\nfewer than two videos have every gated metric: how often it passes cannot be told\n";
    }
    text += std::format("gate {}: its margins {}\n", gate.name,
                        reading.holds() ? "hold"
                                        : "do not hold: widen those an unchanged pipeline passes too seldom, "
                                          "or add videos");
    return text;
}

}  // namespace rr::eval
