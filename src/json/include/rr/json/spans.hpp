// rr/json/spans.hpp — where each value of a JSON text was written: its JSON
// pointer, and its bytes.
//
// Why this exists
//   A parsed document forgets where its values stood in the text, and once
//   in a while that is what matters. A model's answer comes with how likely
//   each of its tokens was (rr/llm/chat.hpp, Choice::probabilities), and how
//   sure the model was of one field of the answer is how likely the tokens
//   that wrote that field were: the tokens whose bytes overlap the field's.
//   spans() says where every value is, so that its tokens can be found.
//
//   Every value gets a span: the document itself, each object and array,
//   each member's value and each element, in the order they begin. A value
//   is named by its JSON pointer (RFC 6901): "" for the document,
//   "/sentences/0/kind" for a member of an element of a member, with "~" and
//   "/" in a key written "~0" and "~1". A span covers the value's own bytes,
//   from its first ('{', '"', '-', a digit, a letter) to its last ('}', the
//   closing quote, …): never the whitespace around it, never a member's key.
//
//   The text is read as parse() reads it, and what parse() refuses is
//   refused with parse()'s own error. There is a span for each value of the
//   document parse() makes, and for nothing else: of a key written twice in
//   one object, parse() keeps the later value, and the earlier one, with
//   everything in it, has no span.
//
// Thread-safety: a pure function.
#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include <rr/core/result.hpp>

namespace rr::json {

// Where one value was written.
struct Span {
    std::string pointer{};  // RFC 6901: "" the document, "/a/0/b" deeper
    std::size_t begin = 0;  // its first byte
    std::size_t end = 0;    // one past its last

    friend bool operator==(const Span&, const Span&) = default;
};

// Every value of `text`, in the order they begin: one span a value of the
// document parse() makes of it. Errc::invalid_argument, as parse() says it,
// for a text that is not one JSON value.
[[nodiscard]] Result<std::vector<Span>> spans(std::string_view text);

// `key` as a JSON pointer writes it: "~" as "~0", "/" as "~1".
[[nodiscard]] std::string pointer_token(std::string_view key);

}  // namespace rr::json
