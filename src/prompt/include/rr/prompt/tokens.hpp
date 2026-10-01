// rr/prompt/tokens.hpp — how many tokens a prompt will take, before it is sent.
//
// Why this exists
//   A mission's prompt must fit the context of the slot that serves it
//   (runtime_api::EngineInfo::ctx_per_slot), with room for its answer. A
//   prompt that does not fit is refused by the engine, or, in a shared pool,
//   fails every request in flight (docs/design/runtime.md). So packets are
//   sized before they are sent, and one that would not fit is split
//   deterministically, never cut short in silence.
//
//   Counting tokens exactly needs the model's tokenizer, which lives on the
//   pod. An estimate is enough, if it errs on the high side: tokens per
//   character, calibrated from calls already made (each call's record has
//   its prompt and the prompt_tokens the engine counted). calibrate() takes
//   a high quantile of the observed ratios, so most prompts are shorter than
//   estimated, and the few longer ones by little; a margin covers the rest.
//
// Thread-safety: immutable values and pure functions.
#pragma once

#include <cstdint>
#include <span>
#include <string_view>

#include <rr/core/result.hpp>
#include <rr/prompt/library.hpp>

namespace rr::prompt {

// One call's prompt: its characters, and the tokens the engine counted.
struct Observation {
    std::int64_t characters = 0;
    std::int64_t tokens = 0;
};

class TokenEstimator {
public:
    // English prose runs about four characters a token for Qwen's and Gemma's
    // tokenizers. Before any calibration, assume three: an overestimate.
    static constexpr double kDefaultTokensPerCharacter = 1.0 / 3.0;
    // What a chat template adds to each message (role markers, newlines).
    static constexpr std::int64_t kTokensPerMessage = 8;

    explicit TokenEstimator(double tokens_per_character = kDefaultTokensPerCharacter);

    [[nodiscard]] double tokens_per_character() const noexcept { return tokens_per_character_; }

    // Tokens for `text`, rounded up.
    [[nodiscard]] std::int64_t estimate(std::string_view text) const noexcept;
    // Tokens for a rendered prompt: its two messages and their markers.
    [[nodiscard]] std::int64_t estimate(const Rendered& prompt) const noexcept;

    // Whether `prompt` and an answer of up to `max_tokens` fit a slot of
    // `context` tokens.
    [[nodiscard]] bool fits(const Rendered& prompt, std::int64_t max_tokens,
                            std::int64_t context) const noexcept;

private:
    double tokens_per_character_;
};

// The estimator for which `quantile` (0.5–1) of the observed prompts had no
// more tokens per character than estimated. Errc::invalid_argument for no
// usable observation (one with characters and tokens), or a quantile out of
// range.
[[nodiscard]] Result<TokenEstimator> calibrate(std::span<const Observation> observations,
                                               double quantile = 0.9);

}  // namespace rr::prompt
