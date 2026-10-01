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
//   What llama-server sends (llama.cpp db00347): a role chunk whose content
//   is null; delta.reasoning_content while a model thinks (its chain of
//   thought, kept apart); delta.content; a finish chunk; with
//   stream_options.include_usage, a last chunk with "choices": [] and the
//   usage; "timings" on the last JSON chunk.
//
//   Dialects differ in two extensions. llama-server switches thinking with
//   chat_template_kwargs.enable_thinking (and reasoning_effort "none" to be
//   sure it is off) and reports "timings". A plain OpenAI-compatible API gets
//   neither: a request that sets `thinking` is refused there, since the
//   extension that would carry it is server-specific (use `extra`).
//
// Thread-safety: thread-safe (the HttpClient is).
#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

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

// Assembles a streamed answer from its events' data.
class ChatStreamReader {
public:
    // `on_first_token` runs once, at the first non-empty content or reasoning.
    explicit ChatStreamReader(const StreamSink& sink, std::function<void()> on_first_token = {})
        : sink_(&sink)
        , on_first_token_(std::move(on_first_token)) {}

    // One event. Fails with the error an error event carries, or
    // Errc::unsupported for a chunk that is not JSON.
    [[nodiscard]] Result<void> on_event(std::string_view data);

    [[nodiscard]] bool done() const noexcept { return done_; }  // "[DONE]" arrived
    [[nodiscard]] ChatResponse take() { return std::move(response_); }

private:
    void read_choice(const json::Json& choice);
    // One piece of text, reasoning or content: the first token if it is one,
    // kept in `answer`, and passed to `listener`.
    void deliver(std::optional<std::string_view> piece, std::string& answer,
                 const std::function<void(std::string_view)>& listener);

    const StreamSink* sink_;
    std::function<void()> on_first_token_;
    ChatResponse response_{};
    bool done_ = false;
    bool any_token_ = false;
};

}  // namespace rr::llm
