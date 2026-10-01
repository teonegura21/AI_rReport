#include <rr/text/sentences.hpp>

#include <cstddef>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <rr/text/unicode.hpp>

#include "lexicon.hpp"

namespace rr::text {

namespace {

// Terminators that end a sentence with no space after them: the ideographic
// and fullwidth full stops, exclamation and question marks.
bool is_ideographic_terminator(char32_t code_point) noexcept {
    return code_point == U'\u3002' || code_point == U'\uFF01' || code_point == U'\uFF1F' ||
           code_point == U'\uFF0E' || code_point == U'\uFF61';
}

// Terminators that end a sentence whatever word comes before them.
bool is_strong_terminator(char32_t code_point) noexcept {
    switch (code_point) {
        case U'!':
        case U'?':
        case U'\u203C':  // double exclamation mark
        case U'\u203D':  // interrobang
        case U'\u2047':  // double question mark
        case U'\u2048':  // question exclamation mark
        case U'\u2049':  // exclamation question mark
        case U'\u061F':  // Arabic question mark
        case U'\u0964':  // Devanagari danda
        case U'\u0965':  // Devanagari double danda
        case U'\u0589':  // Armenian full stop
        case U'\u1362':  // Ethiopic full stop
            return true;
        default:
            return is_ideographic_terminator(code_point);
    }
}

bool is_terminator(char32_t code_point) noexcept {
    return code_point == U'.' || code_point == U'\u2026' || is_strong_terminator(code_point);
}

// What may close a sentence after its terminator: `"Stop."`, `(see above.)`.
bool is_closer(char32_t code_point) noexcept {
    return code_point == U'"' || code_point == U'\'' || is_closing(code_point);
}

// What may open the next one: `"Stop."`, `¿Qué?`.
bool is_opener(char32_t code_point) noexcept {
    return code_point == U'"' || code_point == U'\'' || code_point == U'\u00BF' || code_point == U'\u00A1' ||
           is_opening(code_point);
}

bool is_blank(char32_t code_point) noexcept {
    const CharClass kind = classify(code_point);
    return kind == CharClass::space || kind == CharClass::other;
}

bool is_word_part(char32_t code_point) noexcept {
    const CharClass kind = classify(code_point);
    return kind == CharClass::letter || kind == CharClass::mark || kind == CharClass::digit;
}

// Whether a word may start with this letter: a capital, or a letter of a
// script without case.
bool starts_sentence(char32_t code_point) noexcept {
    return is_upper(code_point) || (classify(code_point) == CharClass::letter && !is_lower(code_point));
}

// A sentence while it is being found: the code points [first, last).
struct Range {
    std::size_t first = 0;
    std::size_t last = 0;
};

class Splitter {
public:
    explicit Splitter(std::string_view text) : text_(text), points_(code_points(text)) {}

    [[nodiscard]] std::vector<Span> split() const {
        std::vector<Range> found;
        std::size_t start = 0;
        std::vector<std::size_t> ends = cuts();
        ends.push_back(points_.size());
        for (const std::size_t end : ends) {
            const Range trimmed = trim({.first = start, .last = end});
            if (trimmed.first < trimmed.last) {
                found.push_back(trimmed);
            }
            start = end;
        }
        std::vector<Span> spans;
        for (const Range& range : with_fragments_joined(found)) {
            spans.push_back(Span{.begin = points_[range.first].begin, .end = points_[range.last - 1].end});
        }
        return spans;
    }

private:
    [[nodiscard]] char32_t at(std::size_t index) const { return points_[index].value; }

    // Where sentences may end: after a terminator and its closers, or at the
    // start of a paragraph break. In order; possibly repeated.
    [[nodiscard]] std::vector<std::size_t> cuts() const {
        std::vector<std::size_t> found;
        std::size_t index = 0;
        while (index < points_.size()) {
            if (is_blank(at(index))) {
                const std::size_t end = blank_end(index);
                if (line_breaks(index, end) >= 2) {
                    found.push_back(index);
                }
                index = end;
                continue;
            }
            if (is_terminator(at(index))) {
                std::size_t marks_end = index;
                while (marks_end < points_.size() && is_terminator(at(marks_end))) {
                    ++marks_end;
                }
                std::size_t end = marks_end;
                while (end < points_.size() && is_closer(at(end))) {
                    ++end;
                }
                if (ends_sentence(index, marks_end, end)) {
                    found.push_back(end);
                }
                index = end;
                continue;
            }
            ++index;
        }
        return found;
    }

    [[nodiscard]] std::size_t blank_end(std::size_t index) const {
        while (index < points_.size() && is_blank(at(index))) {
            ++index;
        }
        return index;
    }

    // The line breaks in [first, last): a carriage return and line feed are
    // one, a paragraph separator is two.
    [[nodiscard]] int line_breaks(std::size_t first, std::size_t last) const {
        int breaks = 0;
        for (std::size_t index = first; index < last; ++index) {
            switch (at(index)) {
                case U'\n':
                case U'\v':
                case U'\f':
                case U'\u0085':  // next line
                case U'\u2028':  // line separator
                    ++breaks;
                    break;
                case U'\r':
                    breaks += index + 1 < last && at(index + 1) == U'\n' ? 0 : 1;
                    break;
                case U'\u2029':  // paragraph separator
                    breaks += 2;
                    break;
                default:
                    break;
            }
        }
        return breaks;
    }

