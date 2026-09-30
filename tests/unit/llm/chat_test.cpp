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
    full.seed = 7;
    full.stop = {"END"};
    full.json_schema = json::Json{{"type", "object"}};
    full.schema_name = "atoms";
    full.thinking = false;
    full.extra = {{"cache_prompt", true}};
    const json::Json document = to_json(full);
    for (const char* field : {"max_tokens", "temperature", "top_p", "seed", "stop", "json_schema",
                              "schema_name", "thinking", "extra"}) {
        EXPECT_TRUE(document.contains(field)) << field;
    }
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
}

}  // namespace
}  // namespace rr::llm
