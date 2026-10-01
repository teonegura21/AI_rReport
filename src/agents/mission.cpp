#include <rr/agents/mission.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <string>

#include <rr/core/strings.hpp>

namespace rr::agents {

namespace {

constexpr std::array kSteps{Step::as_ordered, Step::thinking, Step::commander, Step::other_family};
constexpr std::array kVerdicts{Verdict::accepted, Verdict::invalid, Verdict::refused, Verdict::ceiling};

// How refusals open, lowercased, with a straight apostrophe. Narrow on
// purpose: a refusal that opens otherwise still fails its checks, and climbs
// the ladder as an invalid answer would.
constexpr std::array<std::string_view, 15> kRefusalOpenings{
    "i'm sorry",    "i am sorry",    "sorry, but", "i can't",        "i cannot",
    "i can not",    "i won't",       "i will not", "i'm unable",     "i am unable",
    "i'm not able", "i am not able", "as an ai",   "i must decline", "i'm afraid i"};

// A refusal is a sentence or two. A long answer that opens like one is an
// answer that failed for another reason.
constexpr std::size_t kRefusalLength = 600;

}  // namespace

std::string family_of(std::string_view model) {
    std::string family;
    for (const char c : model) {
        if (std::isalpha(static_cast<unsigned char>(c)) == 0) {
            break;
        }
        family += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return family;
}

std::string_view to_string(Step step) noexcept {
    switch (step) {
        case Step::as_ordered:
            return "as_ordered";
        case Step::thinking:
            return "thinking";
        case Step::commander:
            return "commander";
        case Step::other_family:
            return "other_family";
    }
    return "as_ordered";
}

std::optional<Step> step_from_string(std::string_view name) noexcept {
    for (const Step step : kSteps) {
        if (to_string(step) == name) {
            return step;
        }
    }
    return std::nullopt;
}

bool looks_like_refusal(std::string_view answer) {
    answer = trim(answer);
    if (answer.empty() || answer.size() > kRefusalLength) {
        return false;
    }
    // Typographic apostrophes (U+2019) are as common as straight ones.
    const std::string opening = to_lower_ascii(replace_all(answer.substr(0, 32), "\u2019", "'"));
    return std::ranges::any_of(kRefusalOpenings,
                               [&](std::string_view refusal) { return opening.starts_with(refusal); });
}

std::string_view to_string(Verdict verdict) noexcept {
    switch (verdict) {
        case Verdict::accepted:
            return "accepted";
        case Verdict::invalid:
            return "invalid";
        case Verdict::refused:
            return "refused";
        case Verdict::ceiling:
            return "ceiling";
    }
    return "invalid";
}

std::optional<Verdict> verdict_from_string(std::string_view name) noexcept {
    for (const Verdict verdict : kVerdicts) {
        if (to_string(verdict) == name) {
            return verdict;
        }
    }
    return std::nullopt;
}

}  // namespace rr::agents
