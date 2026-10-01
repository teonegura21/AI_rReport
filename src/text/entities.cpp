#include <rr/text/entities.hpp>

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <rr/text/sentences.hpp>
#include <rr/text/tokens.hpp>
#include <rr/text/unicode.hpp>

#include "lexicon.hpp"

namespace rr::text {

namespace {

// A token of a sentence, with what names care about.
struct Word {
    Token token{};
    std::string folded{};
    bool capitalized = false;    // a word with a capital letter, other than "I"
    bool acronym = false;        // two letters or more, all capitals: "NASA", "US"
    bool inner_capital = false;  // a capital after its first letter: "iPhone", "McDonald", "NASA"
    bool lowercase = false;      // a word with a lowercase letter
    char32_t first = 0;          // its first code point
};

bool is_pronoun_i(std::string_view folded) noexcept {
    return folded == "i" || folded == "i'm" || folded == "i've" || folded == "i'd" || folded == "i'll";
}

Word describe(std::string_view text, const Token& token) {
    const std::string_view raw = text_of(text, token.span);
    Result<std::string> folded = fold(raw);
    Word word{.token = token, .folded = folded.has_value() ? std::move(*folded) : std::string{raw}};
    const std::vector<CodePoint> points = code_points(raw);
    word.first = points.empty() ? 0 : points.front().value;
    if (token.kind != TokenKind::word) {
        return word;
    }
    int letters = 0;
    int capitals = 0;
    for (const CodePoint& point : points) {
        if (classify(point.value) != CharClass::letter) {
            continue;
        }
        if (is_upper(point.value)) {
            ++capitals;
            word.inner_capital = word.inner_capital || letters > 0;
        }
        word.lowercase = word.lowercase || is_lower(point.value);
        ++letters;
    }
    word.capitalized = capitals > 0 && !is_pronoun_i(word.folded);
    word.acronym = letters >= 2 && capitals == letters;
    return word;
}

class NameFinder {
public:
    NameFinder(std::string_view text, Span sentence) : text_(text) {
        for (Token token : tokenize(text_of(text, sentence))) {
            token.span.begin += sentence.begin;
            token.span.end += sentence.begin;
            words_.push_back(describe(text, token));
        }
    }

    void find(std::vector<Span>& found) const {
        if (all_capitals()) {
            return;
        }
        std::size_t index = 0;
        while (index < words_.size()) {
            if (!words_[index].capitalized) {
                ++index;
                continue;
            }
            const std::size_t last = run_end(index);
            if (const std::optional<Span> name = name_in(index, last)) {
                found.push_back(*name);
            }
            index = last + 1;
        }
    }

private:
    // Case says nothing in a sentence of three words or more, none lowercase.
    [[nodiscard]] bool all_capitals() const {
        int words = 0;
        for (const Word& word : words_) {
            if (word.lowercase) {
                return false;
            }
            words += word.capitalized ? 1 : 0;
        }
        return words >= 3;
    }

    // The last word of the run of name words that starts at `first`. A
    // possessive ends it: "Sartre's COVID-19 study".
    [[nodiscard]] std::size_t run_end(std::size_t first) const {
        std::size_t last = first;
        std::size_t index = first + 1;
        while (index < words_.size() && !is_possessive(last)) {
            if (words_[index].capitalized) {
                last = index++;
                continue;
            }
            if (words_[index].token.kind == TokenKind::number &&
                words_[index - 1].capitalized) {  // "Apollo 11"
                last = index++;
                continue;
            }
            const std::optional<std::size_t> across = bridge(index);
            if (!across.has_value()) {
                break;
            }
            last = *across;
            index = *across + 1;
        }
        return last;
    }

    // What joins two parts of a name, from `index`, and the part after it:
    // one or two connectors ("of the"), "&", a joining hyphen, or the period
    // of a title or an initial.
    [[nodiscard]] std::optional<std::size_t> bridge(std::size_t index) const {
        std::size_t next = index;
        bool numbers_may_follow = false;
        if (is_connector(next)) {
            ++next;
            if (is_connector(next)) {
                ++next;
            }
        } else if (is_symbol(next, U'&') || is_joining_hyphen(next)) {
            ++next;
            numbers_may_follow = true;
        } else if (is_name_period(next)) {
            ++next;
        } else {
            return std::nullopt;
        }
        if (next >= words_.size()) {
            return std::nullopt;
        }
        const bool continues =
            words_[next].capitalized || (numbers_may_follow && words_[next].token.kind == TokenKind::number);
        return continues ? std::optional<std::size_t>{next} : std::nullopt;
    }

