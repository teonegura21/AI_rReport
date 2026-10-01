// rr/llm/chat.hpp — one vocabulary for chat calls, whatever serves the model.
//
// Why this exists
//   Every model runs on the pod (ADR-0017), but not always under the same
//   server: llama-server today, possibly vLLM or SGLang for the infantry on
//   the largest pods (plan §4.5). Code above rr_llm speaks only these types;
//   each provider translates them to its wire format. They say *what* is
//   asked, never *how* a server wants it spelled:
//     * `thinking` is a request, not a template flag: llama-server spells it
//       chat_template_kwargs.enable_thinking, other servers otherwise;
//     * `json_schema` is the model-facing schema's text
//       (json::Schema::model_facing()), its fields in the author's order:
//       the provider turns it into constrained decoding where it can. The
//       answer is validated against the full schema afterwards
//       (rr/llm/structured.hpp), never trusted because a grammar was sent;
//     * `tag` says why a call was made (accounting, logs). It is never sent
//       to the model and is not part of a call's identity (request_key()).
//
//   Every call streams (plan §4.1): RunPod's proxy cuts silent connections,
//   and a streamed answer that ends without its "[DONE]" is detectably
//   incomplete (ADR-0014), where a buffered one could only time out.
//
// Thread-safety: plain values.
#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <rr/core/result.hpp>
#include <rr/json/json.hpp>

namespace rr::llm {

enum class MessageRole : std::uint8_t {
    system,
    user,
    assistant
};

[[nodiscard]] std::string_view to_string(MessageRole role) noexcept;

// One piece of a message's content. Images travel by URL: a data: URL, or an
// rr-blob://<library>/<blake3> reference that the runtime inlines from its
// blob store (so an image crosses the network once, not once per call).
struct ContentPart {
    enum class Kind : std::uint8_t {
        text,
        image_url
    };
    Kind kind = Kind::text;
    std::string value{};  // the text, or the image's URL

    friend bool operator==(const ContentPart&, const ContentPart&) = default;
};

struct Message {
    MessageRole role = MessageRole::user;
    std::vector<ContentPart> content{};  // in order: text and images may interleave

    [[nodiscard]] static Message system(std::string text);
    [[nodiscard]] static Message user(std::string text);
    [[nodiscard]] static Message assistant(std::string text);

    // The text parts, concatenated.
    [[nodiscard]] std::string text() const;

    friend bool operator==(const Message&, const Message&) = default;
};

// How hard a model that thinks should think. llama-server hands the level to
// the model's chat template (Qwen3.8 knows low, medium and xhigh; its default,
// xhigh, over-thinks). Thinking off is ChatRequest::thinking = false, not a
// level.
enum class ReasoningEffort : std::uint8_t {
    low,
    medium,
    high,
    xhigh
};

[[nodiscard]] std::string_view to_string(ReasoningEffort effort) noexcept;
// "low", "medium", "high" or "xhigh"; anything else is none.
[[nodiscard]] std::optional<ReasoningEffort> reasoning_effort_from_string(std::string_view name) noexcept;

// A cap on thinking. After `tokens` tokens of thought, the server ends the
// thinking itself: it writes `closing`, then the template's end tag, and the
// model goes on to answer. So a thinking model always answers, instead of
// spending its whole max_tokens thinking (Gemma 4 can loop there).
struct ThinkingBudget {
    int tokens = 0;
    std::string closing{};  // e.g. "I have to answer now."; empty: the end tag alone

    friend bool operator==(const ThinkingBudget&, const ThinkingBudget&) = default;
};

// Why a call was made. Recorded with the call; never sent.
struct CallTag {
    std::string purpose{};     // e.g. "bench", "atomize", "compose"
    std::string job_id{};      // empty outside a job
    std::string mission_id{};  // empty outside a mission

