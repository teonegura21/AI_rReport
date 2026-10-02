#include <rr/llm/chat.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <utility>

#include <rr/core/assert.hpp>

namespace rr::llm {

namespace {

constexpr std::array<std::string_view, 3> kRoleNames{"system", "user", "assistant"};
constexpr std::array<std::string_view, 4> kFinishNames{"stop", "length", "content_filter", "other"};
constexpr std::array<std::string_view, 4> kEffortNames{"low", "medium", "high", "xhigh"};

Message text_message(MessageRole role, std::string text) {
    return Message{.role = role,
                   .content = {ContentPart{.kind = ContentPart::Kind::text, .value = std::move(text)}}};
}

}  // namespace

std::string_view to_string(MessageRole role) noexcept {
    const auto index = static_cast<std::size_t>(role);
    return index < kRoleNames.size() ? kRoleNames[index] : std::string_view{"unknown"};
}

Message Message::system(std::string text) {
    return text_message(MessageRole::system, std::move(text));
}

Message Message::user(std::string text) {
    return text_message(MessageRole::user, std::move(text));
}

Message Message::assistant(std::string text) {
    return text_message(MessageRole::assistant, std::move(text));
}

std::string Message::text() const {
    std::string joined;
    for (const ContentPart& part : content) {
        if (part.kind == ContentPart::Kind::text) {
            joined += part.value;
        }
    }
    return joined;
}

std::string_view to_string(FinishReason reason) noexcept {
    const auto index = static_cast<std::size_t>(reason);
    return index < kFinishNames.size() ? kFinishNames[index] : std::string_view{"other"};
}

std::string_view to_string(ReasoningEffort effort) noexcept {
    const auto index = static_cast<std::size_t>(effort);
    return index < kEffortNames.size() ? kEffortNames[index] : std::string_view{"unknown"};
}

std::optional<ReasoningEffort> reasoning_effort_from_string(std::string_view name) noexcept {
    for (std::size_t index = 0; index < kEffortNames.size(); ++index) {
        if (kEffortNames[index] == name) {
            return static_cast<ReasoningEffort>(index);
        }
    }
    return std::nullopt;
}

const Choice& ChatResponse::first() const {
    RR_ASSERT(!choices.empty(), "a response has at least one choice");
    return choices.front();
}

Choice& ChatResponse::first() {
    RR_ASSERT(!choices.empty(), "a response has at least one choice");
    return choices.front();
}

FinishReason ChatResponse::least_complete_finish() const noexcept {
    // From complete to least complete.
    constexpr std::array<FinishReason, 4> kByCompleteness{FinishReason::stop, FinishReason::other,
                                                          FinishReason::content_filter, FinishReason::length};
    const auto rank = [&](FinishReason reason) {
        return std::ranges::find(kByCompleteness, reason) - kByCompleteness.begin();
    };
    FinishReason least = FinishReason::stop;
    for (const Choice& choice : choices) {
        if (rank(choice.finish) > rank(least)) {
            least = choice.finish;
        }
    }
    return choices.empty() ? FinishReason::other : least;
}

FinishReason finish_reason_from_string(std::string_view name) noexcept {
    if (name == "stop") {
        return FinishReason::stop;
    }
    if (name == "length") {
        return FinishReason::length;
    }
    if (name == "content_filter") {
        return FinishReason::content_filter;
    }
    return FinishReason::other;
}

// Unset fields are left out, so a field added to ChatRequest later changes no
// existing request's key (and no recording goes stale because of it).
json::Json to_json(const ChatRequest& request) {
    json::Json messages = json::Json::array();
    for (const Message& message : request.messages) {
        json::Json parts = json::Json::array();
        for (const ContentPart& part : message.content) {
            parts.push_back(part.kind == ContentPart::Kind::text
                                ? json::Json{{"type", "text"}, {"text", part.value}}
                                : json::Json{{"type", "image_url"}, {"url", part.value}});
        }
        messages.push_back({{"role", to_string(message.role)}, {"content", std::move(parts)}});
    }
    json::Json document{{"model", request.model}, {"messages", std::move(messages)}};
    if (request.max_tokens.has_value()) {
        document["max_tokens"] = *request.max_tokens;
    }
    if (request.temperature.has_value()) {
        document["temperature"] = *request.temperature;
    }
    if (request.top_p.has_value()) {
        document["top_p"] = *request.top_p;
    }
    if (request.top_k.has_value()) {
        document["top_k"] = *request.top_k;
    }
    if (request.min_p.has_value()) {
        document["min_p"] = *request.min_p;
    }
    if (request.presence_penalty.has_value()) {
        document["presence_penalty"] = *request.presence_penalty;
    }
    if (request.repeat_penalty.has_value()) {
        document["repeat_penalty"] = *request.repeat_penalty;
    }
    if (request.seed.has_value()) {
        document["seed"] = *request.seed;
    }
    if (request.n.has_value()) {
        document["n"] = *request.n;
    }
    if (!request.stop.empty()) {
        document["stop"] = request.stop;
    }
    if (request.json_schema.has_value()) {
        document["json_schema"] = *request.json_schema;
        document["schema_name"] = request.schema_name;
    }
    if (request.thinking.has_value()) {
        document["thinking"] = *request.thinking;
    }
    if (request.thinking_budget.has_value()) {
        document["thinking_budget"] = {{"tokens", request.thinking_budget->tokens},
                                       {"closing", request.thinking_budget->closing}};
    }
    if (request.reasoning_effort.has_value()) {
        document["reasoning_effort"] = to_string(*request.reasoning_effort);
    }
    if (request.token_probabilities) {
        document["token_probabilities"] = true;
    }
    if (!request.extra.is_null() && !request.extra.empty()) {
        document["extra"] = request.extra;
    }
    return document;
}

namespace {

// A choice, with its token probabilities when it has them, as [begin, end,
// probability] triples: compact, for the many tokens of an answer.
json::Json choice_to_json(const Choice& choice) {
    json::Json document{{"content", choice.content},
                        {"reasoning", choice.reasoning},
                        {"finish_reason", to_string(choice.finish)},
                        {"completion_tokens", choice.completion_tokens}};
    if (!choice.probabilities.empty()) {
        json::Json probabilities = json::Json::array();
        for (const TokenProbability& token : choice.probabilities) {
            probabilities.push_back(json::Json::array({token.begin, token.end, token.probability}));
        }
        document["probabilities"] = std::move(probabilities);
    }
    return document;
}

}  // namespace

json::Json to_json(const ChatResponse& response) {
    json::Json choices = json::Json::array();
    for (const Choice& choice : response.choices) {
        choices.push_back(choice_to_json(choice));
    }
    return {{"choices", std::move(choices)},
            {"model", response.model},
            {"usage",
             {{"prompt_tokens", response.usage.prompt_tokens},
              {"completion_tokens", response.usage.completion_tokens},
              {"cached_prompt_tokens", response.usage.cached_prompt_tokens}}}};
}

namespace {

// One [begin, end, probability] triple of `content`'s tokens, in order after
// `previous_end`.
Result<TokenProbability> token_from_json(const json::Json& triple, std::size_t previous_end,
                                         std::size_t content_size) {
    if (!triple.is_array() || triple.size() != 3 || !triple[0].is_number_unsigned() ||
        !triple[1].is_number_unsigned() || !triple[2].is_number()) {
        return fail(
            Errc::invalid_argument,
            std::format("a token's probability is [begin, end, probability], not {}", json::dump(triple)));
    }
    const TokenProbability token{.begin = triple[0].get<std::size_t>(),
                                 .end = triple[1].get<std::size_t>(),
                                 .probability = triple[2].get<double>()};
    if (token.begin < previous_end || token.end <= token.begin || token.end > content_size ||
        std::isnan(token.probability) || token.probability < 0 || token.probability > 1) {
        return fail(Errc::invalid_argument,
                    std::format("a token's probability {} is not of the content's next bytes, with a "
                                "probability in [0, 1]",
                                json::dump(triple)));
    }
    return token;
}

Result<Choice> choice_from_json(const json::Json& document) {
    Choice choice;
    RR_TRY_ASSIGN(choice.content, json::get_string(document, "content"));
    RR_TRY_ASSIGN(choice.reasoning, json::get_string(document, "reasoning"));
    RR_TRY_ASSIGN(const std::string finish, json::get_string(document, "finish_reason"));
    choice.finish = finish_reason_from_string(finish);
    RR_TRY_ASSIGN(choice.completion_tokens, json::get_int(document, "completion_tokens"));
    if (const auto probabilities = document.find("probabilities"); probabilities != document.end()) {
        if (!probabilities->is_array()) {
            return fail(Errc::invalid_argument, "'probabilities' must be a list of a choice's tokens");
        }
        std::size_t previous_end = 0;
        for (const json::Json& triple : *probabilities) {
            RR_TRY_ASSIGN(const TokenProbability token,
                          token_from_json(triple, previous_end, choice.content.size()));
            previous_end = token.end;
            choice.probabilities.push_back(token);
        }
    }
    return choice;
}

}  // namespace

Result<ChatResponse> response_from_json(const json::Json& document) {
    ChatResponse response;
    RR_TRY_ASSIGN(const json::Json* choices, json::find(document, "choices"));
    if (!choices->is_array() || choices->empty()) {
        return fail(Errc::invalid_argument, "'choices' must be a list of at least one answer");
    }
    for (const json::Json& entry : *choices) {
        RR_TRY_ASSIGN(Choice choice, choice_from_json(entry));
        response.choices.push_back(std::move(choice));
    }
    RR_TRY_ASSIGN(response.model, json::get_string(document, "model"));
    RR_TRY_ASSIGN(response.usage.prompt_tokens, json::get_int(document, "usage.prompt_tokens"));
    RR_TRY_ASSIGN(response.usage.completion_tokens, json::get_int(document, "usage.completion_tokens"));
    RR_TRY_ASSIGN(response.usage.cached_prompt_tokens, json::get_int(document, "usage.cached_prompt_tokens"));
    return response;
}

std::string request_key(std::string_view provider, const ChatRequest& request) {
    return json::content_hash(json::Json{{"provider", provider}, {"request", to_json(request)}}).hex();
}

}  // namespace rr::llm