    // Whether the terminators [first, marks_end), closed up to `end`, end a
    // sentence.
    [[nodiscard]] bool ends_sentence(std::size_t first, std::size_t marks_end, std::size_t end) const {
        bool strong = false;
        bool ideographic = false;
        bool ellipsis = false;
        std::size_t periods = 0;
        for (std::size_t index = first; index < marks_end; ++index) {
            if (at(index) == U'.') {
                ++periods;
            } else if (at(index) == U'\u2026') {
                ellipsis = true;
            } else {
                strong = true;
                ideographic = ideographic || is_ideographic_terminator(at(index));
            }
        }
        // "3.5", "e.g.x", "Hello!World": inside a word or a number.
        if (!ideographic && end < points_.size() && !is_blank(at(end))) {
            return false;
        }
        const std::optional<std::size_t> next = next_word(end);
        if (!next.has_value()) {
            return true;
        }
        if (strong) {
            return !is_lower(at(*next));
        }
        if (ellipsis || periods > 1) {
            return starts_sentence(at(*next));
        }
        return period_ends_sentence(first, *next);
    }

    // The first code point of the next word, past spaces and opening quotes
    // and brackets; none at the end of the text.
    [[nodiscard]] std::optional<std::size_t> next_word(std::size_t index) const {
        while (index < points_.size() && (is_blank(at(index)) || is_opener(at(index)))) {
            ++index;
        }
        return index < points_.size() ? std::optional<std::size_t>{index} : std::nullopt;
    }

    [[nodiscard]] bool period_ends_sentence(std::size_t period, std::size_t next) const {
        const std::size_t word = word_before(period);
        switch (lexicon::abbreviation(folded(word, period))) {
            case lexicon::Abbreviation::title:
                return false;
            case lexicon::Abbreviation::terminal:
                return starts_new_sentence(next);
            case lexicon::Abbreviation::numbering:
                if (classify(at(next)) == CharClass::digit) {
                    return false;
                }
                break;
            case lexicon::Abbreviation::none:
                break;
        }
        const bool initial = period - word == 1 && is_upper(at(word));
        if (initial) {
            return starts_new_sentence(next);
        }
        return !is_lower(at(next));
    }

    // After a period that may belong to the words around it ("U.S. Army",
    // "John F. Kennedy"), whether the next word starts a sentence anyway: a
    // capitalized function word ("It", "The", "Then"), or a letter of a
    // script without case. Merging two sentences costs less than splitting
    // a name across two.
    [[nodiscard]] bool starts_new_sentence(std::size_t next) const {
        if (!starts_sentence(at(next))) {
            return false;
        }
        return !is_upper(at(next)) || lexicon::is_function_word(folded(next, word_after(next)));
    }

    // Where the word before a period starts: letters, marks and digits, with
    // the periods between them ("e.g", "U.S", "3").
    [[nodiscard]] std::size_t word_before(std::size_t period) const {
        std::size_t begin = period;
        while (begin > 0 && (is_word_part(at(begin - 1)) ||
                             (at(begin - 1) == U'.' && begin >= 2 && is_word_part(at(begin - 2))))) {
            --begin;
        }
        return begin;
    }

    // Where the word that starts at `first` ends, apostrophes between letters
    // included ("It's").
    [[nodiscard]] std::size_t word_after(std::size_t first) const {
        std::size_t end = first;
        while (end < points_.size()) {
            const bool apostrophe = (at(end) == U'\'' || at(end) == U'\u2019') && end > first &&
                                    end + 1 < points_.size() && is_word_part(at(end + 1));
            if (!is_word_part(at(end)) && !apostrophe) {
                break;
            }
            ++end;
        }
        return end;
    }

    // The code points [first, last), folded.
    [[nodiscard]] std::string folded(std::size_t first, std::size_t last) const {
        if (first >= last) {
            return {};
        }
        // Letters, marks, digits and periods are valid UTF-8: the fold succeeds.
        Result<std::string> word =
            fold(text_.substr(points_[first].begin, points_[last - 1].end - points_[first].begin));
        return word.has_value() ? std::move(*word) : std::string{};
    }

    [[nodiscard]] Range trim(Range range) const {
        while (range.first < range.last && is_blank(at(range.first))) {
            ++range.first;
        }
        while (range.last > range.first && is_blank(at(range.last - 1))) {
            --range.last;
        }
        return range;
    }

    [[nodiscard]] bool has_word(Range range) const {
        for (std::size_t index = range.first; index < range.last; ++index) {
            if (is_word_part(at(index))) {
                return true;
            }
        }
        return false;
    }

    // Fragments without a word join the sentence before them, or the first
    // sentence after them at the start; a text of fragments only is one.
    [[nodiscard]] std::vector<Range> with_fragments_joined(const std::vector<Range>& ranges) const {
        std::vector<Range> joined;
        std::optional<std::size_t> leading;  // where leading fragments start
        for (Range range : ranges) {
            if (!has_word(range)) {
                if (!joined.empty()) {
                    joined.back().last = range.last;
                } else if (!leading.has_value()) {
                    leading = range.first;
                }
                continue;
            }
            if (leading.has_value()) {
                range.first = *leading;
                leading.reset();
            }
            joined.push_back(range);
        }
        if (leading.has_value()) {
            joined.push_back(Range{.first = *leading, .last = ranges.back().last});
        }
        return joined;
    }

    std::string_view text_;
    std::vector<CodePoint> points_;
};

}  // namespace

std::vector<Span> sentences(std::string_view text) {
    return Splitter{text}.split();
}

}  // namespace rr::text
