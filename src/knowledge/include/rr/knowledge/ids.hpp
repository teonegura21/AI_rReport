// rr/knowledge/ids.hpp — the short IDs of an analysis: S12, A41, V3.
//
// Why this exists
//   Inside an analysis, everything is named by a short, model-friendly ID
//   (ADR-0009): the transcript's sentences S1, S2, …, the atoms A1, A2, …, the
//   on-screen items V1, V2, …. Models read them in packets, and write them in
//   answers and in the article's markers. Code assigns every one and checks
//   every one (ADR-0006). The types keep the three apart: an AtomId cannot be
//   passed where a SentenceId is meant.
//
//   The written form is strict: the letter, then a number from 1, without
//   leading zeros ("A12"; never "a12", "A 12", "A012" or "A0"). A model's loose
//   spelling is a finding to repair, never a guess (rr/knowledge/article.hpp).
//
// Thread-safety: plain values.
#pragma once

#include <compare>
#include <cstddef>
#include <cstdint>
#include <format>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

#include <rr/core/assert.hpp>

namespace rr::knowledge {

template <char Letter>
class ShortId {
public:
    static constexpr char kLetter = Letter;
    // Nine digits at most: the written form stays short and the number fits 32 bits.
    static constexpr std::uint32_t kMax = 999'999'999;

    // `number` must be 1 to kMax: IDs are numbered from 1.
    constexpr explicit ShortId(std::uint32_t number) : number_(number) {
        const bool numbered = number >= 1 && number <= kMax;
        RR_ASSERT(numbered, "short IDs are numbered from 1 to 999,999,999");
    }

    // The ID written exactly as `text` ("A12"), or none.
    [[nodiscard]] static constexpr std::optional<ShortId> parse(std::string_view text) noexcept {
        constexpr std::size_t kMaxDigits = 9;
        if (text.size() < 2 || text.size() > 1 + kMaxDigits || text.front() != Letter || text[1] == '0') {
            return std::nullopt;
        }
        std::uint32_t number = 0;
        for (const char digit : text.substr(1)) {
            if (digit < '0' || digit > '9') {
                return std::nullopt;
            }
            number = (number * 10) + static_cast<std::uint32_t>(digit - '0');
        }
        return ShortId{number};
    }

    [[nodiscard]] constexpr std::uint32_t number() const noexcept { return number_; }

    // "A12".
    [[nodiscard]] std::string str() const { return std::format("{}{}", Letter, number_); }

    friend constexpr auto operator<=>(const ShortId&, const ShortId&) = default;

private:
    std::uint32_t number_;
};

using SentenceId = ShortId<'S'>;  // a sentence of the transcript
using AtomId = ShortId<'A'>;      // an atom: one unit of what was said
using VisualId = ShortId<'V'>;    // an item on screen

}  // namespace rr::knowledge

template <char Letter>
struct std::hash<rr::knowledge::ShortId<Letter>> {
    std::size_t operator()(const rr::knowledge::ShortId<Letter>& id) const noexcept {
        return std::hash<std::uint32_t>{}(id.number());
    }
};

template <char Letter>
struct std::formatter<rr::knowledge::ShortId<Letter>> : std::formatter<std::string_view> {
    auto format(const rr::knowledge::ShortId<Letter>& id, std::format_context& context) const {
        return std::formatter<std::string_view>::format(id.str(), context);
    }
};
