#include <rr/agents/army.hpp>

#include <atomic>
#include <chrono>
#include <format>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <rr/log/log.hpp>
#include <rr/testkit/result.hpp>

#include "keyed_provider.hpp"

namespace rr::agents {
namespace {

using namespace std::chrono_literals;
using ::testing::HasSubstr;
using testing::KeyedProvider;
using testing::Release;
using testing::reply;
using testkit::unwrap;

constexpr std::string_view kClaim = R"({"label": "claim"})";
constexpr std::string_view kMaybe = R"({"label": "maybe"})";  // not one of the labels
constexpr std::string_view kRefusal = "I'm sorry, but I can't help with that.";

llm::SamplingPreset direct() {
    return llm::SamplingPreset{.temperature = 0.7, .top_p = 0.8, .top_k = 20};
}

llm::SamplingPreset thinking() {
    return llm::SamplingPreset{
        .temperature = 1.0,
        .top_p = 0.95,
        .top_k = 20,
        .thinking = true,
        .thinking_budget = llm::ThinkingBudget{.tokens = 1000, .closing = "Answer now."}};
}

// The infantry: Gemma, `slots` slots of 8192 tokens each.
Engine infantry(int slots = 4) {
    return Engine{.model = "gemma",
                  .roles = {"infantry"},
                  .family = "gemma",
                  .slots = slots,
                  .context = 8192 * slots,
                  .ctx_per_slot = 8192,
                  .direct = direct(),
                  .thinking = thinking()};
}

// The commander: Qwen, `slots` slots sharing a pool of `context` tokens.
Engine commander(int slots = 2, int context = 16384) {
    return Engine{.model = "qwen",
                  .roles = {"commander"},
                  .family = "qwen",
                  .slots = slots,
                  .context = context,
                  .ctx_per_slot = context,
                  .kv_unified = true,
                  .direct = direct(),
                  .thinking = thinking()};
}

// {"label": "claim" or "aside"}
std::shared_ptr<const json::Schema> label_schema() {
    static const auto schema = std::make_shared<const json::Schema>(unwrap(json::Schema::compile_text(R"({
        "type": "object",
        "additionalProperties": false,
        "required": ["label"],
        "properties": {"label": {"type": "string", "enum": ["claim", "aside"]}}
    })")));
    return schema;
}

Mission mission(std::string id, std::string role = "infantry") {
    Mission labelling;
    labelling.id = std::move(id);
    labelling.kind = "label";
    labelling.prompt = "label@1";
    labelling.role = std::move(role);
    labelling.request.messages = {llm::Message::system("Label the sentence: claim or aside."),
                                  llm::Message::user("Water boils at 100 degrees.")};
    labelling.request.seed = 100;
    labelling.answer_tokens = 200;
    labelling.schema = label_schema();
    return labelling;
}

// Battle-log records, kept in memory.
class MemoryLog final : public BattleLog {
public:
    [[nodiscard]] Result<void> record(const MissionRecord& mission) override {
        const std::lock_guard lock(mutex_);
        records_.insert_or_assign(mission.mission, mission);
        return ok();
    }
    [[nodiscard]] MissionRecord at(const std::string& mission) const {
        const std::lock_guard lock(mutex_);
        return records_.at(mission);
    }
    [[nodiscard]] std::size_t size() const {
        const std::lock_guard lock(mutex_);
        return records_.size();
    }

private:
    mutable std::mutex mutex_;
    std::map<std::string, MissionRecord> records_;
};

Army make_army(const llm::LlmProvider& provider, const Clock& clock, BattleLog* log = nullptr,
               std::vector<Engine> engines = {commander(), infantry()}, Ceilings ceilings = {},
               std::chrono::milliseconds elapsed_before = {}) {
    return unwrap(Army::create(Army::Options{.engines = std::move(engines),
                                             .job_id = "job-1",
                                             .ceilings = ceilings,
                                             .elapsed_before = elapsed_before},
                               provider, clock, log));
}

std::vector<Mission> missions(int count, const std::string& role = "infantry") {
    std::vector<Mission> all;
    all.reserve(static_cast<std::size_t>(count));
    for (int index = 0; index < count; ++index) {
        all.push_back(mission(std::format("m{}", index), role));
    }
    return all;
}

// Polls `condition` until it holds, for at most ten seconds.
bool eventually(const std::function<bool()>& condition) {
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while (!condition()) {
        if (std::chrono::steady_clock::now() > deadline) {
            return false;
        }
        std::this_thread::sleep_for(1ms);
    }
    return true;
}

json::Json json_of(std::string_view text) {
    return unwrap(json::parse(text));
}

// --- One mission ---------------------------------------------------------------

TEST(Army, AnswersAMissionAndSaysWhatItAsked) {
    FakeClock clock;
    const KeyedProvider model{[&](const llm::ChatRequest& request) {
        clock.advance(250ms);
        return reply(request, std::string{kClaim});
    }};
    Army army = make_army(model, clock);

    const Outcome outcome = unwrap(army.execute(mission("m1")));
    EXPECT_EQ(outcome.mission, "m1");
    EXPECT_EQ(outcome.verdict, Verdict::accepted);
    ASSERT_EQ(outcome.answers.size(), 1U);
    const Answer& answer = outcome.answers.front();
    EXPECT_EQ(answer.value, json_of(kClaim));
    EXPECT_EQ(answer.sample, 0);
    EXPECT_EQ(answer.step, Step::as_ordered);
    EXPECT_EQ(answer.model, "gemma");
    EXPECT_FALSE(answer.thinking);
    EXPECT_EQ(answer.repairs, 0);
    EXPECT_EQ(outcome.step, Step::as_ordered);
    EXPECT_EQ(outcome.rungs, 1);
    EXPECT_EQ(outcome.calls, 1);
    EXPECT_EQ(outcome.usage, (llm::Usage{.prompt_tokens = 20, .completion_tokens = 5}));
    EXPECT_EQ(outcome.duration, 250ms);

    const llm::ChatRequest sent = model.requests_for("m1").at(0);
    EXPECT_EQ(sent.model, "gemma") << "the role's engine";
    EXPECT_EQ(sent.max_tokens, 200) << "the answer's size, and no thinking";
    EXPECT_EQ(sent.thinking, false);
    EXPECT_FALSE(sent.thinking_budget.has_value());
    EXPECT_EQ(sent.temperature, 0.7) << "the engine's direct preset";
    EXPECT_EQ(sent.top_k, 20);
    EXPECT_EQ(sent.seed, 100);
    EXPECT_FALSE(sent.n.has_value()) << "one answer: no n, so its key is a plain call's";
    EXPECT_EQ(sent.json_schema, label_schema()->model_facing());
    EXPECT_EQ(sent.tag, (llm::CallTag{.purpose = "label", .job_id = "job-1", .mission_id = "m1"}));
}

TEST(Army, AMissionsOwnSamplingWins) {
    const FakeClock clock;
    const KeyedProvider model{
        [](const llm::ChatRequest& request) { return reply(request, std::string{kClaim}); }};
    Army army = make_army(model, clock);
    Mission cool = mission("m1");
    cool.request.temperature = 0.2;
    ASSERT_OK(army.execute(std::move(cool)));
    const llm::ChatRequest sent = model.requests_for("m1").at(0);
    EXPECT_EQ(sent.temperature, 0.2);
    EXPECT_EQ(sent.top_p, 0.8) << "the rest from the preset";
}

TEST(Army, RepairsAFailingAnswerWithItsProblems) {
    const FakeClock clock;
    const KeyedProvider model{[](const llm::ChatRequest& request) {
        return reply(request, std::string{request.messages.size() == 2 ? kMaybe : kClaim});
    }};
    Army army = make_army(model, clock);

    const Outcome outcome = unwrap(army.execute(mission("m1")));
    EXPECT_EQ(outcome.verdict, Verdict::accepted);
    ASSERT_EQ(outcome.answers.size(), 1U);
    EXPECT_EQ(outcome.answers.front().repairs, 1);
    EXPECT_EQ(outcome.calls, 2);
    ASSERT_EQ(outcome.problems.size(), 1U) << "what was repaired";
    EXPECT_THAT(outcome.problems.front(), HasSubstr("/label (enum)"));

    const std::vector<llm::ChatRequest> sent = model.requests_for("m1");
    ASSERT_EQ(sent.size(), 2U);
    ASSERT_EQ(sent[1].messages.size(), 4U);
    EXPECT_EQ(sent[1].messages[2], llm::Message::assistant(std::string{kMaybe}));
    EXPECT_THAT(sent[1].messages[3].text(), HasSubstr("/label (enum)"));
    EXPECT_THAT(sent[1].messages[3].text(), HasSubstr("Reply with the corrected JSON only"));
    EXPECT_EQ(sent[1].seed, 100);
    EXPECT_EQ(sent[1].model, "gemma");
}

TEST(Army, ClimbsToThinkingThenToTheCommander) {
    const FakeClock clock;
    const KeyedProvider model{[](const llm::ChatRequest& request) {
        return reply(request, std::string{request.model == "qwen" ? kClaim : kMaybe});
    }};
    Army army = make_army(model, clock);
    Mission cool = mission("m1");
    cool.request.temperature = 0.2;

    const Outcome outcome = unwrap(army.execute(std::move(cool)));
    EXPECT_EQ(outcome.verdict, Verdict::accepted);
    EXPECT_EQ(outcome.step, Step::commander);
    EXPECT_EQ(outcome.rungs, 3);
    EXPECT_EQ(outcome.calls, 5) << "two on each Gemma rung, one on the commander's";
    ASSERT_EQ(outcome.answers.size(), 1U);
    EXPECT_EQ(outcome.answers.front().model, "qwen");
    EXPECT_TRUE(outcome.answers.front().thinking);
    EXPECT_EQ(outcome.answers.front().step, Step::commander);
    EXPECT_FALSE(outcome.problems.empty()) << "what Gemma got wrong, though the commander answered";

    const std::vector<llm::ChatRequest> sent = model.requests_for("m1");
    ASSERT_EQ(sent.size(), 5U);
    // The thinking rung: the same engine, its thinking budget on top of the answer.
    EXPECT_EQ(sent[2].model, "gemma");
    EXPECT_EQ(sent[2].thinking, true);
    EXPECT_EQ(sent[2].thinking_budget, (llm::ThinkingBudget{.tokens = 1000, .closing = "Answer now."}));
    EXPECT_EQ(sent[2].max_tokens, 200 + 1000 + prompt::TokenEstimator{}.estimate("Answer now."));
    EXPECT_EQ(sent[2].temperature, 1.0) << "a rung that escalates samples as its engine does";
    EXPECT_EQ(sent[2].messages.size(), 2U) << "a rung starts afresh";
    // The commander's rung.
    EXPECT_EQ(sent[4].model, "qwen");
    EXPECT_EQ(sent[4].thinking, true);
    EXPECT_EQ(sent[4].seed, 100);
}

TEST(Army, AnEscalationAsksForOneAnswer) {
    const FakeClock clock;
    const KeyedProvider model{[](const llm::ChatRequest& request) {
        if (request.model == "qwen") {
            return reply(request, std::string{kClaim});
        }
        return reply(request, [](int) { return std::string{kMaybe}; });
    }};
    Army army = make_army(model, clock);
    Mission three = mission("m1");
    three.samples = 3;
    three.repairs = 0;

    const Outcome outcome = unwrap(army.execute(std::move(three)));
    const std::vector<llm::ChatRequest> sent = model.requests_for("m1");
    ASSERT_EQ(sent.size(), 3U);
    EXPECT_EQ(sent[0].n, 3) << "the samples";
    EXPECT_FALSE(sent[1].n.has_value()) << "the thinking rung: one answer";
    EXPECT_FALSE(sent[2].n.has_value()) << "the commander: one answer";
    ASSERT_EQ(outcome.answers.size(), 1U);
    EXPECT_EQ(outcome.answers.front().sample, 0);
}

TEST(Army, ReportsAFindingWhenNoRungAnswers) {
    const FakeClock clock;
    const KeyedProvider model{
        [](const llm::ChatRequest& request) { return reply(request, std::string{kMaybe}); }};
    Army army = make_army(model, clock);

    const Outcome outcome = unwrap(army.execute(mission("m1")));
    EXPECT_EQ(outcome.verdict, Verdict::invalid);
    EXPECT_TRUE(outcome.answers.empty());
    EXPECT_EQ(outcome.step, Step::commander);
    EXPECT_EQ(outcome.rungs, 3);
    EXPECT_EQ(outcome.calls, 6);
    ASSERT_FALSE(outcome.problems.empty());
    EXPECT_THAT(outcome.problems.front(), HasSubstr("/label (enum)"));
}

TEST(Army, KeepsToTheMissionsEscalation) {
    const FakeClock clock;
    const KeyedProvider model{
        [](const llm::ChatRequest& request) { return reply(request, std::string{kMaybe}); }};
    Army army = make_army(model, clock);
    Mission judge = mission("m1");
    judge.escalation = Escalation{.thinking = false, .commander = false, .other_family = false};
    judge.repairs = 2;

    const Outcome outcome = unwrap(army.execute(std::move(judge)));
    EXPECT_EQ(outcome.verdict, Verdict::invalid);
    EXPECT_EQ(outcome.rungs, 1);
    EXPECT_EQ(outcome.calls, 3) << "the answer and its two repairs";
}

// --- Refusals ------------------------------------------------------------------

TEST(Army, ARefusalGoesToAnotherFamilyUnrepaired) {
    const FakeClock clock;
    const KeyedProvider model{[](const llm::ChatRequest& request) {
        return reply(request, std::string{request.model == "gemma" ? kRefusal : kClaim});
    }};
    Army army = make_army(model, clock);

    const Outcome outcome = unwrap(army.execute(mission("m1")));
    EXPECT_EQ(outcome.verdict, Verdict::accepted);
    EXPECT_EQ(outcome.step, Step::commander) << "Gemma's thinking rung is passed over";
    EXPECT_EQ(outcome.calls, 2) << "a refusal is not sent back to be repaired";
}

TEST(Army, ACommandersRefusalGoesToTheInfantry) {
    const FakeClock clock;
    const KeyedProvider model{[](const llm::ChatRequest& request) {
        return reply(request, std::string{request.model == "qwen" ? kRefusal : kClaim});
    }};
    Army army = make_army(model, clock);

    const Outcome outcome = unwrap(army.execute(mission("m1", "commander")));
    EXPECT_EQ(outcome.verdict, Verdict::accepted);
    EXPECT_EQ(outcome.step, Step::other_family);
    ASSERT_EQ(outcome.answers.size(), 1U);
    EXPECT_EQ(outcome.answers.front().model, "gemma");
    EXPECT_TRUE(outcome.answers.front().thinking);
}

TEST(Army, WhatEveryFamilyRefusesIsAFinding) {
    const FakeClock clock;
    const KeyedProvider model{
        [](const llm::ChatRequest& request) { return reply(request, std::string{kRefusal}); }};
    Army army = make_army(model, clock);

    const Outcome outcome = unwrap(army.execute(mission("m1", "commander")));
    EXPECT_EQ(outcome.verdict, Verdict::refused);
    EXPECT_EQ(outcome.calls, 2);
    EXPECT_THAT(outcome.problems, ::testing::Contains("it reads as a refusal"));
}

TEST(Army, AStageCanSayWhatARefusalLooksLike) {
    const FakeClock clock;
    const KeyedProvider model{[](const llm::ChatRequest& request) {
        return reply(request, std::string{request.model == "gemma" ? R"({"label": "softened"})" : kClaim});
    }};
    Army army = make_army(model, clock);
    Mission softened = mission("m1");
    softened.refusal = [](const llm::Choice& answer) {
        return answer.content.find("softened") != std::string::npos;
    };

    const Outcome outcome = unwrap(army.execute(std::move(softened)));
    EXPECT_EQ(outcome.step, Step::commander);
    EXPECT_EQ(outcome.calls, 2);
}

// --- Thinking ------------------------------------------------------------------

TEST(Army, ARepairThatWouldNotFitIsNotSent) {
    // The answer fills about 4300 tokens: sent back with its problems, and
    // with room for a new answer of up to 4000, it would outgrow the slot's 8192.
    const FakeClock clock;
    const KeyedProvider model{[](const llm::ChatRequest& request) {
        return reply(request, R"({"label": ")" + std::string(13000, 'x') + R"("})");
    }};
    Army army = make_army(model, clock);
    Mission verbose = mission("m1");
    verbose.answer_tokens = 4000;
    verbose.escalation = Escalation{.thinking = false, .commander = false, .other_family = false};

    const Outcome outcome = unwrap(army.execute(std::move(verbose)));
    EXPECT_EQ(outcome.verdict, Verdict::invalid);
    EXPECT_EQ(outcome.calls, 1);
    EXPECT_THAT(outcome.problems, ::testing::Contains("its repair would not fit 'gemma'"));
}

TEST(Army, ThinkingThatRunsOutIsAskedAgainWithoutThinking) {
    const FakeClock clock;
    const KeyedProvider model{[](const llm::ChatRequest& request) {
        if (request.thinking == true) {
            return reply(request, "", llm::FinishReason::length);  // all of it spent thinking
        }
        return reply(request, std::string{kClaim});
    }};
    Army army = make_army(model, clock);
    Mission outline = mission("m1", "commander");
    outline.request.thinking = true;

    const Outcome outcome = unwrap(army.execute(std::move(outline)));
    EXPECT_EQ(outcome.verdict, Verdict::accepted);
    EXPECT_EQ(outcome.rungs, 1);
    ASSERT_EQ(outcome.answers.size(), 1U);
    EXPECT_FALSE(outcome.answers.front().thinking);
    EXPECT_EQ(outcome.answers.front().repairs, 1);

    const std::vector<llm::ChatRequest> sent = model.requests_for("m1");
    ASSERT_EQ(sent.size(), 2U);
    EXPECT_EQ(sent[0].max_tokens, 200 + 1000 + prompt::TokenEstimator{}.estimate("Answer now."));
    EXPECT_EQ(sent[1].thinking, false);
    EXPECT_FALSE(sent[1].thinking_budget.has_value());
    EXPECT_EQ(sent[1].max_tokens, 200);
    EXPECT_EQ(sent[1].temperature, 0.7) << "answering directly, as the engine does";
    EXPECT_EQ(sent[1].messages.size(), 2U) << "there was no answer to repair";
}

TEST(Army, NeverAsksTheSameQuestionTwice) {
    // The commander fails directly, then runs out while thinking. Asked
    // again without thinking, the question would be the first one: its answer
    // is known, and the mission ends.
    const FakeClock clock;
    const KeyedProvider model{[](const llm::ChatRequest& request) {
        if (request.thinking == true) {
            return reply(request, "", llm::FinishReason::length);
        }
        return reply(request, std::string{kMaybe});
    }};
    Army army = make_army(model, clock);

    const Outcome outcome = unwrap(army.execute(mission("m1", "commander")));
    EXPECT_EQ(outcome.verdict, Verdict::invalid);
    EXPECT_EQ(outcome.calls, 3) << "the answer, its repair, and the thinking rung";
    EXPECT_EQ(model.requests_for("m1").size(), 3U);
}

TEST(Army, NeverAsksAProviderThatCannotThinkToThink) {
    const FakeClock clock;
    const KeyedProvider model{[](const llm::ChatRequest& request) {
                                  return reply(request,
                                               std::string{request.model == "qwen" ? kClaim : kMaybe});
                              },
                              llm::ProviderCaps{.json_schema = false, .thinking = false}};
    Army army = make_army(model, clock);

    const Outcome outcome = unwrap(army.execute(mission("m1")));
    EXPECT_EQ(outcome.verdict, Verdict::accepted);
    EXPECT_EQ(outcome.rungs, 2) << "no thinking rung: it would ask the same question";
    for (const llm::ChatRequest& sent : model.requests_for("m1")) {
        EXPECT_FALSE(sent.thinking.has_value());
        EXPECT_FALSE(sent.json_schema.has_value()) << "nor a schema it cannot constrain decoding with";
    }

    Mission outline = mission("m2", "commander");
    outline.request.thinking = true;
    EXPECT_ERRC(army.execute(std::move(outline)), Errc::invalid_argument);
}

// --- Samples -------------------------------------------------------------------

TEST(Army, SamplesShareOneReadingOfThePrompt) {
    const FakeClock clock;
    const KeyedProvider model{[](const llm::ChatRequest& request) {
        return reply(request,
                     [](int choice) { return std::string{choice == 1 ? R"({"label": "aside"})" : kClaim}; });
    }};
    Army army = make_army(model, clock);
    Mission three = mission("m1");
    three.samples = 3;

    const Outcome outcome = unwrap(army.execute(std::move(three)));
    const std::vector<llm::ChatRequest> sent = model.requests_for("m1");
    ASSERT_EQ(sent.size(), 1U);
    EXPECT_EQ(sent[0].n, 3);
    EXPECT_EQ(sent[0].seed, 100);
    ASSERT_EQ(outcome.answers.size(), 3U);
    for (int sample = 0; sample < 3; ++sample) {
        EXPECT_EQ(outcome.answers[static_cast<std::size_t>(sample)].sample, sample);
    }
    EXPECT_EQ(outcome.answers[1].value, json_of(R"({"label": "aside"})"));
    EXPECT_EQ(outcome.usage.completion_tokens, 15) << "every answer's tokens";
}

TEST(Army, SamplesAreSplitWhereTheyDoNotFitAtOnce) {
    const FakeClock clock;
    const KeyedProvider model{[](const llm::ChatRequest& request) {
        return reply(request, [](int) { return std::string{kClaim}; });
    }};
    Army army = make_army(model, clock, nullptr, {commander(), infantry(2)});
    Mission five = mission("m1");
    five.samples = 5;

    const Outcome outcome = unwrap(army.execute(std::move(five)));
    const std::vector<llm::ChatRequest> sent = model.requests_for("m1");
    ASSERT_EQ(sent.size(), 3U);
    EXPECT_EQ(sent[0].n, 2);
    EXPECT_EQ(sent[0].seed, 100);
    EXPECT_EQ(sent[1].n, 2);
    EXPECT_EQ(sent[1].seed, 102) << "sample j draws seed + j, however the samples are split";
    EXPECT_FALSE(sent[2].n.has_value());
    EXPECT_EQ(sent[2].seed, 104);
    ASSERT_EQ(outcome.answers.size(), 5U);
    EXPECT_EQ(outcome.answers[4].sample, 4);
}

TEST(Army, ASharedPoolDrawsAsManySamplesAsItsTokensHold) {
    // 2500 tokens: two answers of up to 1000 with their prompt, not three.
    const FakeClock clock;
    const KeyedProvider model{[](const llm::ChatRequest& request) {
        return reply(request, [](int) { return std::string{kClaim}; });
    }};
    Army army = make_army(model, clock, nullptr, {commander(4, 2500), infantry()});
    Mission three = mission("m1", "commander");
    three.samples = 3;
    three.answer_tokens = 1000;

    ASSERT_OK(army.execute(std::move(three)));
    const std::vector<llm::ChatRequest> sent = model.requests_for("m1");
    ASSERT_EQ(sent.size(), 2U);
    EXPECT_EQ(sent[0].n, 2);
    EXPECT_FALSE(sent[1].n.has_value());
    EXPECT_EQ(sent[1].seed, 102);
}

TEST(Army, EachSampleIsRepairedOnItsOwn) {
    const FakeClock clock;
    const KeyedProvider model{[](const llm::ChatRequest& request) {
        if (request.messages.size() > 2) {
            return reply(request, std::string{kClaim});  // a repair
        }
        return reply(request, [](int choice) { return std::string{choice == 1 ? kMaybe : kClaim}; });
    }};
    Army army = make_army(model, clock);
    Mission three = mission("m1");
    three.samples = 3;

    const Outcome outcome = unwrap(army.execute(std::move(three)));
    ASSERT_EQ(outcome.answers.size(), 3U);
    EXPECT_EQ(outcome.answers[1].repairs, 1);
    EXPECT_EQ(outcome.answers[0].repairs, 0);

    const std::vector<llm::ChatRequest> sent = model.requests_for("m1");
    ASSERT_EQ(sent.size(), 2U);
    EXPECT_FALSE(sent[1].n.has_value()) << "the repair asks for its sample alone";
    EXPECT_EQ(sent[1].seed, 101) << "with that sample's seed";
    EXPECT_EQ(sent[1].messages.at(2), llm::Message::assistant(std::string{kMaybe}));
}

TEST(Army, SomeSamplesAreEnough) {
    const FakeClock clock;
    const KeyedProvider model{[](const llm::ChatRequest& request) {
        if (request.messages.size() > 2) {
            return reply(request, std::string{kMaybe});  // sample 2 stays wrong
        }
        return reply(request, [](int choice) { return std::string{choice == 2 ? kMaybe : kClaim}; });
    }};
    Army army = make_army(model, clock);
    Mission three = mission("m1");
    three.samples = 3;

    const Outcome outcome = unwrap(army.execute(std::move(three)));
    EXPECT_EQ(outcome.verdict, Verdict::accepted);
    EXPECT_EQ(outcome.rungs, 1) << "no escalation while a sample was accepted";
    ASSERT_EQ(outcome.answers.size(), 2U);
    EXPECT_EQ(outcome.answers[1].sample, 1);
    EXPECT_FALSE(outcome.problems.empty()) << "what was wrong with sample 2";
}

// --- Prose ---------------------------------------------------------------------

TEST(Army, ProseIsCheckedAndRepairedToo) {
    const FakeClock clock;
    const KeyedProvider model{[](const llm::ChatRequest& request) {
        return reply(request, request.messages.size() == 2 ? "Water boils at 100 degrees."
                                                           : "Water boils at 100 degrees [A1].");
    }};
    Army army = make_army(model, clock);
    Mission write = mission("m1", "commander");
    write.schema.reset();
    write.check = [](const json::Json& answer) {
        return answer.get<std::string>().find("[A1]") == std::string::npos
                   ? std::vector<std::string>{"it cites no atom"}
                   : std::vector<std::string>{};
    };

    const Outcome outcome = unwrap(army.execute(std::move(write)));
    ASSERT_EQ(outcome.answers.size(), 1U);
    EXPECT_EQ(outcome.answers.front().value, json::Json("Water boils at 100 degrees [A1]."));
    const std::vector<llm::ChatRequest> sent = model.requests_for("m1");
    ASSERT_EQ(sent.size(), 2U);
    EXPECT_FALSE(sent[0].json_schema.has_value());
    EXPECT_THAT(sent[1].messages[3].text(), HasSubstr("it cites no atom"));
    EXPECT_THAT(sent[1].messages[3].text(), HasSubstr("Reply with the corrected answer only"));
}

TEST(Army, ProseCutShortOrEmptyIsAProblem) {
    const FakeClock clock;
    const KeyedProvider model{[](const llm::ChatRequest& request) {
        if (request.tag.mission_id == "cut") {
            return reply(request, "Water boils at", llm::FinishReason::length);
        }
        return reply(request, "  ");
    }};
    Army army = make_army(model, clock);
    Mission cut = mission("cut", "commander");
    cut.schema.reset();
    cut.escalation = Escalation{.thinking = false, .commander = false, .other_family = false};
    cut.repairs = 0;
    Mission empty = cut;
    empty.id = "empty";

    const std::vector<Outcome> outcomes = unwrap(army.squad({std::move(cut), std::move(empty)}));
    EXPECT_EQ(outcomes[0].verdict, Verdict::invalid);
    EXPECT_THAT(outcomes[0].problems, ::testing::Contains(HasSubstr("cut off")));
    EXPECT_THAT(outcomes[1].problems, ::testing::Contains("the answer is empty"));
}

TEST(Army, ACheckThatThrowsFailsItsMission) {
    const FakeClock clock;
    const KeyedProvider model{
        [](const llm::ChatRequest& request) { return reply(request, std::string{kClaim}); }};
    Army army = make_army(model, clock);
    Mission buggy = mission("m1");
    buggy.check = [](const json::Json&) -> std::vector<std::string> { throw std::runtime_error("a bug"); };

    const Result<Outcome> outcome = army.execute(std::move(buggy));
    EXPECT_ERRC(outcome, Errc::internal);
    EXPECT_THAT(outcome.error().message(), HasSubstr("a bug"));
}

// --- Squads --------------------------------------------------------------------

TEST(Army, OutcomesComeBackInTheirMissionsOrder) {
    const FakeClock clock;
    const KeyedProvider model{[](const llm::ChatRequest& request) {
        // The first missions take longest, so they finish last.
        const int index = std::stoi(request.tag.mission_id.substr(1));
        std::this_thread::sleep_for(std::chrono::milliseconds{12 - index});
        return reply(request, std::string{kClaim});
    }};
    Army army = make_army(model, clock);

    const std::vector<Outcome> outcomes = unwrap(army.squad(missions(12)));
    ASSERT_EQ(outcomes.size(), 12U);
    for (std::size_t index = 0; index < outcomes.size(); ++index) {
        EXPECT_EQ(outcomes[index].mission, std::format("m{}", index));
        EXPECT_EQ(outcomes[index].verdict, Verdict::accepted);
    }
}

TEST(Army, NeverSendsAnEngineMoreThanItsSlots) {
    const FakeClock clock;
    KeyedProvider model{[](const llm::ChatRequest& request) { return reply(request, std::string{kClaim}); }};
    Army army = make_army(model, clock, nullptr, {commander(), infantry(3)});
    model.hold("gemma");

    std::optional<Result<std::vector<Outcome>>> outcomes;
    std::jthread squad{[&] { outcomes.emplace(army.squad(missions(10))); }};
    {
        const Release release{model, "gemma"};
        EXPECT_TRUE(
            model.wait_until([](const KeyedProvider::State& seen) { return seen.flying("gemma") >= 3; }));
        EXPECT_FALSE(model.wait_until(
            [](const KeyedProvider::State& seen) { return seen.flying("gemma") > 3; }, 100ms))
            << "three slots, three calls";
        EXPECT_TRUE(eventually([&] { return army.waiting() == 7; })) << "the rest wait for a worker";
    }
    squad.join();

    ASSERT_TRUE(outcomes.has_value());
    EXPECT_EQ(unwrap(std::move(*outcomes)).size(), 10U);
    EXPECT_EQ(model.state().peak.at("gemma"), 3);
    EXPECT_EQ(army.waiting(), 0U);
}

TEST(Army, ASharedPoolRunsOnlyWhatItsTokensHold) {
    // Four slots, but 2500 tokens: each mission reserves its prompt and an
    // answer of up to 1000, so two run at once, never three.
    const FakeClock clock;
    KeyedProvider model{[](const llm::ChatRequest& request) { return reply(request, std::string{kClaim}); }};
    Army army = make_army(model, clock, nullptr, {commander(4, 2500), infantry()});
    model.hold("qwen");
    std::vector<Mission> four = missions(4, "commander");
    for (Mission& each : four) {
        each.answer_tokens = 1000;
    }

    std::optional<Result<std::vector<Outcome>>> outcomes;
    std::jthread squad{[&] { outcomes.emplace(army.squad(std::move(four))); }};
    {
        const Release release{model, "qwen"};
        EXPECT_TRUE(
            model.wait_until([](const KeyedProvider::State& seen) { return seen.flying("qwen") >= 2; }));
        EXPECT_FALSE(model.wait_until(
            [](const KeyedProvider::State& seen) { return seen.flying("qwen") > 2; }, 100ms));
    }
    squad.join();

    ASSERT_TRUE(outcomes.has_value());
    EXPECT_EQ(unwrap(std::move(*outcomes)).size(), 4U);
    EXPECT_EQ(model.state().peak.at("qwen"), 2);
}

TEST(Army, EachAnswerTakesASlot) {
    // Four slots; each mission asks for three answers at once. The second
    // waits for three free slots, and nobody overtakes it.
    const FakeClock clock;
    KeyedProvider model{[](const llm::ChatRequest& request) {
        return reply(request, [](int) { return std::string{kClaim}; });
    }};
    Army army = make_army(model, clock, nullptr, {commander(), infantry(4)});
    model.hold("gemma");
    std::vector<Mission> threes = missions(4);
    for (Mission& each : threes) {
        each.samples = 3;
    }

    std::optional<Result<std::vector<Outcome>>> outcomes;
    std::jthread squad{[&] { outcomes.emplace(army.squad(std::move(threes))); }};
    {
        const Release release{model, "gemma"};
        EXPECT_TRUE(
            model.wait_until([](const KeyedProvider::State& seen) { return seen.flying("gemma") >= 3; }));
        EXPECT_FALSE(model.wait_until(
            [](const KeyedProvider::State& seen) { return seen.flying("gemma") > 4; }, 100ms))
            << "three answers hold three of the four slots";
    }
    squad.join();

    ASSERT_TRUE(outcomes.has_value());
    EXPECT_EQ(unwrap(std::move(*outcomes)).size(), 4U);
    EXPECT_EQ(model.state().peak.at("gemma"), 3);
}

TEST(Army, ASharedPoolReservesRoomForEveryAnswer) {
    // 2500 tokens. `two` asks for two answers of up to 1000 at once, and so
    // holds about 2040 tokens; `one` holds about 1040. Whichever reaches the
    // pool first, the other waits: together they would need about 3080.
    const FakeClock clock;
    KeyedProvider model{[](const llm::ChatRequest& request) {
        return reply(request, [](int) { return std::string{kClaim}; });
    }};
    Army army = make_army(model, clock, nullptr, {commander(4, 2500), infantry()});
    model.hold("qwen");
    Mission two = mission("two", "commander");
    two.samples = 2;
    two.answer_tokens = 1000;
    Mission one = mission("one", "commander");
    one.answer_tokens = 1000;

    std::optional<Result<std::vector<Outcome>>> outcomes;
    std::jthread squad{[&] { outcomes.emplace(army.squad({std::move(two), std::move(one)})); }};
    {
        const Release release{model, "qwen"};
        EXPECT_TRUE(
            model.wait_until([](const KeyedProvider::State& seen) { return seen.flying("qwen") >= 1; }));
        EXPECT_FALSE(model.wait_until(
            [](const KeyedProvider::State& seen) { return seen.flying("qwen") >= 3; }, 100ms))
            << "never both at once";
    }
    squad.join();
    ASSERT_TRUE(outcomes.has_value());
    EXPECT_OK(*outcomes);
}

TEST(Army, EscalationsNeverHoldUpTheInfantry) {
    // One infantry slot. Mission a fails there and climbs to the commander,
    // which is busy; mission b is served by the infantry meanwhile.
    const FakeClock clock;
    KeyedProvider model{[](const llm::ChatRequest& request) {
        const bool fails = request.tag.mission_id == "a" && request.model == "gemma";
        return reply(request, std::string{fails ? kMaybe : kClaim});
    }};
    Army army = make_army(model, clock, nullptr, {commander(1), infantry(1)});
    model.hold("qwen");

    std::optional<Result<std::vector<Outcome>>> outcomes;
    std::jthread squad{[&] { outcomes.emplace(army.squad({mission("a"), mission("b")})); }};
    {
        const Release release{model, "qwen"};
        EXPECT_TRUE(model.wait_until([](const KeyedProvider::State& seen) {
            return seen.flying("qwen") == 1 && seen.asked("b") == 1 && seen.flying("gemma") == 0;
        })) << "b was answered while a waited for the commander";
    }
    squad.join();

    ASSERT_TRUE(outcomes.has_value());
    const std::vector<Outcome> done = unwrap(std::move(*outcomes));
    EXPECT_EQ(done[0].step, Step::commander);
    EXPECT_EQ(done[1].step, Step::as_ordered);
}

TEST(Army, AHigherPriorityGoesFirst) {
    const FakeClock clock;
    KeyedProvider model{[](const llm::ChatRequest& request) { return reply(request, std::string{kClaim}); }};
    Army army = make_army(model, clock, nullptr, {commander(), infantry(1)});
    model.hold("gemma");

    // The only worker is busy with `first` while the others queue.
    std::jthread busy{[&] { ASSERT_OK(army.execute(mission("first"))); }};
    std::vector<Mission> queued{mission("low"), mission("also-low"), mission("high")};
    queued[2].priority = Priority{10};
    std::optional<std::jthread> squad;
    {
        const Release release{model, "gemma"};
        EXPECT_TRUE(
            model.wait_until([](const KeyedProvider::State& seen) { return seen.flying("gemma") == 1; }));
        squad.emplace([&] { ASSERT_OK(army.squad(std::move(queued))); });
        EXPECT_TRUE(eventually([&] { return army.waiting() == 3; }));
    }
    busy.join();
    squad->join();

    std::vector<std::string> order;
    for (const llm::ChatRequest& sent : model.state().requests) {
        order.push_back(sent.tag.mission_id);
    }
    EXPECT_EQ(order, (std::vector<std::string>{"first", "high", "low", "also-low"}));
}

TEST(Army, TheFirstFailureStopsTheSquad) {
    const FakeClock clock;
    MemoryLog log;
    KeyedProvider model{[](const llm::ChatRequest& request) -> Result<llm::ChatResponse> {
        if (request.model == "qwen") {
            return fail(Errc::unavailable, "the pod stopped");
        }
        return reply(request, std::string{kClaim});
    }};
    Army army = make_army(model, clock, &log, {commander(1), infantry(2)});
    model.hold("gemma");  // the infantry's calls are in flight when the commander fails
    std::vector<Mission> five = missions(4);
    five.push_back(mission("bad", "commander"));

    std::optional<Result<std::vector<Outcome>>> outcomes;
    std::atomic<bool> done{false};
    std::jthread squad{[&] {
        outcomes.emplace(army.squad(std::move(five)));
        done = true;
    }};
    {
        const Release release{model, "gemma"};  // only if the stop failed to end them
        EXPECT_TRUE(eventually([&] { return done.load(); })) << "the failure stopped the squad's held calls";
    }
    squad.join();
    ASSERT_TRUE(outcomes.has_value());
    EXPECT_ERRC(*outcomes, Errc::unavailable);
    EXPECT_THAT(outcomes->error().describe(), HasSubstr("while running mission 'bad' on 'qwen'"));
    EXPECT_EQ(log.size(), 5U) << "every mission is logged, however it ended";
    EXPECT_EQ(log.at("bad").error->code(), Errc::unavailable);
    for (int index = 0; index < 4; ++index) {
        const MissionRecord stopped = log.at(std::format("m{}", index));
        ASSERT_TRUE(stopped.error.has_value());
        EXPECT_EQ(stopped.error->code(), Errc::cancelled);
        EXPECT_FALSE(stopped.verdict.has_value());
    }
    // m2 and m3 waited for the two infantry workers, freed only by the stop.
    EXPECT_TRUE(model.requests_for("m2").empty()) << "nothing starts once the squad has stopped";
    EXPECT_TRUE(model.requests_for("m3").empty());
}

TEST(Army, StoppingTheJobStopsItsMissions) {
    const FakeClock clock;
    KeyedProvider model{[](const llm::ChatRequest& request) { return reply(request, std::string{kClaim}); }};
    Army army = make_army(model, clock);
    model.hold("gemma");
    std::stop_source stop;

    std::optional<Result<std::vector<Outcome>>> outcomes;
    std::atomic<bool> done{false};
    std::jthread squad{[&] {
        outcomes.emplace(army.squad(missions(6), stop.get_token()));
        done = true;
    }};
    {
        const Release release{model, "gemma"};  // only if the stop failed to end them
        EXPECT_TRUE(
            model.wait_until([](const KeyedProvider::State& seen) { return seen.flying("gemma") > 0; }));
        stop.request_stop();
        EXPECT_TRUE(eventually([&] { return done.load(); })) << "the job's stop reached its missions";
    }
    squad.join();

    ASSERT_TRUE(outcomes.has_value());
    EXPECT_ERRC(*outcomes, Errc::cancelled);
}

// --- Ceilings ------------------------------------------------------------------

TEST(Army, ATokenCeilingStopsNewCalls) {
    const FakeClock clock;
    MemoryLog log;
    const KeyedProvider model{
        [](const llm::ChatRequest& request) { return reply(request, std::string{kClaim}); }};
    // One infantry slot: the missions run one after another, five tokens each.
    Army army = make_army(model, clock, &log, {commander(), infantry(1)}, Ceilings{.completion_tokens = 10});

    const std::vector<Outcome> outcomes = unwrap(army.squad(missions(4)));
    EXPECT_EQ(outcomes[0].verdict, Verdict::accepted);
    EXPECT_EQ(outcomes[1].verdict, Verdict::accepted);
    EXPECT_EQ(outcomes[2].verdict, Verdict::ceiling);
    EXPECT_EQ(outcomes[2].calls, 0);
    EXPECT_EQ(outcomes[2].rungs, 0) << "no rung was climbed";
    EXPECT_EQ(outcomes[3].verdict, Verdict::ceiling);

    const std::optional<CeilingHit> hit = army.ceiling();
    ASSERT_TRUE(hit.has_value());
    EXPECT_EQ(hit->kind, CeilingKind::completion_tokens);
    EXPECT_EQ(hit->completion_tokens, 10);
    EXPECT_EQ(hit->mission, "m2");
    EXPECT_EQ(army.spent().calls, 2);

    const MissionRecord stopped = log.at("m2");
    EXPECT_EQ(stopped.verdict, Verdict::ceiling);
    EXPECT_EQ(stopped.rungs, 0);
    EXPECT_FALSE(stopped.step.has_value()) << "no rung was climbed, so none is named";
    EXPECT_EQ(stopped.calls, 0);
}

TEST(Army, TheWallClockCeilingCountsTheJobsEarlierTime) {
    FakeClock clock;
    const KeyedProvider model{[&](const llm::ChatRequest& request) {
        clock.advance(2s);
        return reply(request, std::string{kClaim});
    }};
    // A resumed job that had run for 59 seconds, with a minute to spend.
    Army army =
        make_army(model, clock, nullptr, {commander(), infantry(1)}, Ceilings{.wall_clock = 60s}, 59s);

    const std::vector<Outcome> outcomes = unwrap(army.squad(missions(2)));
    EXPECT_EQ(outcomes[0].verdict, Verdict::accepted);
    EXPECT_EQ(outcomes[1].verdict, Verdict::ceiling);
    const std::optional<CeilingHit> hit = army.ceiling();
    ASSERT_TRUE(hit.has_value());
    EXPECT_EQ(hit->kind, CeilingKind::wall_clock);
    EXPECT_EQ(hit->elapsed, 61s);
    EXPECT_EQ(army.spent().elapsed, 61s);
}

TEST(Army, ACeilingStopsARepair) {
    const FakeClock clock;
    const KeyedProvider model{
        [](const llm::ChatRequest& request) { return reply(request, std::string{kMaybe}); }};
    Army army = make_army(model, clock, nullptr, {commander(), infantry()}, Ceilings{.completion_tokens = 5});

    const Outcome outcome = unwrap(army.execute(mission("m1")));
    EXPECT_EQ(outcome.verdict, Verdict::ceiling);
    EXPECT_EQ(outcome.calls, 1) << "the answer's five tokens reached the ceiling: no repair";
    EXPECT_FALSE(outcome.problems.empty());
}

TEST(Army, ACeilingMidwayKeepsWhatWasAnswered) {
    const FakeClock clock;
    const KeyedProvider model{[](const llm::ChatRequest& request) {
        return reply(request, [](int) { return std::string{kClaim}; });
    }};
    // One slot: three samples, one request each, five tokens apiece.
    Army army =
        make_army(model, clock, nullptr, {commander(), infantry(1)}, Ceilings{.completion_tokens = 10});
    Mission three = mission("m1");
    three.samples = 3;

    const Outcome outcome = unwrap(army.execute(std::move(three)));
    EXPECT_EQ(outcome.verdict, Verdict::ceiling);
    EXPECT_EQ(outcome.answers.size(), 2U);
    EXPECT_EQ(outcome.calls, 2);
}

// --- The battle log --------------------------------------------------------------

TEST(Army, TheBattleLogKeepsEveryMission) {
    FakeClock clock;
    MemoryLog log;
    const KeyedProvider model{[&](const llm::ChatRequest& request) {
        clock.advance(1s);
        const bool fails = request.model == "gemma" && request.tag.mission_id != "plain";
        return reply(request, std::string{fails ? kMaybe : kClaim});
    }};
    Army army = make_army(model, clock, &log, {commander(), infantry(1)});
    Mission stuck = mission("stuck");
    stuck.escalation = Escalation{.thinking = false, .commander = false, .other_family = false};

    ASSERT_OK(army.squad({mission("plain"), mission("rescued"), std::move(stuck)}));
    ASSERT_EQ(log.size(), 3U);

    const MissionRecord plain = log.at("plain");
    EXPECT_EQ(plain.job_id, "job-1");
    EXPECT_EQ(plain.kind, "label");
    EXPECT_EQ(plain.prompt, "label@1");
    EXPECT_EQ(plain.role, "infantry");
    EXPECT_EQ(plain.verdict, Verdict::accepted);
    EXPECT_FALSE(plain.error.has_value());
    EXPECT_EQ(plain.step, Step::as_ordered);
    EXPECT_EQ(plain.rungs, 1);
    EXPECT_EQ(plain.answered_by, "gemma");
    EXPECT_EQ(plain.samples, 1);
    EXPECT_EQ(plain.accepted, 1);
    EXPECT_EQ(plain.calls, 1);
    EXPECT_EQ(plain.usage, (llm::Usage{.prompt_tokens = 20, .completion_tokens = 5}));
    EXPECT_EQ(plain.duration, 1s);
    EXPECT_TRUE(plain.problems.empty());

    const MissionRecord rescued = log.at("rescued");
    EXPECT_EQ(rescued.step, Step::commander);
    EXPECT_EQ(rescued.rungs, 3);
    EXPECT_EQ(rescued.answered_by, "qwen");
    EXPECT_EQ(rescued.calls, 5);

    const MissionRecord lost = log.at("stuck");
    EXPECT_EQ(lost.verdict, Verdict::invalid);
    EXPECT_EQ(lost.accepted, 0);
    EXPECT_EQ(lost.answered_by, "");
    EXPECT_FALSE(lost.problems.empty());
}

TEST(Army, ABattleLogThatFailsNeverFailsAMission) {
    class BrokenLog final : public BattleLog {
    public:
        [[nodiscard]] Result<void> record(const MissionRecord& /*mission*/) override {
            return fail(Errc::io, "the disk is full");
        }
    };

    auto memory = std::make_unique<log::MemorySink>();
    const log::MemorySink* const lines = memory.get();
    std::vector<std::unique_ptr<log::Sink>> sinks;
    sinks.push_back(std::move(memory));
    log::set_sinks(std::move(sinks));
    {
        const FakeClock clock;
        BrokenLog broken;
        const KeyedProvider model{
            [](const llm::ChatRequest& request) { return reply(request, std::string{kClaim}); }};
        Army army = make_army(model, clock, &broken);
        EXPECT_EQ(unwrap(army.execute(mission("m1"))).verdict, Verdict::accepted);
    }
    const bool logged = std::ranges::any_of(lines->entries(), [](const log::MemorySink::Entry& entry) {
        return entry.message.find("was not written to the battle log") != std::string::npos;
    });
    log::set_sinks({});
    EXPECT_TRUE(logged);
}

TEST(Army, CountsWhatTheJobSpent) {
    FakeClock clock;
    const KeyedProvider model{[&](const llm::ChatRequest& request) {
        clock.advance(3s);
        return reply(request, std::string{request.messages.size() == 2 ? kMaybe : kClaim});
    }};
    Army army = make_army(model, clock, nullptr, {commander(), infantry(1)});
    ASSERT_OK(army.squad(missions(2)));

    const Army::Spent spent = army.spent();
    EXPECT_EQ(spent.calls, 4) << "two answers and their repairs";
    EXPECT_EQ(spent.usage, (llm::Usage{.prompt_tokens = 80, .completion_tokens = 20}));
    EXPECT_EQ(spent.elapsed, 12s);
}

// --- What the army refuses --------------------------------------------------------

TEST(Army, RefusesMissionsItCannotRun) {
    const FakeClock clock;
    const KeyedProvider model{
        [](const llm::ChatRequest& request) { return reply(request, std::string{kClaim}); }};
    Army army = make_army(model, clock);

    const std::vector<std::pair<std::string, std::function<void(Mission&)>>> broken{
        {"no id", [](Mission& m) { m.id.clear(); }},
        {"no kind", [](Mission& m) { m.kind.clear(); }},
        {"no engine for the role", [](Mission& m) { m.role = "cavalry"; }},
        {"no messages", [](Mission& m) { m.request.messages.clear(); }},
        {"a model", [](Mission& m) { m.request.model = "gemma"; }},
        {"a max_tokens", [](Mission& m) { m.request.max_tokens = 100; }},
        {"an n", [](Mission& m) { m.request.n = 2; }},
        {"a schema in the request", [](Mission& m) { m.request.json_schema = "{}"; }},
        {"a tag", [](Mission& m) { m.request.tag.purpose = "other"; }},
        {"no answer tokens", [](Mission& m) { m.answer_tokens = 0; }},
        {"no samples", [](Mission& m) { m.samples = 0; }},
        {"negative repairs", [](Mission& m) { m.repairs = -1; }},
        {"no seed", [](Mission& m) { m.request.seed.reset(); }},
        {"a penalty", [](Mission& m) { m.request.presence_penalty = 1.5; }},
        {"a bad temperature", [](Mission& m) { m.request.temperature = -1.0; }},
        {"greedy samples",
         [](Mission& m) {
             m.request.temperature = 0.0;
             m.samples = 2;
         }},
        {"too long for a slot", [](Mission& m) { m.answer_tokens = 8192; }},
    };
    for (const auto& [why, breaks] : broken) {
        Mission bad = mission("m1");
        breaks(bad);
        EXPECT_ERRC(army.execute(std::move(bad)), Errc::invalid_argument) << why;
    }

    Mission long_one = mission("m1");
    long_one.answer_tokens = 8192;
    EXPECT_THAT(army.execute(std::move(long_one)).error().message(), HasSubstr("split its packet"));

    Mission picture = mission("m1");
    picture.request.messages.back().content.push_back(
        llm::ContentPart{.kind = llm::ContentPart::Kind::image_url, .value = "data:image/png;base64,AA=="});
    EXPECT_ERRC(army.execute(std::move(picture)), Errc::unsupported);

    EXPECT_ERRC(army.squad({mission("twin"), mission("twin")}), Errc::invalid_argument);
    EXPECT_TRUE(model.state().requests.empty()) << "a squad with one bad mission does not start";
    EXPECT_TRUE(unwrap(army.squad({})).empty());
}

TEST(Army, RefusesEnginesItCannotUse) {
    FakeClock clock;
    const KeyedProvider model{
        [](const llm::ChatRequest& request) { return reply(request, std::string{kClaim}); }};
    MemoryLog log;
    const auto refused = [&](std::vector<Engine> engines, BattleLog* with = nullptr, Ceilings ceilings = {},
                             std::string job = "job-1") {
        const Result<Army> army = Army::create(
            Army::Options{.engines = std::move(engines), .job_id = std::move(job), .ceilings = ceilings},
            model, clock, with);
        return !army.has_value() && army.error().code() == Errc::invalid_argument;
    };
    const auto changed = [](const std::function<void(Engine&)>& change) {
        Engine engine = infantry();
        change(engine);
        return std::vector<Engine>{commander(), engine};
    };

    EXPECT_TRUE(refused({}));
    EXPECT_TRUE(refused(changed([](Engine& e) { e.model.clear(); })));
    EXPECT_TRUE(refused(changed([](Engine& e) { e.model = "qwen"; }))) << "listed twice";
    EXPECT_TRUE(refused(changed([](Engine& e) { e.roles.clear(); })));
    EXPECT_TRUE(refused(changed([](Engine& e) { e.roles = {""}; })));
    EXPECT_TRUE(refused(changed([](Engine& e) { e.roles = {"commander"}; }))) << "a role served twice";
    EXPECT_TRUE(refused(changed([](Engine& e) { e.family.clear(); })));
    EXPECT_TRUE(refused(changed([](Engine& e) { e.slots = 0; })));
    EXPECT_TRUE(refused(changed([](Engine& e) { e.ctx_per_slot = 0; })));
    EXPECT_TRUE(refused(changed([](Engine& e) { e.context = e.ctx_per_slot - 1; })));
    EXPECT_TRUE(refused(changed([](Engine& e) { e.direct.top_p = 0.0; })));
    EXPECT_TRUE(refused(changed([](Engine& e) { e.direct = thinking(); }))) << "a direct preset that thinks";
    EXPECT_TRUE(refused(changed([](Engine& e) { e.thinking.temperature = -1.0; })));
    EXPECT_TRUE(refused(changed([](Engine& e) { e.thinking.thinking_budget.reset(); })))
        << "a thinking without end";
    EXPECT_TRUE(refused(changed([](Engine& e) { e.thinking = direct(); })));
    EXPECT_TRUE(refused({commander(), infantry()}, &log, {}, "")) << "a battle log needs a job";
    EXPECT_TRUE(refused({commander(), infantry()}, nullptr, Ceilings{.completion_tokens = 0}));
    EXPECT_TRUE(refused({commander(), infantry()}, nullptr, Ceilings{.wall_clock = 0ms}));
    EXPECT_OK(Army::create(Army::Options{.engines = {commander(), infantry()}}, model, clock));
}

}  // namespace
}  // namespace rr::agents
