// rr/text/numbers.hpp — the numbers a text states, by value.
//
// Why this exists
//   "Every number right" is a promise rReport checks, not one it hopes for. A
//   number in the transcript must reach the article with its value, whatever
//   the wording. The speaker says "thirty-seven percent", the article writes
//   "37%"; "one and a half million" is "1.5 million". So numbers are found
//   here, in digits and in words, and compared by value.
//
// What is read
//   * digits: "37", "1,234,567", "3.14", ".5", in any script's digits; and
//     "21st" (an ordinal), "1990s";
//   * words: "thirty-seven", "one hundred and five", "twelve hundred",
//     "two thousand and eight", "a dozen", "three point one four", "two and
//     a half"; ordinals ("third", "twenty-first", "hundredth");
//   * years said in pairs, from the 1500s to the 2000s: "nineteen eighty-four"
//     (1984), "twenty twenty" (2020), "nineteen oh five" (1905). "Ten thirty"
//     is a time, and stays 10 and 30;
//   * a scale after either: "1.5 million", "two billion", "5 thousand";
//   * a sign: "-5", "−5", "minus five", "negative five";
//   * percent: "%", "percent", "per cent".
//   A range shares its unit: in "5-10%", "5 to 10 percent" and "between 5 and
//   10 million", both ends are percentages, or millions. A scale is shared
//   only while the first end is at most ten times the second's count: "from
//   500 to 1 million" starts at 500.
//
// What is not
//   * fractions, passed over whole: "one third", "two thirds", "three
//     quarters", "a third of", "a thousandth of" (but "two and a half" is
//     2.5);
//   * multiples ("twice", "10x") and vague amounts ("dozens", "a few",
//     "millions of");
//   * a scale or "point" with no number before it: "the hundred best", and
//     the "point one" of "at this point one could";
//   * "one" as a pronoun ("no one", "the one", "each one", "one another"),
//     and "second" as time ("a second", "per second");
//   * digits glued to letters ("3D", "GPT4", "5k"): names, not quantities.
//
// Thread-safety: pure functions.
#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

#include <rr/text/span.hpp>

namespace rr::text {

// How a number is written.
enum class NumberForm : std::uint8_t {
    digits,  // "37", "1,234", "21st"
    words,   // "thirty-seven", "third"
    mixed,   // "1.5 million", "5 thousand"
};

struct NumberMention {
    double value = 0.0;  // finite; a percentage's value is its count ("37%": 37)
    bool percent = false;
    bool ordinal = false;
    NumberForm form = NumberForm::digits;
    Span span{};

    friend bool operator==(const NumberMention&, const NumberMention&) = default;
};

// The numbers of `text`, in order (see the header comment).
[[nodiscard]] std::vector<NumberMention> numbers(std::string_view text);

// Whether two values are one number: equal within a relative 1e-9, which
// forgives the rounding of decimals and nothing else.
[[nodiscard]] bool same_value(double lhs, double rhs) noexcept;

// Whether two mentions state one number: the same value, and both
// percentages or neither, both ordinals or neither.
[[nodiscard]] bool same_number(const NumberMention& lhs, const NumberMention& rhs) noexcept;

}  // namespace rr::text
