// rr/knowledge/outline.hpp — the video's sections, and the brief every later
// mission is given.
//
// Why this exists
//   An article is written section by section, then joined at its seams (plan
//   §3.6), and each mission sees its own section and no more (§3.7). The
//   outline says where the sections are and what each is about. The brief
//   says, in about 1,500 tokens, what any mission needs to know of the whole
//   video: its genre, who speaks, the words it uses for things, and its gist.
//   Prompts put the brief in their system message, the part every mission of
//   a stage shares, so the engine reads it once (rr/prompt/library.hpp).
//   The outline stage writes both (rr/pipeline/outline_stage.hpp).
//
// What reading checks
//   Beyond the schema (schemas/artifacts/outline.json): the sections run in
//   order, each from its first sentence to its last, the first from S1, each
//   starting right after the one before. Whether the last ends at the
//   transcript's last sentence is a matter between the two artifacts:
//   outline_problems() says it.
//
// Thread-safety: plain values and pure functions.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <rr/core/result.hpp>
#include <rr/json/json.hpp>
#include <rr/knowledge/ids.hpp>
#include <rr/knowledge/transcript.hpp>

namespace rr::knowledge {

struct Section {
    SentenceId first;
    SentenceId last;
    std::string title;
    std::optional<std::string> gist{};  // what it says, in a sentence or two; none: no model wrote one

    friend bool operator==(const Section&, const Section&) = default;
};

enum class Genre : std::uint8_t {
    talk,          // a talk or a lecture
    conversation,  // a podcast, an interview, a panel
    tutorial,      // what is shown matters: slides, code, a whiteboard
    narrative,     // a documentary, a news report
    other,
};

[[nodiscard]] std::string_view to_string(Genre genre) noexcept;

// The genre `name` names ("talk"), as the artifact writes it; none for a
// word that names none.
[[nodiscard]] std::optional<Genre> genre_named(std::string_view name);

struct Speaker {
    std::string name;
    std::optional<std::string> role{};  // "host", "guest", "lecturer": as the video says; none: unsaid

    friend bool operator==(const Speaker&, const Speaker&) = default;
};

struct LexiconTerm {
    std::string term;
    std::optional<std::string> meaning{};  // as the video explains it; none: unexplained

    friend bool operator==(const LexiconTerm&, const LexiconTerm&) = default;
};

struct Brief {
    Genre genre = Genre::other;
    std::vector<Speaker> speakers{};
    std::vector<LexiconTerm> lexicon{};
    std::optional<std::string> gist{};  // the whole video, in a paragraph

    friend bool operator==(const Brief&, const Brief&) = default;
};

enum class SectionsFrom : std::uint8_t {
    chapters,  // the creator's chapters
    models,    // the commander's outlines, agreed on
    pauses,    // cut where the speaker paused longest: no chapters, no model
};

[[nodiscard]] std::string_view to_string(SectionsFrom from) noexcept;

struct Outline {
    std::vector<Section> sections{};  // in order, each right after the one before
    Brief brief{};
    SectionsFrom sections_from = SectionsFrom::pauses;

    friend bool operator==(const Outline&, const Outline&) = default;
};

[[nodiscard]] json::Json to_json(const Outline& outline);

// Errc::invalid_argument when `document` breaks the schema, or its sections
// are not in order from S1, each right after the one before.
[[nodiscard]] Result<Outline> outline_from_json(const json::Json& document);

// Where `outline` and `transcript` disagree, said as what to correct: a
// section past the transcript's end, a transcript longer than its sections.
[[nodiscard]] std::vector<std::string> outline_problems(const Outline& outline, const Transcript& transcript);

}  // namespace rr::knowledge
