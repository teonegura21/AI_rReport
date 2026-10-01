#include <rr/llm/structured.hpp>

#include <format>
#include <string_view>

#include <rr/core/strings.hpp>
#include <rr/llm/sampling.hpp>

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

ChatRequest structured_request(ChatRequest request, const json::Schema& schema, ProviderCaps caps) {
    if (caps.json_schema) {
        request.json_schema = schema.model_facing();
    }
    if (caps.thinking && !request.thinking.has_value()) {
        request.thinking = false;
    }
    return request;
}

CheckedAnswer check_structured(const Choice& answer, const json::Schema& schema, const AnswerCheck& check) {
    CheckedAnswer checked;
    auto parsed = json::parse(unfenced(answer.content));
    if (!parsed.has_value()) {
        checked.problems.push_back(std::format("it is not JSON: {}", parsed.error().message()));
    } else {
        for (const json::Violation& violation : schema.check(*parsed)) {
            checked.problems.push_back(
                std::format("{} ({}): {}", violation.instance_path.empty() ? "/" : violation.instance_path,
                            violation.keyword, violation.message));
        }
        if (checked.problems.empty() && check) {
            checked.problems = check(*parsed);
        }
        if (checked.problems.empty()) {
            checked.value = std::move(*parsed);
        }
    }
    if (!checked.problems.empty() && answer.finish == FinishReason::length) {
        checked.problems.emplace_back("the answer was cut off at the token limit, so it must be shorter");
    }
    return checked;
}

ChatRequest repair_request(ChatRequest request, const Choice& answer,
                           const std::vector<std::string>& problems, std::string_view instruction) {
    request.messages.push_back(Message::assistant(answer.content));
    request.messages.push_back(Message::user(replace_all(instruction, "{problems}", as_lines(problems))));
    return request;
}

Result<StructuredAnswer> structured_json(const LlmProvider& provider, ChatRequest request,
                                         const json::Schema& schema, const StreamSink& sink,
                                         const std::stop_token& stop, const StructuredOptions& options) {
    // A penalty biases the answer's labels and paraphrases its quotes
    // (rr/llm/sampling.hpp): a mistake in the caller, refused before any call.
    if (const std::vector<std::string> penalties = penalty_problems(request); !penalties.empty()) {
        return fail(Errc::invalid_argument,
                    std::format("a structured answer from '{}' cannot be asked with penalties: {}",
                                request.model, join(penalties, "; ")));
    }
    if (request.n.value_or(1) != 1) {
        return fail(
            Errc::invalid_argument,
            std::format("structured_json() takes one answer from '{}', not n = {}: each sample needs its "
                        "own validation and repair",
                        request.model, request.n.value_or(1)));
    }
    request = structured_request(std::move(request), schema, provider.caps());
    for (int repairs = 0;; ++repairs) {
        if (repairs > 0 && sink.on_restart) {
            sink.on_restart();  // the answer shown so far is void
        }
        RR_TRY_ASSIGN(ChatResponse response, provider.chat(request, sink, stop));
        CheckedAnswer checked = check_structured(response.first(), schema, options.check);
        if (checked.usable()) {
            return StructuredAnswer{
                .value = std::move(checked.value), .response = std::move(response), .repairs = repairs};
        }
        if (repairs >= options.repairs) {
            return fail(Errc::model_output_invalid,
                        std::format("'{}' gave no usable answer in {} attempt(s); the last one: {}",
                                    request.model, repairs + 1, join(checked.problems, "; ")));
        }
        request = repair_request(std::move(request), response.first(), checked.problems,
                                 options.repair_instruction);
    }
}

}  // namespace rr::llm
