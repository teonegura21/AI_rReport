// rr/text/tokens.hpp — words, numbers and punctuation, with where they are.
//
// Why this exists
//   Every comparison of texts in rReport is a comparison of tokens:
//     * a quote is aligned with the transcript word by word (align.hpp);
//     * a term is found by its words (entities.hpp);
//     * a sentence is retrieved by them (bm25.hpp).
//   Tokens keep their byte offsets in the text, so whatever matches can be
//   shown, and cited, where it is.
//
//   The rules, which hold for any script (rr/text/unicode.hpp):
//     * a word is a run of letters, marks and digits with at least one
//       letter ("GPT4", "3D", "1st"). It may hold an apostrophe between
//       letters ("don't", "l’été"). A hyphen splits: "state-of-the-art" is
//       four words, and "thirty-seven" two number words;
//     * a number is a run of digits. A point between digits is a decimal
//       point ("3.5"); a comma followed by exactly three digits separates
//       thousands ("1,234,567"). "1,2,3" is three numbers;
//     * punctuation and symbols are one code point each ("%", "$", "—");
//     * spaces and controls separate tokens, and are not tokens.
//   Invalid UTF-8 bytes are read as U+FFFD, a symbol: tokenizing never fails.
//
// Thread-safety: pure functions.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <rr/text/span.hpp>

namespace rr::text {

enum class TokenKind : std::uint8_t {
    word,
    number,
    punctuation,
    symbol
};

struct Token {
    TokenKind kind = TokenKind::word;
    Span span{};

    friend bool operator==(const Token&, const Token&) = default;
};

// The tokens of `text`, in order (see the header comment).
[[nodiscard]] std::vector<Token> tokenize(std::string_view text);

// A word or a number, folded for comparison (fold()), and where it is in the
// text it came from.
struct Term {
    std::string folded{};
    Span span{};

    friend bool operator==(const Term&, const Term&) = default;
};

// The words and numbers of `text`, folded: what texts are compared by.
// Punctuation and symbols are left out, and so is a word of combining marks
// alone, which folds to nothing (fold() removes accents).
[[nodiscard]] std::vector<Term> terms(std::string_view text);

}  // namespace rr::text
