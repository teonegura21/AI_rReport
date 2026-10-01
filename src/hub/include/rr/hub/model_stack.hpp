// rr/hub/model_stack.hpp — the providers every model call goes through.
//
// Why this exists
//   Every model call goes to the pod's runtime (ADR-0017). ModelStack is the
//   one place where providers for real work are built, so that rule is a
//   property of the code rather than a habit: it builds them against the
//   runtime's endpoint (runtime.url, or RunPod's proxy URL for the pod), and
//   against nothing else. It also wires llm.mode, outermost first:
//
//     live     retries → accounting → the runtime
//     record   recording → retries → accounting → the runtime
//     replay   retries → accounting → recorded answers (no runtime, no network)
//
//   Retries sit above accounting, so every attempt is accounted for (a flaky
//   pod shows as several records of one call). A recording sits above the
//   retries, so a call is recorded once, when it has succeeded. A replay
//   keeps the accounting, so a replayed job reports the tokens its
//   recordings cost. A job's memo (ADR-0020) wraps the stack, per job.
//
// Thread-safety: immutable once built; its providers are thread-safe.
#pragma once

#include <memory>
#include <string>

#include <rr/core/clock.hpp>
#include <rr/core/result.hpp>
#include <rr/hub/hub_config.hpp>
#include <rr/llm/accounting.hpp>
#include <rr/llm/provider.hpp>
#include <rr/llm/replay.hpp>
#include <rr/net/http_client.hpp>

namespace rr::hub {

class ModelStack {
public:
    struct Options {
        // Off for the benchmark, where a failed call is a result to report,
        // not something to hide by trying again.
        bool retries = true;
    };

    // The stack `settings` describe. Errc::invalid_argument in live or
    // record mode when no runtime is configured. `http`, `recorder` and
    // `clock` must outlive the stack.
    [[nodiscard]] static Result<ModelStack> build(const HubSettings& settings, const net::HttpClient& http,
                                                  llm::CallRecorder& recorder, const Clock& clock,
                                                  Options options);

    ModelStack(ModelStack&&) noexcept = default;
    ModelStack& operator=(ModelStack&&) noexcept = default;
    ModelStack(const ModelStack&) = delete;
    ModelStack& operator=(const ModelStack&) = delete;
    ~ModelStack() = default;

    // The provider to call: the top of the stack.
    [[nodiscard]] const llm::LlmProvider& provider() const noexcept { return *top_; }

    // Where model calls go, for people: "the runtime at <url>", or "recorded
    // answers in <directory>".
    [[nodiscard]] const std::string& destination() const noexcept { return destination_; }

private:
    ModelStack() = default;

    // Each layer refers to the one below it, so every layer lives on the heap
    // and keeps its address when the stack moves.
    std::unique_ptr<llm::LlmProvider> base_{};
    std::unique_ptr<llm::AccountedProvider> accounted_{};
    std::unique_ptr<llm::RetryingProvider> retrying_{};
    std::unique_ptr<llm::RecordingProvider> recording_{};
    const llm::LlmProvider* top_ = nullptr;
    std::string destination_{};
};

}  // namespace rr::hub
