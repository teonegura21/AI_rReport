#include <rr/oracle/oracle.hpp>

#include <format>
#include <memory>
#include <regex>
#include <string>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <rr/agents/army.hpp>
#include <rr/core/clock.hpp>
#include <rr/json/json.hpp>
#include <rr/oracle/answers.hpp>
#include <rr/testkit/result.hpp>

namespace rr::oracle {
namespace {

using ::testing::HasSubstr;
using testkit::unwrap;

llm::ChatRequest asked(std::string kind, std::string mission = "m1", std::string model = "gemma") {
    llm::ChatRequest request{.model = std::move(model),
                             .messages = {llm::Message::user("S1. Water boils at 100 °C.")}};
    request.tag = llm::CallTag{.purpose = std::move(kind), .job_id = "J1", .mission_id = std::move(mission)};
    return request;
}

llm::Choice answer(std::string content) {
    return llm::Choice{.content = std::move(content), .finish = llm::FinishReason::stop};
}

// --- The oracle ------------------------------------------------------------------

TEST(Oracle, AnswersTheKindsItWasTaught) {
    Oracle oracle;
    oracle.teach("label",
                 [](const llm::ChatRequest&) -> Result<std::string> { return R"({"label": "claim"})"; });

    llm::ChatRequest three = asked("label");
    three.n = 3;
    const llm::ChatResponse response = unwrap(oracle.chat(three, {}, {}));
    ASSERT_EQ(response.choices.size(), 3U) << "one right answer, as many times as asked";
    EXPECT_EQ(response.choices[2].content, R"({"label": "claim"})");
    EXPECT_EQ(response.choices[2].finish, llm::FinishReason::stop);
    EXPECT_EQ(response.usage.prompt_tokens, 7) << "four characters a token, rounded up";
    EXPECT_EQ(response.usage.completion_tokens, 3 * 5);

    const Result<llm::ChatResponse> unknown = oracle.chat(asked("outline"), {}, {});
    EXPECT_ERRC(unknown, Errc::unsupported) << "the oracle never guesses";
    EXPECT_THAT(unknown.error().message(), HasSubstr("'outline'"));

    const std::stop_source stopped;
    stopped.request_stop();
    EXPECT_ERRC(oracle.chat(asked("label"), {}, stopped.get_token()), Errc::cancelled);
}

// --- Each fault ------------------------------------------------------------------

// A punctuate mission's request for `excerpt`, as the prompt renders it.
llm::ChatRequest punctuate(std::string_view excerpt) {
    llm::ChatRequest request = asked("punctuate");
    request.messages = {
        llm::Message::system("You restore the punctuation."),
        llm::Message::user(std::format("The excerpt:\n<excerpt>\n{}\n</excerpt>\n", excerpt))};
    return request;
}

TEST(OracleAnswers, PunctuateFromTheScriptTheCaptionsWereMadeFrom) {
    Oracle oracle;
    teach_stages(oracle, "So, Marie Curie won two Nobel prizes. She called it radium!");
    const llm::ChatResponse answer = unwrap(oracle.chat(punctuate("marie curie won two nobel"), {}, {}));
    EXPECT_EQ(answer.first().content, R"({"text":"Marie Curie won two Nobel"})")
        << "the script's text for them";
    llm::ChatRequest repair = punctuate("she called it radium");
    repair.messages.push_back(llm::Message::assistant(R"({"text":"she called it radium"})"));
    repair.messages.push_back(llm::Message::user("Your answer cannot be used."));
    EXPECT_EQ(unwrap(oracle.chat(repair, {}, {})).first().content, R"({"text":"She called it radium!"})")
        << "a repair is answered from the order's excerpt";
    EXPECT_ERRC(oracle.chat(punctuate("pierre curie"), {}, {}), Errc::invalid_argument) << "never a guess";
}

TEST(OracleAnswers, WithoutAScriptASentenceEndsEveryTwelveWords) {
    Oracle oracle;
    teach_stages(oracle);
    const std::string excerpt = "a b c d e f g h i j k l m n";
    EXPECT_EQ(unwrap(oracle.chat(punctuate(excerpt), {}, {})).first().content,
              R"({"text":"A b c d e f g h i j k l. M n."})");
    EXPECT_ERRC(oracle.chat(asked("punctuate"), {}, {}), Errc::invalid_argument) << "no excerpt to punctuate";
}

// An annotate mission's request for `sentences`, as its prompt renders them,
// with the other reader's notes when there are some.
llm::ChatRequest annotating(std::string kind, std::string_view sentences, std::string_view found = "") {
    llm::ChatRequest request = asked(std::move(kind));
    std::string task = std::format("The sentences to annotate:\n<sentences>\n{}\n</sentences>\n", sentences);
    if (!found.empty()) {
        task += std::format("\nThe other reader's notes on them:\n<found>\n{}\n</found>\n", found);
    }
    request.messages = {llm::Message::system("You read part of a video's transcript."),
                        llm::Message::user(task)};
    return request;
}

TEST(OracleAnswers, AnnotateNotesEachSentenceOfFiveWordsOrMore) {
    Oracle oracle;
    teach_stages(oracle);
    const std::string_view sentences =
        "S1 [0:00] Welcome back.\nS2 [0:02] Marie Curie measured 7 samples in 1902.\nS3 [0:06] The samples "
        "glowed in the dark.\nS4 [0:09] That took months.";
    const json::Json expected = unwrap(json::parse(R"({"facts": [
        {"sentences": ["S2"], "quote": "Marie Curie measured 7 samples in 1902.",
         "claim": "Marie Curie measured 7 samples in 1902.", "importance": "core"},
        {"sentences": ["S3"], "quote": "The samples glowed in the dark.",
         "claim": "The samples glowed in the dark.", "importance": "detail"}],
        "must_quotes": [], "terms": []})"));
    EXPECT_EQ(
        unwrap(json::parse(unwrap(oracle.chat(annotating("annotate", sentences), {}, {})).first().content)),
        expected)
        << "each sentence of five words or more, whole, core when it holds a number";

