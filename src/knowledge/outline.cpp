// The video's sections, and its brief (rr/knowledge/outline.hpp).
#include <rr/knowledge/outline.hpp>

#include <array>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <utility>

#include <rr/core/assert.hpp>
#include <rr/knowledge/artifacts.hpp>

#include "fields.hpp"

namespace rr::knowledge {

namespace {

constexpr std::array<std::string_view, 5> kGenres{"talk", "conversation", "tutorial", "narrative", "other"};
constexpr std::array<std::string_view, 3> kSectionsFrom{"chapters", "models", "pauses"};

// The schema allows S1, S2, … only.
SentenceId sentence(const json::Json& written) {
    const std::optional<SentenceId> id = SentenceId::parse(written.get<std::string>());
    RR_ASSERT(id.has_value(), "the outline's schema allows S1, S2, \u2026 only");
    return *id;
}

}  // namespace

std::string_view to_string(Genre genre) noexcept {
    return fields::name_of(genre, kGenres);
}

std::optional<Genre> genre_named(std::string_view name) {
    const Result<Genre> genre = fields::named<Genre>(name, kGenres);
    return genre.has_value() ? std::optional<Genre>{*genre} : std::nullopt;
}

std::string_view to_string(SectionsFrom from) noexcept {
    return fields::name_of(from, kSectionsFrom);
}

json::Json to_json(const Outline& outline) {
    json::Json sections = json::Json::array();
    for (const Section& section : outline.sections) {
        sections.push_back(json::Json{{"first", section.first.str()},
                                      {"last", section.last.str()},
                                      {"title", section.title},
                                      {"gist", fields::json_or_null(section.gist)}});
    }
    json::Json speakers = json::Json::array();
    for (const Speaker& speaker : outline.brief.speakers) {
        speakers.push_back(json::Json{{"name", speaker.name}, {"role", fields::json_or_null(speaker.role)}});
    }
    json::Json lexicon = json::Json::array();
    for (const LexiconTerm& term : outline.brief.lexicon) {
        lexicon.push_back(json::Json{{"term", term.term}, {"meaning", fields::json_or_null(term.meaning)}});
    }
    return json::Json{{"sections", std::move(sections)},
                      {"brief", json::Json{{"genre", std::string{to_string(outline.brief.genre)}},
                                           {"speakers", std::move(speakers)},
                                           {"lexicon", std::move(lexicon)},
                                           {"gist", fields::json_or_null(outline.brief.gist)}}},
                      {"sections_from", std::string{to_string(outline.sections_from)}}};
}

Result<Outline> outline_from_json(const json::Json& document) {
    RR_TRY(artifact_schemas().validate("artifacts/outline", document));
    Outline outline;
    std::uint32_t next = 1;  // the sentence the next section must start at
    for (const json::Json& item : document.at("sections")) {
        Section section{.first = sentence(item.at("first")),
                        .last = sentence(item.at("last")),
                        .title = item.at("title").get<std::string>(),
                        .gist = fields::optional_string(item.at("gist"))};
        if (section.first.number() != next) {
            return fail(Errc::invalid_argument,
                        std::format("a section starts at {}: sections run in order from S1, each right after "
                                    "the one before, and S{} comes next",
                                    section.first, next));
        }
        if (section.last < section.first) {
            return fail(Errc::invalid_argument, std::format("the section {}\u2013{} ends before it starts",
                                                            section.first, section.last));
        }
        next = section.last.number() + 1;
        outline.sections.push_back(std::move(section));
    }
    const json::Json& brief = document.at("brief");
    RR_TRY_ASSIGN(outline.brief.genre, fields::named<Genre>(brief.at("genre").get<std::string>(), kGenres));
    for (const json::Json& speaker : brief.at("speakers")) {
        outline.brief.speakers.push_back(Speaker{.name = speaker.at("name").get<std::string>(),
                                                 .role = fields::optional_string(speaker.at("role"))});
    }
    for (const json::Json& term : brief.at("lexicon")) {
        outline.brief.lexicon.push_back(LexiconTerm{.term = term.at("term").get<std::string>(),
                                                    .meaning = fields::optional_string(term.at("meaning"))});
    }
    outline.brief.gist = fields::optional_string(brief.at("gist"));
    RR_TRY_ASSIGN(outline.sections_from, fields::named<SectionsFrom>(
                                             document.at("sections_from").get<std::string>(), kSectionsFrom));
    return outline;
}

std::vector<std::string> outline_problems(const Outline& outline, const Transcript& transcript) {
    std::vector<std::string> problems;
    const std::size_t said = transcript.sentences.size();
    const std::size_t covered = outline.sections.empty() ? 0 : outline.sections.back().last.number();
    if (covered > said) {
        problems.push_back(
            std::format("the sections run to S{}, past the transcript's last sentence, S{}", covered, said));
    } else if (covered < said) {
        problems.push_back(
            std::format("the sections end at S{}, before the transcript's last sentence, S{}: "
                        "S{}\u2013S{} are in no section",
                        covered, said, covered + 1, said));
    }
    return problems;
}

}  // namespace rr::knowledge
