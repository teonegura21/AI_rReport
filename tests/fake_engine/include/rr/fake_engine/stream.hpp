// rr/fake_engine/stream.hpp — what rr_fake_engine streams for a chat call,
// event by event, the way llama-server streams it. The fake engine and the
// tests that check what arrives share it, so the two cannot drift apart.
//
// llama-server's stream for POST /v1/chat/completions with "stream": true
// (llama.cpp db00347, 2026-09-30):
//   1. a chunk whose delta is {"role": "assistant", "content": null}, sent
//      with the first token (the prompt is read in silence);
//   2. a chunk per token: delta.reasoning_content while the model thinks,
//      then delta.content;
//   3. a chunk with "delta": {} and the finish_reason ("stop", "length");
//   4. with stream_options.include_usage: a chunk with "choices": [] and the
//      usage. "timings" ride on the last of these JSON chunks;
//   5. "[DONE]".
// The fake's chunks are deterministic ("id" and "created" are fixed), so
// tests can compare events exactly.
#pragma once

#include <cstdint>
#include <format>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <rr/json/json.hpp>

namespace rr::fake_engine {

struct StreamPlan {
    std::string model = "fake";
    std::vector<std::string> reasoning{};  // reasoning_content pieces, before the content
    std::vector<std::string> content{};
    std::string finish_reason = "stop";
    bool include_usage = false;
    std::int64_t prompt_tokens = 1;
    int token_ms = 20;  // the engine's pace, reported in "timings"
};

// Five tokens, tok0 … tok4: the fake's answer when a request asks for nothing else.
inline StreamPlan default_plan(std::string model) {
    StreamPlan plan{.model = std::move(model)};
    for (int index = 0; index < 5; ++index) {
        plan.content.push_back(std::format("tok{}", index));
    }
    return plan;
}

inline std::string chunk(const StreamPlan& plan, json::Json choices,
                         const json::Json& extra = json::Json::object()) {
    json::Json document{{"choices", std::move(choices)},
                        {"created", 0},
                        {"id", "chatcmpl-fake"},
                        {"model", plan.model},
                        {"object", "chat.completion.chunk"}};
    for (const auto& [key, value] : extra.items()) {
        document[key] = value;
    }
    return json::dump(document);
}

inline json::Json one_choice(json::Json delta, json::Json finish_reason = nullptr) {
    return json::Json::array(
        {json::Json{{"delta", std::move(delta)}, {"finish_reason", std::move(finish_reason)}, {"index", 0}}});
}

inline std::string role_event(const StreamPlan& plan) {
    return chunk(plan, one_choice(json::Json{{"content", nullptr}, {"role", "assistant"}}));
}

inline std::string content_event(const StreamPlan& plan, std::string_view text) {
    return chunk(plan, one_choice(json::Json{{"content", text}}));
}

inline std::string reasoning_event(const StreamPlan& plan, std::string_view text) {
    return chunk(plan, one_choice(json::Json{{"reasoning_content", text}}));
}

inline std::int64_t completion_tokens(const StreamPlan& plan) {
    return static_cast<std::int64_t>(plan.reasoning.size() + plan.content.size());
}

inline json::Json timings(const StreamPlan& plan) {
    const std::int64_t predicted = completion_tokens(plan);
    const double pace = plan.token_ms > 0 ? 1000.0 / plan.token_ms : 1'000'000.0;
    return {{"cache_n", 0},
            {"prompt_n", plan.prompt_tokens},
            {"prompt_ms", static_cast<double>(plan.prompt_tokens)},
            {"prompt_per_second", 1000.0},
            {"predicted_n", predicted},
            {"predicted_ms", static_cast<double>(predicted * plan.token_ms)},
            {"predicted_per_second", pace}};
}

inline std::string finish_event(const StreamPlan& plan) {
    return chunk(plan, one_choice(json::Json::object(), plan.finish_reason),
                 plan.include_usage ? json::Json::object() : json::Json{{"timings", timings(plan)}});
}

inline std::string usage_event(const StreamPlan& plan) {
    const std::int64_t completion = completion_tokens(plan);
    return chunk(plan, json::Json::array(),
                 {{"usage",
                   {{"completion_tokens", completion},
                    {"prompt_tokens", plan.prompt_tokens},
                    {"prompt_tokens_details", {{"cached_tokens", 0}}},
                    {"total_tokens", plan.prompt_tokens + completion}}},
                  {"timings", timings(plan)}});
}

// Every event's data, in order, "[DONE]" last.
inline std::vector<std::string> events(const StreamPlan& plan) {
    std::vector<std::string> all{role_event(plan)};
    for (const std::string& piece : plan.reasoning) {
        all.push_back(reasoning_event(plan, piece));
    }
    for (const std::string& piece : plan.content) {
        all.push_back(content_event(plan, piece));
    }
    all.push_back(finish_event(plan));
    if (plan.include_usage) {
        all.push_back(usage_event(plan));
    }
    all.emplace_back("[DONE]");
    return all;
}

}  // namespace rr::fake_engine
