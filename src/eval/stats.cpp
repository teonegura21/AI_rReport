// Two runs compared (rr/eval/stats.hpp).
#include <rr/eval/stats.hpp>

#include <algorithm>
#include <cmath>
#include <format>
#include <numbers>
#include <numeric>
#include <random>
#include <ranges>
#include <utility>

#include <rr/core/error.hpp>
#include <rr/core/strings.hpp>

namespace rr::eval {

namespace {

// `metric`'s value in `entry`, and the scorer that computed it.
struct Valued {
    double value = 0;
    const std::string* scorer = nullptr;
};

std::optional<Valued> value_of(const ScoredEntry& entry, std::string_view metric) {
    for (const Scoring& scoring : entry.scorings) {
        const auto found = std::ranges::find(scoring.scores, metric, &Score::metric);
        if (found != scoring.scores.end()) {
            return Valued{.value = found->value, .scorer = &scoring.scorer};
        }
    }
    return std::nullopt;
}

double mean(std::span<const double> values) {
    return std::accumulate(values.begin(), values.end(), 0.0) / static_cast<double>(values.size());
}

// The sample standard deviation (n − 1), of two values or more.
double standard_deviation(std::span<const double> values) {
    RR_ASSERT(values.size() >= 2, "a sample's deviation needs two values");
    const double centre = mean(values);
    double squares = 0;
    for (const double value : values) {
        squares += (value - centre) * (value - centre);
    }
    return std::sqrt(squares / static_cast<double>(values.size() - 1));
}

// The value at `share` of `sorted`, by nearest rank: the smallest value with
// at least that share of the values at or below it.
double percentile(std::span<const double> sorted, double share) {
    RR_ASSERT(!sorted.empty(), "a percentile of nothing");
    const auto rank = static_cast<std::size_t>(std::ceil(share * static_cast<double>(sorted.size())));
    return sorted[std::clamp<std::size_t>(rank, 1, sorted.size()) - 1];
}

// `resamples` means of `values` resampled with replacement. Each index is the
// generator's own number modulo the count: the standard fixes mt19937_64's
// sequence, but leaves its distributions' algorithms to each library, and a
// comparison must print the same interval on every machine. The modulo's
// bias, for a few hundred videos against 2⁶⁴, is below 10⁻¹⁶.
std::vector<double> resampled_means(std::span<const double> values, std::uint64_t seed,
                                    std::size_t resamples) {
    std::mt19937_64 generator{seed};
    std::vector<double> means;
    means.reserve(resamples);
    for (std::size_t draw = 0; draw < resamples; ++draw) {
        double sum = 0;
        for (std::size_t drawn = 0; drawn < values.size(); ++drawn) {
            sum += values[generator() % values.size()];
        }
        means.push_back(sum / static_cast<double>(values.size()));
    }
    return means;
}

// "code@1 and code@2", for a message.
std::string scorers_named(const std::set<std::string, std::less<>>& scorers) {
    const std::vector<std::string> named{scorers.begin(), scorers.end()};
    return join(named, " and ");
}

}  // namespace

VideoValues per_video(std::span<const ScoredEntry> entries, std::string_view metric) {
    std::map<std::string, std::pair<double, std::size_t>, std::less<>> sums;
    VideoValues values;
    for (const ScoredEntry& entry : entries) {
        if (const std::optional<Valued> valued = value_of(entry, metric); valued.has_value()) {
            auto& [sum, count] = sums[entry.entry.video];
            sum += valued->value;
            ++count;
            values.scorers.insert(*valued->scorer);
        }
    }
    for (const auto& [video, kept] : sums) {
        values.means.emplace(video, kept.first / static_cast<double>(kept.second));
    }
    return values;
}

std::uint64_t seed_of(std::string_view metric) noexcept {
    constexpr std::uint64_t kOffset = 14'695'981'039'346'656'037ULL;
    constexpr std::uint64_t kPrime = 1'099'511'628'211ULL;
    std::uint64_t hash = kOffset;
    for (const char character : metric) {
        hash ^= static_cast<unsigned char>(character);
        hash *= kPrime;
    }
    return hash;
}

std::optional<Interval> bootstrap_mean(std::span<const double> values, std::uint64_t seed,
                                       std::size_t resamples) {
    if (values.empty() || resamples == 0) {
        return std::nullopt;
    }
    std::vector<double> means = resampled_means(values, seed, resamples);
    std::ranges::sort(means);
    return Interval{.low = percentile(means, 0.025), .high = percentile(means, 0.975)};
}

Result<Comparison> compare(std::span<const ScoredEntry> a, std::span<const ScoredEntry> b,
                           std::string_view metric) {
    const VideoValues in_a = per_video(a, metric);
    const VideoValues in_b = per_video(b, metric);
    const auto scored_alike = [&](std::string_view run, const VideoValues& values) -> Result<void> {
        if (values.scorers.size() > 1) {
            return fail(Errc::conflict,
                        std::format("{}: run {}'s entries were scored by {}, which measure it differently: "
                                    "score the run again, whole",
                                    metric, run, scorers_named(values.scorers)));
        }
        return ok();
    };
    RR_TRY(scored_alike("A", in_a));
    RR_TRY(scored_alike("B", in_b));
    if (!in_a.scorers.empty() && !in_b.scorers.empty() && in_a.scorers != in_b.scorers) {
        return fail(Errc::conflict,
                    std::format("{}: run A was scored by {} and run B by {}, which measure it differently: "
                                "score both with one",
                                metric, *in_a.scorers.begin(), *in_b.scorers.begin()));
    }
    Comparison comparison{.metric = std::string{metric}};
    if (!in_a.scorers.empty()) {
        comparison.scorer = *in_a.scorers.begin();
    } else if (!in_b.scorers.empty()) {
        comparison.scorer = *in_b.scorers.begin();
    }
    std::vector<double> differences;
    double sum_a = 0;
    double sum_b = 0;
    for (const auto& [video, value] : in_a.means) {
        const auto other = in_b.means.find(video);
        if (other == in_b.means.end()) {
            comparison.unpaired.push_back(video);
            continue;
        }
        sum_a += value;
        sum_b += other->second;
        differences.push_back(other->second - value);
        comparison.differences.emplace(video, differences.back());
    }
    for (const auto& [video, value] : in_b.means) {
        if (!in_a.means.contains(video)) {
            comparison.unpaired.push_back(video);
        }
    }
    std::ranges::sort(comparison.unpaired);
    comparison.videos = differences.size();
    if (differences.empty()) {
        return comparison;
    }
    const auto videos = static_cast<double>(differences.size());
    comparison.a = sum_a / videos;
    comparison.b = sum_b / videos;
    comparison.difference = comparison.b - comparison.a;
    if (differences.size() >= 2) {
        comparison.interval = bootstrap_mean(differences, seed_of(metric));
        comparison.standard_error = standard_deviation(differences) / std::sqrt(videos);
    }
    return comparison;
}

std::optional<double> Comparison::detectable() const {
    if (!standard_error.has_value()) {
        return std::nullopt;
    }
    return kDetectable * *standard_error;
}

std::optional<double> unchanged_pass_rate(std::span<const NullMetric> metrics, std::uint64_t seed,
                                          std::size_t resamples) {
    if (metrics.empty()) {
        return 1.0;  // every metric of none passes
    }
    std::vector<std::string> videos;  // those every metric has
    for (const auto& [video, difference] : metrics.front().differences) {
        if (std::ranges::all_of(
                metrics, [&](const NullMetric& metric) { return metric.differences.contains(video); })) {
            videos.push_back(video);
        }
    }
    if (videos.size() < 2 || resamples == 0) {
        return std::nullopt;
    }
    // Each metric's differences on those videos, centred on zero, with its
    // standard error.
    struct Centred {
        std::vector<double> values{};
        double standard_error = 0;
        double margin = 0;
    };
    std::vector<Centred> centred;
    centred.reserve(metrics.size());
    const auto count = static_cast<double>(videos.size());
    for (const NullMetric& metric : metrics) {
        Centred one{.margin = metric.margin};
        for (const std::string& video : videos) {
            one.values.push_back(metric.differences.find(video)->second);
        }
        const double centre = mean(one.values);
        for (double& value : one.values) {
            value -= centre;
        }
        one.standard_error = standard_deviation(one.values) / std::sqrt(count);
        centred.push_back(std::move(one));
    }
    std::mt19937_64 generator{seed};
    std::vector<double> sums(centred.size());
    std::size_t passed = 0;
    for (std::size_t draw = 0; draw < resamples; ++draw) {
        std::ranges::fill(sums, 0.0);
        for (std::size_t drawn = 0; drawn < videos.size(); ++drawn) {
            const std::size_t video = generator() % videos.size();
            for (std::size_t index = 0; index < centred.size(); ++index) {
                sums[index] += centred[index].values[video];
            }
        }
        const bool every =
            std::ranges::all_of(std::views::iota(std::size_t{0}, centred.size()), [&](std::size_t index) {
                const Centred& one = centred[index];
                return (sums[index] / count) - (kZ95 * one.standard_error) >= -one.margin;
            });
        if (every) {
            ++passed;
        }
    }
    return static_cast<double>(passed) / static_cast<double>(resamples);
}

double alone_pass_rate(double margin, double standard_error) noexcept {
    if (standard_error <= 0) {
        return margin >= 0 ? 1.0 : 0.0;  // no noise: an unchanged pipeline differs by nothing
    }
    // Φ(x) = erfc(−x / √2) / 2.
    const double x = (margin / standard_error) - kZ95;
    return 0.5 * std::erfc(-x / std::numbers::sqrt2);
}

}  // namespace rr::eval
