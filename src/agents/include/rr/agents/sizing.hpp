// rr/agents/sizing.hpp — whether a request fits an engine, and what it holds
// there while it runs.
//
// Why this exists
//   An engine holds a fixed number of tokens (docs/design/runtime.md, "Context
//   per slot, and the shared pool"). One sequence, a prompt and its answer,
//   may hold at most ctx_per_slot. In a shared pool (kv_unified) the
//   sequences at work together may hold at most `context` between them, and
//   when they outgrow it llama-server fails every one of them. So the army
//   sizes each request before sending it, from an estimate of its prompt
//   (prompt::TokenEstimator, which errs high) and the most each answer may
//   take (max_tokens: the answer, and a thinking budget):
//     * a request that cannot fit is not sent;
//     * the n answers to one prompt each need their own room, while in a
//       shared pool they keep the prompt once;
//     * in a shared pool, a request reserves its worst case, the prompt and
//       n × max_tokens, for as long as it runs, so that the requests at work
//       together can never outgrow the pool. The prompts idle slots keep
//       cached need no reservation: llama-server purges them when a running
//       sequence needs the room (try_clear_idle_slots() in
//       tools/server/server-context.cpp, llama.cpp 7fe450e).
//
// Thread-safety: pure functions.
#pragma once

#include <cstdint>

#include <rr/agents/mission.hpp>
#include <rr/core/result.hpp>
#include <rr/llm/chat.hpp>
#include <rr/prompt/tokens.hpp>

namespace rr::agents {

struct Footprint {
    std::int64_t prompt = 0;  // the prompt's tokens, estimated
    std::int64_t answer = 0;  // the most one answer may take (max_tokens)

    friend bool operator==(const Footprint&, const Footprint&) = default;
};

// The tokens of a request's prompt: each message's text, and its template
// markers. Errc::unsupported for a message with an image, whose tokens
// cannot be estimated from the request.
[[nodiscard]] Result<std::int64_t> prompt_tokens(const llm::ChatRequest& request,
                                                 const prompt::TokenEstimator& estimator);

// prompt_tokens(), and the request's max_tokens (which must be set).
[[nodiscard]] Result<Footprint> footprint_of(const llm::ChatRequest& request,
                                             const prompt::TokenEstimator& estimator);

// The most answers to one prompt `engine` can hold at once; 0 when not even
// one fits.
[[nodiscard]] int choices_that_fit(const Engine& engine, const Footprint& footprint) noexcept;

// The tokens of `engine`'s shared pool that a request for `choices` answers
// holds while it runs; 0 for an engine whose slots each have their own share.
[[nodiscard]] std::int64_t reservation(const Engine& engine, const Footprint& footprint,
                                       int choices) noexcept;

}  // namespace rr::agents
