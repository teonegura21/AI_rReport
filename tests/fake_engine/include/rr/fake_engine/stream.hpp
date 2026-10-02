// rr/fake_engine/stream.hpp — what rr_fake_engine streams for a chat call,
// event by event, the way llama-server streams it. The fake engine and the
// tests that check what arrives share it, so the two cannot drift apart.
//
// llama-server's stream for POST /v1/chat/completions with "stream": true
// (llama.cpp 7fe450e), for each choice (one, unless the request asks for n):
//   1. a chunk whose delta is {"role": "assistant", "content": null}, sent
//      with the choice's first token (the prompt is read in silence);
//   2. a chunk per token: delta.reasoning_content while the model thinks,
//      then delta.content;
//   3. a chunk with "delta": {} and the finish_reason ("stop", "length");
//   4. with stream_options.include_usage: a chunk with "choices": [] and the
//      choice's usage, written together with its finish chunk. "timings" ride
//      on the last of these JSON chunks.
// Every choice chunk carries its "index"; the choices' tokens interleave. One
// "[DONE]" ends the stream. The fake's chunks are deterministic ("id" and
// "created" are fixed), so tests can compare events exactly.
//
// With "logprobs": true (and "top_logprobs" 1, as rr_llm asks), each token's
// chunk carries, beside its delta, "logprobs": {"content": [one entry]}: the
// token's id, text and bytes, its probability, and its alternatives, here
// itself. The probability is "prob" (and "top_probs") after the sampler
// chain, with "post_sampling_probs": true, else "logprob" (and
// "top_logprobs"), its logarithm before it; a probability of 0 has the
// lowest float's logarithm, since JSON has no -inf (llama-server's
// completion_token_output::logarithm()). Reasoning tokens carry theirs too.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <rr/json/json.hpp>

