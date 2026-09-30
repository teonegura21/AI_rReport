#include <rr/llm/accounting.hpp>

#include <utility>

#include <rr/log/log.hpp>

namespace rr::llm {

AccountedProvider::AccountedProvider(const LlmProvider& inner, CallRecorder& recorder, const Clock& clock)
    : inner_(&inner)
    , recorder_(&recorder)
    , clock_(&clock)
    , ids_(clock) {}

Result<ChatResponse> AccountedProvider::chat(const ChatRequest& request, const StreamSink& sink,
                                             const std::stop_token& stop) const {
    CallRecord call{.id = ids_.next(),
                    .started_at = clock_->now(),
                    .provider = std::string{inner_->name()},
                    .model = request.model,
                    .tag = request.tag,
                    .request_key = request_key(inner_->name(), request),
                    .request = to_json(request),
                    .response = json::Json::object()};
    const auto started = clock_->steady_now();

    auto result = inner_->chat(request, sink, stop);

    call.duration = std::chrono::duration_cast<std::chrono::milliseconds>(clock_->steady_now() - started);
    if (result.has_value()) {
        call.first_token = result->first_token;
        call.finish = result->finish;
        call.usage = result->usage;
        call.response = json::Json{{"content", result->content}, {"reasoning", result->reasoning}};
    } else {
        call.error = result.error();
    }
    if (auto recorded = recorder_->record(call); !recorded.has_value()) {
        log::error("a {} call to '{}' was not recorded: {}", call.provider, call.model,
                   recorded.error().describe());
    }
    return result;
}

}  // namespace rr::llm
