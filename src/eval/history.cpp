// Every eval run of a library on one page (rr/eval/history.hpp).
#include <rr/eval/history.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <format>
#include <map>
#include <set>
#include <utility>

#include <rr/core/assert.hpp>
#include <rr/eval/metrics.hpp>
#include <rr/report/page.hpp>

#include "shown.hpp"

namespace rr::eval {

namespace {

using report::escaped;

// The chart's size, and the margins around its plot, in SVG units.
constexpr double kWidth = 720;
constexpr double kHeight = 440;
constexpr double kLeft = 72;
constexpr double kRight = 24;
constexpr double kTop = 16;
constexpr double kBottom = 56;
constexpr double kTickLength = 5;

// The metrics the table of runs shows, each run's mean.
constexpr std::array<std::string_view, 5> kHeadline{"sentences_accounted", "atoms_told", "facts_told",
                                                    "sentences_supported", "minutes_per_video_hour"};

constexpr std::string_view kStyle = R"(<style>
:root { --ink: #1d1d1f; --page: #fbfbf8; --soft: #6b6b70; --mark: #2a5db0; }
@media (prefers-color-scheme: dark) { :root { --ink: #e8e8e3; --page: #141416; --soft: #a0a0a6; --mark: #8fb3ff; } }
body { margin: 0 auto; max-width: 60rem; padding: 2rem 1rem; background: var(--page); color: var(--ink);
       font: 1rem/1.5 system-ui, sans-serif; }
table { border-collapse: collapse; font-size: .85rem; width: 100%; }
td, th { border-bottom: 1px solid color-mix(in srgb, var(--soft) 30%, transparent); padding: .3rem .4rem;
         text-align: left; vertical-align: top; }
td.n, th.n { text-align: right; font-variant-numeric: tabular-nums; }
.meta, .missing { color: var(--soft); }
svg { width: 100%; height: auto; font-size: 11px; }
svg .axis { stroke: var(--soft); }
svg .bar { stroke: var(--mark); stroke-width: 1.5; }
svg circle { fill: var(--mark); }
svg text { fill: var(--ink); }
</style>)";

// About how many steps an axis is long.
constexpr double kSteps = 5;
constexpr double kHair = 1e-9;  // in steps

// The round factor at or above `ratio`, which is in [1, 10): 1, 2, 5 or 10.
double round_up(double ratio) {
    for (const double round : {1.0, 2.0, 5.0}) {
        if (ratio <= round) {
            return round;
        }
    }
    return 10.0;
}

// A run plotted: its two means.
struct Point {
    const RunScores* run = nullptr;
    RunMean cost{};
    RunMean quality{};
};

// Where `value` falls across the plot, and up it.
double across(const Axis& axis, double value) {
    return kLeft + ((value - axis.low) / (axis.high - axis.low) * (kWidth - kLeft - kRight));
}
double up(const Axis& axis, double value) {
    return kTop + ((axis.high - value) / (axis.high - axis.low) * (kHeight - kTop - kBottom));
}

// A tick's label, with as many decimals as its axis's step needs.
std::string tick_label(double value, double step) {
    const int decimals = std::max(0, -static_cast<int>(std::floor(std::log10(step))));
    const double shown = std::abs(value) < step / 2 ? 0.0 : value;  // never "-0.0"
    return std::format("{:.{}f}", shown, decimals);
}

std::string line(double x1, double y1, double x2, double y2, std::string_view kind) {
    return std::format(R"(<line class="{}" x1="{:.1f}" y1="{:.1f}" x2="{:.1f}" y2="{:.1f}"/>)", kind, x1, y1,
                       x2, y2) +
           "\n";
}

// "0.812 (0.790 to 0.833)"
std::string with_interval(const RunMean& mean) {
    if (!mean.interval.has_value()) {
        return detail::shown(mean.mean);
    }
    return std::format("{} ({} to {})", detail::shown(mean.mean), detail::shown(mean.interval->low),
                       detail::shown(mean.interval->high));
}

// Every value a point needs room for: its mean and its interval's ends.
void room_for(const RunMean& mean, std::vector<double>& values) {
    values.push_back(mean.mean);
    if (mean.interval.has_value()) {
        values.push_back(mean.interval->low);
        values.push_back(mean.interval->high);
    }
}

std::string axes_svg(const Axis& cost, const Axis& quality, const PageOptions& options) {
    const double bottom = kHeight - kBottom;
    std::string svg = line(kLeft, bottom, kWidth - kRight, bottom, "axis");
    svg += line(kLeft, kTop, kLeft, bottom, "axis");
    for (const double tick : cost.ticks) {
        const double x = across(cost, tick);
        svg += line(x, bottom, x, bottom + kTickLength, "axis");
        svg += std::format(R"(<text x="{:.1f}" y="{:.1f}" text-anchor="middle">{}</text>)", x, bottom + 18,
                           tick_label(tick, cost.step)) +
               "\n";
    }
    for (const double tick : quality.ticks) {
        const double y = up(quality, tick);
        svg += line(kLeft - kTickLength, y, kLeft, y, "axis");
        svg += std::format(R"(<text x="{:.1f}" y="{:.1f}" text-anchor="end">{}</text>)", kLeft - 8, y + 4,
                           tick_label(tick, quality.step)) +
               "\n";
    }
    svg += std::format(R"(<text x="{:.1f}" y="{:.1f}" text-anchor="middle">{}</text>)",
                       (kLeft + kWidth - kRight) / 2, kHeight - 12, escaped(options.cost)) +
           "\n";
    svg +=
        std::format(
            R"svg(<text x="16" y="{:.1f}" text-anchor="middle" transform="rotate(-90 16 {:.1f})">{}</text>)svg",
            (kTop + bottom) / 2, (kTop + bottom) / 2, escaped(options.quality)) +
        "\n";
    return svg;
}

std::string point_svg(const Point& point, const Axis& cost, const Axis& quality, const PageOptions& options) {
    const double x = across(cost, point.cost.mean);
    const double y = up(quality, point.quality.mean);
    std::string svg;
    if (point.quality.interval.has_value()) {
        svg += line(x, up(quality, point.quality.interval->low), x, up(quality, point.quality.interval->high),
                    "bar");
    }
    if (point.cost.interval.has_value()) {
        svg += line(across(cost, point.cost.interval->low), y, across(cost, point.cost.interval->high), y,
                    "bar");
    }
    const std::string& label = point.run->run.spec.label;
    svg += std::format(R"(<circle cx="{:.1f}" cy="{:.1f}" r="4"><title>{}: {} {}, {} {}</title></circle>)", x,
                       y, escaped(label), escaped(options.quality), with_interval(point.quality),
                       escaped(options.cost), with_interval(point.cost)) +
           "\n";
    svg += std::format(R"(<text x="{:.1f}" y="{:.1f}">{}</text>)", x + 7, y - 7, escaped(label)) + "\n";
    return svg;
}

std::string chart_html(std::span<const RunScores> runs, const PageOptions& options) {
    std::vector<Point> points;
    std::vector<std::string> unplotted;
    for (const RunScores& run : runs) {
        const std::optional<RunMean> cost = run_mean(run.scores, options.cost);
        const std::optional<RunMean> quality = run_mean(run.scores, options.quality);
        if (cost.has_value() && quality.has_value()) {
            points.push_back(Point{.run = &run, .cost = *cost, .quality = *quality});
        } else {
            unplotted.push_back(run.run.spec.label);
        }
    }
    std::string html =
        std::format("<h2>{} against {}</h2>\n", escaped(options.quality), escaped(options.cost));
    html +=
        "<p class=\"meta\">Each run at its mean over its videos, each video's replicates averaged first; "
        "the bars span each mean's 95% interval, from 10,000 resamples of the videos.</p>\n";
    if (points.empty()) {
        html +=
            "<p class=\"missing\">No run has both yet: rreport eval score, then rreport eval judge.</p>\n";
    } else {
        std::vector<double> costs;
        std::vector<double> qualities;
        for (const Point& point : points) {
            room_for(point.cost, costs);
            room_for(point.quality, qualities);
        }
        const Axis cost = axis_over(costs);
        const Axis quality = axis_over(qualities);
        html += std::format(R"(<svg viewBox="0 0 {} {}" role="img" aria-label="{} against {}">)", kWidth,
                            kHeight, escaped(options.quality), escaped(options.cost)) +
                "\n";
        html += axes_svg(cost, quality, options);
        for (const Point& point : points) {
            html += point_svg(point, cost, quality, options);
        }
        html += "</svg>\n";
    }
    if (!unplotted.empty()) {
        std::string named;
        for (const std::string& label : unplotted) {
            named += std::format("{}{}", named.empty() ? "" : ", ", escaped(label));
        }
        html += std::format("<p class=\"missing\">Not plotted, missing either: {}</p>\n", named);
    }
    return html;
}

// How many of a run's entries a scorer family has scored.
std::size_t scored_by(std::span<const ScoredEntry> scores, std::string_view family) {
    return static_cast<std::size_t>(std::ranges::count_if(scores, [&](const ScoredEntry& entry) {
        return std::ranges::any_of(entry.scorings, [&](const Scoring& scoring) {
            return scorer_family(scoring.scorer) == std::optional<std::string_view>{family};
        });
    }));
}

std::string runs_html(std::span<const RunScores> runs) {
    std::string html =
        "<h2>The runs</h2>\n<table>\n<tr><th>run</th><th>made</th><th>videos</th><th>tier</th>"
        "<th>replicates</th><th class=\"n\">entries</th><th class=\"n\">scored</th>"
        "<th class=\"n\">judged</th>";
    for (const std::string_view metric : kHeadline) {
        html += std::format("<th class=\"n\">{}</th>", metric);
    }
    html += "</tr>\n";
    for (const RunScores& run : runs) {
        const RunSpec& spec = run.run.spec;
        html += std::format(
            "<tr><td>{}</td><td>{:%Y-%m-%d %H:%M}</td><td>{} {}</td><td>{}</td><td>{} to {}</td>"
            "<td class=\"n\">{}</td><td class=\"n\">{}</td><td class=\"n\">{}</td>",
            escaped(spec.label), std::chrono::floor<std::chrono::minutes>(run.run.created_at),
            escaped(spec.eval_set), to_string(spec.split), escaped(spec.tier), spec.first_replicate,
            spec.first_replicate + spec.replicates - 1, run.scores.size(), scored_by(run.scores, "code"),
            scored_by(run.scores, "judges"));
        for (const std::string_view metric : kHeadline) {
            const std::optional<RunMean> mean = run_mean(run.scores, metric);
            html +=
                std::format("<td class=\"n\">{}</td>", mean.has_value() ? detail::shown(mean->mean) : "-");
        }
        html += "</tr>\n";
    }
    return html + "</table>\n";
}

// One run's videos: a row a video, a column a metric it has.
std::string videos_html(const RunScores& run) {
    std::vector<std::pair<std::string_view, VideoValues>> columns;
    std::set<std::string, std::less<>> videos;
    for (const Metric& metric : metrics()) {
        VideoValues values = per_video(run.scores, metric.name);
        if (values.means.empty()) {
            continue;  // none of it: none of flow either, which is read pairwise
        }
        for (const auto& [video, mean] : values.means) {
            videos.insert(video);
        }
        columns.emplace_back(metric.name, std::move(values));
    }
    std::string html = std::format("<details><summary>{}: {} video{}</summary>\n",
                                   escaped(run.run.spec.label), videos.size(), videos.size() == 1 ? "" : "s");
    if (columns.empty()) {
        return html + "<p class=\"missing\">No score yet.</p>\n</details>\n";
    }
    html += "<table>\n<tr><th>video</th>";
    for (const auto& [metric, values] : columns) {
        html += std::format("<th class=\"n\">{}</th>", metric);
    }
    html += "</tr>\n";
    for (const std::string& video : videos) {
        html += std::format("<tr><td>{}</td>", escaped(video));
        for (const auto& [metric, values] : columns) {
            const auto found = values.means.find(video);
            html +=
                std::format("<td class=\"n\">{}</td>",
                            found == values.means.end() ? std::string{"-"} : detail::shown(found->second));
        }
        html += "</tr>\n";
    }
    return html + "</table>\n</details>\n";
}

}  // namespace

std::optional<RunMean> run_mean(std::span<const ScoredEntry> scores, std::string_view metric) {
    const VideoValues values = per_video(scores, metric);
    if (values.means.empty()) {
        return std::nullopt;
    }
    std::vector<double> means;
    means.reserve(values.means.size());
    for (const auto& [video, mean] : values.means) {
        means.push_back(mean);
    }
    RunMean run{.videos = means.size()};
    double sum = 0;
    for (const double mean : means) {
        sum += mean;
    }
    run.mean = sum / static_cast<double>(means.size());
    if (means.size() >= 2) {
        run.interval = bootstrap_mean(means, seed_of(metric));
    }
    return run;
}

Axis axis_over(std::span<const double> values) {
    RR_ASSERT(!values.empty(), "an axis holds at least one value");
    const auto [lowest, highest] = std::ranges::minmax(values);
    double low = lowest;
    double high = highest;
    if (high == low) {
        const double room = low == 0 ? 1.0 : std::abs(low) / 10;  // one value: room around it
        low -= room;
        high += room;
    }
    const double raw = (high - low) / kSteps;
    const double magnitude = std::pow(10.0, std::floor(std::log10(raw)));
    const double step = magnitude * round_up(raw / magnitude);
    // A value a hair off a tick, by the rounding of a division (0.6 / 0.1 is
    // 5.999…), is on it, and gets no step of room it does not need.
    Axis axis{.low = std::floor((low / step) + kHair) * step,
              .high = std::ceil((high / step) - kHair) * step,
              .step = step};
    const auto count = static_cast<int>(std::lround((axis.high - axis.low) / step));
    for (int index = 0; index <= count; ++index) {
        axis.ticks.push_back(axis.low + (index * step));
    }
    return axis;
}

std::string default_quality(std::span<const RunScores> runs) {
    const bool judged = std::ranges::any_of(
        runs, [](const RunScores& run) { return run_mean(run.scores, "facts_told").has_value(); });
    return judged ? "facts_told" : "atoms_told";
}

std::string eval_page(std::span<const RunScores> runs, const PageOptions& options) {
    std::string html = std::format(
        "<!doctype html>\n<html lang=\"en\"><head><meta charset=\"utf-8\">\n"
        "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n"
        "<title>The eval's runs</title>\n{}\n</head><body>\n<h1>The eval's runs</h1>\n"
        "<p class=\"meta\">{} run{}, as the library had them at {}.</p>\n",
        kStyle, runs.size(), runs.size() == 1 ? "" : "s", escaped(options.made));
    html += chart_html(runs, options);
    html += runs_html(runs);
    html += "<h2>Each run's videos</h2>\n";
    for (const RunScores& run : runs) {
        html += videos_html(run);
    }
    return html + "</body></html>\n";
}

}  // namespace rr::eval
