// A provider that plays a script, for testing what wraps providers.
//
// Each call takes the script's next step (an answer or an error), records the
// request it was given, and streams each choice's reasoning and content to the
// sink. `during_call` runs inside each call (to advance a FakeClock, say).
#pragma once

#include <cstddef>
#include <functional>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <rr/llm/provider.hpp>

namespace rr::llm::testing {

class ScriptedProvider final : public LlmProvider {
public:
    using Step = Result<ChatResponse>;

    explicit ScriptedProvider(std::vector<Step> steps, ProviderCaps caps = {}, std::string name = "scripted")
        : steps_(std::move(steps))
        , caps_(caps)
        , name_(std::move(name)) {}

    std::function<void()> during_call{};

    [[nodiscard]] std::string_view name() const noexcept override { return name_; }
    [[nodiscard]] ProviderCaps caps() const noexcept override { return caps_; }

    [[nodiscard]] Result<ChatResponse> chat(const ChatRequest& request, const StreamSink& sink,
                                            const std::stop_token& /*stop*/) const override {
        Step step = fail(Errc::internal, "the script has no more steps");
        {
            const std::lock_guard lock(mutex_);
            requests_.push_back(request);
            if (next_ < steps_.size()) {
                step = steps_[next_++];
            }
        }
        if (during_call) {
            during_call();
        }
        if (step.has_value()) {
            for (std::size_t index = 0; index < step->choices.size(); ++index) {
                const Choice& choice = step->choices[index];
                if (sink.on_reasoning && !choice.reasoning.empty()) {
                    sink.on_reasoning(index, choice.reasoning);
                }
                if (sink.on_content && !choice.content.empty()) {
                    sink.on_content(index, choice.content);
                }
            }
        }
        return step;
    }

    [[nodiscard]] std::vector<ChatRequest> requests() const {
        const std::lock_guard lock(mutex_);
        return requests_;
    }

private:
    mutable std::mutex mutex_;
    std::vector<Step> steps_;
    mutable std::size_t next_ = 0;
    mutable std::vector<ChatRequest> requests_;
    ProviderCaps caps_;
    std::string name_;
};

// An answer with `content` that finished normally.
inline ChatResponse answer(std::string content) {
    return ChatResponse{
        .choices = {Choice{
            .content = std::move(content), .finish = FinishReason::stop, .completion_tokens = 5}},
        .usage = {.prompt_tokens = 10, .completion_tokens = 5},
        .model = "scripted-model"};
}

// Answers to a request for n, one per text, each finished normally.
inline ChatResponse answers(const std::vector<std::string>& contents) {
    ChatResponse response{.usage = {.prompt_tokens = 10}, .model = "scripted-model"};
    for (const std::string& content : contents) {
        response.choices.push_back(
            Choice{.content = content, .finish = FinishReason::stop, .completion_tokens = 5});
        response.usage.completion_tokens += 5;
    }
    return response;
}

// A one-message request to `model`.
inline ChatRequest ask(std::string text, std::string model = "commander") {
    return ChatRequest{.model = std::move(model), .messages = {Message::user(std::move(text))}};
}

}  // namespace rr::llm::testing