    friend bool operator==(const CallTag&, const CallTag&) = default;
};

// Sampling fields left unset take the server's defaults, which are not the
// model cards' and change between llama.cpp versions: real calls set them all
// from a preset (rr/llm/sampling.hpp). Ranges are checked before a call is
// sent (sampling_problems()).
struct ChatRequest {
    std::string model{};  // the provider's name for the model ("commander" on the runtime)
    std::vector<Message> messages{};
    std::optional<int> max_tokens{};
    std::optional<double> temperature{};
    std::optional<double> top_p{};
    std::optional<int> top_k{};                // 0: off
    std::optional<double> min_p{};             // 0: off
    std::optional<double> presence_penalty{};  // 0: off
    std::optional<double> repeat_penalty{};    // 1: off
    std::optional<std::int64_t> seed{};
    std::vector<std::string> stop{};
    // Constrained output: the model-facing schema as compact JSON text
    // (json::Schema::model_facing()), and a name for it (some APIs require
    // one: [a-zA-Z0-9_-], at most 64 characters).
    //   Text, not a Json value: Json sorts object members, and their order is
    //   part of what the model is told. llama.cpp builds its grammar from the
    //   properties in the order it reads them, so the model writes its
    //   answer's fields in this text's order (evidence before verdict). The
    //   order is part of the request's identity for the same reason.
    std::optional<std::string> json_schema{};
    std::string schema_name = "answer";
    // Chain of thought for models that can think: on, off, or the model's default.
    std::optional<bool> thinking{};
    // How long and how hard to think. Both need thinking = true.
    std::optional<ThinkingBudget> thinking_budget{};
    std::optional<ReasoningEffort> reasoning_effort{};
    // Fields merged into the request body as they are, for server-specific
    // parameters (llama-server: "cache_prompt", "id_slot"). A JSON object;
    // a key the provider sets itself is refused rather than overwritten.
    json::Json extra = json::Json::object();
    CallTag tag{};

    friend bool operator==(const ChatRequest&, const ChatRequest&) = default;
};

enum class FinishReason : std::uint8_t {
    stop,            // the model ended its answer (or hit a stop sequence)
    length,          // max_tokens or the context ran out: the answer is cut short
    content_filter,  // the server withheld the rest
    other            // anything else a server reports, or none
};

[[nodiscard]] std::string_view to_string(FinishReason reason) noexcept;
// OpenAI's names ("stop", "length", "content_filter"); anything else is `other`.
[[nodiscard]] FinishReason finish_reason_from_string(std::string_view name) noexcept;

struct Usage {
    std::int64_t prompt_tokens = 0;
    std::int64_t completion_tokens = 0;     // reasoning included
    std::int64_t cached_prompt_tokens = 0;  // prompt tokens served from a cache

    friend bool operator==(const Usage&, const Usage&) = default;
};

struct ChatResponse {
    std::string content{};
    std::string reasoning{};  // chain of thought, when the server reports it apart
    FinishReason finish = FinishReason::other;
    Usage usage{};
    std::string model{};  // as the server names it
    // Measured by the client, from sending the request.
    std::optional<std::chrono::milliseconds> first_token{};  // first content or reasoning
    std::chrono::milliseconds duration{};
    // Measured by the server, when it reports them (llama-server's "timings").
    std::optional<double> prompt_tokens_per_second{};
    std::optional<double> generation_tokens_per_second{};
};

// Receives an answer while it is generated (progress, early parsing). Called
// on the thread that called chat(). Every callback is optional.
struct StreamSink {
    std::function<void(std::string_view text)> on_content{};
    std::function<void(std::string_view text)> on_reasoning{};
    // What was received so far is void: the call is being tried again, and
    // the answer will arrive again from its start.
    std::function<void()> on_restart{};
};

// The identity of a call: BLAKE3 over the canonical JSON of everything the
// model sees (model, messages, sampling, schema, thinking, extra), prefixed
// with the provider's name. The tag is left out. Recordings are filed under
// it (rr/llm/replay.hpp), and repeated attempts of one call share it.
[[nodiscard]] std::string request_key(std::string_view provider, const ChatRequest& request);

// The provider-neutral JSON form of a request: what request_key() hashes and
// what recordings store for people to read.
[[nodiscard]] json::Json to_json(const ChatRequest& request);

}  // namespace rr::llm
