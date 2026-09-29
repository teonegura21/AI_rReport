// rr/core/strings.hpp — small, allocation-conscious string helpers.
//
// Why this exists
//   Config keys, HTTP headers, CLI arguments and file formats all need the same
//   handful of operations. They are ASCII-only on purpose: protocol text
//   (header names, TOML keys, hex) is ASCII, and locale-dependent functions
//   such as std::tolower change behavior with the user's locale.
//   Human-language text normalization belongs to the rr_text module (M1).
//
// Thread-safety: pure functions.
#pragma once

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace rr {

// Removes leading and trailing ASCII whitespace (space, \t, \n, \r, \f, \v).
[[nodiscard]] std::string_view trim(std::string_view text) noexcept;

// Splits on every occurrence of `delimiter`, keeping empty fields:
//   split("a,,b", ',') == {"a", "", "b"};  split("", ',') == {""}
[[nodiscard]] std::vector<std::string_view> split(std::string_view text, char delimiter);

[[nodiscard]] std::string join(std::span<const std::string> parts, std::string_view separator);
[[nodiscard]] std::string join(std::span<const std::string_view> parts, std::string_view separator);

[[nodiscard]] std::string to_lower_ascii(std::string_view text);
[[nodiscard]] std::string to_upper_ascii(std::string_view text);
[[nodiscard]] bool iequals_ascii(std::string_view lhs, std::string_view rhs) noexcept;

// Replaces every non-overlapping occurrence of `from` (which must not be empty).
[[nodiscard]] std::string replace_all(std::string_view text, std::string_view from, std::string_view to);

}  // namespace rr
