// rr/llm/accounting.hpp — every attempt at a model call, recorded.
//
// Why this exists
//   Tokens and GPU time are what the product costs to run (plan §4.3,
//   "Metering"). AccountedProvider wraps any provider and hands one
//   CallRecord per attempt to a CallRecorder: why the call was made (its
//   tag), where it went, how long it took (to the first token and in all),
//   the tokens in and out, and how it ended (finished, or the error class).
//   Attempts of one call share a request key, so a flaky pod shows up as
//   several records for one call rather than as one slow call.
//
//   rr_llm knows nothing about storage. The library database implements
//   CallRecorder (rr/library/call_log.hpp); tests use an in-memory one.
//
//   Recording never fails a call. An answer the model has already produced
//   (and the GPU has already been paid for) is not thrown away because a
//   record could not be written: the failure is logged instead.
//
// Thread-safety: AccountedProvider is safe to call from many threads if its
//   CallRecorder is (the library database's is).
#pragma once

#include <chrono>
#include <optional>
#include <string>
#include <string_view>

#include <rr/core/clock.hpp>
#include <rr/core/result.hpp>
#include <rr/core/ulid.hpp>
#include <rr/json/json.hpp>
#include <rr/llm/chat.hpp>
#include <rr/llm/provider.hpp>

namespace rr::llm {

struct CallRecord {
    std::string id{};  // a ULID: records sort by start time
    std::chrono::system_clock::time_point started_at{};
    std::chrono::milliseconds duration{};
    std::optional<std::chrono::milliseconds> first_token{};
    std::string provider{};
    std::string model{};
    CallTag tag{};
    std::string request_key{};
    std::optional<Error> error{};  // how the attempt failed; nullopt if it succeeded
    // For a success: the least complete way a choice ended
    // (ChatResponse::least_complete_finish()), so a cut-short answer shows.
    std::optional<FinishReason> finish{};
    Usage usage{};
    json::Json request{};  // to_json() of the request
    // {"choices": [{"content": …, "reasoning": …}, …]}; an empty object after a failure
    json::Json response{};
};

class CallRecorder {
public:
    CallRecorder() = default;
    CallRecorder(const CallRecorder&) = delete;
    CallRecorder& operator=(const CallRecorder&) = delete;
    CallRecorder(CallRecorder&&) = delete;
    CallRecorder& operator=(CallRecorder&&) = delete;
    virtual ~CallRecorder() = default;

    // Called concurrently from every thread that makes calls.
    [[nodiscard]] virtual Result<void> record(const CallRecord& call) = 0;
};

class AccountedProvider final : public LlmProvider {
public:
    // `inner`, `recorder` and `clock` must outlive this provider.
    AccountedProvider(const LlmProvider& inner, CallRecorder& recorder, const Clock& clock);

    [[nodiscard]] std::string_view name() const noexcept override { return inner_->name(); }
    [[nodiscard]] ProviderCaps caps() const noexcept override { return inner_->caps(); }
    [[nodiscard]] Result<ChatResponse> chat(const ChatRequest& request, const StreamSink& sink,
                                            const std::stop_token& stop) const override;

private:
    const LlmProvider* inner_;
    CallRecorder* recorder_;
    const Clock* clock_;
    mutable UlidGenerator ids_;  // thread-safe; mutable because issuing an id is not an observable change
};

}  // namespace rr::llm
