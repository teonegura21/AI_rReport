// rr/agents/mission.hpp — what the army is asked to do, what it does it with,
// and how it went.
//
// Why this exists
//   Pipeline stages ask models for small, checkable pieces of work: label
//   these 25 sentences, outline this transcript, judge this paragraph
//   (ADR-0011). Each is a *mission*: a role ("infantry"), a conversation,
//   the form of its answer (a JSON schema, or prose) and the stage's own
//   checks. The stage says *what*; the army (rr/agents/army.hpp) decides
//   which engine serves it, how many run at once, what happens to an answer
//   that fails, and keeps the battle log. These are the types they exchange.
//
//   A mission states the size of its answer (answer_tokens), never a
//   max_tokens: whether a call thinks is the army's decision at each rung of
//   the escalation ladder, and a call that thinks needs its thinking budget
//   on top of the answer (ADR-0018).
//
// Thread-safety: plain values and pure functions.
#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <rr/core/limiter.hpp>
#include <rr/json/json.hpp>
#include <rr/json/schema.hpp>
#include <rr/llm/chat.hpp>
#include <rr/llm/sampling.hpp>
#include <rr/llm/structured.hpp>

namespace rr::agents {

// An engine on the pod that missions can be sent to: what it serves, and how
// much it holds (as the runtime reports it: runtime_api::EngineInfo).
struct Engine {
    std::string model{};               // the runtime's id for it: what requests name
    std::vector<std::string> roles{};  // the roles it serves: "commander", "infantry"
    // Its model family ("qwen", "gemma"; family_of()). A mission one model
    // refuses goes to a model of another family (ADR-0017).
    std::string family{};
    int slots = 1;            // sequences at once (llama-server -np)
    int context = 0;          // tokens of attention cache, all slots together
    int ctx_per_slot = 0;     // the most one sequence, a prompt and its answer, may hold
    bool kv_unified = false;  // the slots share one pool of `context` tokens
    // How it samples: answering directly, and thinking. A mission's request
    // fills what it leaves unset from one of them (llm::apply()); a rung that
    // escalates uses one whole. `direct` must not think; `thinking` must,
    // with a budget, so that every escalation ends.
    llm::SamplingPreset direct{};
    llm::SamplingPreset thinking{};
};

// A model's family, from the catalog id it is served under: its leading
// letters, lowercased ("qwen3.8-27b-q4" → "qwen", "gemma-4-26b-a4b" →
// "gemma"). The catalog names every model family first.
[[nodiscard]] std::string family_of(std::string_view model);

// The rungs of the escalation ladder a mission may climb, beyond the first
// (rr/agents/ladder.hpp). A judge that must stay of another family than the
// writer it judges turns off `commander` and `other_family`.
struct Escalation {
    bool thinking = true;      // its own engine, thinking
    bool commander = true;     // the commander, thinking
    bool other_family = true;  // after a refusal, a model of another family, thinking
};

// The rungs, in the order a mission climbs them.
enum class Step : std::uint8_t {
    as_ordered,   // the role's engine, thinking or not as the mission asked
    thinking,     // the same engine, thinking
    commander,    // the commander, thinking
    other_family  // after a refusal: a model of a family that has not refused, thinking
};

[[nodiscard]] std::string_view to_string(Step step) noexcept;
// The inverse of to_string(); none for anything else.
[[nodiscard]] std::optional<Step> step_from_string(std::string_view name) noexcept;

struct Mission {
    std::string id{};      // unique in its job: tags its calls, names its battle-log row
    std::string kind{};    // what it does ("atomize"): its calls' purpose
    std::string prompt{};  // the prompt and its version ("atomize@3"), for the log; may be empty
    std::string role{};    // who should do it: "commander", "infantry"
    // The conversation, its seed, and any sampling the mission chooses
    // itself (the rest comes from the engine's presets). Every mission has a
    // seed, derived for it (llm::derived_seed()): its samples and its
    // escalations sample, and a job's memo needs to tell them apart. The
    // army sets model, max_tokens, n, json_schema and tag, so a mission
    // leaves them unset.
    llm::ChatRequest request{};
    int answer_tokens = 0;  // the most an answer may take; a thinking call gets its budget on top
    std::shared_ptr<const json::Schema> schema{};  // a JSON answer in this schema; none: prose
    // The stage's own checks of an answer: the JSON answer, or a prose
    // answer as a JSON string. Runs on the army's threads, and must not call
    // the army.
    llm::AnswerCheck check{};
    // Sent with a failing answer for it to be repaired; "{problems}" becomes
    // its problems. Empty: the default for the answer's form.
    std::string repair_instruction{};
    int repairs = 1;  // times a failing answer is sent back, per sample and per rung
    int samples = 1;  // answers wanted, drawn from one reading of the prompt where they fit
    Priority priority{};
    Escalation escalation{};
    // Whether a failing answer is a refusal (or a softening, which a stage's
    // check can tell): it then goes to another family instead of being
    // repaired. Unset: looks_like_refusal() of its text. A server's content
    // filter is always a refusal.
    std::function<bool(const llm::Choice&)> refusal{};
};

// The default refusal test: a short answer that opens the way refusals do
// ("I'm sorry", "I can't", "As an AI", …). Asked only of an answer that
// already failed its checks, so a valid answer that happens to open that way
// is never mistaken for one.
[[nodiscard]] bool looks_like_refusal(std::string_view answer);

enum class Verdict : std::uint8_t {
    accepted,  // answers passed every check
    invalid,   // the ladder's top was reached without one: a finding
    refused,   // the models declined it: a finding
    ceiling    // a job ceiling stopped it (it may hold answers it had by then)
};

[[nodiscard]] std::string_view to_string(Verdict verdict) noexcept;
// The inverse of to_string(); none for anything else.
[[nodiscard]] std::optional<Verdict> verdict_from_string(std::string_view name) noexcept;

// An answer that passed every check.
struct Answer {
    json::Json value{};    // the JSON answer, or the prose as a JSON string
    llm::Choice choice{};  // as the model gave it: its text, its reasoning, how it ended
    int sample = 0;        // which of the mission's samples (0 on an escalation)
    Step step = Step::as_ordered;
    std::string model{};    // who answered
    bool thinking = false;  // whether the call that gave it thought
    int repairs = 0;        // the calls it took after its first
};

struct Outcome {
    std::string mission{};
    Verdict verdict = Verdict::invalid;
    std::vector<Answer> answers{};  // the accepted answers, by sample
    // What was wrong with the last answer that failed (none if none failed):
    // what a finding reports.
    std::vector<std::string> problems{};
    Step step = Step::as_ordered;  // the last rung climbed
    int rungs = 0;                 // rungs climbed (0: none, a ceiling came first)
    int calls = 0;                 // model calls made, answered from a job's memo too
    llm::Usage usage{};            // over all of them
    std::chrono::milliseconds duration{};
};

}  // namespace rr::agents
