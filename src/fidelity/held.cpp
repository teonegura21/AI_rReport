#include "held.hpp"

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include <rr/text/entities.hpp>
#include <rr/text/tokens.hpp>
#include <rr/text/unicode.hpp>

namespace rr::fidelity::held {

std::vector<text::NumberMention> numbers(std::string_view text) {
    std::vector<text::NumberMention> found = text::numbers(text);
    std::erase_if(found, [](const text::NumberMention& mention) {
        return mention.form == text::NumberForm::words && mention.value == 1.0 && !mention.ordinal &&
               !mention.percent;
    });
    return found;
}

bool states(const std::vector<text::NumberMention>& numbers, const text::NumberMention& wanted) {
    return std::ranges::any_of(
        numbers, [&](const text::NumberMention& stated) { return text::same_number(stated, wanted); });
}

bool names(std::string_view text, std::string_view name) {
    if (!text::mentions(text, name).empty()) {
        return true;
    }
    const std::vector<text::Term> words = text::terms(name);
    return words.size() > 1 && !text::mentions(text, text::text_of(name, words.back().span)).empty();
}

bool uses(std::string_view text, std::string_view term) {
    return !text::mentions(text, term).empty();
}

std::string said(const knowledge::Transcript& transcript,
                 const std::vector<knowledge::SentenceId>& sentences) {
    std::string text;
    for (const knowledge::SentenceId& id : sentences) {
        if (const knowledge::Sentence* sentence = transcript.find(id)) {
            text += text.empty() ? sentence->text : " " + sentence->text;
        }
    }
    return text;
}

std::optional<std::string> closest(const text::Alignment& alignment, std::string_view text) {
    constexpr double kClose = 0.5;
    if (alignment.score == 0 || alignment.identity() < kClose) {
        return std::nullopt;
    }
    return excerpt(text::text_of(text, alignment.text));
}

std::string excerpt(std::string_view text, std::size_t limit) {
    if (text.size() <= limit) {
        return std::string{text};
    }
    std::size_t end = 0;
    for (const text::CodePoint& point : text::code_points(text)) {
        if (point.end > limit) {
            break;
        }
        end = point.end;
    }
    return std::string{text.substr(0, end)} + "\u2026";
}

}  // namespace rr::fidelity::held
