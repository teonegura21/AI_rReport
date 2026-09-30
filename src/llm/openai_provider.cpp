#include <rr/llm/openai_provider.hpp>

#include <format>
#include <utility>

#include <rr/core/strings.hpp>
#include <rr/net/http.hpp>
#include <rr/net/sse.hpp>

namespace rr::llm {

namespace {

using Clock = std::chrono::steady_clock;
using std::chrono::milliseconds;

// A message's content: a plain string when it is one piece of text (what
// every OpenAI-compatible server understands), else an array of parts.
json::Json content_json(const Message& message) {
    if (message.content.size() == 1 && message.content[0].kind == ContentPart::Kind::text) {
        return message.content[0].value;
    }
    json::Json parts = json::Json::array();
    for (const ContentPart& part : message.content) {
        if (part.kind == ContentPart::Kind::text) {
            parts.push_back({{"type", "text"}, {"text", part.value}});
        } else {
            parts.push_back({{"type", "image_url"}, {"image_url", {{"url", part.value}}}});
        }
    }
    return parts;
}

bool has_images(const ChatRequest& request) {
    for (const Message& message : request.messages) {
        for (const ContentPart& part : message.content) {
            if (part.kind == ContentPart::Kind::image_url) {
                return true;
            }
        }
    }
    return false;
}

// A string member, if present.
std::optional<std::string_view> string_at(const json::Json& object, const char* key) {
    const auto found = object.find(key);
    if (found == object.end() || !found->is_string()) {
        return std::nullopt;
    }
    return std::string_view{found->get_ref<const std::string&>()};
}

std::int64_t integer_at(const json::Json& object, const char* key) {
    const auto found = object.find(key);
    return found != object.end() && found->is_number_integer() ? found->get<std::int64_t>() : 0;
}

std::optional<double> number_at(const json::Json& object, const char* key) {
    const auto found = object.find(key);
    return found != object.end() && found->is_number() ? std::optional{found->get<double>()} : std::nullopt;
}

void read_usage(const json::Json& usage, Usage& into) {
    into.prompt_tokens = integer_at(usage, "prompt_tokens");
    into.completion_tokens = integer_at(usage, "completion_tokens");
    if (const auto details = usage.find("prompt_tokens_details");
        details != usage.end() && details->is_object()) {
        into.cached_prompt_tokens = integer_at(*details, "cached_tokens");
    }
}

}  // namespace

Result<json::Json> chat_request_body(const ChatRequest& request, Dialect dialect, const ProviderCaps& caps) {
    if (has_images(request) && !caps.images) {
        return fail(Errc::unsupported, std::format("'{}' takes no images", request.model));
    }
    json::Json messages = json::Json::array();
    for (const Message& message : request.messages) {
        messages.push_back({{"role", to_string(message.role)}, {"content", content_json(message)}});
    }
    json::Json body{{"model", request.model},
                    {"messages", std::move(messages)},
                    {"stream", true},
                    {"stream_options", {{"include_usage", true}}}};
    if (request.max_tokens.has_value()) {
        body["max_tokens"] = *request.max_tokens;
    }
    if (request.temperature.has_value()) {
        body["temperature"] = *request.temperature;
    }
    if (request.top_p.has_value()) {
        body["top_p"] = *request.top_p;
    }
    if (request.seed.has_value()) {
        body["seed"] = *request.seed;
    }
    if (!request.stop.empty()) {
        body["stop"] = request.stop;
    }
    if (request.json_schema.has_value()) {
        if (!caps.json_schema) {
            return fail(Errc::unsupported,
                        std::format("'{}' cannot be held to a JSON schema", request.model));
        }
        // OpenAI's shape: the one llama-server reads the schema from.
        body["response_format"] = {
            {"type", "json_schema"},
            {"json_schema",
             {{"name", request.schema_name}, {"schema", *request.json_schema}, {"strict", true}}}};
    }
    if (request.thinking.has_value()) {
        if (dialect != Dialect::llama_server || !caps.thinking) {
            return fail(
                Errc::unsupported,
                std::format("thinking cannot be switched for '{}' here; say it in `extra`", request.model));
        }
        body["chat_template_kwargs"] = {{"enable_thinking", *request.thinking}};
        if (!*request.thinking) {
            body["reasoning_effort"] = "none";  // honoured by templates that ignore the flag above
        }
    }
    if (!request.extra.is_null()) {
        if (!request.extra.is_object()) {
            return fail(Errc::invalid_argument, "a request's `extra` must be a JSON object");
        }
        for (const auto& [key, value] : request.extra.items()) {
            if (body.contains(key)) {
                return fail(Errc::invalid_argument,
                            std::format("`extra` may not set '{}', which the request itself sets", key));
            }
            body[key] = value;
        }
    }
    return body;
}

Result<void> ChatStreamReader::on_event(std::string_view data) {
    if (data == "[DONE]") {
        done_ = true;
        return ok();
    }
    if (auto error = net::error_from_event(data); error.has_value()) {
        return fail(std::move(*error));
    }
    auto parsed = json::parse(data);
    if (!parsed.has_value() || !parsed->is_object()) {
        return fail(Errc::unsupported,
                    std::format("the server streamed a chunk that is not a JSON object: {}",
                                std::string{trim(data).substr(0, 200)}));
    }
    const json::Json& chunk = *parsed;
    if (const auto model = string_at(chunk, "model"); model.has_value() && response_.model.empty()) {
        response_.model = std::string{*model};
    }
    if (const auto choices = chunk.find("choices");
        choices != chunk.end() && choices->is_array() && !choices->empty()) {
        read_choice(choices->front());
    }
    if (const auto usage = chunk.find("usage"); usage != chunk.end() && usage->is_object()) {
        read_usage(*usage, response_.usage);
    }
    if (const auto timings = chunk.find("timings"); timings != chunk.end() && timings->is_object()) {
        response_.prompt_tokens_per_second = number_at(*timings, "prompt_per_second");
        response_.generation_tokens_per_second = number_at(*timings, "predicted_per_second");
    }
    return ok();
}

void ChatStreamReader::read_choice(const json::Json& choice) {
    if (const auto delta = choice.find("delta"); delta != choice.end() && delta->is_object()) {
        deliver(string_at(*delta, "reasoning_content"), response_.reasoning, sink_->on_reasoning);
        deliver(string_at(*delta, "content"), response_.content, sink_->on_content);
    }
    if (const auto finish = string_at(choice, "finish_reason"); finish.has_value()) {
        response_.finish = finish_reason_from_string(*finish);
    }
}

void ChatStreamReader::deliver(std::optional<std::string_view> piece, std::string& answer,
                               const std::function<void(std::string_view)>& listener) {
    if (!piece.has_value() || piece->empty()) {  // the role chunk's content is null
        return;
    }
    if (!any_token_) {
        any_token_ = true;
        if (on_first_token_) {
            on_first_token_();
        }
    }
    answer += *piece;
    if (listener) {
        listener(*piece);
    }
}

OpenAiProvider::OpenAiProvider(const net::HttpClient& http, OpenAiOptions options)
    : http_(&http)
    , options_(std::move(options)) {
    while (options_.base_url.ends_with('/')) {
        options_.base_url.pop_back();
    }
}

Result<ChatResponse> OpenAiProvider::chat(const ChatRequest& request, const StreamSink& sink,
                                          const std::stop_token& stop) const {
    RR_TRY_ASSIGN(const json::Json body, chat_request_body(request, options_.dialect, options_.caps));
    net::HttpClient::Request call{.method = net::Method::post,
                                  .url = options_.base_url + "/v1/chat/completions",
                                  .body = json::dump(body),
                                  .timeout = options_.call_timeout,
                                  .idle_timeout = options_.idle_timeout};
    call.headers.set("Content-Type", "application/json");
    call.headers.set("Accept", "text/event-stream");
    if (!options_.api_key.empty()) {
        call.headers.set("Authorization", std::format("Bearer {}", options_.api_key.expose()));
    }

    const auto started = Clock::now();
    std::optional<milliseconds> first_token;
    ChatStreamReader reader{
        sink, [&] { first_token = std::chrono::duration_cast<milliseconds>(Clock::now() - started); }};
    net::SseParser parser;
    const auto context = std::format("while calling '{}' at {}", request.model, options_.name);
    auto response = http_->stream(
        call,
        {.on_head = [&](int status, const net::Headers& headers) -> Result<void> {
             const bool success = status >= 200 && status < 300;
             if (success && !headers.get("Content-Type").value_or("").starts_with("text/event-stream")) {
                 return fail(Errc::unsupported,
                             std::format("the server answered with '{}', not an event stream",
                                         headers.get("Content-Type").value_or("no content type")));
             }
             return ok();
         },
         .on_chunk =
             [&](std::string_view chunk) {
                 return parser.feed(chunk,
                                    [&](const net::SseEvent& event) { return reader.on_event(event.data); });
             }},
        stop);
    RR_TRY(with_context(net::expect_success(std::move(response)), context));
    if (!reader.done()) {
        return fail(
            Errc::network,
            std::format("the answer ended without its [DONE], so it may be incomplete ({})", context));
    }
    ChatResponse answer = reader.take();
    answer.first_token = first_token;
    answer.duration = std::chrono::duration_cast<milliseconds>(Clock::now() - started);
    return answer;
}

}  // namespace rr::llm
