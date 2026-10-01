// rr/llm/structured.hpp — JSON answers that satisfy a schema, or an error
// that says why they do not.
//
// Why this exists
//   Missions ask models for data, not prose: atoms, plans, verdicts. Nothing
//   uses an answer before it is validated (plan §8). A grammar sent for
//   constrained decoding narrows what a model can say, but is never taken as
//   proof. structured() puts the steps together:
//     1. The schema's model-facing form (json::Schema::model_facing(), its
//       fields in the order they were written) goes with the request when
//       the provider can use it. Thinking is switched off
//       unless the request says otherwise: a reasoning model under a grammar
//       can spend its whole budget thinking, and some servers constrain the
//       thinking too.
//     2. The answer is parsed (a Markdown code fence around it is tolerated)
//       and validated against the *full* schema, then by the caller's own
//       checks: what a schema cannot say ("every atom cites a sentence that
//       exists"), and the conversion to a C++ type.
//     3. An answer that fails is sent back (once, by default) with its
//       problems listed, for the model to repair. ADR-0007's policy for
//       model_output_invalid: one repair, then a fallback model.
//     4. Still invalid: Errc::model_output_invalid listing every problem, so
//       the caller can escalate to another model.
//   Each attempt is an ordinary provider call: accounted, retried and
//   replayable like any other.
//
//   The steps are public too (structured_request(), check_structured(),
//   repair_request()), for a caller that runs the loop itself: rr_agents'
//   Army gives each attempt its own engine slot, and may take an answer that
//   cannot be repaired to another model. The loop is then written once, and
//   only the scheduling differs.
//
// Thread-safety: reentrant; as thread-safe as the provider.
#pragma once

#include <functional>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <rr/core/assert.hpp>
#include <rr/core/result.hpp>
#include <rr/json/schema.hpp>
#include <rr/llm/chat.hpp>
#include <rr/llm/provider.hpp>

namespace rr::llm {

// Problems the caller finds in an answer the schema accepted (none: accepted).
using AnswerCheck = std::function<std::vector<std::string>(const json::Json& answer)>;

// Sent (as the user) with an answer to repair; "{problems}" becomes the list
// of problems, one per line.
inline constexpr std::string_view kRepairInstruction =
    "Your answer cannot be used. Problems:\n{problems}\n"
    "Reply with the corrected JSON only, and nothing else.";

struct StructuredOptions {
    int repairs = 1;  // times an unacceptable answer is sent back to be fixed
    AnswerCheck check{};
    std::string repair_instruction{kRepairInstruction};
};

struct StructuredAnswer {
    json::Json value{};       // satisfies the schema and the checks
    ChatResponse response{};  // the last call's; its content is the raw answer
    int repairs = 0;          // repairs it took
};

// An answer, read: its value, or what is wrong with it.
struct CheckedAnswer {
    json::Json value{};                   // the answer, when it has no problem
    std::vector<std::string> problems{};  // what is wrong with it (empty: nothing)

    [[nodiscard]] bool usable() const noexcept { return problems.empty(); }
};

// Step 1: the request to send for an answer in `schema`. It carries the
// schema's model-facing form when the provider can constrain decoding, and
// thinking is off unless the request says otherwise.
[[nodiscard]] ChatRequest structured_request(ChatRequest request, const json::Schema& schema,
                                             ProviderCaps caps);

// Step 2: `answer` read as JSON (a Markdown code fence around it is
// tolerated), checked against `schema`, then by `check` (which may be empty).
// An answer with problems that was cut short says so: the cut is likely why.
[[nodiscard]] CheckedAnswer check_structured(const Choice& answer, const json::Schema& schema,
                                             const AnswerCheck& check);

// Step 3: `request` continued with `answer` and the instruction to repair it
// ("{problems}" in `instruction` becomes the problems, one per line).
[[nodiscard]] ChatRequest repair_request(ChatRequest request, const Choice& answer,
                                         const std::vector<std::string>& problems,
                                         std::string_view instruction);

// Asks for an answer satisfying `schema` (see the header comment).
[[nodiscard]] Result<StructuredAnswer> structured_json(const LlmProvider& provider, ChatRequest request,
                                                       const json::Schema& schema, const StreamSink& sink,
                                                       const std::stop_token& stop,
                                                       const StructuredOptions& options = {});

template <class T>
struct Structured {
    T value;
    ChatResponse response{};
    int repairs = 0;
};

// structured_json(), then `convert` (callable as Result<T>(const json::Json&)).
// A conversion failure is a problem like any other: its message goes back to
// the model with the answer.
template <class T, class Convert>
[[nodiscard]] Result<Structured<T>> structured(const LlmProvider& provider, ChatRequest request,
                                               const json::Schema& schema, Convert convert,
                                               const StreamSink& sink, const std::stop_token& stop,
                                               const StructuredOptions& options = {}) {
    std::optional<T> converted;
    StructuredOptions converting = options;
    converting.check = [&](const json::Json& answer) {
        std::vector<std::string> problems =
            options.check ? options.check(answer) : std::vector<std::string>{};
        if (problems.empty()) {
            Result<T> value = convert(answer);
            if (value.has_value()) {
                converted = std::move(*value);
            } else {
                problems.push_back(value.error().message());
            }
        }
        return problems;
    };
    RR_TRY_ASSIGN(StructuredAnswer answer,
                  structured_json(provider, std::move(request), schema, sink, stop, converting));
    RR_ASSERT(converted.has_value(), "an accepted answer was converted");
    Structured<T> result{.value = std::move(*converted)};
    result.response = std::move(answer.response);
    result.repairs = answer.repairs;
    return result;
}

}  // namespace rr::llm
