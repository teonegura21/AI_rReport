#include <rr/text/tokens.hpp>

#include <rr/text/unicode.hpp>

namespace rr::text {

namespace {

bool is_apostrophe(char32_t code_point) noexcept {
    return code_point == U'\'' || code_point == U'\u2019';
}

bool is_letter(const CodePoint& point) noexcept {
    return classify(point.value) == CharClass::letter;
}

bool is_digit(const CodePoint& point) noexcept {
    return classify(point.value) == CharClass::digit;
}

// Whether the comma at `at` separates thousands: exactly three digits follow it.
bool thousands_comma(const std::vector<CodePoint>& points, std::size_t at) noexcept {
    if (at + 3 >= points.size()) {
        return false;
    }
    for (std::size_t digit = at + 1; digit <= at + 3; ++digit) {
        if (!is_digit(points[digit])) {
            return false;
        }
    }
    return at + 4 == points.size() || !is_digit(points[at + 4]);
}

// The end of the run of letters, marks and digits that starts at `at`, and
// whether it holds a letter.
std::size_t run_end(const std::vector<CodePoint>& points, std::size_t at, bool& has_letter) {
    has_letter = false;
    std::size_t end = at;
    while (end < points.size()) {
        const CharClass kind = classify(points[end].value);
        if (kind == CharClass::letter || kind == CharClass::mark) {
            has_letter = has_letter || kind == CharClass::letter;
            ++end;
            continue;
        }
        if (kind == CharClass::digit) {
            ++end;
            continue;
        }
        const bool between_letters =
            end > at && end + 1 < points.size() && is_letter(points[end - 1]) && is_letter(points[end + 1]);
        if (is_apostrophe(points[end].value) && between_letters) {
            ++end;
            continue;
        }
        // Separators belong to numbers only: "v1.2" ends at the point.
        const bool between_digits = !has_letter && end > at && end + 1 < points.size() &&
                                    is_digit(points[end - 1]) && is_digit(points[end + 1]);
        if (points[end].value == U'.' && between_digits) {
            ++end;
            continue;
        }
        if (points[end].value == U',' && between_digits && thousands_comma(points, end)) {
            ++end;
            continue;
        }
        break;
    }
    return end;
}

}  // namespace

std::vector<Token> tokenize(std::string_view text) {
    const std::vector<CodePoint> points = code_points(text);
    std::vector<Token> tokens;
    for (std::size_t at = 0; at < points.size();) {
        const CharClass kind = classify(points[at].value);
        if (kind == CharClass::space || kind == CharClass::other) {
            ++at;
            continue;
        }
        if (kind == CharClass::letter || kind == CharClass::mark || kind == CharClass::digit) {
            bool has_letter = false;
            const std::size_t end = run_end(points, at, has_letter);
            tokens.push_back(
                Token{.kind = has_letter || kind == CharClass::mark ? TokenKind::word : TokenKind::number,
                      .span = {.begin = points[at].begin, .end = points[end - 1].end}});
            at = end;
            continue;
        }
        tokens.push_back(
            Token{.kind = kind == CharClass::punctuation ? TokenKind::punctuation : TokenKind::symbol,
                  .span = {.begin = points[at].begin, .end = points[at].end}});
        ++at;
    }
    return tokens;
}

std::vector<Term> terms(std::string_view text) {
    std::vector<Term> found;
    for (const Token& token : tokenize(text)) {
        if (token.kind != TokenKind::word && token.kind != TokenKind::number) {
            continue;
        }
        // A word or a number is made of valid code points, so it folds.
        Result<std::string> folded = fold(text_of(text, token.span));
        std::string term = folded.has_value() ? std::move(*folded) : std::string{text_of(text, token.span)};
        // Combining marks alone fold to nothing: there is nothing to compare.
        if (term.empty()) {
            continue;
        }
        found.push_back(Term{.folded = std::move(term), .span = token.span});
    }
    return found;
}

}  // namespace rr::text
