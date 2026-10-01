// rr/text/entities.hpp — the names a text gives, and where it mentions a term.
//
// Why this exists
//   A retelling must keep who and what. The article may not lose a name the
//   transcript gives, misspell it, or hand a claim to someone else. So two
//   questions are asked of texts, again and again:
//     * which names does this text give? (names())
//     * where does it mention this term? (mentions())
//
// Names
//   A name is a run of capitalized words ("Marie Curie", "NASA", "iPhone"). It
//   may hold:
//     * one or two lowercase connectors between capitalized words: "Bank of
//       America", "Leonardo da Vinci", "Pirates of the Caribbean";
//     * "&" and joining hyphens: "AT&T", "Jean-Paul Sartre", "COVID-19";
//     * numbers after a capitalized word: "Apollo 11", "Windows 10";
//     * the period of a title or an initial: "Dr. Smith", "John F. Kennedy".
//   It does not hold:
//     * function words at its head ("The Beatles" gives "Beatles"; "Yesterday
//       Apple" gives "Apple"), unless written in capitals ("US", "IT");
//     * "I", and its contractions.
//   And some runs are no names:
//     * a single capitalized word that starts a sentence, or a quote, since
//       grammar capitalizes it anyway: "Paris is lovely" gives no name. An
//       acronym or a word capitalized inside is kept: "NASA", "iPhone";
//     * any in a sentence written all in capitals, where case says nothing.
//   A possessive "'s" is left out: "Curie's" gives "Curie".
//
// Mentions
//   A term is mentioned where its words appear in a row, compared folded
//   (rr/text/unicode.hpp): "marie curie" mentions "Marie Curie", "Curie's"
//   mentions "Curie", and "state-of-the-art" mentions "state of the art", as
//   punctuation is not compared.
//
// Thread-safety: pure functions.
#pragma once

#include <string_view>
#include <vector>

#include <rr/text/span.hpp>

namespace rr::text {

// The names `text` gives, in order (see the header comment).
[[nodiscard]] std::vector<Span> names(std::string_view text);

// Where `text` mentions `term`, in order, without overlaps. A term with no
// words is mentioned nowhere.
[[nodiscard]] std::vector<Span> mentions(std::string_view text, std::string_view term);

}  // namespace rr::text