    // The length in bytes of a possessive ending of the word at `index`
    // ("'s", "\u2019s"), or 0.
    [[nodiscard]] std::size_t possessive_size(std::size_t index) const {
        const Word& word = words_[index];
        if (word.token.kind != TokenKind::word) {
            return 0;
        }
        const std::string_view raw = text_of(text_, word.token.span);
        for (const std::string_view ending : {std::string_view{"'s"}, std::string_view{"\u2019s"}}) {
            if (raw.size() > ending.size() && raw.ends_with(ending)) {
                return ending.size();
            }
        }
        return 0;
    }

    [[nodiscard]] bool is_possessive(std::size_t index) const { return possessive_size(index) > 0; }

    [[nodiscard]] bool is_connector(std::size_t index) const {
        return index < words_.size() && words_[index].token.kind == TokenKind::word &&
               !words_[index].capitalized && lexicon::is_name_connector(words_[index].folded);
    }

    [[nodiscard]] bool is_symbol(std::size_t index, char32_t symbol) const {
        return index < words_.size() && words_[index].token.kind != TokenKind::word &&
               words_[index].token.kind != TokenKind::number && words_[index].first == symbol;
    }

    [[nodiscard]] bool touches_previous(std::size_t index) const {
        return index > 0 && words_[index - 1].token.span.end == words_[index].token.span.begin;
    }

    [[nodiscard]] bool touches_next(std::size_t index) const {
        return index + 1 < words_.size() &&
               words_[index].token.span.end == words_[index + 1].token.span.begin;
    }

    [[nodiscard]] bool is_joining_hyphen(std::size_t index) const {
        return (is_symbol(index, U'-') || is_symbol(index, U'\u2010')) && touches_previous(index) &&
               touches_next(index);
    }

    // "Dr." in "Dr. Smith", "F." in "John F. Kennedy", "U." in "U.S. Army".
    [[nodiscard]] bool is_name_period(std::size_t index) const {
        if (!is_symbol(index, U'.') || !touches_previous(index)) {
            return false;
        }
        const Word& before = words_[index - 1];
        if (before.token.kind != TokenKind::word) {
            return false;
        }
        const bool initial = before.capitalized && code_points(text_of(text_, before.token.span)).size() == 1;
        return initial || lexicon::abbreviation(before.folded) == lexicon::Abbreviation::title;
    }

    // Whether the word at `index` starts the sentence, or a quote or clause in
    // it, where grammar capitalizes it.
    [[nodiscard]] bool starts_clause(std::size_t index) const {
        std::size_t first_word = 0;
        while (first_word < words_.size() && words_[first_word].token.kind != TokenKind::word &&
               words_[first_word].token.kind != TokenKind::number) {
            ++first_word;
        }
        if (index == first_word) {
            return true;
        }
        if (index == 0) {
            return false;
        }
        const Word& before = words_[index - 1];
        return before.token.kind == TokenKind::punctuation &&
               (is_opening(before.first) || before.first == U'"' || before.first == U':');
    }

    // The name in the run [first, last], if it gives one.
    [[nodiscard]] std::optional<Span> name_in(std::size_t first, std::size_t last) const {
        // Function words at the head ("The", "Yesterday"), and whatever is left
        // before a capitalized word ("In 2019 Apple" gives "Apple").
        while (first <= last) {
            const Word& head = words_[first];
            const bool function_word = lexicon::is_function_word(head.folded) && !head.acronym;
            if (head.capitalized && !function_word) {
                break;
            }
            ++first;
        }
        if (first > last) {
            return std::nullopt;
        }
        const Word& head = words_[first];
        if (first == last && starts_clause(first) && !head.inner_capital) {
            return std::nullopt;
        }
        // "Curie's" gives "Curie".
        return Span{.begin = head.token.span.begin,
                    .end = words_[last].token.span.end - possessive_size(last)};
    }

    std::string_view text_;
    std::vector<Word> words_;
};

}  // namespace

std::vector<Span> names(std::string_view text) {
    std::vector<Span> found;
    for (const Span& sentence : sentences(text)) {
        NameFinder{text, sentence}.find(found);
    }
    return found;
}

std::vector<Span> mentions(std::string_view text, std::string_view term) {
    const std::vector<Term> wanted = terms(term);
    if (wanted.empty()) {
        return {};
    }
    const std::vector<Term> found = terms(text);
    std::vector<Span> spans;
    std::size_t index = 0;
    while (index + wanted.size() <= found.size()) {
        bool all = true;
        for (std::size_t offset = 0; offset < wanted.size() && all; ++offset) {
            const std::string& have = found[index + offset].folded;
            const std::string& want = wanted[offset].folded;
            const bool last = offset + 1 == wanted.size();
            all = have == want ||
                  (last && have.size() == want.size() + 2 && have.starts_with(want) && have.ends_with("'s"));
        }
        if (all) {
            spans.push_back(
                Span{.begin = found[index].span.begin, .end = found[index + wanted.size() - 1].span.end});
            index += wanted.size();
        } else {
            ++index;
        }
    }
    return spans;
}

}  // namespace rr::text
