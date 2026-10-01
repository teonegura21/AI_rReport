// rr/knowledge/article.hpp — the article, and the atoms each sentence cites.
//
// Why this exists
//   The writer composes Markdown with inline markers (ADR-0006):
//       He hedges: "I could be wrong" [A42]. The data came first [A41][A43].
//   A marker names an atom the sentence retells. Through markers, every
//   sentence of the article traces back to what was said, and so to the second
//   of the video it came from, and every atom can be checked to be told. So
//   markers are read by code, exactly, and never by a model.
//
// Markers
//   * A marker is "[A", a number from 1 without leading zeros, and "]":
//     "[A41]". A sentence that retells several atoms writes several markers:
//     "[A41][A43]".
//   * A citation that is no marker is a bad marker, reported with what to
//     write instead, which the repair message quotes:
//         "[A41, A43]", "[A41-A43]"     one atom per marker: "[A41][A43]"
//         "[a41]", "[ A41 ]", "[A041]"   "[A41]"
//         "[S12]", "[V3]"               the article cites atoms: "[A#]"
//         "[A0]"                        atoms are numbered from A1
//         "[A]", "[A#]"                 the atom's number is missing
//         "[A41 above]"                 a marker holds an atom's ID only
//         "[A41" (unclosed)             "[A41]"
//     A bracket is a citation when what it holds starts with A, S or V and a
//     digit, spaces allowed between ("[Applause]" and "[1]" are text).
//   * Code is not prose: in fenced code blocks and `inline code`, nothing is a
//     marker. "\[" is a bracket written as text.
//   * A marker belongs to the sentence it is in. One written after the
//     sentence's terminator ("… at sea level. [A12]") belongs to that sentence
//     still: a sentence of markers alone joins the one before it.
//
// Blocks
//   The article is cut into blocks, as Markdown is: headings ("# …"),
//   paragraphs, list items ("- …", "1. …"), block quotes ("> …"), and fenced
//   code ("```"), which runs to the end if never closed. Blank lines separate
//   blocks. Every block but code is cut into sentences (rr/text/sentences.hpp),
//   so a block quote's sentence may hold the ">" of its next line.
//
// Thread-safety: pure functions.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <rr/core/result.hpp>
#include <rr/json/json.hpp>
#include <rr/knowledge/ids.hpp>
#include <rr/text/span.hpp>

namespace rr::knowledge {

// The write stage's artifact (plan P20): the article's Markdown, markers and all.
struct Article {
    std::string markdown{};

    friend bool operator==(const Article&, const Article&) = default;
};

[[nodiscard]] json::Json to_json(const Article& article);

// Errc::invalid_argument when `document` breaks the schema
// (schemas/artifacts/article.json). Its markers are not judged here: a bad
// marker is a finding for the repair to fix (plan P13), not an unreadable
// artifact.
[[nodiscard]] Result<Article> article_from_json(const json::Json& document);

struct Marker {
    AtomId atom;
    text::Span span{};  // "[A41]" in the article

    friend bool operator==(const Marker&, const Marker&) = default;
};

struct BadMarker {
    text::Span span{};     // what was written
    std::string reason{};  // what is wrong, and what to write instead

    friend bool operator==(const BadMarker&, const BadMarker&) = default;
};

struct CitingSentence {
    text::Span span{};
    std::vector<Marker> markers{};  // in order; none for a sentence that cites nothing

    friend bool operator==(const CitingSentence&, const CitingSentence&) = default;
};

enum class BlockKind : std::uint8_t {
    heading,
    paragraph,
    list_item,
    quote,
    code,
};

struct Block {
    BlockKind kind = BlockKind::paragraph;
    text::Span span{};                        // the block's lines
    std::vector<CitingSentence> sentences{};  // none in code

    friend bool operator==(const Block&, const Block&) = default;
};

struct ParsedArticle {
    std::vector<Block> blocks{};
    std::vector<BadMarker> bad_markers{};

    // Every atom cited, once, in the order first cited.
    [[nodiscard]] std::vector<AtomId> cited() const;

    friend bool operator==(const ParsedArticle&, const ParsedArticle&) = default;
};

// The blocks, sentences and markers of `markdown` (see the header comment).
[[nodiscard]] ParsedArticle parse_article(std::string_view markdown);

// The text of `span`, a span of the parsed article, without its markers and
// the spaces before them: what a reader sees, and what checks compare.
// "He hedges [A42]." gives "He hedges.". Bad markers are kept: they are text
// until repaired.
[[nodiscard]] std::string without_markers(std::string_view markdown, const ParsedArticle& parsed,
                                          text::Span span);

// The inline code of `span`, a span of `markdown`: from a run of backticks to
// the next run of as many, as CommonMark reads it; a run never matched is
// text. Inside, nothing is a marker, and no quote mark opens a quotation.
[[nodiscard]] std::vector<text::Span> inline_code(std::string_view markdown, text::Span span);

}  // namespace rr::knowledge
