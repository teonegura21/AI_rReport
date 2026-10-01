#include <rr/hub/model_stack.hpp>

#include <format>
#include <utility>

#include <rr/runtime_client/runtime_client.hpp>

namespace rr::hub {

Result<ModelStack> ModelStack::build(const HubSettings& settings, const net::HttpClient& http,
                                     llm::CallRecorder& recorder, const Clock& clock, Options options) {
    ModelStack stack;
    const llm::Recordings recordings{settings.recordings};
    if (settings.llm_mode == LlmMode::replay) {
        // Filed under the runtime's name and capabilities, as the live calls
        // were recorded: otherwise no key would match.
        stack.base_ = std::make_unique<llm::ReplayProvider>(std::string{runtime_client::kRuntimeProviderName},
                                                            runtime_client::kRuntimeProviderCaps, recordings);
        stack.destination_ = std::format("recorded answers in {}", settings.recordings.string());
    } else {
        if (!settings.runtime.has_value()) {
            return fail(Errc::invalid_argument,
                        "model calls go to the pod's runtime, and none is configured: set runpod.pod_id (or "
                        "runtime.url)");
        }
        // The one model endpoint (ADR-0017).
        stack.base_ = runtime_client::runtime_provider(http, *settings.runtime, settings.call_limits);
        stack.destination_ = std::format("the runtime at {}", settings.runtime->url);
    }
    stack.accounted_ = std::make_unique<llm::AccountedProvider>(*stack.base_, recorder, clock);
    stack.top_ = stack.accounted_.get();
    if (options.retries) {
        stack.retrying_ = std::make_unique<llm::RetryingProvider>(
            *stack.top_, net::RetryPolicy{.max_attempts = settings.call_attempts});
        stack.top_ = stack.retrying_.get();
    }
    if (settings.llm_mode == LlmMode::record) {
        stack.recording_ = std::make_unique<llm::RecordingProvider>(*stack.top_, recordings);
        stack.top_ = stack.recording_.get();
    }
    return stack;
}

}  // namespace rr::hub
