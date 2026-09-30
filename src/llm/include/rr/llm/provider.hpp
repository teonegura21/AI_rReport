// rr/llm/provider.hpp — the interface every model backend implements.
//
// Why this exists
//   A provider turns a ChatRequest into a ChatResponse: one attempt, nothing
//   more. Everything else wraps a provider in another provider, so each
//   concern is written once and composes with every backend:
//     * RetryingProvider (below): transient failures are tried again;
//     * AccountedProvider (rr/llm/accounting.hpp): every attempt is recorded;
//     * RecordingProvider and ReplayProvider (rr/llm/replay.hpp): answers
//       are recorded once and replayed in tests, without a model;
//     * structured() (rr/llm/structured.hpp) sits on top of any of them.
//
//   Failures are Errors whose class says what to do (ADR-0007): network,
//   timeout, unavailable and rate_limited are transient; invalid_argument
//   (a bad request, a prompt too long for the context) is not;
//   permission_denied is a wrong token; cancelled means the stop token was
//   triggered.
//
// Thread-safety: chat() may be called from many threads at once; every
//   provider in this module is safe for that.
#pragma once

#include <stop_token>
#include <string>
#include <string_view>

#include <rr/core/result.hpp>
#include <rr/llm/chat.hpp>
#include <rr/net/retry.hpp>

namespace rr::llm {

// What a backend can do. Callers adapt (e.g. no images to a text-only model);
// providers refuse what they cannot do instead of dropping it silently.
struct ProviderCaps {
    bool json_schema = false;  // constrained decoding from a JSON schema
    bool images = false;       // image parts in messages
    bool thinking = false;     // `thinking` can be switched per request
};

class LlmProvider {
public:
    LlmProvider() = default;
    LlmProvider(const LlmProvider&) = delete;
    LlmProvider& operator=(const LlmProvider&) = delete;
    LlmProvider(LlmProvider&&) = delete;
    LlmProvider& operator=(LlmProvider&&) = delete;
    virtual ~LlmProvider() = default;

    // A short, stable name ("runtime", "qwen-api"), recorded with every call.
    [[nodiscard]] virtual std::string_view name() const noexcept = 0;
    [[nodiscard]] virtual ProviderCaps caps() const noexcept = 0;

    // One attempt at the call. Deltas reach `sink` as they arrive.
    [[nodiscard]] virtual Result<ChatResponse> chat(const ChatRequest& request, const StreamSink& sink,
                                                    const std::stop_token& stop) const = 0;
};

// Tries a call again after transient failures, with backoff (rr/net/retry.hpp).
// Before each new attempt the sink hears on_restart(): the text it received
// is void, and the answer will stream again from its start.
class RetryingProvider final : public LlmProvider {
public:
    // `inner` must outlive this provider.
    RetryingProvider(const LlmProvider& inner, net::RetryPolicy policy, net::Pause pause = net::pause_for);

    [[nodiscard]] std::string_view name() const noexcept override { return inner_->name(); }
    [[nodiscard]] ProviderCaps caps() const noexcept override { return inner_->caps(); }
    [[nodiscard]] Result<ChatResponse> chat(const ChatRequest& request, const StreamSink& sink,
                                            const std::stop_token& stop) const override;

private:
    const LlmProvider* inner_;
    net::RetryPolicy policy_;
    net::Pause pause_;
};

}  // namespace rr::llm
