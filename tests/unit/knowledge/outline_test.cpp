#include <rr/knowledge/outline.hpp>

#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include <rr/core/error.hpp>
#include <rr/testkit/result.hpp>

namespace rr::knowledge {
namespace {

using testkit::unwrap;

TimeSpan span(std::int64_t start, std::int64_t end) {
    return unwrap(TimeSpan::make(Millis{start}, Millis{end}));
}

Transcript sentences(std::uint32_t count) {
    Transcript transcript;
    for (std::uint32_t number = 1; number <= count; ++number) {
        transcript.sentences.push_back(
            Sentence{.id = SentenceId{number},
                     .text = "Something was said.",
                     .span = span(std::int64_t{number - 1} * 1'000, std::int64_t{number} * 1'000)});
    }
    return transcript;
}

// Two sections of S1–S4, one with a gist and one without; a brief with every
// optional field both ways.
Outline two_sections() {
    return Outline{
        .sections = {Section{.first = SentenceId{1},
                             .last = SentenceId{3},
                             .title = "Polonium",
                             .gist = "Found first."},
                     Section{.first = SentenceId{4}, .last = SentenceId{4}, .title = "Radium"}},
        .brief = Brief{.genre = Genre::talk,
                       .speakers = {Speaker{.name = "Ada", .role = "lecturer"}, Speaker{.name = "Ben"}},
                       .lexicon = {LexiconTerm{.term = "pitchblende", .meaning = "a uranium ore"},
                                   LexiconTerm{.term = "curie"}},
                       .gist = "How two elements were found."},
        .sections_from = SectionsFrom::models};
}

TEST(Outline, RoundTripsThroughItsArtifact) {
    const Outline outline = two_sections();
    const json::Json document = to_json(outline);
    EXPECT_EQ(document.at("sections").at(1).at("gist"), nullptr)
        << "a gist no model wrote is null, never empty";
    EXPECT_EQ(document.at("brief").at("speakers").at(1).at("role"), nullptr);
    EXPECT_EQ(document.at("brief").at("lexicon").at(1).at("meaning"), nullptr);
    EXPECT_EQ(unwrap(outline_from_json(document)), outline);

    Outline bare = two_sections();
    bare.brief = Brief{};
    bare.sections_from = SectionsFrom::pauses;
    EXPECT_EQ(to_json(bare).at("brief").at("gist"), nullptr);
    EXPECT_EQ(unwrap(outline_from_json(to_json(bare))), bare) << "a brief with nothing in it";
}

TEST(Outline, NamesItsGenresAndWhereItsSectionsCameFrom) {
    const std::vector<std::pair<Genre, std::string_view>> genres{{Genre::talk, "talk"},
                                                                 {Genre::conversation, "conversation"},
                                                                 {Genre::tutorial, "tutorial"},
                                                                 {Genre::narrative, "narrative"},
                                                                 {Genre::other, "other"}};
    for (const auto& [genre, name] : genres) {
        EXPECT_EQ(to_string(genre), name);
        EXPECT_EQ(genre_named(name), genre);
        Outline outline = two_sections();
        outline.brief.genre = genre;
        EXPECT_EQ(unwrap(outline_from_json(to_json(outline))).brief.genre, genre) << name;
    }
    EXPECT_EQ(genre_named("opera"), std::nullopt);
    EXPECT_EQ(genre_named("Talk"), std::nullopt) << "as the artifact writes it";
    const std::vector<std::pair<SectionsFrom, std::string_view>> sources{{SectionsFrom::chapters, "chapters"},
                                                                         {SectionsFrom::models, "models"},
                                                                         {SectionsFrom::pauses, "pauses"}};
    for (const auto& [from, name] : sources) {
        EXPECT_EQ(to_string(from), name);
        Outline outline = two_sections();
        outline.sections_from = from;
        EXPECT_EQ(unwrap(outline_from_json(to_json(outline))).sections_from, from) << name;
    }
}

void expect_refused(const json::Json& document, std::string_view because) {
    const Result<Outline> read = outline_from_json(document);
    ASSERT_FALSE(read.has_value()) << because;
    EXPECT_EQ(read.error().code(), Errc::invalid_argument);
    EXPECT_NE(read.error().message().find(because), std::string::npos) << read.error().message();
}

TEST(Outline, SectionsRunInOrderFromS1EachRightAfterTheOneBefore) {
    struct Case {
        std::string_view why;
        std::string first;  // of the second section; the first is S1–S3
        std::string because;
    };
    const std::vector<Case> cases{
        {"a gap", "S5",
         "a section starts at S5: sections run in order from S1, each right after the one before, "
         "and S4 comes next"},
        {"an overlap", "S3", "a section starts at S3"},
        {"a repeat", "S1", "a section starts at S1"},
    };
    for (const Case& test : cases) {
        json::Json document = to_json(two_sections());
        document["sections"][1]["first"] = test.first;
        document["sections"][1]["last"] = "S9";
        SCOPED_TRACE(test.why);
        expect_refused(document, test.because);
    }
    json::Json late = to_json(two_sections());
    late["sections"][0]["first"] = "S2";
    expect_refused(late, "a section starts at S2");
}

TEST(Outline, ASectionEndsWhereOrAfterItStarts) {
    json::Json document = to_json(two_sections());
    document["sections"][1]["first"] = "S4";
    document["sections"][1]["last"] = "S3";
    expect_refused(document, "the section S4\u2013S3 ends before it starts");
}

TEST(Outline, TheSchemaIsStrict) {
    struct Case {
        std::string_view why;
        json::Json document;
    };
    std::vector<Case> cases;
    cases.push_back({"an unknown field", to_json(two_sections())});
    cases.back().document["sections"][0]["confidence"] = 0.9;
    cases.push_back({"a missing gist", to_json(two_sections())});
    cases.back().document["sections"][0].erase("gist");
    cases.push_back({"an empty gist", to_json(two_sections())});
    cases.back().document["sections"][0]["gist"] = "";
    cases.push_back({"an empty title", to_json(two_sections())});
    cases.back().document["sections"][0]["title"] = "";
    cases.push_back({"no sections", to_json(two_sections())});
    cases.back().document["sections"] = json::Json::array();
    cases.push_back({"a sentence that is not one", to_json(two_sections())});
    cases.back().document["sections"][0]["first"] = "S0";
    cases.push_back({"a genre of no kind", to_json(two_sections())});
    cases.back().document["brief"]["genre"] = "opera";
    cases.push_back({"a source of no kind", to_json(two_sections())});
    cases.back().document["sections_from"] = "guesses";
    cases.push_back({"a speaker without a name", to_json(two_sections())});
    cases.back().document["brief"]["speakers"][0]["name"] = "";
    cases.push_back({"an empty meaning", to_json(two_sections())});
    cases.back().document["brief"]["lexicon"][0]["meaning"] = "";
    for (const Case& test : cases) {
        SCOPED_TRACE(test.why);
        expect_refused(test.document, "schema violation");
    }
}

TEST(Outline, SaysWhereItAndItsTranscriptDisagree) {
    EXPECT_TRUE(outline_problems(two_sections(), sentences(4)).empty());
    const std::vector<std::string> longer = outline_problems(two_sections(), sentences(6));
    ASSERT_EQ(longer.size(), 1U);
    EXPECT_EQ(longer[0],
              "the sections end at S4, before the transcript's last sentence, S6: S5\u2013S6 are in no "
              "section");
    const std::vector<std::string> shorter = outline_problems(two_sections(), sentences(3));
    ASSERT_EQ(shorter.size(), 1U);
    EXPECT_EQ(shorter[0], "the sections run to S4, past the transcript's last sentence, S3");
    EXPECT_EQ(outline_problems(Outline{}, sentences(2)).size(), 1U)
        << "an outline without sections covers nothing";
    EXPECT_TRUE(outline_problems(Outline{}, sentences(0)).empty());
}

}  // namespace
}  // namespace rr::knowledge
