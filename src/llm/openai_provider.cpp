#include <rr/llm/openai_provider.hpp>

#include <algorithm>
#include <format>
#include <utility>

#include <rr/core/strings.hpp>
#include <rr/llm/sampling.hpp>
#include <rr/net/http.hpp>
#include <rr/net/sse.hpp>

namespace rr::llm {

namespace {

using Clock = std::chrono::steady_clock;
using std::chrono::milliseconds;

// A message's content: a plain string when it is one piece of text (what
// every OpenAI-compatible server understands), else an array of parts.
json::OrderedJson content_json(const Message& message) {
    if (message.content.size() == 1 && message.content[0].kind == ContentPart::Kind::text) {
        return message.content[0].value;
    }
    json::OrderedJson parts = json::OrderedJson::array();
    for (const ContentPart& part : message.content) {
        if (part.kind == ContentPart::Kind::text) {
            parts.push_back({{"type", "text"}, {"text", part.value}});
        } else {
            parts.push_back({{"type", "image_url"}, {"image_url", {{"url", part.value}}}});
        }
    }
    return parts;
}

// The fields only llama-server reads, as the request sets them. A plain
// OpenAI-compatible server has no standard field for these.
std::vector<std::string_view> llama_server_fields(const ChatRequest& request) {
    std::vector<std::string_view> fields;
    if (request.top_k.has_value()) {
        fields.emplace_back("top_k");
    }
    if (request.min_p.has_value()) {
        fields.emplace_back("min_p");
    }
    if (request.repeat_penalty.has_value()) {
        fields.emplace_back("repeat_penalty");
    }
    if (request.thinking.has_value()) {
        fields.emplace_back("thinking");
    }
    if (request.thinking_budget.has_value()) {
        fields.emplace_back("thinking_budget");
    }
    if (request.reasoning_effort.has_value()) {
        fields.emplace_back("reasoning_effort");
    }
    return fields;
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

// What the request asks that no server, or not this one, can carry: refused
// before anything is built.
Result<void> check_carriable(const ChatRequest& request, Dialect dialect, const ProviderCaps& caps) {
    if (const std::vector<std::string> problems = sampling_problems(request); !problems.empty()) {
        return fail(Errc::invalid_argument,
                    std::format("a request to '{}' cannot be sent: {}", request.model, join(problems, "; ")));
    }
    if (const std::vector<std::string_view> fields = llama_server_fields(request);
        !fields.empty() && dialect != Dialect::llama_server) {
        return fail(Errc::unsupported,
                    std::format("'{}' here has no standard field for {}; say it in `extra`", request.model,
                                join(fields, ", ")));
    }
    if (has_images(request) && !caps.images) {
        return fail(Errc::unsupported, std::format("'{}' takes no images", request.model));
    }
    if (request.json_schema.has_value() && !caps.json_schema) {
        return fail(Errc::unsupported, std::format("'{}' cannot be held to a JSON schema", request.model));
    }
    if (request.thinking.has_value() && !caps.thinking) {
        return fail(Errc::unsupported, std::format("'{}' cannot think", request.model));
    }
    return ok();
}

// A field the request sets, under the name the server reads.
template <class T>
void put(json::OrderedJson& body, const char* name, const std::optional<T>& value) {
    if (value.has_value()) {
        body[name] = *value;
    }
}

void add_sampling(const ChatRequest& request, json::OrderedJson& body) {
    put(body, "max_tokens", request.max_tokens);
    put(body, "temperature", request.temperature);
    put(body, "top_p", request.top_p);
    put(body, "top_k", request.top_k);
    put(body, "min_p", request.min_p);
    put(body, "presence_penalty", request.presence_penalty);
    put(body, "repeat_penalty", request.repeat_penalty);
    put(body, "seed", request.seed);
    put(body, "n", request.n);
    if (!request.stop.empty()) {
        body["stop"] = request.stop;
    }
}

Result<void> add_schema(const ChatRequest& request, json::OrderedJson& body) {
    if (!request.json_schema.has_value()) {
        return ok();
    }
    // Parsed in order, so the grammar gets the fields in the order written.
    auto schema = json::parse_ordered(*request.json_schema);
    if (!schema.has_value()) {
        return fail(Errc::invalid_argument, std::format("the schema for '{}' is not JSON: {}", request.model,
                                                        schema.error().message()));
    }
    // OpenAI's shape: the one llama-server reads the schema from.
    body["response_format"] = {
        {"type", "json_schema"},
        {"json_schema", {{"name", request.schema_name}, {"schema", std::move(*schema)}, {"strict", true}}}};
    return ok();
}

void add_thinking(const ChatRequest& request, json::OrderedJson& body) {
    if (!request.thinking.has_value()) {
        return;
    }
    body["chat_template_kwargs"] = {{"enable_thinking", *request.thinking}};
    if (!*request.thinking) {
        body["reasoning_effort"] = "none";  // honoured by templates that ignore the flag above
        return;
    }
    // sampling_problems() let a budget and an effort through only with thinking on.
    if (request.reasoning_effort.has_value()) {
        body["reasoning_effort"] = to_string(*request.reasoning_effort);  // the template's kwarg
    }
    if (request.thinking_budget.has_value()) {
        body["reasoning_budget_tokens"] = request.thinking_budget->tokens;
        if (!request.thinking_budget->closing.empty()) {
            body["reasoning_budget_message"] = request.thinking_budget->closing;
        }
    }
}

// Server-specific fields, as they are. One the request already sets is a
// conflict, refused rather than silently overwritten.
Result<void> add_extra(const ChatRequest& request, json::OrderedJson& body) {
    if (request.extra.is_null()) {
        return ok();
    }
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
    return ok();
}

}  // namespace

Result<json::OrderedJson> chat_request_body(const ChatRequest& request, Dialect dialect,
                                            const ProviderCaps& caps) {
    RR_TRY(check_carriable(request, dialect, caps));
    json::OrderedJson messages = json::OrderedJson::array();
    for (const Message& message : request.messages) {
        messages.push_back({{"role", to_string(message.role)}, {"content", content_json(message)}});
    }
    json::OrderedJson body{{"model", request.model},
                           {"messages", std::move(messages)},
                           {"stream", true},
                           {"stream_options", {{"include_usage", true}}}};
    add_sampling(request, body);
    RR_TRY(add_schema(request, body));
    add_thinking(request, body);
    RR_TRY(add_extra(request, body));
    return body;
}

ChatStreamReader::ChatStreamReader(const StreamSink& sink, std::size_t choices,
                                   std::function<void()> on_first_token)
    : sink_(&sink)
    , on_first_token_(std::move(on_first_token))
    , finished_(std::max<std::size_t>(choices, 1), false)
    , generation_speeds_(finished_.size()) {
    response_.choices.resize(finished_.size());
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
    if (const auto choices = chunk.find("choices"); choices != chunk.end() && choices->is_array()) {
        for (const json::Json& choice : *choices) {
            RR_TRY(read_choice(choice));
        }
    }
    read_report(chunk);
    return ok();
}

Result<void> ChatStreamReader::read_choice(const json::Json& choice) {
    // A server may leave the index out when there is one choice. JSON's
    // parser keeps every whole number >= 0 as unsigned: anything else (a
    // negative or fractional number, a string) is no index.
    std::size_t index = 0;
    if (const auto found = choice.find("index"); found != choice.end()) {
        if (!found->is_number_unsigned()) {
            return fail(
                Errc::unsupported,
                std::format("the server streamed a choice index that is not one: {}", json::dump(*found)));
        }
        index = found->get<std::size_t>();
    }
    if (index >= finished_.size()) {
        return fail(Errc::unsupported, std::format("the server streamed choice {} of a request for {}", index,
                                                   finished_.size()));
    }
    Choice& answer = response_.choices[index];
    if (const auto delta = choice.find("delta"); delta != choice.end() && delta->is_object()) {
        deliver(index, string_at(*delta, "reasoning_content"), answer.reasoning, sink_->on_reasoning);
        deliver(index, string_at(*delta, "content"), answer.content, sink_->on_content);
    }
    if (const auto finish = string_at(choice, "finish_reason"); finish.has_value()) {
        answer.finish = finish_reason_from_string(*finish);
        finished_[index] = true;
        last_finished_ = index;
    }
    return ok();
}

// Usage and timings. llama-server writes a choice's finish chunk and its
// usage chunk ("choices": []) together, with the timings on the last of
// them: both belong to the choice that just finished.
void ChatStreamReader::read_report(const json::Json& chunk) {
    if (const auto usage = chunk.find("usage"); usage != chunk.end() && usage->is_object()) {
        UsageReport report{.choice = last_finished_, .usage = {}};
        read_usage(*usage, report.usage);
        usage_reports_.push_back(report);
    }
    if (const auto timings = chunk.find("timings"); timings != chunk.end() && timings->is_object()) {
        const std::size_t choice = last_finished_.value_or(0);
        generation_speeds_[choice] = number_at(*timings, "predicted_per_second");
        if (!prompt_speed_.has_value() || choice == 0) {
            prompt_speed_ = number_at(*timings, "prompt_per_second");  // choice 0 read the prompt
        }
    }
}

void ChatStreamReader::deliver(std::size_t choice, std::optional<std::string_view> piece, std::string& answer,
                               const std::function<void(std::size_t, std::string_view)>& listener) {
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
        listener(choice, *piece);
    }
}

Result<ChatResponse> ChatStreamReader::take() {
    for (std::size_t index = 0; index < finished_.size(); ++index) {
        if (!finished_[index]) {
            return fail(Errc::unsupported,
                        std::format("the server ended the stream with choice {} of {} unfinished", index,
                                    finished_.size()));
        }
    }
    // One report per choice (llama-server): each choice's own tokens; the
    // prompt counts once (each choice reports the same, copied prompt), and
    // the completions add up. Anything else (a server that reports once, for
    // the whole call): the last report is the call's.
    std::vector<bool> reported(finished_.size(), false);
    bool per_choice = usage_reports_.size() == finished_.size();
    for (const UsageReport& report : usage_reports_) {
        per_choice = per_choice && report.choice.has_value() && !reported[*report.choice];
        if (report.choice.has_value()) {
            reported[*report.choice] = true;
        }
    }
    if (per_choice) {
        for (const UsageReport& report : usage_reports_) {
            if (report.choice.has_value()) {  // always, when each report is a choice's
                response_.choices[*report.choice].completion_tokens = report.usage.completion_tokens;
            }
            response_.usage.prompt_tokens =
                std::max(response_.usage.prompt_tokens, report.usage.prompt_tokens);
            response_.usage.cached_prompt_tokens =
                std::max(response_.usage.cached_prompt_tokens, report.usage.cached_prompt_tokens);
            response_.usage.completion_tokens += report.usage.completion_tokens;
        }
    } else if (!usage_reports_.empty()) {
        response_.usage = usage_reports_.back().usage;
    }
    response_.prompt_tokens_per_second = prompt_speed_;
    for (const std::optional<double>& speed : generation_speeds_) {
        if (speed.has_value()) {
            response_.generation_tokens_per_second =
                response_.generation_tokens_per_second.value_or(0.0) + *speed;
        }
    }
    return std::move(response_);
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
    // OrderedJson to the end: a Json would sort the schema's fields again.
    RR_TRY_ASSIGN(const json::OrderedJson body, chat_request_body(request, options_.dialect, options_.caps));
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
    // n was checked by chat_request_body() (sampling_problems(): n >= 1).
    ChatStreamReader reader{
        sink, static_cast<std::size_t>(request.n.value_or(1)),
        [&] { first_token = std::chrono::duration_cast<milliseconds>(Clock::now() - started); }};
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
    RR_TRY_ASSIGN(ChatResponse answer, with_context(reader.take(), context));
    answer.first_token = first_token;
    answer.duration = std::chrono::duration_cast<milliseconds>(Clock::now() - started);
    return answer;
}

}  // namespace rr::llm
