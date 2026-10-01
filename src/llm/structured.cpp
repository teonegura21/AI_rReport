#include <rr/llm/structured.hpp>

#include <format>
#include <string_view>

#include <rr/core/strings.hpp>

namespace rr::llm {

namespace {

// The JSON in an answer: all of it, or what a Markdown code fence around it
// holds ("```json\n{...}\n```"), which models without a grammar like to add.
std::string_view unfenced(std::string_view text) {
    text = trim(text);
    if (!text.starts_with("```")) {
        return text;
    }
    const std::size_t body = text.find('\n');
    const std::size_t closing = text.rfind("```");
    if (body == std::string_view::npos || closing <= body) {
        return text;
    }
    return trim(text.substr(body + 1, closing - body - 1));
}

// What is wrong with an answer (nothing: `value` holds it).
std::vector<std::string> problems_with(const ChatResponse& response, const json::Schema& schema,
                                       const AnswerCheck& check, json::Json& value) {
    std::vector<std::string> problems;
    auto parsed = json::parse(unfenced(response.content));
    if (!parsed.has_value()) {
        problems.push_back(std::format("it is not JSON: {}", parsed.error().message()));
    } else {
        for (const json::Violation& violation : schema.check(*parsed)) {
            problems.push_back(std::format("{} ({}): {}",
                                           violation.instance_path.empty() ? "/" : violation.instance_path,
                                           violation.keyword, violation.message));
        }
        if (problems.empty() && check) {
            problems = check(*parsed);
        }
        if (problems.empty()) {
            value = std::move(*parsed);
        }
    }
    if (!problems.empty() && response.finish == FinishReason::length) {
        problems.emplace_back("the answer was cut off at the token limit, so it must be shorter");
    }
    return problems;
}

std::string as_lines(const std::vector<std::string>& problems) {
    std::string lines;
    for (const std::string& problem : problems) {
        if (!lines.empty()) {
            lines += '\n';
        }
        lines += "- ";
        lines += problem;
    }
    return lines;
}

}  // namespace

Result<StructuredAnswer> structured_json(const LlmProvider& provider, ChatRequest request,
                                         const json::Schema& schema, const StreamSink& sink,
                                         const std::stop_token& stop, const StructuredOptions& options) {
    const ProviderCaps caps = provider.caps();
    if (caps.json_schema) {
        request.json_schema = schema.model_facing();
    }
    if (caps.thinking && !request.thinking.has_value()) {
        request.thinking = false;
    }
    for (int repairs = 0;; ++repairs) {
        if (repairs > 0 && sink.on_restart) {
            sink.on_restart();  // the answer shown so far is void
        }
        RR_TRY_ASSIGN(ChatResponse response, provider.chat(request, sink, stop));
        json::Json value;
        const std::vector<std::string> problems = problems_with(response, schema, options.check, value);
        if (problems.empty()) {
            return StructuredAnswer{
                .value = std::move(value), .response = std::move(response), .repairs = repairs};
        }
        if (repairs >= options.repairs) {
            return fail(Errc::model_output_invalid,
                        std::format("'{}' gave no usable answer in {} attempt(s); the last one: {}",
                                    request.model, repairs + 1, join(problems, "; ")));
        }
        request.messages.push_back(Message::assistant(response.content));
        request.messages.push_back(
            Message::user(replace_all(options.repair_instruction, "{problems}", as_lines(problems))));
    }
}

}  // namespace rr::llm
