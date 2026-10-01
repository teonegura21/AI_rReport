// rr/text/unicode.hpp — UTF-8 text, made comparable.
//
// Why this exists
//   Transcripts and articles are compared constantly. Is this quote in the
//   transcript? Does this paragraph name that person? Unicode lets one text
//   be written many ways, and comparing bytes would call the same words
//   different:
//     * "é" is one code point, or an "e" followed by a combining accent;
//     * "ﬁ" is a ligature of "f" and "i";
//     * "Straße" and "STRASSE" are one word;
//     * "don’t" and "don't" differ only in their apostrophe.
//   So there are two forms:
//     * nfc(), the canonical form a text is kept in (Unicode NFC): the same
//       characters, the same bytes;
//     * fold(), the form texts are *compared* in. It decomposes ligatures
//       (NFKC: "ﬁ" → "fi") and folds case ("Straße" → "strasse"). It
//       removes accents, which captions often lose ("café" → "cafe"), and
//       makes every apostrophe a straight one.
//   Invalid UTF-8, such as a file in another encoding, is refused by both;
//   repair() replaces each invalid byte with U+FFFD, for text that must be
//   read anyway.
//
// Built on utf8proc (ADR-0012).
//
// Thread-safety: pure functions.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <rr/core/result.hpp>

namespace rr::text {

[[nodiscard]] bool valid_utf8(std::string_view text) noexcept;

// `text` with each byte that is not part of valid UTF-8 replaced by U+FFFD.
[[nodiscard]] std::string repair(std::string_view text);

// The canonical form (NFC). Errc::invalid_argument for invalid UTF-8.
[[nodiscard]] Result<std::string> nfc(std::string_view text);

// The form texts are compared in (see the header comment). Errc::invalid_argument
// for invalid UTF-8.
[[nodiscard]] Result<std::string> fold(std::string_view text);

// One code point of a text, and where it is (byte offsets).
struct CodePoint {
    char32_t value = 0;
    std::size_t begin = 0;
    std::size_t end = 0;
};

// The code points of `text`, in order. An invalid byte is U+FFFD, one byte long.
[[nodiscard]] std::vector<CodePoint> code_points(std::string_view text);

// What a code point is, as text processing needs to know.
enum class CharClass : std::uint8_t {
    letter,       // L*: letters of any script
    mark,         // M*: combining marks, which belong to the letter before them
    digit,        // Nd: decimal digits
    punctuation,  // P*
    symbol,       // S*, and numbers that are not decimal digits (No, Nl: "²", "Ⅻ")
    space,        // Z*, and the whitespace controls (tab, newlines)
    other         // other controls, formats, unassigned
};

[[nodiscard]] CharClass classify(char32_t code_point) noexcept;

// An uppercase or titlecase letter (Lu, Lt).
[[nodiscard]] bool is_upper(char32_t code_point) noexcept;

// A lowercase letter (Ll).
[[nodiscard]] bool is_lower(char32_t code_point) noexcept;

// Punctuation that opens a phrase (Ps, Pi: "(", "[", "“", "«") or closes one
// (Pe, Pf: ")", "]", "”", "»"). The straight quotes '"' and "'" are neither:
// they do both, and a caller decides by position.
[[nodiscard]] bool is_opening(char32_t code_point) noexcept;
[[nodiscard]] bool is_closing(char32_t code_point) noexcept;

// The value, 0 to 9, of a decimal digit of any script ("7", "٧", "७", "７"), or
// -1 for a code point that is not one (Nd).
[[nodiscard]] int digit_value(char32_t code_point) noexcept;

}  // namespace rr::text
