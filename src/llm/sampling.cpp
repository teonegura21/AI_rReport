#include <rr/llm/sampling.hpp>

#include <array>
#include <cmath>
#include <format>
#include <string_view>

#include <rr/json/json.hpp>

namespace rr::llm {

namespace {

// Each check is written so that NaN fails it: every comparison with NaN is
// false, so "value >= low && value <= high" rejects it, where "value < low ||
// value > high" would let it through.
bool finite_at_least(double value, double low) {
    return std::isfinite(value) && value >= low;
}

bool within(double value, double low, double high) {
    return value >= low && value <= high;
}

template <class T, class Accepts>
void check(std::vector<std::string>& problems, const std::optional<T>& field, std::string_view name,
           std::string_view rule, Accepts accepts) {
    if (field.has_value() && !accepts(*field)) {
        problems.push_back(std::format("{} must be {}, not {}", name, rule, *field));
    }
}

// A penalty as a server-specific field in `extra`, and the value that turns it off.
struct ExtraPenalty {
    std::string_view key;
    double neutral;
};

constexpr std::array<ExtraPenalty, 5> kExtraPenalties{{
    {.key = "presence_penalty", .neutral = 0.0},
    {.key = "frequency_penalty", .neutral = 0.0},
    {.key = "repeat_penalty", .neutral = 1.0},
    {.key = "dry_multiplier", .neutral = 0.0},   // DRY: "don't repeat yourself"
    {.key = "xtc_probability", .neutral = 0.0},  // XTC: "exclude top choices"
}};

}  // namespace

void apply(const SamplingPreset& preset, ChatRequest& request) {
    const auto fill = [](auto& field, const auto& value) {
        if (!field.has_value()) {
            field = value;
        }
    };
    fill(request.temperature, preset.temperature);
    fill(request.top_p, preset.top_p);
    fill(request.top_k, preset.top_k);
    fill(request.min_p, preset.min_p);
    fill(request.presence_penalty, preset.presence_penalty);
    fill(request.repeat_penalty, preset.repeat_penalty);
    fill(request.thinking, preset.thinking);
    if (request.thinking.value_or(false)) {
        if (!request.thinking_budget.has_value()) {
            request.thinking_budget = preset.thinking_budget;
        }
        if (!request.reasoning_effort.has_value()) {
            request.reasoning_effort = preset.reasoning_effort;
        }
    }
}

std::vector<std::string> sampling_problems(const ChatRequest& request) {
    std::vector<std::string> problems;
    check(problems, request.temperature, "temperature", "a finite number >= 0",
          [](double value) { return finite_at_least(value, 0.0); });
    check(problems, request.top_p, "top_p", "in (0, 1]",
          [](double value) { return value > 0.0 && value <= 1.0; });
    check(problems, request.top_k, "top_k", ">= 0", [](int value) { return value >= 0; });
    check(problems, request.min_p, "min_p", "in [0, 1]",
          [](double value) { return within(value, 0.0, 1.0); });
    check(problems, request.presence_penalty, "presence_penalty", "in [-2, 2]",
          [](double value) { return within(value, -2.0, 2.0); });
    check(problems, request.repeat_penalty, "repeat_penalty", "a finite number > 0",
          [](double value) { return std::isfinite(value) && value > 0.0; });
    check(problems, request.max_tokens, "max_tokens", ">= 1", [](int value) { return value >= 1; });
    check(problems, request.n, "n", ">= 1", [](int value) { return value >= 1; });
    // Greedy decoding ignores the seed: every choice would be the same answer.
    if (request.n.value_or(1) > 1 && request.temperature == 0.0) {
        problems.emplace_back("n > 1 needs a temperature above 0: at 0 every choice is the same answer");
    }
    if (request.thinking_budget.has_value() && request.thinking_budget->tokens < 0) {
        problems.push_back(
            std::format("a thinking budget must be >= 0 tokens, not {}", request.thinking_budget->tokens));
    }
    const bool thinking = request.thinking.value_or(false);
    if (request.thinking_budget.has_value() && !thinking) {
        problems.emplace_back("a thinking budget needs thinking = true");
    }
    if (request.reasoning_effort.has_value() && !thinking) {
        problems.emplace_back("a reasoning effort needs thinking = true");
    }
    return problems;
}

std::vector<std::string> penalty_problems(const ChatRequest& request) {
    std::vector<std::string> problems;
    // "!=" is true for NaN, so a NaN penalty is reported too.
    if (request.presence_penalty.has_value() && *request.presence_penalty != 0.0) {
        problems.push_back(std::format("presence_penalty = {}", *request.presence_penalty));
    }
    if (request.repeat_penalty.has_value() && *request.repeat_penalty != 1.0) {
        problems.push_back(std::format("repeat_penalty = {}", *request.repeat_penalty));
    }
    if (request.extra.is_object()) {
        for (const ExtraPenalty& penalty : kExtraPenalties) {
            const auto found = request.extra.find(penalty.key);
            if (found != request.extra.end() &&
                (!found->is_number() || found->get<double>() != penalty.neutral)) {
                problems.push_back(std::format("extra.{} = {}", penalty.key, json::dump(*found)));
            }
        }
    }
    return problems;
}

std::vector<std::string> preset_problems(const SamplingPreset& preset) {
    ChatRequest probe;
    apply(preset, probe);
    std::vector<std::string> problems = sampling_problems(probe);
    // apply() leaves a budget and an effort out when thinking is off, so the
    // probe cannot show that mistake: say it here.
    if (!preset.thinking && preset.thinking_budget.has_value()) {
        problems.emplace_back("a thinking budget needs thinking = true");
    }
    if (!preset.thinking && preset.reasoning_effort.has_value()) {
        problems.emplace_back("a reasoning effort needs thinking = true");
    }
    return problems;
}

json::Json to_json(const SamplingPreset& preset) {
    json::Json budget = nullptr;
    if (preset.thinking_budget.has_value()) {
        budget = json::Json{{"tokens", preset.thinking_budget->tokens},
                            {"closing", preset.thinking_budget->closing}};
    }
    json::Json effort = nullptr;
    if (preset.reasoning_effort.has_value()) {
        effort = std::string{to_string(*preset.reasoning_effort)};
    }
    return json::Json{{"temperature", preset.temperature},
                      {"top_p", preset.top_p},
                      {"top_k", preset.top_k},
                      {"min_p", preset.min_p},
                      {"presence_penalty", preset.presence_penalty},
                      {"repeat_penalty", preset.repeat_penalty},
                      {"thinking", preset.thinking},
                      {"thinking_budget", std::move(budget)},
                      {"reasoning_effort", std::move(effort)}};
}

}  // namespace rr::llm
