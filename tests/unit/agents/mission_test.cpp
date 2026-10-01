#include <rr/agents/mission.hpp>

#include <string>

#include <gtest/gtest.h>

namespace rr::agents {
namespace {

TEST(Mission, AFamilyIsTheModelsLeadingLetters) {
    EXPECT_EQ(family_of("qwen3.8-27b-q4"), "qwen");
    EXPECT_EQ(family_of("gemma-4-26b-a4b"), "gemma");
    EXPECT_EQ(family_of("Qwen3-Embedding-0.6B"), "qwen") << "lowercased";
    EXPECT_EQ(family_of("4bit"), "");
}

TEST(Mission, RefusalsAreShortAndOpenTheWayRefusalsDo) {
    EXPECT_TRUE(looks_like_refusal("I'm sorry, but I can't help with that."));
    EXPECT_TRUE(looks_like_refusal("  I cannot assist with this request."));
    EXPECT_TRUE(looks_like_refusal("I can\u2019t help with that.")) << "a typographic apostrophe";
    EXPECT_TRUE(looks_like_refusal("AS AN AI, I must refuse.")) << "any case";
    EXPECT_TRUE(looks_like_refusal("I won't summarize this."));

    EXPECT_FALSE(looks_like_refusal(""));
    EXPECT_FALSE(looks_like_refusal(R"({"label": "claim"})"));
    EXPECT_FALSE(looks_like_refusal("The speaker says she can't attend."));
    EXPECT_FALSE(looks_like_refusal("I can't " + std::string(600, 'x'))) << "a long answer is not a refusal";
}

TEST(Mission, StepsAndVerdictsHaveStableNames) {
    for (const Step step : {Step::as_ordered, Step::thinking, Step::commander, Step::other_family}) {
        EXPECT_EQ(step_from_string(to_string(step)), step);
    }
    EXPECT_EQ(to_string(Step::other_family), "other_family");
    EXPECT_EQ(step_from_string("escalated"), std::nullopt);
    for (const Verdict verdict : {Verdict::accepted, Verdict::invalid, Verdict::refused, Verdict::ceiling}) {
        EXPECT_EQ(verdict_from_string(to_string(verdict)), verdict);
    }
    EXPECT_EQ(to_string(Verdict::ceiling), "ceiling");
    EXPECT_EQ(verdict_from_string("ok"), std::nullopt);
}

}  // namespace
}  // namespace rr::agents
