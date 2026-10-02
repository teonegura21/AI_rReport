#include <rr/eval/metrics.hpp>

#include <set>
#include <string_view>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <rr/eval/panel.hpp>
#include <rr/eval/run.hpp>
#include <rr/eval/score.hpp>

namespace rr::eval {
namespace {

TEST(Metrics, AreEachNamedOnceAndFoundByName) {
    std::set<std::string_view> named;
    for (const Metric& metric : metrics()) {
        EXPECT_TRUE(named.insert(metric.name).second) << metric.name << " twice";
        EXPECT_EQ(find_metric(metric.name), &metric);
    }
    EXPECT_EQ(find_metric("atoms_tolt"), nullptr);
    EXPECT_EQ(find_metric(""), nullptr);
}

TEST(Metrics, AreComputedByTheScorersTheyName) {
    const std::string judges = Panel{.name = "eval-v1"}.scorer();  // the families are views of it
    const std::set<std::string_view> families{*scorer_family(kCodeScorer), *scorer_family(judges)};
    for (const Metric& metric : metrics()) {
        EXPECT_TRUE(families.contains(metric.family)) << metric.name << " of " << metric.family;
    }
}

TEST(Metrics, SayWhichWayTheyAreBetter) {
    EXPECT_EQ(find_metric("atoms_told")->better, Better::higher);
    EXPECT_EQ(find_metric("invalid_markers")->better, Better::lower);
    EXPECT_EQ(find_metric("contradicts")->better, Better::lower);
    for (const std::string_view shown : {"length_ratio", "facts_agreement", "sentences_agreement"}) {
        EXPECT_EQ(find_metric(shown)->better, Better::neither) << shown << ": shown, never better or worse";
    }
    EXPECT_EQ(to_string(Better::higher), "higher");
    EXPECT_EQ(to_string(Better::neither), "neither");
}

TEST(Metrics, CostsAreTheCodesAndBetterLower) {
    std::set<std::string_view> costs;
    for (const Metric& metric : metrics()) {
        if (metric.cost) {
            costs.insert(metric.name);
            EXPECT_EQ(metric.better, Better::lower) << metric.name;
            EXPECT_EQ(metric.family, "code") << metric.name;
        }
    }
    EXPECT_EQ(costs, (std::set<std::string_view>{"calls", "completion_tokens", "escalations", "minutes",
                                                 "minutes_per_video_hour", "prompt_tokens"}));
}

}  // namespace
}  // namespace rr::eval
