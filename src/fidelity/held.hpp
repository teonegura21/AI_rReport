// held.hpp — what the checks hold texts to (private to rr_fidelity).
//
// The rules of rr/fidelity/checks.hpp's header comment, "What the checks hold
// texts to", written once for both ledgers.
#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <rr/knowledge/ids.hpp>
#include <rr/knowledge/transcript.hpp>
#include <rr/text/align.hpp>
#include <rr/text/numbers.hpp>

namespace rr::fidelity::held {

// The numbers of `text` a check holds: all but a bare "one".
[[nodiscard]] std::vector<text::NumberMention> numbers(std::string_view text);

// Whether `numbers` states `wanted`: the same value and unit.
[[nodiscard]] bool states(const std::vector<text::NumberMention>& numbers, const text::NumberMention& wanted);

// Whether `text` gives `name`, whole or by its last word.
[[nodiscard]] bool names(std::string_view text, std::string_view name);

// Whether `text` uses `term`, whole.
[[nodiscard]] bool uses(std::string_view text, std::string_view term);

// The texts of `sentences`, joined by spaces; sentences not in `transcript`
// are left out.
[[nodiscard]] std::string said(const knowledge::Transcript& transcript,
                               const std::vector<knowledge::SentenceId>& sentences);

// "S2, S3": IDs as a message lists them.
template <typename Id>
[[nodiscard]] std::string listed(const std::vector<Id>& ids) {
    std::string list;
    for (const Id& id : ids) {
        list += list.empty() ? id.str() : ", " + id.str();
    }
    return list;
}

// The words of `text` a quote aligned with, when they are close enough to show
// a repair what was said: at least half the quote's words matched. A lone
// "the" in common shows nothing.
[[nodiscard]] std::optional<std::string> closest(const text::Alignment& alignment, std::string_view text);

// `text` cut to `limit` bytes at a code point boundary, with "…" when cut:
// quoted in a message, a long passage stays readable.
[[nodiscard]] std::string excerpt(std::string_view text, std::size_t limit = 160);

}  // namespace rr::fidelity::held
