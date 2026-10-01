// rr/text/align.hpp — where a quote is in a text, and how faithfully.
//
// Why this exists
//   A quotation in the article must be what was said, word for word. A writer
//   model softens ("I think it's bad" for "it's terrible"), trims, or blends
//   two sentences into one quote. Searching for the quote as a string would
//   only say "not found". An alignment says where the quote is closest to the
//   transcript, and what differs, so the repair message can show the
//   transcript's own words.
//
// How
//   Local alignment (Smith and Waterman, 1981) of the quote's terms against
//   the text's, both folded (rr/text/tokens.hpp: words and numbers, without
//   punctuation, case or accents). It finds the stretch of the text that best
//   matches some stretch of the quote:
//     * a matching term scores `match`;
//     * a different term `mismatch`;
//     * a term on one side only `gap`.
//   The quote is verbatim when every one of its terms matched, in order, with
//   nothing between: punctuation, case and accents aside, the text says it.
//   Where several places align equally well, the first in the text is chosen.
//
//   The cost is the product of the two lengths: a 50-word quote against a
//   90-minute transcript (about 15,000 words) takes a few milliseconds.
//   Callers aligning many quotes against one text compute its terms once.
//
// Thread-safety: pure functions.
#pragma once

#include <cstddef>
#include <string_view>
#include <vector>

#include <rr/text/span.hpp>
#include <rr/text/tokens.hpp>

namespace rr::text {

struct AlignmentScores {
    int match = 2;      // > 0
    int mismatch = -1;  // <= 0
    int gap = -1;       // < 0
};

struct Alignment {
    int score = 0;
    std::size_t quote_terms = 0;  // the quote's terms
    std::size_t matched = 0;      // those matched by an equal term, in order
    std::size_t edits = 0;        // mismatches and gaps between the aligned ends
    Span quote{};                 // the aligned stretch of the quote (bytes of the quote)
    Span text{};                  // where it aligned in the text (bytes of the text)

    // The share of the quote's terms matched: 0 for a quote without terms.
    [[nodiscard]] double identity() const noexcept;

    // Every term of the quote matched, in order, with no edit: the text says it.
    [[nodiscard]] bool verbatim() const noexcept;

    friend bool operator==(const Alignment&, const Alignment&) = default;
};

// The best local alignment of `quote` in `text`. When nothing matches, the
// score and spans are zero. The scores must satisfy their comments.
[[nodiscard]] Alignment align(std::string_view quote, std::string_view text, AlignmentScores scores = {});

// The same, on terms computed once (terms()); the spans are those of the terms.
[[nodiscard]] Alignment align(const std::vector<Term>& quote, const std::vector<Term>& text,
                              AlignmentScores scores = {});

}  // namespace rr::text
