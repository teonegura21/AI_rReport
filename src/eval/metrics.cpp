// Every metric the eval scores (rr/eval/metrics.hpp).
#include <rr/eval/metrics.hpp>

#include <array>

namespace rr::eval {

namespace {

constexpr std::string_view kCode = "code";
constexpr std::string_view kJudges = "judges";

constexpr std::array kMetrics{
    // What a faithful article never breaks, and keeps of its atoms (code).
    Metric{.name = "sentences_accounted", .family = kCode, .better = Better::higher},
    Metric{.name = "invalid_markers", .family = kCode, .better = Better::lower},
    Metric{.name = "quotes_kept", .family = kCode, .better = Better::higher},
    Metric{.name = "numbers_kept", .family = kCode, .better = Better::higher},
    Metric{.name = "atoms_told", .family = kCode, .better = Better::higher},
    // Against the gold a person reviewed (code).
    Metric{.name = "gold_quotes_kept", .family = kCode, .better = Better::higher},
    Metric{.name = "gold_terms_used", .family = kCode, .better = Better::higher},
    Metric{.name = "facts_cited", .family = kCode, .better = Better::higher},
    Metric{.name = "core_facts_cited", .family = kCode, .better = Better::higher},
    // What a reader learns, and whether the video supports what it reads (judges).
    Metric{.name = "facts_told", .family = kJudges, .better = Better::higher},
    Metric{.name = "facts_told_partly", .family = kJudges, .better = Better::higher},
    Metric{.name = "core_facts_told", .family = kJudges, .better = Better::higher},
    Metric{.name = "sentences_supported", .family = kJudges, .better = Better::higher},
    Metric{.name = "adds", .family = kJudges, .better = Better::lower},
    Metric{.name = "softens", .family = kJudges, .better = Better::lower},
    Metric{.name = "intensifies", .family = kJudges, .better = Better::lower},
    Metric{.name = "misattributes", .family = kJudges, .better = Better::lower},
    Metric{.name = "contradicts", .family = kJudges, .better = Better::lower},
    // The article's form: its length (code), and how it reads beside the
    // other run's (judges, read pairwise); and how alike the judges read
    // (judges). A longer article is not a better one, and agreement says how
    // far to trust the panel, not the article.
    Metric{.name = "length_ratio", .family = kCode, .better = Better::neither},
    Metric{.name = "flow", .family = kJudges, .better = Better::higher, .pairwise = true},
    Metric{.name = "facts_agreement", .family = kJudges, .better = Better::neither},
    Metric{.name = "sentences_agreement", .family = kJudges, .better = Better::neither},
    // What it cost (code).
    Metric{.name = "minutes", .family = kCode, .better = Better::lower, .cost = true},
    Metric{.name = "minutes_per_video_hour", .family = kCode, .better = Better::lower, .cost = true},
    Metric{.name = "calls", .family = kCode, .better = Better::lower, .cost = true},
    Metric{.name = "prompt_tokens", .family = kCode, .better = Better::lower, .cost = true},
    Metric{.name = "completion_tokens", .family = kCode, .better = Better::lower, .cost = true},
    Metric{.name = "escalations", .family = kCode, .better = Better::lower, .cost = true},
};

}  // namespace

std::string_view to_string(Better better) noexcept {
    switch (better) {
        case Better::higher:
            return "higher";
        case Better::lower:
            return "lower";
        case Better::neither:
            break;
    }
    return "neither";
}

std::span<const Metric> metrics() noexcept {
    return kMetrics;
}

const Metric* find_metric(std::string_view name) noexcept {
    for (const Metric& metric : kMetrics) {
        if (metric.name == name) {
            return &metric;
        }
    }
    return nullptr;
}

}  // namespace rr::eval
