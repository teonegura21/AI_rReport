// A model's punctuation, laid onto the words said (rr/transcript/projection.hpp).
#include <rr/transcript/projection.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <rr/core/assert.hpp>
#include <rr/text/tokens.hpp>
#include <rr/text/unicode.hpp>

namespace rr::transcript {

namespace {

// The sentence ends of every script rr/text/sentences.hpp knows.
bool ends_sentence(char32_t code_point) noexcept {
    static constexpr std::u32string_view kEnds = U".!?\u2026\u203D\u3002\uFF0E\uFF01\uFF1F\u061F\u0964\u0965";
    return kEnds.contains(code_point);
}

// Punctuation that changes no word and no number: sentence ends, commas,
// semicolons, colons, dashes, quotes and brackets (the header comment).
bool projectable(char32_t code_point) noexcept {
    static constexpr std::u32string_view kMarks =
        U",;:\"'\u00A1\u00BF\u2013\u2014\u2015\u3001\uFF0C\uFF1B\uFF1A\u060C\u061B";
    return ends_sentence(code_point) || kMarks.contains(code_point) || text::is_opening(code_point) ||
           text::is_closing(code_point);
}

// A space, or what rr/text never reads as text (a control, a format mark):
// what separates words.
bool is_blank(char32_t code_point) noexcept {
    const text::CharClass kind = text::classify(code_point);
    return kind == text::CharClass::space || kind == text::CharClass::other;
}

// The projectable punctuation of `text`, in order: what a word may gain.
std::string marks_of(std::string_view text) {
    std::string marks;
    for (const text::CodePoint& point : text::code_points(text)) {
        if (projectable(point.value)) {
            marks.append(text.substr(point.begin, point.end - point.begin));
        }
    }
    return marks;
}

bool has_projectable(std::string_view text) {
    return std::ranges::any_of(text::code_points(text),
                               [](const text::CodePoint& point) { return projectable(point.value); });
}

bool has_sentence_end(std::string_view text) {
    return std::ranges::any_of(text::code_points(text),
                               [](const text::CodePoint& point) { return ends_sentence(point.value); });
}

// Whether `written` is `said` in other capitals, and nothing else.
bool recased(std::string_view said, std::string_view written) {
    const std::vector<text::CodePoint> from = text::code_points(said);
    const std::vector<text::CodePoint> to = text::code_points(written);
    if (from.size() != to.size() || said == written) {
        return false;
    }
    return std::ranges::equal(from, to, [](const text::CodePoint& left, const text::CodePoint& right) {
        return left.value == right.value || text::to_lower(left.value) == text::to_lower(right.value);
    });
}

// The model's text, as punctuation around its words. Between two words, the
// last run of spaces divides what belongs to the word before (".", ",", "”")
// from what belongs to the word after ("“", "("). Without a space, an
// opening mark starts what belongs to the word after: "said(quietly" gives
// "(" to "quietly", and "U.S" gives "." to "U".
class Written {
public:
    explicit Written(std::string_view text) : text_(text), terms_(text::terms(text)) {}

    [[nodiscard]] const std::vector<text::Term>& terms() const { return terms_; }

    [[nodiscard]] std::string_view word(std::size_t term) const {
        return text::text_of(text_, terms_[term].span);
    }

    // The marks touching `term` before it.
    [[nodiscard]] std::string before(std::size_t term) const { return marks_of(divided(term).second); }

