#include <rr/llm/chat.hpp>

#include <string>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "scripted_provider.hpp"

namespace rr::llm {
namespace {

using testing::ask;

TEST(Chat, MessagesAreMadeOfParts) {
    const Message message{.role = MessageRole::user,
                          .content = {{.kind = ContentPart::Kind::text, .value = "What is on "},
                                      {.kind = ContentPart::Kind::image_url, .value = "rr-blob://lib/abc"},
                                      {.kind = ContentPart::Kind::text, .value = "this slide?"}}};
    EXPECT_EQ(message.text(), "What is on this slide?") << "text parts only, in order";
    EXPECT_EQ(Message::system("Be brief.").role, MessageRole::system);
    EXPECT_EQ(Message::assistant("ok").text(), "ok");
    EXPECT_EQ(to_string(MessageRole::assistant), "assistant");
}

TEST(Chat, FinishReasonsUseOpenAisNames) {
    EXPECT_EQ(finish_reason_from_string("stop"), FinishReason::stop);
    EXPECT_EQ(finish_reason_from_string("length"), FinishReason::length);
    EXPECT_EQ(finish_reason_from_string("content_filter"), FinishReason::content_filter);
    EXPECT_EQ(finish_reason_from_string("tool_calls"), FinishReason::other);
    EXPECT_EQ(finish_reason_from_string(""), FinishReason::other);
    EXPECT_EQ(to_string(FinishReason::length), "length");
}

TEST(Chat, TheJsonFormLeavesUnsetFieldsOut) {
    const json::Json minimal = to_json(ask("hi"));
    EXPECT_EQ(json::dump(minimal),
              R"({"messages":[{"content":[{"text":"hi","type":"text"}],"role":"user"}],"model":"commander"})")
        << "so a field added later changes no existing key";

    ChatRequest full = ask("hi");
    full.max_tokens = 100;
    full.temperature = 0.5;
    full.top_p = 0.9;
    full.top_k = 20;
    full.min_p = 0.05;
    full.presence_penalty = 0.0;
    full.repeat_penalty = 1.0;
    full.seed = 7;
    full.n = 3;
    full.stop = {"END"};
    full.json_schema = R"({"type":"object"})";
    full.schema_name = "atoms";
    full.thinking = true;
    full.thinking_budget = ThinkingBudget{.tokens = 512, .closing = "Answer now."};
    full.reasoning_effort = ReasoningEffort::medium;
    full.token_probabilities = true;
    full.extra = {{"cache_prompt", true}};
    const json::Json document = to_json(full);
    for (const char* field : {"max_tokens", "temperature", "top_p", "top_k", "min_p", "presence_penalty",
                              "repeat_penalty", "seed", "n", "stop", "json_schema", "schema_name", "thinking",
                              "thinking_budget", "reasoning_effort", "token_probabilities", "extra"}) {
        EXPECT_TRUE(document.contains(field)) << field;
    }
    EXPECT_EQ(document["thinking_budget"], (json::Json{{"tokens", 512}, {"closing", "Answer now."}}));
    EXPECT_EQ(document["reasoning_effort"], "medium");
    EXPECT_FALSE(document.contains("tag"));
}

TEST(Chat, TheRequestKeyIsWhatTheModelSees) {
    const std::string key = request_key("runtime", ask("hi"));
    EXPECT_EQ(key.size(), 64U);
    EXPECT_EQ(request_key("runtime", ask("hi")), key) << "stable";

    ChatRequest tagged = ask("hi");
    tagged.tag = {.purpose = "bench", .job_id = "J1", .mission_id = "M1"};
    EXPECT_EQ(request_key("runtime", tagged), key) << "why a call is made is not part of it";

    EXPECT_NE(request_key("qwen-api", ask("hi")), key) << "the provider is";
    EXPECT_NE(request_key("runtime", ask("hi", "infantry")), key) << "the model is";
    EXPECT_NE(request_key("runtime", ask("hi!")), key) << "the messages are";
    ChatRequest cooler = ask("hi");
    cooler.temperature = 0.1;
    EXPECT_NE(request_key("runtime", cooler), key) << "sampling is";
    ChatRequest extra = ask("hi");
    extra.extra = {{"id_slot", 2}};
    EXPECT_NE(request_key("runtime", extra), key) << "server-specific extras are";
    ChatRequest samples = ask("hi");
    samples.n = 3;
    EXPECT_NE(request_key("runtime", samples), key) << "how many answers are asked for is";
    ChatRequest neutral = ask("hi");
    neutral.repeat_penalty = 1.0;
    EXPECT_NE(request_key("runtime", neutral), key)
        << "an explicit neutral value overrides a server's default";
    ChatRequest thinking = ask("hi");
    thinking.thinking = true;
    const std::string thinking_key = request_key("runtime", thinking);
    thinking.thinking_budget = ThinkingBudget{.tokens = 512};
    EXPECT_NE(request_key("runtime", thinking), thinking_key) << "the thinking budget is";
    const std::string budget_key = request_key("runtime", thinking);
    thinking.thinking_budget->closing = "Answer now.";
    EXPECT_NE(request_key("runtime", thinking), budget_key)
        << "so is what the model is made to say at its end";
    ChatRequest sure = ask("hi");
    sure.token_probabilities = true;
    EXPECT_NE(request_key("runtime", sure), key)
        << "asking how sure the model was is: an answer kept without them has none to give";
}

// The memo and recordings keep a response in its JSON form: its token
// probabilities too, as [begin, end, probability] triples, read strictly.
TEST(Chat, AResponsesTokenProbabilitiesSurviveItsJsonForm) {
    const ChatResponse response{
        .choices = {Choice{.content = R"({"kind":"claim"})",
                           .finish = FinishReason::stop,
                           .completion_tokens = 4,
                           .probabilities = {{.begin = 0, .end = 2, .probability = 1},
                                             {.begin = 8, .end = 15, .probability = 0.25}}},
                    Choice{.content = "{}", .finish = FinishReason::stop}},
        .model = "infantry"};
    const json::Json document = to_json(response);
    EXPECT_EQ(document["choices"][0]["probabilities"], json::parse("[[0, 2, 1.0], [8, 15, 0.25]]").value())
        << "a gap where no token's probability came";
    EXPECT_FALSE(document["choices"][1].contains("probabilities")) << "none: left out, as before P28";
    EXPECT_EQ(response_from_json(document).value().choices, response.choices);

    for (const char* probabilities :
         {R"("all sure")", "[[0, 2]]", R"([["0", 2, 1]])", "[[-1, 2, 1]]", "[[0, 2, 1], [1, 3, 1]]",
          "[[2, 2, 1]]", "[[0, 17, 1]]", "[[0, 2, 1.5]]", "[[0, 2, -0.5]]"}) {
        json::Json broken = document;
        broken["choices"][0]["probabilities"] = json::parse(probabilities).value();
        const Result<ChatResponse> read = response_from_json(broken);
        ASSERT_FALSE(read.has_value()) << probabilities;
        EXPECT_EQ(read.error().code(), Errc::invalid_argument) << probabilities;
    }
}

TEST(Chat, TheSchemasFieldOrderIsPartOfTheKey) {
    ChatRequest quote_first = ask("Is the sentence supported?");
    quote_first.json_schema = R"({"type":"object","properties":{"quote":{},"decision":{}}})";
    ChatRequest decision_first = quote_first;
    decision_first.json_schema = R"({"type":"object","properties":{"decision":{},"quote":{}}})";
    EXPECT_NE(request_key("runtime", quote_first), request_key("runtime", decision_first))
        << "the model writes its fields in the schema's order, so the order can change the answer";
}

}  // namespace
}  // namespace rr::llm
