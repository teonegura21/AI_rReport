// Timed words, cut into the transcript's sentences (rr/transcript/sentences.hpp).
#include <rr/transcript/sentences.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <rr/core/assert.hpp>
#include <rr/text/sentences.hpp>
#include <rr/text/unicode.hpp>

namespace rr::transcript {

namespace {

using knowledge::Sentence;
using knowledge::SentenceId;
using text::Span;

// The sentence ends of every script rr/text/sentences.hpp knows.
bool ends_sentence(char32_t code_point) noexcept {
    static constexpr std::u32string_view kEnds = U".!?\u2026\u203D\u3002\uFF0E\uFF01\uFF1F\u061F\u0964\u0965";
    return kEnds.contains(code_point);
}

// Whether `word` ends a sentence: its last mark before closing quotes and
// brackets is a sentence end ("it.", "it?”").
bool ends_with_sentence_end(std::string_view word) {
    for (const text::CodePoint& point : text::code_points(word) | std::views::reverse) {
        if (!text::is_closing(point.value)) {
            return ends_sentence(point.value);
        }
    }
    return false;
}

// The words, joined as text: a space between two, a blank line before a
// paragraph (which rr/text/sentences.hpp never crosses).
class Joined {
public:
    explicit Joined(const std::vector<Word>& words) {
        for (std::size_t index = 0; index < words.size(); ++index) {
            if (index > 0) {
                text_ += words[index].paragraph ? "\n\n" : " ";
            }
            placed_.push_back(Span{.begin = text_.size(), .end = text_.size() + words[index].text.size()});
            said_.push_back(Said{.start = words[index].start, .end = words[index].end});
            text_ += words[index].text;
        }
    }

    [[nodiscard]] const std::string& text() const { return text_; }
    [[nodiscard]] const Span& placed(std::size_t word) const { return placed_[word]; }

    // The first word that ends after `offset`.
    [[nodiscard]] std::size_t first_word(std::size_t offset) const {
        return static_cast<std::size_t>(std::ranges::upper_bound(placed_, offset, {}, &Span::end) -
                                        placed_.begin());
    }

    // The last word that begins before `offset`.
    [[nodiscard]] std::size_t last_word(std::size_t offset) const {
        const auto after = std::ranges::lower_bound(placed_, offset, {}, &Span::begin);
        return static_cast<std::size_t>(after - placed_.begin()) - 1;
    }

    // When the text at `offset` inside `word` was said: in proportion to
    // where in the word it is.
    [[nodiscard]] Millis time_at(std::size_t word, std::size_t offset) const {
        const Span& span = placed_[word];
        const Said& said = said_[word];
        const std::size_t clamped = std::clamp(offset, span.begin, span.end);
        const auto into = static_cast<std::int64_t>(clamped - span.begin);
        const auto length = static_cast<std::int64_t>(std::max<std::size_t>(span.end - span.begin, 1));
        return said.start + ((said.end - said.start) * into / length);
    }

    // The pause after `word`, before the next.
    [[nodiscard]] Millis pause_after(std::size_t word) const {
        return said_[word + 1].start - said_[word].end;
    }

private:
    // When a word was said.
    struct Said {
        Millis start{0};
        Millis end{0};
    };

    std::string text_;
    std::vector<Span> placed_;  // each word's place in the text
    std::vector<Said> said_;    // and when it was said
};

// `sentence`, cut where its words pause longest until no part holds more
// than `max_words` words: its parts, in order.
std::vector<Span> cut(const Joined& joined, Span sentence, std::size_t max_words) {
    std::vector<Span> parts;
    std::vector<Span> pending{sentence};  // the last is cut next, so parts come in order
    while (!pending.empty()) {
        const Span part = pending.back();
        pending.pop_back();
        const std::size_t first = joined.first_word(part.begin);
        const std::size_t last = joined.last_word(part.end);
        if (last - first + 1 <= max_words) {
            parts.push_back(part);
            continue;
        }
        std::size_t at = first;  // the word the part is cut after
        for (std::size_t word = first; word < last; ++word) {
            const Millis pause = joined.pause_after(word);
            const Millis best = joined.pause_after(at);
            // Nearest the middle: the two sides' sizes differ least.
            const auto imbalance = [&](std::size_t after) {
                const auto left = static_cast<std::int64_t>(after - first + 1);
                const auto right = static_cast<std::int64_t>(last - after);
                return left > right ? left - right : right - left;
            };
            if (pause > best || (pause == best && imbalance(word) < imbalance(at))) {
                at = word;
            }
        }
        pending.push_back(Span{.begin = joined.placed(at + 1).begin, .end = part.end});
        pending.push_back(Span{.begin = part.begin, .end = joined.placed(at).end});
    }
    return parts;
}

}  // namespace

knowledge::Transcript transcript_of(const std::vector<Word>& words, const SentenceOptions& options) {
    const Joined joined{words};
    const std::size_t max_words = std::max<std::size_t>(options.max_words, 1);
    knowledge::Transcript transcript;
    for (const Span& sentence : text::sentences(joined.text())) {
        for (const Span& part : cut(joined, sentence, max_words)) {
            const std::size_t first = joined.first_word(part.begin);
            const std::size_t last = joined.last_word(part.end);
            const Millis start = joined.time_at(first, part.begin);
            const Millis end = std::max(joined.time_at(last, part.end), start);
            Result<TimeSpan> span = TimeSpan::make(start, end);
            RR_ASSERT(span.has_value(), "word times are never negative, and end after they start");
            transcript.sentences.push_back(
                Sentence{.id = SentenceId{static_cast<std::uint32_t>(transcript.sentences.size() + 1)},
                         .text = std::string{text::text_of(joined.text(), part)},
                         .span = *span,
                         .speaker = words[first].speaker});
        }
    }
    return transcript;
}

bool punctuated(const std::vector<Word>& words, std::size_t every) {
    const auto ends = static_cast<std::size_t>(
        std::ranges::count_if(words, [](const Word& word) { return ends_with_sentence_end(word.text); }));
    return words.empty() || (ends > 0 && ends * std::max<std::size_t>(every, 1) >= words.size());
}

}  // namespace rr::transcript
