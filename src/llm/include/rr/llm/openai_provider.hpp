// rr/llm/openai_provider.hpp — chat over the OpenAI-compatible API, streamed.
//
// Why this exists
//   llama-server (behind the runtime's relay) and the other local inference
//   servers (vLLM, SGLang) speak OpenAI's chat completions API. Every model
//   call goes to the pod's runtime (ADR-0017). This provider speaks it, always
//   streaming (plan §4.1), and reads the end of a stream as honestly as the
//   relay writes it (ADR-0014):
//     * a call succeeded only if "[DONE]" arrived. A stream that ends
//       without it, or is cut short, is Errc::network: transient, so the
//       call is tried again instead of a partial answer being trusted;
//     * an error event ({"error": {...}}) is the call's error, in the exact
//       class the engine or the runtime gave it (net::error_from_event());
//     * comment lines (": keep-alive") keep the connection open and are
//       ignored; an idle timeout longer than the runtime's keep-alive
//       interval ends only a call that has really gone silent;
//     * a 2xx answer that is not an event stream is refused
//       (Errc::unsupported) rather than waited out.
//
//   What llama-server sends (llama.cpp 7fe450e): for each choice, a role
//   chunk whose content is null; delta.reasoning_content while the model
//   thinks (its chain of thought, kept apart); delta.content; a finish
//   chunk; and, with stream_options.include_usage, a chunk with
//   "choices": [] and that choice's usage. "timings" ride on a choice's last
//   chunk. Every choice chunk carries its "index"; with n choices their
//   chunks interleave, but one choice's finish and usage chunks are written
//   together, so a usage chunk belongs to the choice that just finished.
//   One "[DONE]" ends the stream.
//
//   Dialects differ in their extensions. llama-server reads top_k, min_p,
//   repeat_penalty and the thinking fields, and reports "timings". A plain
//   OpenAI-compatible API gets none of them: a request that sets one is
//   refused there, since the field that would carry it is server-specific
//   (use `extra`).
//
// Thread-safety: thread-safe (the HttpClient is).
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <rr/core/result.hpp>
#include <rr/core/secret.hpp>
#include <rr/json/json.hpp>
#include <rr/llm/chat.hpp>
#include <rr/llm/provider.hpp>
#include <rr/net/http_client.hpp>

namespace rr::llm {

enum class Dialect : std::uint8_t {
    llama_server,
    openai
};

struct OpenAiOptions {
    std::string name = "openai";  // the provider's name in records and request keys
    std::string base_url{};       // calls go to <base_url>/v1/chat/completions
    Secret api_key{};             // sent as "Authorization: Bearer"; none if empty
    Dialect dialect = Dialect::llama_server;
    ProviderCaps caps{};
    // No byte for this long: the call has failed. The runtime sends a
    // keep-alive every 15 s, llama-server every 30 s.
    std::chrono::milliseconds idle_timeout{std::chrono::seconds{60}};
    std::optional<std::chrono::milliseconds> call_timeout{};  // the whole call
};

class OpenAiProvider final : public LlmProvider {
public:
    // `http` must outlive the provider.
    OpenAiProvider(const net::HttpClient& http, OpenAiOptions options);

    [[nodiscard]] std::string_view name() const noexcept override { return options_.name; }
    [[nodiscard]] ProviderCaps caps() const noexcept override { return options_.caps; }
    [[nodiscard]] Result<ChatResponse> chat(const ChatRequest& request, const StreamSink& sink,
                                            const std::stop_token& stop) const override;

private:
    const net::HttpClient* http_;
    OpenAiOptions options_;
};

// The request body as sent. An OrderedJson, so the schema inside keeps its
// fields' order on the wire (ChatRequest::json_schema). Errc::unsupported for
// what the provider cannot carry (images, a schema, thinking);
// Errc::invalid_argument for a schema that is not JSON, or an `extra` that is
// not an object or would override a field set here.
[[nodiscard]] Result<json::OrderedJson> chat_request_body(const ChatRequest& request, Dialect dialect,
                                                          const ProviderCaps& caps);

// Assembles a streamed answer, every choice of it, from its events' data.
//
// Token probabilities (ChatRequest::token_probabilities) come as
// llama-server sends them: on a chunk, under "logprobs": {"content": [...]},
// an entry a token with its bytes and its "prob" (after the sampler chain)
// or "logprob" (before it). A chunk's entries are its last tokens, so they
// are placed at the end of the content the chunk adds, one after another;
// a token whose bytes reach back past that content (part of its text went
// elsewhere, a tag the server read) is cut where the content began. Entries
// on a chunk that adds no content (reasoning, a role) are not kept.
class ChatStreamReader {
public:
    // `choices`: how many answers the request asked for (n, at least 1).
    // `on_first_token` runs once, at the first non-empty content or reasoning
    // of any choice.
    ChatStreamReader(const StreamSink& sink, std::size_t choices, std::function<void()> on_first_token = {});

    // One event. Fails with the error an error event carries, or
    // Errc::unsupported for a chunk that is not JSON or names a choice the
    // request did not ask for.
    [[nodiscard]] Result<void> on_event(std::string_view data);

    [[nodiscard]] bool done() const noexcept { return done_; }  // "[DONE]" arrived

    // The answer: every choice, the call's usage and speeds. Errc::unsupported
    // if a choice never finished, for the stream then lacks part of it.
    [[nodiscard]] Result<ChatResponse> take();

private:
    // A usage chunk, and the choice that finished just before it.
    struct UsageReport {
        std::optional<std::size_t> choice;
        Usage usage;
    };

    [[nodiscard]] Result<void> read_choice(const json::Json& choice);
    // The probabilities `choice` carries, of the tokens of `answer`'s content
    // from `begin` on: the piece this chunk added.
    [[nodiscard]] static Result<void> read_probabilities(const json::Json& choice, std::size_t begin,
                                                         Choice& answer);
    void read_report(const json::Json& chunk);
    // One piece of text, reasoning or content: the first token if it is one,
    // kept in `answer`, and passed to `listener` with its choice.
    void deliver(std::size_t choice, std::optional<std::string_view> piece, std::string& answer,
                 const std::function<void(std::size_t, std::string_view)>& listener);

    const StreamSink* sink_;
    std::function<void()> on_first_token_;
    ChatResponse response_{};
    std::vector<bool> finished_;
    std::optional<std::size_t> last_finished_{};
    std::vector<UsageReport> usage_reports_{};
    std::vector<std::optional<double>> generation_speeds_;  // by choice
    std::optional<double> prompt_speed_{};
    bool done_ = false;
    bool any_token_ = false;
};

}  // namespace rr::llm
