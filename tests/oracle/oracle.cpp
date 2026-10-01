#include <rr/oracle/oracle.hpp>

#include <charconv>
#include <cstdint>
#include <format>
#include <limits>
#include <regex>
#include <utility>

namespace rr::oracle {

namespace {

constexpr std::int64_t kCharactersPerToken = 4;

std::int64_t tokens(std::string_view text) {
    return (static_cast<std::int64_t>(text.size()) + kCharactersPerToken - 1) / kCharactersPerToken;
}

bool matches(const When& when, const llm::ChatRequest& request) {
    return (when.mission.empty() || when.mission == request.tag.mission_id) &&
           (when.kind.empty() || when.kind == request.tag.purpose) &&
           (when.model.empty() || when.model == request.model) &&
           (!when.thinking.has_value() || *when.thinking == request.thinking.value_or(false));
}

// Replaces the first match of `pattern` in `text` with what `replacement`
// makes of it; whether there was one.
bool replace_first(std::string& text, const std::regex& pattern,
                   const std::function<std::string(const std::smatch&)>& replacement) {
    std::smatch match;
    if (!std::regex_search(text, match, pattern)) {
        return false;
    }
    const auto at = static_cast<std::size_t>(match.position(0));
    const auto length = static_cast<std::size_t>(match.length(0));
    text.replace(at, length, replacement(match));
    return true;
}

// A number, plus one; a number too long to add to gets a 1 appended.
std::string plus_one(const std::string& digits) {
    std::uint64_t value = 0;
    const auto [end, error] = std::from_chars(digits.data(), digits.data() + digits.size(), value);
    if (error != std::errc{} || end != digits.data() + digits.size() ||
        value == std::numeric_limits<std::uint64_t>::max()) {
        return digits + "1";
    }
    return std::to_string(value + 1);
}

}  // namespace

Oracle::Oracle(llm::ProviderCaps caps) : caps_(caps) {}

void Oracle::teach(std::string kind, Answerer answerer) {
    answerers_.insert_or_assign(std::move(kind), std::move(answerer));
}

Result<llm::ChatResponse> Oracle::chat(const llm::ChatRequest& request, const llm::StreamSink& sink,
                                       const std::stop_token& stop) const {
    if (stop.stop_requested()) {
        return fail(Errc::cancelled, "the oracle was stopped");
    }
    const auto answerer = answerers_.find(request.tag.purpose);
    if (answerer == answerers_.end()) {
        return fail(Errc::unsupported,
                    std::format("the oracle was not taught the mission kind '{}'", request.tag.purpose));
    }
    RR_TRY_ASSIGN(const std::string answer, answerer->second(request));
    llm::ChatResponse response{.model = request.model};
    for (const llm::Message& message : request.messages) {
        response.usage.prompt_tokens += tokens(message.text());
    }
    for (int choice = 0; choice < request.n.value_or(1); ++choice) {
        response.choices.push_back(llm::Choice{
            .content = answer, .finish = llm::FinishReason::stop, .completion_tokens = tokens(answer)});
        response.usage.completion_tokens += tokens(answer);
        if (sink.on_content) {
            sink.on_content(static_cast<std::size_t>(choice), answer);
        }
    }
    return response;
}

Fault refusal(When when) {
    return Fault{.name = "refusal", .when = std::move(when), .change = [](llm::Choice& answer) {
                     answer.content = "I'm sorry, but I can't help with that.";
                     answer.finish = llm::FinishReason::stop;
                     return true;
                 }};
}

Fault truncation(When when) {
    return Fault{.name = "truncation", .when = std::move(when), .change = [](llm::Choice& answer) {
                     std::size_t half = answer.content.size() / 2;
                     // Never inside a UTF-8 sequence: back off its continuation bytes.
                     while (half > 0 && (static_cast<unsigned char>(answer.content[half]) & 0xC0U) == 0x80U) {
                         --half;
                     }
                     const bool changed = half < answer.content.size();
                     answer.content.resize(half);
                     answer.finish = llm::FinishReason::length;
                     return changed;
                 }};
}

Fault broken_json(When when) {
    return Fault{.name = "broken_json", .when = std::move(when), .change = [](llm::Choice& answer) {
                     const std::size_t closing = answer.content.find_last_of("}]");
                     if (closing == std::string::npos) {
                         return false;
                     }
                     answer.content.erase(closing, 1);
                     return true;
                 }};
}

Fault out_while_thinking(When when) {
    return Fault{.name = "out_while_thinking", .when = std::move(when), .change = [](llm::Choice& answer) {
                     answer.reasoning = "Let me think about this again, more carefully, from the start.";
                     answer.content.clear();
                     answer.finish = llm::FinishReason::length;
                     return true;
                 }};
}

Fault wrong_number(When when) {
    return Fault{.name = "wrong_number", .when = std::move(when), .change = [](llm::Choice& answer) {
                     static const std::regex number{R"(\d+)"};
                     return replace_first(answer.content, number,
                                          [](const std::smatch& match) { return plus_one(match.str()); });
                 }};
}

Fault bad_marker(When when) {
    return Fault{.name = "bad_marker", .when = std::move(when), .change = [](llm::Choice& answer) {
                     static const std::regex marker{R"(\[A\d+\])"};
                     return replace_first(answer.content, marker,
                                          [](const std::smatch&) { return "[A999]"; });
                 }};
}

Fault dropped_reference(When when) {
    return Fault{.name = "dropped_reference", .when = std::move(when), .change = [](llm::Choice& answer) {
                     // The reference and one comma beside it, so a JSON list stays a list.
                     static const std::regex after_another{R"(,\s*"S\d+")"};
                     static const std::regex before_another{R"("S\d+"\s*,\s*)"};
                     static const std::regex alone{R"("S\d+")"};
                     const auto nothing = [](const std::smatch&) { return std::string{}; };
                     return replace_first(answer.content, after_another, nothing) ||
                            replace_first(answer.content, before_another, nothing) ||
                            replace_first(answer.content, alone, nothing);
                 }};
}

Fault server_error(When when, Errc code) {
    return Fault{
        .name = std::format("server_error:{}", to_string(code)), .when = std::move(when), .error = code};
}

Fault edit(std::string name, When when, std::function<std::string(std::string)> change) {
    return Fault{.name = std::move(name),
                 .when = std::move(when),
                 .change = [change = std::move(change)](llm::Choice& answer) {
                     std::string changed = change(answer.content);
                     const bool different = changed != answer.content;
                     answer.content = std::move(changed);
                     return different;
                 }};
}

FaultyProvider::FaultyProvider(const llm::LlmProvider& inner, std::vector<Fault> faults)
    : inner_(&inner)
    , faults_(std::move(faults)) {}

Result<llm::ChatResponse> FaultyProvider::chat(const llm::ChatRequest& request, const llm::StreamSink& sink,
                                               const std::stop_token& stop) const {
    std::optional<std::size_t> hit;
    {
        const std::lock_guard lock(mutex_);
        for (std::size_t index = 0; index < faults_.size() && !hit.has_value(); ++index) {
            const Fault& fault = faults_[index];
            if (!matches(fault.when, request)) {
                continue;
            }
            int& calls = hits_[{index, request.tag.mission_id}];
            if (fault.when.calls == 0 || calls < fault.when.calls) {
                ++calls;
                hit = index;
            }
        }
    }
    if (!hit.has_value()) {
        return inner_->chat(request, sink, stop);
    }
    const Fault& fault = faults_[*hit];
    const auto record = [&](bool changed) {
        const std::lock_guard lock(mutex_);
        injected_.push_back(Injection{.fault = fault.name,
                                      .mission = request.tag.mission_id,
                                      .model = request.model,
                                      .changed = changed});
    };
    if (fault.error.has_value()) {
        record(true);
        return fail(*fault.error, std::format("injected fault: {}", fault.name));
    }
    RR_TRY_ASSIGN(llm::ChatResponse response, inner_->chat(request, sink, stop));
    bool changed = false;
    for (llm::Choice& choice : response.choices) {
        changed = fault.change(choice) || changed;  // every choice is changed
    }
    record(changed);
    return response;
}

std::vector<Injection> FaultyProvider::injected() const {
    const std::lock_guard lock(mutex_);
    return injected_;
}

}  // namespace rr::oracle
