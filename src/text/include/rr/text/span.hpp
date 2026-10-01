// rr/text/span.hpp — where something is in a text.
//
// Why this exists
//   Everything rr_text finds is reported with its place: a token, a sentence, a
//   number, a name, the transcript words a quote matches. The place is a pair
//   of byte offsets into the text it was found in. So whatever a check flags
//   can be shown, highlighted and cited where it is, and findings can be
//   ordered and compared without copying text.
//
//   Offsets fall on code point boundaries of valid UTF-8, or on the byte an
//   invalid sequence was read at (rr/text/unicode.hpp).
//
// Thread-safety: a plain value.
#pragma once

#include <cstddef>
#include <string_view>

namespace rr::text {

// The bytes [begin, end) of a text.
struct Span {
    std::size_t begin = 0;
    std::size_t end = 0;

    [[nodiscard]] constexpr std::size_t size() const noexcept { return end - begin; }
    [[nodiscard]] constexpr bool empty() const noexcept { return begin == end; }

    friend constexpr bool operator==(const Span&, const Span&) = default;
};

// The text a span covers. The span must lie within `text`.
[[nodiscard]] constexpr std::string_view text_of(std::string_view text, Span span) noexcept {
    return text.substr(span.begin, span.size());
}

}  // namespace rr::text
