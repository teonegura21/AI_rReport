// lexicon.hpp — the words rr_text's rules must know by name (private to rr_text).
//
// Sentences and names are found by rules about letters and punctuation, and
// a few words break those rules:
//   * an abbreviation's period need not end a sentence ("Dr. Smith");
//   * a capitalized word that starts a sentence is usually no name ("The",
//     "However"), and "of" may sit inside one ("Bank of America").
// The lists are English, as the videos rReport retells mostly are; another
// language's words simply fall back to the general rules.
//
// Words are looked up folded (rr/text/unicode.hpp): lowercase, without
// accents, with straight apostrophes. An abbreviation is looked up without
// its final period ("e.g", "u.s").
//
// Thread-safety: pure functions over constant tables.
#pragma once

#include <cstdint>
#include <string_view>

namespace rr::text::lexicon {

// What a word followed by a period is.
enum class Abbreviation : std::uint8_t {
    none,       // no known abbreviation: the period may end the sentence
    title,      // never ends one: "Dr. Smith", "e.g. this", "Roe v. Wade"
    terminal,   // often ends one: "etc.", "Inc.", "U.S.", "a.m."
    numbering,  // comes before a number: "No. 5", "p. 12", "Fig. 3"
};

[[nodiscard]] Abbreviation abbreviation(std::string_view folded) noexcept;

// A word that starts sentences but is never part of a name: articles,
// pronouns, conjunctions, prepositions, sentence adverbs ("the", "it's",
// "however", "today").
[[nodiscard]] bool is_function_word(std::string_view folded) noexcept;

// A lowercase word that can join the words of one name: "Bank of America",
// "Leonardo da Vinci", "Ludwig van Beethoven".
[[nodiscard]] bool is_name_connector(std::string_view folded) noexcept;

}  // namespace rr::text::lexicon
