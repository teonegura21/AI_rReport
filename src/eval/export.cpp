// A run's scores, exported (rr/eval/export.hpp).
#include <rr/eval/export.hpp>

#include <algorithm>
#include <format>
#include <functional>
#include <set>
#include <string>
#include <string_view>
#include <utility>

#include <rr/media/video.hpp>

namespace rr::eval {

namespace {

// `field` as CSV writes it: quoted when it holds a comma, a quote or a line
// break, its quotes doubled.
std::string csv_field(std::string_view field) {
    if (field.find_first_of(",\"\r\n") == std::string_view::npos) {
        return std::string{field};
    }
    std::string quoted = "\"";
    for (const char c : field) {
        if (c == '"') {
            quoted += '"';
        }
        quoted += c;
    }
    quoted += '"';
    return quoted;
}

// The video's type and caption kind, as `set` says; none for a video it has
// no more.
std::pair<std::string, std::string> kinds_of(const EvalSet& set, std::string_view video) {
    const EvalVideo* const found = set.find(video);
    if (found == nullptr) {
        return {};
    }
    return {std::string{to_string(found->type)}, std::string{media::to_string(found->captions)}};
}

// Every metric any entry has, sorted by name.
std::set<std::string, std::less<>> metrics_of(std::span<const ScoredEntry> entries) {
    std::set<std::string, std::less<>> metrics;
    for (const ScoredEntry& entry : entries) {
        for (const Scoring& scoring : entry.scorings) {
            for (const Score& score : scoring.scores) {
                metrics.insert(score.metric);
            }
        }
    }
    return metrics;
}

// `entry`'s score for `metric`, whichever scorer computed it; none when none did.
const Score* score_of(const ScoredEntry& entry, std::string_view metric) {
    for (const Scoring& scoring : entry.scorings) {
        const auto found = std::ranges::find(scoring.scores, metric, &Score::metric);
        if (found != scoring.scores.end()) {
            return &*found;
        }
    }
    return nullptr;
}

// `entry`'s scorers as one field: "code@1+judges@eval-v1".
std::string scorers_of(const ScoredEntry& entry) {
    std::string scorers;
    for (const Scoring& scoring : entry.scorings) {
        scorers += (scorers.empty() ? "" : "+") + scoring.scorer;
    }
    return scorers;
}

}  // namespace

std::string to_csv(const EvalRun& run, const EvalSet& set, std::span<const ScoredEntry> entries) {
    const std::set<std::string, std::less<>> metrics = metrics_of(entries);
    std::string csv = "run,tier,video,type,captions,replicate,job,scorers";
    for (const std::string& metric : metrics) {
        csv += ',' + csv_field(metric);
    }
    csv += "\r\n";
    for (const ScoredEntry& scored : entries) {
        const auto [type, captions] = kinds_of(set, scored.entry.video);
        csv += std::format("{},{},{},{},{},{},{},{}", csv_field(run.spec.label), csv_field(run.spec.tier),
                           csv_field(scored.entry.video), type, captions, scored.entry.replicate,
                           csv_field(scored.entry.job), csv_field(scorers_of(scored)));
        for (const std::string& metric : metrics) {
            csv += ',';
            if (const Score* const score = score_of(scored, metric); score != nullptr) {
                csv += std::format("{}", score->value);  // the shortest text that reads back the same
            }
        }
        csv += "\r\n";
    }
    return csv;
}

json::Json to_json(const EvalRun& run, const EvalSet& set, std::span<const ScoredEntry> entries) {
    json::Json rows = json::Json::array();
    for (const ScoredEntry& scored : entries) {
        const auto [type, captions] = kinds_of(set, scored.entry.video);
        json::Json scores = json::Json::object();
        for (const Scoring& scoring : scored.scorings) {
            json::Json values = json::Json::object();
            for (const Score& score : scoring.scores) {
                values[score.metric] = score.value;
            }
            scores[scoring.scorer] = std::move(values);
        }
        rows.push_back({{"video", scored.entry.video},
                        {"replicate", scored.entry.replicate},
                        {"job", scored.entry.job},
                        {"type", type},
                        {"captions", captions},
                        {"scores", std::move(scores)}});
    }
    return {{"run",
             {{"label", run.spec.label},
              {"set", run.spec.eval_set},
              {"split", to_string(run.spec.split)},
              {"tier", run.spec.tier},
              {"replicates", run.spec.replicates}}},
            {"entries", std::move(rows)}};
}

}  // namespace rr::eval