namespace rr::fake_engine {

// One answer: what it thinks and says, in pieces, and how it ends.
struct ChoicePlan {
    std::vector<std::string> reasoning{};  // reasoning_content pieces, before the content
    std::vector<std::string> content{};
    std::string finish_reason = "stop";
    double probability = 1;  // each token's, when they are sent
};

// The token probabilities a stream carries (see the header comment).
enum class Probabilities : std::uint8_t {
    none,
    before_sampling,  // "logprob"
    after_sampling    // "prob": post_sampling_probs
};

struct StreamPlan {
    std::string model = "fake";
    std::vector<ChoicePlan> choices{ChoicePlan{}};  // by index
    Probabilities probabilities = Probabilities::none;
    bool include_usage = false;
    std::int64_t prompt_tokens = 1;
    std::int64_t cached_tokens = 0;  // of the prompt's, served from a cache, not read
    int token_ms = 20;               // the engine's pace, reported in "timings"
};

// Five tokens, tok0 … tok4: the fake's answer when a request asks for nothing else.
inline StreamPlan default_plan(std::string model) {
    StreamPlan plan{.model = std::move(model)};
    for (int index = 0; index < 5; ++index) {
        plan.choices[0].content.push_back(std::format("tok{}", index));
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

inline json::Json one_choice(json::Json delta, json::Json finish_reason = nullptr, std::size_t index = 0) {
    return json::Json::array({json::Json{
        {"delta", std::move(delta)}, {"finish_reason", std::move(finish_reason)}, {"index", index}}});
}

inline std::string role_event(const StreamPlan& plan, std::size_t choice = 0) {
    return chunk(plan, one_choice(json::Json{{"content", nullptr}, {"role", "assistant"}}, nullptr, choice));
}

// The "logprobs" of one token, `text`, drawn with `probability`.
inline json::Json token_logprobs(Probabilities kind, std::string_view text, double probability) {
    json::Json bytes = json::Json::array();
    for (const char byte : text) {
        bytes.push_back(static_cast<unsigned char>(byte));
    }
    const bool after = kind == Probabilities::after_sampling;
    const double logarithm =
        probability > 0 ? std::log(probability) : static_cast<double>(std::numeric_limits<float>::lowest());
    json::Json token{{"id", 0}, {"token", text}, {"bytes", std::move(bytes)}};
    token[after ? "prob" : "logprob"] = after ? probability : logarithm;
    json::Json entry = token;
    entry[after ? "top_probs" : "top_logprobs"] = json::Json::array({std::move(token)});
    return json::Json{{"content", json::Json::array({std::move(entry)})}};
}

// A chunk of one token, `text`, of choice `choice`'s `field`.
inline std::string token_event(const StreamPlan& plan, std::string_view field, std::string_view text,
                               std::size_t choice) {
    json::Json choices = one_choice(json::Json{{std::string{field}, text}}, nullptr, choice);
    if (plan.probabilities != Probabilities::none) {
        choices[0]["logprobs"] =
            token_logprobs(plan.probabilities, text, plan.choices.at(choice).probability);
    }
    return chunk(plan, std::move(choices));
}

inline std::string content_event(const StreamPlan& plan, std::string_view text, std::size_t choice = 0) {
    return token_event(plan, "content", text, choice);
}

inline std::string reasoning_event(const StreamPlan& plan, std::string_view text, std::size_t choice = 0) {
    return token_event(plan, "reasoning_content", text, choice);
}

inline std::int64_t completion_tokens(const ChoicePlan& choice) {
    return static_cast<std::int64_t>(choice.reasoning.size() + choice.content.size());
}

inline json::Json timings(const StreamPlan& plan, std::size_t choice) {
    const std::int64_t predicted = completion_tokens(plan.choices.at(choice));
    const double pace = plan.token_ms > 0 ? 1000.0 / plan.token_ms : 1'000'000.0;
    const std::int64_t read = plan.prompt_tokens - plan.cached_tokens;
    return {{"cache_n", plan.cached_tokens},
            {"prompt_n", read},
            {"prompt_ms", static_cast<double>(read)},
            {"prompt_per_second", 1000.0},
            {"predicted_n", predicted},
            {"predicted_ms", static_cast<double>(predicted * plan.token_ms)},
            {"predicted_per_second", pace}};
}

inline std::string finish_event(const StreamPlan& plan, std::size_t choice = 0) {
    return chunk(plan, one_choice(json::Json::object(), plan.choices.at(choice).finish_reason, choice),
                 plan.include_usage ? json::Json::object() : json::Json{{"timings", timings(plan, choice)}});
}

// The choice's usage. Every choice reports the whole prompt: llama-server
// copies the prompt, read once, to each choice's slot. Its cached part was
// not read (llama-server's usage counts it in prompt_tokens all the same).
inline std::string usage_event(const StreamPlan& plan, std::size_t choice = 0) {
    const std::int64_t completion = completion_tokens(plan.choices.at(choice));
    return chunk(plan, json::Json::array(),
                 {{"usage",
                   {{"completion_tokens", completion},
                    {"prompt_tokens", plan.prompt_tokens},
                    {"prompt_tokens_details", {{"cached_tokens", plan.cached_tokens}}},
                    {"total_tokens", plan.prompt_tokens + completion}}},
                  {"timings", timings(plan, choice)}});
}

// One event of the stream's body, before the finish chunks.
struct Scheduled {
    std::string data;
    bool token = false;  // a token, not a role chunk
};

// The role and token events of every choice, interleaved one round at a
// time, as parallel slots produce them: each choice's role chunk first.
inline std::vector<Scheduled> body_events(const StreamPlan& plan) {
    std::vector<std::vector<Scheduled>> queues;
    for (std::size_t index = 0; index < plan.choices.size(); ++index) {
        const ChoicePlan& choice = plan.choices[index];
        std::vector<Scheduled> queue{{.data = role_event(plan, index), .token = false}};
        for (const std::string& piece : choice.reasoning) {
            queue.push_back({.data = reasoning_event(plan, piece, index), .token = true});
        }
        for (const std::string& piece : choice.content) {
            queue.push_back({.data = content_event(plan, piece, index), .token = true});
        }
        queues.push_back(std::move(queue));
    }
    std::vector<Scheduled> all;
    std::size_t longest = 0;
    for (const auto& queue : queues) {
        longest = std::max(longest, queue.size());
    }
    for (std::size_t round = 0; round < longest; ++round) {
        for (const auto& queue : queues) {
            if (round < queue.size()) {
                all.push_back(queue[round]);
            }
        }
    }
    return all;
}

// Every event's data, in order, "[DONE]" last.
inline std::vector<std::string> events(const StreamPlan& plan) {
    std::vector<std::string> all;
    for (Scheduled& event : body_events(plan)) {
        all.push_back(std::move(event.data));
    }
    for (std::size_t index = 0; index < plan.choices.size(); ++index) {
        all.push_back(finish_event(plan, index));
        if (plan.include_usage) {
            all.push_back(usage_event(plan, index));
        }
    }
    all.emplace_back("[DONE]");
    return all;
}

}  // namespace rr::fake_engine
