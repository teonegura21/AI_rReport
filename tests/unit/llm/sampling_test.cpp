#include <rr/llm/sampling.hpp>

#include <limits>
#include <string>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "scripted_provider.hpp"

namespace rr::llm {
namespace {

using testing::ask;
using ::testing::ElementsAre;
using ::testing::HasSubstr;
using ::testing::IsEmpty;

constexpr double kNan = std::numeric_limits<double>::quiet_NaN();

// Qwen3.8's card, thinking.
const SamplingPreset kQwenThinking{
    .temperature = 1.0,
    .top_p = 0.95,
    .top_k = 20,
    .min_p = 0.0,
    .presence_penalty = 0.0,
    .repeat_penalty = 1.0,
    .thinking = true,
    .thinking_budget = ThinkingBudget{.tokens = 8192, .closing = "Answer now."},
    .reasoning_effort = ReasoningEffort::medium};

TEST(Sampling, APresetFillsWhatTheRequestLeavesOpen) {
    ChatRequest request = ask("plan the article");
    request.temperature = 0.6;  // the mission's own choice
    apply(kQwenThinking, request);
    EXPECT_EQ(request.temperature, 0.6) << "a value the request sets stands";
    EXPECT_EQ(request.top_p, 0.95);
    EXPECT_EQ(request.top_k, 20);
    EXPECT_EQ(request.min_p, 0.0);
    EXPECT_EQ(request.presence_penalty, 0.0) << "neutral values are sent too: no server default applies";
    EXPECT_EQ(request.repeat_penalty, 1.0);
    EXPECT_EQ(request.thinking, true);
    EXPECT_EQ(request.thinking_budget, (ThinkingBudget{.tokens = 8192, .closing = "Answer now."}));
    EXPECT_EQ(request.reasoning_effort, ReasoningEffort::medium);
    EXPECT_THAT(sampling_problems(request), IsEmpty());
}

TEST(Sampling, ABudgetAndAnEffortComeOnlyWithThinking) {
    ChatRequest request = ask("draft the section");
    request.thinking = false;
    apply(kQwenThinking, request);
    EXPECT_EQ(request.thinking, false);
    EXPECT_FALSE(request.thinking_budget.has_value()) << "a budget would be refused with thinking off";
    EXPECT_FALSE(request.reasoning_effort.has_value());
    EXPECT_THAT(sampling_problems(request), IsEmpty());
}

TEST(Sampling, ValuesAreCheckedAndNanFailsEveryRange) {
    ChatRequest fine = ask("x");
    fine.temperature = 0.0;
    fine.top_p = 1.0;
    fine.top_k = 0;
    fine.min_p = 1.0;
    fine.presence_penalty = -2.0;
    fine.repeat_penalty = 1.0;
    fine.max_tokens = 1;
    EXPECT_THAT(sampling_problems(fine), IsEmpty()) << "the edges of every range are inside it";

    const auto problems_with = [](auto set) {
        ChatRequest request = ask("x");
        set(request);
        return sampling_problems(request);
    };
    EXPECT_THAT(problems_with([](ChatRequest& r) { r.temperature = -0.1; }),
                ElementsAre(HasSubstr("temperature")));
    EXPECT_THAT(problems_with([](ChatRequest& r) { r.temperature = kNan; }),
                ElementsAre(HasSubstr("temperature")));
    EXPECT_THAT(
        problems_with([](ChatRequest& r) { r.temperature = std::numeric_limits<double>::infinity(); }),
        ElementsAre(HasSubstr("temperature")));
    EXPECT_THAT(problems_with([](ChatRequest& r) { r.top_p = 0.0; }), ElementsAre(HasSubstr("top_p")));
    EXPECT_THAT(problems_with([](ChatRequest& r) { r.top_p = kNan; }), ElementsAre(HasSubstr("top_p")));
    EXPECT_THAT(problems_with([](ChatRequest& r) { r.top_k = -1; }), ElementsAre(HasSubstr("top_k")));
    EXPECT_THAT(problems_with([](ChatRequest& r) { r.min_p = 1.5; }), ElementsAre(HasSubstr("min_p")));
    EXPECT_THAT(problems_with([](ChatRequest& r) { r.min_p = kNan; }), ElementsAre(HasSubstr("min_p")));
    EXPECT_THAT(problems_with([](ChatRequest& r) { r.presence_penalty = 2.5; }),
                ElementsAre(HasSubstr("presence_penalty")));
    EXPECT_THAT(problems_with([](ChatRequest& r) { r.presence_penalty = kNan; }),
                ElementsAre(HasSubstr("presence_penalty")));
    EXPECT_THAT(problems_with([](ChatRequest& r) { r.repeat_penalty = 0.0; }),
                ElementsAre(HasSubstr("repeat_penalty")));
    EXPECT_THAT(problems_with([](ChatRequest& r) { r.repeat_penalty = kNan; }),
                ElementsAre(HasSubstr("repeat_penalty")));
    EXPECT_THAT(problems_with([](ChatRequest& r) { r.max_tokens = 0; }),
                ElementsAre(HasSubstr("max_tokens")));
    EXPECT_THAT(problems_with([](ChatRequest& r) { r.n = 0; }), ElementsAre(HasSubstr("n must be")));
}

TEST(Sampling, SeveralAnswersNeedSampling) {
    ChatRequest greedy = ask("three drafts");
    greedy.n = 3;
    greedy.temperature = 0.0;
    EXPECT_THAT(sampling_problems(greedy), ElementsAre(HasSubstr("every choice is the same answer")))
        << "greedy decoding ignores the seed: three copies of one answer, at three times the cost";
    greedy.temperature = 0.7;
    EXPECT_THAT(sampling_problems(greedy), IsEmpty());
    greedy.temperature.reset();
    EXPECT_THAT(sampling_problems(greedy), IsEmpty()) << "the server's default samples";
    ChatRequest one = ask("one draft");
    one.n = 1;
    one.temperature = 0.0;
    EXPECT_THAT(sampling_problems(one), IsEmpty());
}

TEST(Sampling, ABudgetAndAnEffortNeedThinking) {
    ChatRequest request = ask("x");
    request.thinking_budget = ThinkingBudget{.tokens = 100};
    request.reasoning_effort = ReasoningEffort::low;
    EXPECT_THAT(sampling_problems(request),
                ElementsAre(HasSubstr("budget needs thinking"), HasSubstr("effort needs thinking")))
        << "the model's default may be not to think: say it";
    request.thinking = false;
    EXPECT_EQ(sampling_problems(request).size(), 2U);
    request.thinking = true;
    EXPECT_THAT(sampling_problems(request), IsEmpty());
    request.thinking_budget->tokens = -1;
    EXPECT_THAT(sampling_problems(request), ElementsAre(HasSubstr(">= 0 tokens")));
}

TEST(Sampling, PenaltiesAreFoundInTheFieldsAndInExtra) {
    ChatRequest neutral = ask("label these sentences");
    neutral.presence_penalty = 0.0;
    neutral.repeat_penalty = 1.0;
    neutral.extra = {
        {"frequency_penalty", 0}, {"dry_multiplier", 0.0}, {"xtc_probability", 0}, {"id_slot", 2}};
    EXPECT_THAT(penalty_problems(neutral), IsEmpty()) << "neutral values penalize nothing";

    ChatRequest penalized = ask("label these sentences");
    penalized.presence_penalty = 1.5;  // Qwen3.8's card, without thinking
    penalized.repeat_penalty = 1.1;
    EXPECT_THAT(penalty_problems(penalized),
                ElementsAre(HasSubstr("presence_penalty = 1.5"), HasSubstr("repeat_penalty = 1.1")));

    ChatRequest through_extra = ask("label these sentences");
    through_extra.extra = {{"presence_penalty", 0.5},
                           {"frequency_penalty", 0.2},
                           {"repeat_penalty", "1.1"},
                           {"dry_multiplier", 0.8},
                           {"xtc_probability", 0.5}};
    EXPECT_EQ(penalty_problems(through_extra).size(), 5U) << "a string is no neutral value either";

    ChatRequest not_a_number = ask("label these sentences");
    not_a_number.presence_penalty = kNan;
    EXPECT_EQ(penalty_problems(not_a_number).size(), 1U);
}

TEST(Sampling, APresetIsCheckedAsAWhole) {
    EXPECT_THAT(preset_problems(kQwenThinking), IsEmpty());
    SamplingPreset direct = kQwenThinking;
    direct.thinking = false;
    EXPECT_THAT(preset_problems(direct),
                ElementsAre(HasSubstr("budget needs thinking"), HasSubstr("effort needs thinking")))
        << "apply() would drop them silently: a preset that sets them must think";
    SamplingPreset hot = kQwenThinking;
    hot.top_p = 1.2;
    EXPECT_THAT(preset_problems(hot), ElementsAre(HasSubstr("top_p")));
}

TEST(Sampling, EffortsHaveNames) {
    for (const ReasoningEffort effort :
         {ReasoningEffort::low, ReasoningEffort::medium, ReasoningEffort::high, ReasoningEffort::xhigh}) {
        EXPECT_EQ(reasoning_effort_from_string(to_string(effort)), effort);
    }
    EXPECT_EQ(to_string(ReasoningEffort::xhigh), "xhigh");
    EXPECT_FALSE(reasoning_effort_from_string("none").has_value()) << "thinking off is thinking = false";
    EXPECT_FALSE(reasoning_effort_from_string("Medium").has_value());
}

}  // namespace
}  // namespace rr::llm
