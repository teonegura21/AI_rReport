// rr/llm/sampling.hpp — how a model samples: presets, the checks on every
// request's values, and the rule against penalties.
//
// Why this exists
//   Sampling is part of the prompt. The same request at another temperature,
//   top-k or penalty is a different question, so three rules apply:
//
//   1. **Every real call says how to sample.** A field left unset takes the
//      server's default, and llama-server's defaults are neither the model
//      cards' (temperature 0.8, top-k 40, min-p 0.05) nor stable across
//      versions. A preset sets every field from the model's card, and missions
//      apply one (apply()).
//
//   2. **Values are checked before they are sent** (sampling_problems()). A
//      NaN would be serialized as null, which a server reads as "use your
//      default": a silent change of question. A thinking budget or a reasoning
//      effort with thinking off would be ignored, just as silently.
//
//   3. **No penalties where the answer must stay close to its source**
//      (penalty_problems()). Repetition penalties (presence, frequency,
//      repeat), DRY and XTC change which token comes next because of what was
//      already written, and in rReport's answers repetition is the point:
//        * a structured answer repeats its keys, its quotes and its labels:
//          the tenth atom is a "claim" as legitimately as the first, but a
//          penalty pushes it toward a label that has not been used yet. The
//          penalty biases the classification;
//        * an article repeats names and terms, and quotes verbatim (must-quote
//          atoms, plan §3.1). A penalty pushes toward synonyms and paraphrase:
//          it manufactures the softening and term drift the Fidelity Ledger
//          exists to catch. DRY penalizes repeated sequences, which is what a
//          quote is; XTC removes the most probable tokens, often the exact word.
//      Qwen3.8's card recommends presence_penalty 1.5 without thinking, against
//      endless repetition. rReport bounds that with max_tokens and validation
//      instead: a looping answer fails its checks and is asked again.
//      structured_json() refuses a request that sets a penalty.
//
// Thread-safety: plain values and pure functions.
#pragma once

#include <optional>
#include <string>
#include <vector>

#include <rr/llm/chat.hpp>

namespace rr::llm {

// A complete way to sample: every field has a value (the defaults below are
// the neutral ones). Presets live in the hub's configuration ([sampling.*]),
// with values from the model cards.
struct SamplingPreset {
    double temperature = 1.0;
    double top_p = 1.0;
    int top_k = 0;                  // 0: off
    double min_p = 0.0;             // 0: off
    double presence_penalty = 0.0;  // 0: off
    double repeat_penalty = 1.0;    // 1: off
    bool thinking = false;
    std::optional<ThinkingBudget> thinking_budget{};    // only with thinking
    std::optional<ReasoningEffort> reasoning_effort{};  // only with thinking

    friend bool operator==(const SamplingPreset&, const SamplingPreset&) = default;
};

// Fills every sampling field the request leaves unset from the preset, so a
// mission can still choose, say, its own temperature. The budget and the
// effort are taken only when the request ends up thinking.
void apply(const SamplingPreset& preset, ChatRequest& request);

// What is wrong with the request's sampling values (empty: nothing):
//   * temperature ≥ 0; top_p in (0, 1]; top_k ≥ 0; min_p in [0, 1];
//     presence_penalty in [-2, 2]; repeat_penalty > 0; max_tokens ≥ 1;
//     a thinking budget ≥ 0 tokens. NaN fails every range;
//   * a thinking budget or a reasoning effort needs thinking = true.
[[nodiscard]] std::vector<std::string> sampling_problems(const ChatRequest& request);

// The penalties a request sets, in its fields or in `extra` (empty: none).
// Neutral values are not penalties: presence and frequency 0, repeat 1, DRY's
// multiplier 0, XTC's probability 0.
[[nodiscard]] std::vector<std::string> penalty_problems(const ChatRequest& request);

// The preset's problems, as sampling_problems() sees them.
[[nodiscard]] std::vector<std::string> preset_problems(const SamplingPreset& preset);

}  // namespace rr::llm