    llm::ChatRequest repair = annotating("annotate", sentences);
    repair.messages.push_back(llm::Message::assistant(R"({"facts": []})"));
    repair.messages.push_back(llm::Message::user("Your answer cannot be used."));
    EXPECT_EQ(unwrap(json::parse(unwrap(oracle.chat(repair, {}, {})).first().content)), expected)
        << "a repair is answered from the order's sentences";
    EXPECT_ERRC(oracle.chat(asked("annotate"), {}, {}), Errc::invalid_argument) << "no sentences to annotate";
}

TEST(OracleAnswers, AnnotateMoreAddsTheSentencesTheNotesDoNotCite) {
    Oracle oracle;
    teach_stages(oracle);
    const llm::ChatRequest request = annotating("annotate_more",
                                                "S2 [0:02] Marie Curie measured 7 samples in 1902.\n"
                                                "S3 [0:06] The samples glowed in the dark.\n"
                                                "S4 [0:09] That took months.\nS5 [0:11] Thank you.\n"
                                                "S12 [0:30] It glowed for years.",
                                                "- [S1, S2] Marie Curie measured seven samples.\n"
                                                "- [S3, S7] They glowed.\n- [S120] Something later.");
    EXPECT_EQ(unwrap(json::parse(unwrap(oracle.chat(request, {}, {})).first().content)),
              unwrap(json::parse(R"({"facts": [
        {"sentences": ["S4"], "quote": "That took months.", "claim": "That took months.", "importance": "detail"},
        {"sentences": ["S12"], "quote": "It glowed for years.", "claim": "It glowed for years.",
         "importance": "detail"}],
        "must_quotes": [], "terms": []})")))
        << "three words or more, cited by no note, last or not in its list: S12 is not S120";
    EXPECT_ERRC(oracle.chat(annotating("annotate_more", "S3 [0:06] That took months."), {}, {}),
                Errc::invalid_argument)
        << "no notes to add to";
}

TEST(RequestLog, KeepsEveryRequestItPassesOn) {
    Oracle oracle;
    oracle.teach("label",
                 [](const llm::ChatRequest&) -> Result<std::string> { return R"({"label": "claim"})"; });
    const RequestLog log{oracle};
    EXPECT_EQ(log.name(), "oracle");
    EXPECT_TRUE(log.caps().thinking);
    EXPECT_EQ(unwrap(log.chat(asked("label", "m1"), {}, {})).first().content, R"({"label": "claim"})");
    EXPECT_ERRC(log.chat(asked("outline", "m2"), {}, {}), Errc::unsupported);
    const std::vector<llm::ChatRequest> requests = log.requests();
    ASSERT_EQ(requests.size(), 2U) << "answered or not";
    EXPECT_EQ(requests[0].tag.mission_id, "m1");
    EXPECT_EQ(requests[1].tag.mission_id, "m2");
}

TEST(Faults, EachMakesItsMistake) {
    const auto made = [](const Fault& fault, std::string content) {
        llm::Choice choice = answer(std::move(content));
        EXPECT_TRUE(fault.change(choice)) << fault.name;
        return choice;
    };

    EXPECT_EQ(made(refusal({}), "anything").content, "I'm sorry, but I can't help with that.");

    const llm::Choice cut = made(truncation({}), "Water boils.");
    EXPECT_EQ(cut.content, "Water ");
    EXPECT_EQ(cut.finish, llm::FinishReason::length);
    EXPECT_EQ(made(truncation({}), "aéé").content, "a") << "never inside a UTF-8 sequence";

    EXPECT_EQ(made(broken_json({}), R"({"atoms": [1]})").content, R"({"atoms": [1])");

    const llm::Choice thought = made(out_while_thinking({}), "an answer");
    EXPECT_TRUE(thought.content.empty());
    EXPECT_FALSE(thought.reasoning.empty());
    EXPECT_EQ(thought.finish, llm::FinishReason::length);

    EXPECT_EQ(made(wrong_number({}), "Water boils at 100 °C, 212 °F.").content,
              "Water boils at 101 °C, 212 °F.");
    EXPECT_EQ(made(wrong_number({}), "99999999999999999999 atoms").content, "999999999999999999991 atoms")
        << "too long to add to";
    EXPECT_EQ(made(bad_marker({}), "Water boils [A3], and freezes [A4].").content,
              "Water boils [A999], and freezes [A4].");

    EXPECT_EQ(made(dropped_reference({}), R"({"sentences": ["S1", "S2"]})").content,
              R"({"sentences": ["S1"]})");
    EXPECT_EQ(made(dropped_reference({}), R"({"sentences": ["S1"], "a": 1})").content,
              R"({"sentences": [], "a": 1})");

    const Fault softened = edit("softened", {}, [](const std::string& text) {
        return std::regex_replace(text, std::regex{"boils"}, "may boil");
    });
    EXPECT_EQ(made(softened, "Water boils.").content, "Water may boil.");
}

TEST(Faults, SayWhenThereWasNothingToChange) {
    llm::Choice plain = answer("no digits, no markers");
    EXPECT_FALSE(wrong_number({}).change(plain));
    EXPECT_FALSE(bad_marker({}).change(plain));
    EXPECT_FALSE(dropped_reference({}).change(plain));
    EXPECT_FALSE(broken_json({}).change(plain));
    EXPECT_FALSE(edit("noop", {}, [](std::string text) { return text; }).change(plain));
    EXPECT_EQ(plain.content, "no digits, no markers");
}

// --- Which calls a fault hits ---------------------------------------------------------

TEST(Faults, HitTheCallsTheyAreAimedAt) {
    Oracle oracle;
    oracle.teach("label",
                 [](const llm::ChatRequest&) -> Result<std::string> { return R"({"label": "claim"})"; });
    oracle.teach("judge", [](const llm::ChatRequest&) -> Result<std::string> { return R"({"verdict": 1})"; });
    const FaultyProvider faulty{
        oracle,
        {broken_json(When{.kind = "label", .calls = 1}), wrong_number(When{.mission = "j2", .calls = 0})}};

    // The first label call of each mission is broken; the second is not.
    EXPECT_EQ(unwrap(faulty.chat(asked("label", "m1"), {}, {})).choices[0].content, R"({"label": "claim")");
    EXPECT_EQ(unwrap(faulty.chat(asked("label", "m1"), {}, {})).choices[0].content, R"({"label": "claim"})");
    EXPECT_EQ(unwrap(faulty.chat(asked("label", "m2"), {}, {})).choices[0].content, R"({"label": "claim")");
    // Every call of mission j2, and no other.
    EXPECT_EQ(unwrap(faulty.chat(asked("judge", "j1"), {}, {})).choices[0].content, R"({"verdict": 1})");
    EXPECT_EQ(unwrap(faulty.chat(asked("judge", "j2"), {}, {})).choices[0].content, R"({"verdict": 2})");
    EXPECT_EQ(unwrap(faulty.chat(asked("judge", "j2"), {}, {})).choices[0].content, R"({"verdict": 2})");

    const std::vector<Injection> made = faulty.injected();
    ASSERT_EQ(made.size(), 4U);
    EXPECT_EQ(made[0].fault, "broken_json");
    EXPECT_EQ(made[0].mission, "m1");
    EXPECT_TRUE(made[0].changed);
    EXPECT_EQ(made[3].fault, "wrong_number");

    const FaultyProvider by_model{oracle,
                                  {refusal(When{.model = "qwen"}), truncation(When{.thinking = true})}};
    EXPECT_EQ(unwrap(by_model.chat(asked("label"), {}, {})).choices[0].content, R"({"label": "claim"})");
    EXPECT_THAT(unwrap(by_model.chat(asked("label", "m1", "qwen"), {}, {})).choices[0].content,
                HasSubstr("sorry"));
    llm::ChatRequest thinking = asked("label", "m3");
    thinking.thinking = true;
    EXPECT_EQ(unwrap(by_model.chat(thinking, {}, {})).choices[0].finish, llm::FinishReason::length);

    // Where two faults match a call, the first one listed is made.
    const FaultyProvider both{oracle, {refusal(When{.kind = "label"}), truncation(When{.kind = "label"})}};
    EXPECT_THAT(unwrap(both.chat(asked("label", "m9"), {}, {})).choices[0].content, HasSubstr("sorry"));
    ASSERT_EQ(both.injected().size(), 1U);
    EXPECT_EQ(both.injected()[0].fault, "refusal");

    const FaultyProvider down{oracle, {server_error(When{}, Errc::unavailable)}};
    const Result<llm::ChatResponse> failed = down.chat(asked("label"), {}, {});
    EXPECT_ERRC(failed, Errc::unavailable);
    EXPECT_THAT(failed.error().message(), HasSubstr("server_error:unavailable"));
    ASSERT_EQ(down.injected().size(), 1U) << "an error is a fault made, too";
    EXPECT_EQ(down.injected()[0].fault, "server_error:unavailable");
}

// --- The rule: every fault ends repaired, or as a finding ---------------------------------

llm::SamplingPreset direct() {
    return llm::SamplingPreset{.temperature = 0.7, .top_p = 0.8, .top_k = 20};
}

llm::SamplingPreset thinking() {
    return llm::SamplingPreset{
        .temperature = 1.0,
        .top_p = 0.95,
        .top_k = 20,
        .thinking = true,
        .thinking_budget = llm::ThinkingBudget{.tokens = 500, .closing = "Answer now."}};
}

std::vector<agents::Engine> engines() {
    return {agents::Engine{.model = "qwen",
                           .roles = {"commander"},
                           .family = "qwen",
                           .slots = 2,
                           .context = 16384,
                           .ctx_per_slot = 16384,
                           .kv_unified = true,
                           .direct = direct(),
                           .thinking = thinking()},
            agents::Engine{.model = "gemma",
                           .roles = {"infantry"},
                           .family = "gemma",
                           .slots = 2,
                           .context = 16384,
                           .ctx_per_slot = 8192,
                           .direct = direct(),
                           .thinking = thinking()}};
}

// A mission whose right answer is {"boils_at": 100, "sentences": ["S1", "S2"]}:
// the number as the packet says it, and both sentences accounted for.
agents::Mission accounting(std::string id, std::string role = "infantry") {
    static const auto schema = std::make_shared<const json::Schema>(unwrap(json::Schema::compile_text(R"({
        "type": "object",
        "additionalProperties": false,
        "required": ["sentences", "boils_at"],
        "properties": {
            "sentences": {"type": "array", "items": {"type": "string"}},
            "boils_at": {"type": "integer"}
        }
    })")));
    agents::Mission mission;
    mission.id = std::move(id);
    mission.kind = "account";
    mission.role = std::move(role);
    mission.request.messages = {llm::Message::system("Account for every sentence."),
                                llm::Message::user("S1. Water boils at 100 °C. S2. At sea level.")};
    mission.request.seed = 7;
    mission.answer_tokens = 200;
    mission.schema = schema;
    mission.check = [](const json::Json& value) {
        std::vector<std::string> problems;
        if (value["sentences"] != json::Json{"S1", "S2"}) {
            problems.emplace_back("every sentence must be accounted for, once: S1 and S2");
        }
        if (value["boils_at"] != 100) {
            problems.emplace_back("boils_at must be the packet's number, 100");
        }
        return problems;
    };
    return mission;
}

struct Rig {
    explicit Rig(std::vector<Fault> faults) : faulty(oracle, std::move(faults)) {
        oracle.teach("account", [](const llm::ChatRequest&) -> Result<std::string> {
            return R"({"boils_at": 100, "sentences": ["S1", "S2"]})";
        });
    }

    Oracle oracle;
    FaultyProvider faulty;
    FakeClock clock;
    agents::Army army =
        unwrap(agents::Army::create(agents::Army::Options{.engines = engines()}, faulty, clock));
};

TEST(Faults, EachEndsRepairedOrAsAFinding) {
    struct Case {
        Fault fault;
        agents::Verdict verdict;
        agents::Step step;
    };
    const std::vector<Case> repaired{
        {.fault = truncation({}), .verdict = agents::Verdict::accepted, .step = agents::Step::as_ordered},
        {.fault = broken_json({}), .verdict = agents::Verdict::accepted, .step = agents::Step::as_ordered},
        {.fault = wrong_number({}), .verdict = agents::Verdict::accepted, .step = agents::Step::as_ordered},
        {.fault = dropped_reference({}),
         .verdict = agents::Verdict::accepted,
         .step = agents::Step::as_ordered},
        {.fault = refusal({}), .verdict = agents::Verdict::accepted, .step = agents::Step::commander},
        // Every call wrong, everywhere: the ladder's top, and a finding.
        {.fault = wrong_number(When{.calls = 0}),
         .verdict = agents::Verdict::invalid,
         .step = agents::Step::commander},
        {.fault = refusal(When{.calls = 0}),
         .verdict = agents::Verdict::refused,
         .step = agents::Step::commander},
    };
    for (const Case& each : repaired) {
        Rig rig{{each.fault}};
        const agents::Outcome outcome = unwrap(rig.army.execute(accounting("m1")));
        EXPECT_EQ(outcome.verdict, each.verdict) << each.fault.name;
        EXPECT_EQ(outcome.step, each.step) << each.fault.name;
        EXPECT_FALSE(rig.faulty.injected().empty()) << each.fault.name << " was made";
        if (outcome.verdict != agents::Verdict::accepted) {
            EXPECT_FALSE(outcome.problems.empty()) << "a finding says why";
        }
    }
}

TEST(Faults, ThinkingThatRunsOutEndsWithAnAnswer) {
    Rig rig{{out_while_thinking(When{.thinking = true})}};
    agents::Mission outline = accounting("m1", "commander");
    outline.request.thinking = true;
    const agents::Outcome outcome = unwrap(rig.army.execute(std::move(outline)));
    EXPECT_EQ(outcome.verdict, agents::Verdict::accepted);
    ASSERT_EQ(outcome.answers.size(), 1U);
    EXPECT_FALSE(outcome.answers.front().thinking) << "asked again without thinking";
}

TEST(Faults, AServerThatStaysDownIsAnErrorNotAFinding) {
    Rig rig{{server_error(When{.calls = 0}, Errc::unavailable)}};
    EXPECT_ERRC(rig.army.execute(accounting("m1")), Errc::unavailable) << "the job stops, to resume later";
}

}  // namespace
}  // namespace rr::oracle