    // The marks after `term`, up to the next word's own.
    [[nodiscard]] std::string after(std::size_t term) const { return marks_of(divided(term + 1).first); }

private:
    // The text between term `term - 1` and term `term` (the start and the end
    // of the text stand for the terms before the first and after the last),
    // divided into what belongs to the word before and to the word after.
    [[nodiscard]] std::pair<std::string_view, std::string_view> divided(std::size_t term) const {
        const std::size_t begin = term == 0 ? 0 : terms_[term - 1].span.end;
        const std::size_t end = term == terms_.size() ? text_.size() : terms_[term].span.begin;
        const std::string_view gap = text_.substr(begin, end - begin);
        struct Run {
            std::size_t begin = 0;
            std::size_t end = 0;
        };
        std::optional<Run> spaces;           // the last run of spaces
        std::optional<std::size_t> opening;  // the first opening mark
        for (const text::CodePoint& point : text::code_points(gap)) {
            if (is_blank(point.value)) {
                if (spaces.has_value() && spaces->end == point.begin) {
                    spaces->end = point.end;
                } else {
                    spaces = Run{.begin = point.begin, .end = point.end};
                }
            } else if (!opening.has_value() && text::is_opening(point.value)) {
                opening = point.begin;
            }
        }
        if (spaces.has_value()) {
            return {gap.substr(0, spaces->begin), gap.substr(spaces->end)};
        }
        const std::size_t cut = opening.value_or(gap.size());
        return {gap.substr(0, cut), gap.substr(cut)};
    }

