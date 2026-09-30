#include <rr/llm/provider.hpp>

#include <utility>

#include <rr/log/log.hpp>

namespace rr::llm {

RetryingProvider::RetryingProvider(const LlmProvider& inner, net::RetryPolicy policy, net::Pause pause)
    : inner_(&inner)
    , policy_(policy)
    , pause_(std::move(pause)) {}

Result<ChatResponse> RetryingProvider::chat(const ChatRequest& request, const StreamSink& sink,
                                            const std::stop_token& stop) const {
    return net::with_retries(
        policy_,
        [&](int attempt) -> Result<ChatResponse> {
            if (attempt > 1 && sink.on_restart) {
                sink.on_restart();
            }
            auto result = inner_->chat(request, sink, stop);
            if (!result.has_value() && result.error().transient() && attempt < policy_.max_attempts) {
                log::warn("{} call to '{}' failed (attempt {} of {}), trying again: {}", inner_->name(),
                          request.model, attempt, policy_.max_attempts, result.error().describe());
            }
            return result;
        },
        stop, pause_);
}

}  // namespace rr::llm
