// Two runs side by side (rr/eval/compare.hpp).
#include <rr/eval/compare.hpp>

#include <algorithm>
#include <format>
#include <set>
#include <utility>

#include <rr/core/error.hpp>
#include <rr/core/strings.hpp>

#include "shown.hpp"

namespace rr::eval {

namespace {

// A row of the table, or its header, in the same columns: every cell is
// text, so that both share one format.
std::string table_line(std::string_view metric, std::string_view videos, std::string_view a,
                       std::string_view b, std::string_view difference, std::string_view interval,
                       std::string_view standard_error, std::string_view detectable, std::string_view margin,
                       std::string_view seen) {
    return std::format("  {:<24}{:>7}{:>10}{:>10}{:>10}   {:<20}{:>8}{:>8}{:>8}   {}\n", metric, videos, a, b,
                       difference, interval, standard_error, detectable, margin, seen);
}

std::string optional_shown(const std::optional<double>& value) {
    return value.has_value() ? detail::shown(*value) : std::string{"-"};
}

std::string row_text(const MetricRow& row, const Comparison& compared) {
    const std::string interval = compared.interval.has_value()
                                     ? std::format("{} to {}", detail::signed_shown(compared.interval->low),
                                                   detail::signed_shown(compared.interval->high))
                                     : std::string{"-"};
    // No video scored by both: no difference to show, not one of zero.
    const std::string difference = compared.videos == 0 ? "-" : detail::signed_shown(compared.difference);
    return table_line(row.metric.name, std::to_string(compared.videos), optional_shown(compared.a),
                      optional_shown(compared.b), difference, interval,
                      optional_shown(compared.standard_error), optional_shown(compared.detectable()),
                      optional_shown(row.margin), to_string(seen_in(row)));
}

// A line of the video-by-video table.
std::string video_line(std::string_view video, std::string_view a, std::string_view b,
                       std::string_view difference) {
    return std::format("  {:<28}{:>10}{:>10}{:>10}\n", video, a, b, difference);
}

// A line of a metric read pairwise, video by video: no A, no B.
std::string pairwise_line(std::string_view video, std::string_view difference) {
    return std::format("  {:<28}{:>10}\n", video, difference);
}

}  // namespace

std::string_view to_string(Seen seen) noexcept {
    switch (seen) {
        case Seen::better:
            return "better";
        case Seen::worse:
            return "worse";
        case Seen::higher:
            return "higher";
        case Seen::lower:
            return "lower";
        case Seen::none:
            return "no difference";
        case Seen::unknown:
            break;
    }
    return "too few videos";
}

Seen seen_in(const MetricRow& row) noexcept {
    if (!row.comparison.has_value() || !row.comparison->interval.has_value()) {
        return Seen::unknown;
    }
    const Interval& interval = *row.comparison->interval;
    const bool up = interval.low > 0;
    if (!up && interval.high >= 0) {
        return Seen::none;
    }
    switch (row.metric.better) {
        case Better::higher:
            return up ? Seen::better : Seen::worse;
        case Better::lower:
            return up ? Seen::worse : Seen::better;
        case Better::neither:
            break;
    }
    return up ? Seen::higher : Seen::lower;
}

std::vector<MetricRow> compare_runs(std::span<const ScoredEntry> a, std::span<const ScoredEntry> b,
                                    std::span<const Gated> gated, std::span<const Comparison> read_pairwise) {
    std::vector<MetricRow> rows;
    for (const Metric& metric : metrics()) {
        MetricRow row{.metric = metric};
        if (const auto held = std::ranges::find(gated, metric.name, &Gated::metric); held != gated.end()) {
            row.margin = held->margin;
        }
        if (metric.pairwise) {
            const auto read = std::ranges::find(read_pairwise, metric.name, &Comparison::metric);
            if (read != read_pairwise.end()) {
                row.comparison = *read;
                rows.push_back(std::move(row));
            }
            continue;  // no run's own score
        }
        if (per_video(a, metric.name).means.empty() && per_video(b, metric.name).means.empty()) {
            continue;  // neither run has it
        }
        if (Result<Comparison> compared = compare(a, b, metric.name); compared.has_value()) {
            row.comparison = *std::move(compared);
        } else {
            row.why = compared.error().message();
        }
        rows.push_back(std::move(row));
    }
    return rows;
}

std::string to_text(std::string_view a, std::string_view b, const std::vector<MetricRow>& rows) {
    std::string text = std::format(
        "A: {}\nB: {}\n\nEach video's replicates are averaged first. B − A is paired by video, and its 95% "
        "interval\ncomes from {} resamples of the videos. SE: its standard error; detect: 2.8 of them, the "
        "smallest\ndifference these videos see four times in five; margin: how much worse the gate lets B "
        "be.\n",
        a, b, kResamples);
    if (rows.empty()) {
        return text + "\nNeither run has a score yet: rreport eval score, then rreport eval judge.\n";
    }
    // Grouped by scorer, in the order each first comes, and each group's rows
    // in metrics() order.
    std::vector<std::string> scorers;
    for (const MetricRow& row : rows) {
        if (row.comparison.has_value() && !std::ranges::contains(scorers, row.comparison->scorer)) {
            scorers.push_back(row.comparison->scorer);
        }
    }
    for (const std::string& scorer : scorers) {
        text += std::format("\n{}\n", scorer);
        text += table_line("metric", "videos", "A", "B", "B - A", "95% interval", "SE", "detect", "margin",
                           "seen");
        for (const MetricRow& row : rows) {
            if (row.comparison.has_value() && row.comparison->scorer == scorer) {
                text += row_text(row, *row.comparison);
            }
        }
    }
    std::string refused;
    std::set<std::string, std::less<>> unpaired;
    for (const MetricRow& row : rows) {
        if (!row.comparison.has_value()) {
            refused += std::format("  {}\n", row.why);
        } else {
            unpaired.insert(row.comparison->unpaired.begin(), row.comparison->unpaired.end());
        }
    }
    if (!refused.empty()) {
        text += "\nnot compared:\n" + refused;
    }
    if (!unpaired.empty()) {
        const std::vector<std::string> left_out{unpaired.begin(), unpaired.end()};
        text += std::format("\nleft out, scored by one run and not the other: {}\n", join(left_out, ", "));
    }
    return text;
}

Result<std::string> by_video_text(std::string_view a, std::string_view b,
                                  std::span<const ScoredEntry> a_entries,
                                  std::span<const ScoredEntry> b_entries, std::string_view metric) {
    if (find_metric(metric) == nullptr) {
        return fail(
            Errc::not_found,
            std::format("'{}' is not a metric this rreport scores (docs/design/eval.md lists them)", metric));
    }
    const VideoValues in_a = per_video(a_entries, metric);
    const VideoValues in_b = per_video(b_entries, metric);
    std::set<std::string, std::less<>> videos;
    for (const auto& [video, value] : in_a.means) {
        videos.insert(video);
    }
    for (const auto& [video, value] : in_b.means) {
        videos.insert(video);
    }
    std::string text = std::format(
        "{}, video by video, each video's mean over its replicates (A: {}, B: {}):\n", metric, a, b);
    if (videos.empty()) {
        return text + "  neither run has it\n";
    }
    text += video_line("video", "A", "B", "B - A");
    for (const std::string& video : videos) {
        const auto found_a = in_a.means.find(video);
        const auto found_b = in_b.means.find(video);
        const bool in_both = found_a != in_a.means.end() && found_b != in_b.means.end();
        text += video_line(video, found_a != in_a.means.end() ? detail::shown(found_a->second) : "-",
                           found_b != in_b.means.end() ? detail::shown(found_b->second) : "-",
                           in_both ? detail::signed_shown(found_b->second - found_a->second) : "-");
    }
    return text;
}

std::string by_video_text(std::string_view a, std::string_view b, const Comparison& read_pairwise) {
    std::string text = std::format(
        "{}, video by video, read pairwise: B against A, each video's mean over its "
        "pairs of replicates (A: {}, B: {}):\n",
        read_pairwise.metric, a, b);
    if (read_pairwise.differences.empty()) {
        return text + "  no video read\n";
    }
    text += pairwise_line("video", "B - A");
    for (const auto& [video, difference] : read_pairwise.differences) {
        text += pairwise_line(video, detail::signed_shown(difference));
    }
    return text;
}

}  // namespace rr::eval
