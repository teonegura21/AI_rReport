#include "lexicon.hpp"

#include <algorithm>
#include <array>
#include <functional>
#include <string_view>

namespace rr::text::lexicon {

namespace {

struct Entry {
    std::string_view word;
    Abbreviation kind;
};

// Sorted by word, byte order: looked up by binary search.
constexpr auto kAbbreviations = std::to_array<Entry>({
    {.word = "a.m", .kind = Abbreviation::terminal},  {.word = "adm", .kind = Abbreviation::title},
    {.word = "al", .kind = Abbreviation::terminal},   {.word = "approx", .kind = Abbreviation::title},
    {.word = "art", .kind = Abbreviation::numbering}, {.word = "bros", .kind = Abbreviation::terminal},
    {.word = "capt", .kind = Abbreviation::title},    {.word = "cf", .kind = Abbreviation::title},
    {.word = "ch", .kind = Abbreviation::numbering},  {.word = "cmdr", .kind = Abbreviation::title},
    {.word = "co", .kind = Abbreviation::terminal},   {.word = "col", .kind = Abbreviation::title},
    {.word = "corp", .kind = Abbreviation::terminal}, {.word = "cpt", .kind = Abbreviation::title},
    {.word = "d.c", .kind = Abbreviation::terminal},  {.word = "dept", .kind = Abbreviation::title},
    {.word = "dr", .kind = Abbreviation::title},      {.word = "e.g", .kind = Abbreviation::title},
    {.word = "e.u", .kind = Abbreviation::terminal},  {.word = "eq", .kind = Abbreviation::numbering},
    {.word = "est", .kind = Abbreviation::numbering}, {.word = "etc", .kind = Abbreviation::terminal},
    {.word = "fig", .kind = Abbreviation::numbering}, {.word = "figs", .kind = Abbreviation::numbering},
    {.word = "fr", .kind = Abbreviation::title},      {.word = "gen", .kind = Abbreviation::title},
    {.word = "gov", .kind = Abbreviation::title},     {.word = "hon", .kind = Abbreviation::title},
    {.word = "i.e", .kind = Abbreviation::title},     {.word = "inc", .kind = Abbreviation::terminal},
    {.word = "insp", .kind = Abbreviation::title},    {.word = "jr", .kind = Abbreviation::terminal},
    {.word = "llc", .kind = Abbreviation::terminal},  {.word = "lt", .kind = Abbreviation::title},
    {.word = "ltd", .kind = Abbreviation::terminal},  {.word = "maj", .kind = Abbreviation::title},
    {.word = "messrs", .kind = Abbreviation::title},  {.word = "mlle", .kind = Abbreviation::title},
    {.word = "mme", .kind = Abbreviation::title},     {.word = "mr", .kind = Abbreviation::title},
    {.word = "mrs", .kind = Abbreviation::title},     {.word = "ms", .kind = Abbreviation::title},
    {.word = "mt", .kind = Abbreviation::title},      {.word = "mx", .kind = Abbreviation::title},
    {.word = "no", .kind = Abbreviation::numbering},  {.word = "nos", .kind = Abbreviation::numbering},
    {.word = "nr", .kind = Abbreviation::numbering},  {.word = "p", .kind = Abbreviation::numbering},
    {.word = "p.m", .kind = Abbreviation::terminal},  {.word = "pg", .kind = Abbreviation::numbering},
    {.word = "plc", .kind = Abbreviation::terminal},  {.word = "pp", .kind = Abbreviation::numbering},
    {.word = "pres", .kind = Abbreviation::title},    {.word = "prof", .kind = Abbreviation::title},
    {.word = "rep", .kind = Abbreviation::title},     {.word = "rev", .kind = Abbreviation::title},
    {.word = "sec", .kind = Abbreviation::numbering}, {.word = "sen", .kind = Abbreviation::title},
    {.word = "sgt", .kind = Abbreviation::title},     {.word = "sr", .kind = Abbreviation::terminal},
    {.word = "st", .kind = Abbreviation::title},      {.word = "supt", .kind = Abbreviation::title},
    {.word = "u.k", .kind = Abbreviation::terminal},  {.word = "u.n", .kind = Abbreviation::terminal},
    {.word = "u.s", .kind = Abbreviation::terminal},  {.word = "u.s.a", .kind = Abbreviation::terminal},
    {.word = "v", .kind = Abbreviation::title},       {.word = "viz", .kind = Abbreviation::title},
    {.word = "vol", .kind = Abbreviation::numbering}, {.word = "vols", .kind = Abbreviation::numbering},
    {.word = "vs", .kind = Abbreviation::title},
});
static_assert(std::ranges::is_sorted(kAbbreviations, std::less{}, &Entry::word));

constexpr auto kFunctionWords = std::to_array<std::string_view>({
    "a",         "about",   "above",      "after",    "again",   "against",  "all",       "also",
    "although",  "always",  "among",      "an",       "and",     "another",  "any",       "anyway",
    "are",       "aren't",  "around",     "as",       "at",      "because",  "before",    "being",
    "below",     "between", "both",       "but",      "by",      "can",      "can't",     "could",
    "did",       "didn't",  "do",         "does",     "doesn't", "don't",    "down",      "during",
    "each",      "either",  "even",       "ever",     "every",   "finally",  "first",     "for",
    "from",      "further", "had",        "has",      "have",    "he",       "he's",      "hence",
    "her",       "here",    "here's",     "hers",     "herself", "him",      "himself",   "his",
    "how",       "however", "i",          "i'd",      "i'll",    "i'm",      "i've",      "if",
    "in",        "indeed",  "instead",    "into",     "is",      "isn't",    "it",        "it's",
    "its",       "itself",  "just",       "last",     "later",   "let",      "let's",     "many",
    "maybe",     "me",      "meanwhile",  "might",    "more",    "moreover", "most",      "much",
    "must",      "my",      "myself",     "neither",  "never",   "next",     "no",        "nor",
    "not",       "now",     "of",         "off",      "often",   "oh",       "ok",        "okay",
    "on",        "once",    "one",        "only",     "or",      "other",    "otherwise", "our",
    "ours",      "out",     "over",       "perhaps",  "please",  "quite",    "rather",    "really",
    "same",      "second",  "she",        "she's",    "should",  "since",    "so",        "some",
    "sometimes", "still",   "such",       "than",     "that",    "that's",   "the",       "their",
    "theirs",    "them",    "themselves", "then",     "there",   "there's",  "therefore", "these",
    "they",      "they're", "third",      "this",     "those",   "though",   "through",   "thus",
    "to",        "today",   "together",   "tomorrow", "too",     "under",    "unless",    "until",
    "up",        "upon",    "us",         "usually",  "very",    "was",      "wasn't",    "we",
    "we're",     "well",    "were",       "weren't",  "what",    "what's",   "whatever",  "when",
    "whenever",  "where",   "whereas",    "whether",  "which",   "while",    "who",       "whoever",
    "whom",      "whose",   "why",        "will",     "with",    "within",   "without",   "won't",
    "would",     "yes",     "yesterday",  "yet",      "you",     "you're",   "your",      "yours",
    "yourself",
});
static_assert(std::ranges::is_sorted(kFunctionWords));

constexpr auto kNameConnectors = std::to_array<std::string_view>({
    "al",
    "bin",
    "da",
    "de",
    "del",
    "della",
    "der",
    "di",
    "dos",
    "du",
    "el",
    "la",
    "le",
    "of",
    "the",
    "van",
    "von",
    "y",
});
static_assert(std::ranges::is_sorted(kNameConnectors));

}  // namespace

Abbreviation abbreviation(std::string_view folded) noexcept {
    const auto* const found = std::ranges::lower_bound(kAbbreviations, folded, std::less{}, &Entry::word);
    return found != kAbbreviations.end() && found->word == folded ? found->kind : Abbreviation::none;
}

bool is_function_word(std::string_view folded) noexcept {
    return std::ranges::binary_search(kFunctionWords, folded);
}

bool is_name_connector(std::string_view folded) noexcept {
    return std::ranges::binary_search(kNameConnectors, folded);
}

}  // namespace rr::text::lexicon