    std::string_view text_;
    std::vector<text::Term> terms_;
};

// A term of a caption word, as aligned.
struct Said {
    std::size_t word = 0;  // which caption word
    text::Term term{};     // where in its text
};

// Whether term `index` of `said` is the first, or the last, of its word.
bool first_of_word(const std::vector<Said>& said, std::size_t index) {
    return index == 0 || said[index - 1].word != said[index].word;
}
bool last_of_word(const std::vector<Said>& said, std::size_t index) {
    return index + 1 == said.size() || said[index + 1].word != said[index].word;
}

enum class Step : std::uint8_t {
    both,     // a caption term and the model's, the same or changed
    dropped,  // a caption term the model left out
    added,    // a term the model added
};

// One step of an alignment, with the terms it holds.
struct Pair {
    Step step = Step::both;
    std::size_t said = 0;     // the caption term: both, dropped
    std::size_t written = 0;  // the model's term: both, added
};

// The fewest changes (changed, dropped or added words, one each) that turn
// the captions' terms into the model's, in order. Ties go along the diagonal:
// a word the model changed stays paired with the one it stands for.
std::vector<Pair> align(const std::vector<Said>& said, const std::vector<text::Term>& written) {
    const std::size_t rows = said.size();
    const std::size_t columns = written.size();
    std::vector<Step> steps((rows + 1) * (columns + 1), Step::both);
    std::vector<std::uint32_t> previous(columns + 1);
    std::vector<std::uint32_t> current(columns + 1);
    for (std::size_t column = 0; column <= columns; ++column) {
        previous[column] = static_cast<std::uint32_t>(column);
        steps[column] = Step::added;
    }
    for (std::size_t row = 1; row <= rows; ++row) {
        current[0] = static_cast<std::uint32_t>(row);
        steps[row * (columns + 1)] = Step::dropped;
        for (std::size_t column = 1; column <= columns; ++column) {
            const bool same = said[row - 1].term.folded == written[column - 1].folded;
            const std::uint32_t diagonal = previous[column - 1] + (same ? 0U : 1U);
            const std::uint32_t up = previous[column] + 1;
            const std::uint32_t left = current[column - 1] + 1;
            const std::uint32_t best = std::min({diagonal, up, left});
            current[column] = best;
            Step step = Step::added;
            if (best == diagonal) {
                step = Step::both;
            } else if (best == up) {
                step = Step::dropped;
            }
            steps[(row * (columns + 1)) + column] = step;
        }
        std::swap(previous, current);
    }
    std::vector<Pair> pairs;
    std::size_t row = rows;
    std::size_t column = columns;
    while (row > 0 || column > 0) {
        const Step step = steps[(row * (columns + 1)) + column];
        if (step != Step::added) {
            --row;
        }
        if (step != Step::dropped) {
            --column;
        }
        pairs.push_back(Pair{.step = step, .said = row, .written = column});
    }
    std::ranges::reverse(pairs);
    return pairs;
}

// What one caption word gains from the model's text.
struct Gain {
    std::string before{};   // the marks before it
    std::string after{};    // the marks after it
    std::string carried{};  // a sentence end written after a word the model added
    std::vector<std::pair<text::Span, std::string_view>> capitals{};  // its terms, in the model's capitals
};

// What each caption word gains, step by step along the alignment; the
// projection counts the steps.
std::vector<Gain> gains(const std::vector<Word>& words, const std::vector<Said>& said, const Written& written,
                        Projection& projection) {
    std::vector<Gain> gained(words.size());
    std::optional<std::size_t> last_paired;  // the caption term paired last
    for (const Pair& pair : align(said, written.terms())) {
        if (pair.step == Step::dropped) {
            ++projection.dropped;
            continue;
        }
        if (pair.step == Step::added) {
            ++projection.added;
            const std::string marks = written.after(pair.written);
            if (last_paired.has_value() && last_of_word(said, *last_paired) && has_sentence_end(marks)) {
                gained[said[*last_paired].word].carried = marks;
            }
            continue;
        }
        const Said& term = said[pair.said];
        const std::string_view model = written.word(pair.written);
        const bool same = term.term.folded == written.terms()[pair.written].folded;
        ++(same ? projection.matched : projection.changed);
        Gain& gain = gained[term.word];
        if (first_of_word(said, pair.said)) {
            gain.before = written.before(pair.written);
        }
        if (last_of_word(said, pair.said)) {
            gain.after = written.after(pair.written);
        }
        if (same && recased(text::text_of(words[term.word].text, term.term.span), model)) {
            gain.capitals.emplace_back(term.term.span, model);
        }
        last_paired = pair.said;
    }
    return gained;
}

// `word` with what it gains: new capitals, and marks where the captions have
// none of their own (the header comment).
std::string punctuated_word(std::string word, Gain gain) {
    for (const auto& [span, model] : gain.capitals | std::views::reverse) {
        word.replace(span.begin, span.end - span.begin, model);
    }
    const std::vector<text::Term> terms = text::terms(word);
    if (terms.empty()) {
        return word;  // no word to punctuate: "—", "&"
    }
    if (!has_sentence_end(gain.after) && !gain.carried.empty()) {
        gain.after = std::move(gain.carried);
    }
    if (!has_projectable(
            text::text_of(word, text::Span{.begin = terms.back().span.end, .end = word.size()}))) {
        word += gain.after;
    }
    if (!has_projectable(text::text_of(word, text::Span{.begin = 0, .end = terms.front().span.begin}))) {
        word.insert(0, gain.before);
    }
    return word;
}

// The folded words of `text`, for the check that none changed.
std::vector<std::string> folded_words(std::string_view text) {
    std::vector<std::string> words;
    for (text::Term& term : text::terms(text)) {
        words.push_back(std::move(term.folded));
    }
    return words;
}

}  // namespace

Result<Projection> project(const std::vector<Word>& words, std::string_view punctuated) {
    std::vector<Said> said;
    for (std::size_t index = 0; index < words.size(); ++index) {
        for (text::Term& term : text::terms(words[index].text)) {
            said.push_back(Said{.word = index, .term = std::move(term)});
        }
    }
    const Written written{punctuated};
    if (said.size() > kMaxProjected || written.terms().size() > kMaxProjected) {
        return fail(
            Errc::invalid_argument,
            std::format("{} words of captions and {} of the model's text: project at most {} at a time",
                        said.size(), written.terms().size(), kMaxProjected));
    }
    Projection projection{.words = words};
    std::vector<Gain> gained = gains(words, said, written, projection);
    for (std::size_t index = 0; index < words.size(); ++index) {
        std::string& text = projection.words[index].text;
        text = punctuated_word(std::move(text), std::move(gained[index]));
        RR_ASSERT(folded_words(text) == folded_words(words[index].text), "a projection changes no word");
    }
    return projection;
}

}  // namespace rr::transcript
