// rr/llm/memo.hpp — a job's memory of its model calls, so that a resumed job
// repeats none it already made (ADR-0020).
//
// Why this exists
//   A job makes thousands of calls over an hour or more, and a pod stop, a
//   crash or Ctrl-C can interrupt it anywhere. A finished stage is a cache
//   hit (ADR-0004), but a stage interrupted at 90% would start again from
//   zero. MemoProvider remembers each call's answer as it arrives; on resume,
//   every call asked before is answered from memory. Sampled calls cannot be
//   re-derived (a server picks a random seed when a request has none, and a
//   GPU's batching makes even a seeded request vary slightly): they can only
//   be remembered.
//
//   Three rules make that sound:
//     1. The key is the request's key and the *served model*: the request
//        names a role ("commander"), and the role's model can change between
//        a crash and a resume. The served model's file checksum, as the
//        runtime reports it, enters the key (memo_key()).
//     2. A request that may sample carries an explicit seed, derived
//        (derived_seed()); one without is refused. Otherwise three samples of
//        one window would be three identical requests, and all three would
//        get the first one's answer.
//     3. Only successes are kept, after they arrive, and a failure to keep
//        one is logged, never fatal: losing a memo entry costs a repeated
//        call, not a wrong answer.
//
//   A remembered answer reaches the stream sink whole, choice by choice, and
//   is not accounted again: it was accounted when it was made.
//
// Thread-safety: MemoProvider is thread-safe if its store and fingerprint
//   function are (the library's store is).
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>

#include <rr/core/result.hpp>
#include <rr/llm/chat.hpp>
#include <rr/llm/provider.hpp>

namespace rr::llm {

// Where one job's answers are kept. The library database keeps them
// (rr/library/call_memo.hpp); tests keep them in memory.
class MemoStore {
public:
    MemoStore() = default;
    MemoStore(const MemoStore&) = delete;
    MemoStore& operator=(const MemoStore&) = delete;
    MemoStore(MemoStore&&) = delete;
    MemoStore& operator=(MemoStore&&) = delete;
    virtual ~MemoStore() = default;

    // The answer kept under `key`, if any.
    [[nodiscard]] virtual Result<std::optional<ChatResponse>> find(std::string_view key) const = 0;

    // Keeps `response` under `key`. The first answer kept under a key stays.
    [[nodiscard]] virtual Result<void> keep(std::string_view key, const ChatResponse& response) = 0;
};

// The served model behind a request's model name: its file's checksum, as
// the runtime reports it.
using ModelFingerprint = std::function<Result<std::string>(std::string_view model)>;

// Whether a request may sample: its temperature is unset (servers then
// sample by default) or above 0.
[[nodiscard]] bool may_sample(const ChatRequest& request) noexcept;

// The memo's key: BLAKE3 over the request's key (for `provider`) and the
// served model's fingerprint.
[[nodiscard]] std::string memo_key(std::string_view provider, const ChatRequest& request,
                                   std::string_view fingerprint);

// The seed of sample `sample` of a mission (ADR-0020): base + sample, where
// base = BLAKE3(salt, stage, mission) mod 2^31. The salt is the eval's
// replicate (0 otherwise), so two jobs with the same inputs ask the same
// questions. A request for samples j … j + n − 1 is sent with
// derived_seed(…, j): llama-server gives its choice i the seed + i, so its
// choices draw the same seeds as n single requests. `sample` is 0 or more,
// and the seed stays far below 2^32 − 1, which llama-server reads as "pick
// one at random".
[[nodiscard]] std::int64_t derived_seed(std::int64_t salt, std::string_view stage, std::string_view mission,
                                        int sample);

class MemoProvider final : public LlmProvider {
public:
    // `inner` and `store` must outlive this provider.
    MemoProvider(const LlmProvider& inner, MemoStore& store, ModelFingerprint fingerprint);

    [[nodiscard]] std::string_view name() const noexcept override { return inner_->name(); }
    [[nodiscard]] ProviderCaps caps() const noexcept override { return inner_->caps(); }

    // A remembered answer, or the inner provider's, kept. Errc::invalid_argument
    // for a request that may sample without a seed; the fingerprint's error
    // if the served model cannot be named.
    [[nodiscard]] Result<ChatResponse> chat(const ChatRequest& request, const StreamSink& sink,
                                            const std::stop_token& stop) const override;

    // Calls answered from memory, and calls made.
    [[nodiscard]] std::int64_t remembered() const noexcept { return remembered_.load(); }
    [[nodiscard]] std::int64_t made() const noexcept { return made_.load(); }

private:
    const LlmProvider* inner_;
    MemoStore* store_;
    ModelFingerprint fingerprint_;
    mutable std::atomic<std::int64_t> remembered_{0};
    mutable std::atomic<std::int64_t> made_{0};
};

}  // namespace rr::llm
