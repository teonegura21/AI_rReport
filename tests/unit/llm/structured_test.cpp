#include <rr/llm/structured.hpp>

#include <string>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <rr/testkit/result.hpp>

#include "scripted_provider.hpp"

namespace rr::llm {
namespace {

using testing::answer;
using testing::ask;
using ::testing::HasSubstr;
using ::testing::Not;
using testing::ScriptedProvider;
using testkit::unwrap;

// {"title": string (at least 3 characters), "year": integer}
json::Schema book_schema() {
    return unwrap(json::Schema::compile(unwrap(json::parse(R"({
        "type": "object",
        "additionalProperties": false,
        "required": ["title", "year"],
        "properties": {
            "title": {"type": "string", "minLength": 3},
            "year": {"type": "integer"}
        }
    })"))));
}

struct Book {
    std::string title;
    int year = 0;
};

Result<Book> to_book(const json::Json& value) {
    const int year = value["year"].get<int>();
    if (year > 2100) {
        return fail(Errc::invalid_argument, "year is in the future");
    }
    return Book{.title = value["title"].get<std::string>(), .year = year};
}

constexpr ProviderCaps kLlamaServer{.json_schema = true, .thinking = true};

TEST(Structured, AcceptsAValidAnswer) {
    const ScriptedProvider model{{answer(R"({"title": "Dune", "year": 1965})")}, kLlamaServer};
    const auto book = unwrap(structured<Book>(model, ask("a book"), book_schema(), to_book, {}, {}));
    EXPECT_EQ(book.value.title, "Dune");
    EXPECT_EQ(book.value.year, 1965);
    EXPECT_EQ(book.repairs, 0);

    const ChatRequest sent = model.requests().at(0);
    ASSERT_TRUE(sent.json_schema.has_value()) << "constrained where the provider can";
    EXPECT_FALSE(sent.json_schema->at("properties").at("title").contains("minLength"))
        << "the grammar gets the model-facing schema; validation uses the full one";
    EXPECT_EQ(sent.thinking, false) << "thinking is off unless the request says otherwise";
}

TEST(Structured, LeavesWhatTheProviderCannotDoAlone) {
    const ScriptedProvider plain{{answer("```json\n{\"title\": \"Emma\", \"year\": 1815}\n```")}};
    const auto book = unwrap(structured<Book>(plain, ask("a book"), book_schema(), to_book, {}, {}));
    EXPECT_EQ(book.value.title, "Emma") << "a Markdown fence around the JSON is tolerated";
    const ChatRequest sent = plain.requests().at(0);
    EXPECT_FALSE(sent.json_schema.has_value());
    EXPECT_FALSE(sent.thinking.has_value());

    ChatRequest thinking = ask("a book");
    thinking.thinking = true;
    const ScriptedProvider server{{answer(R"({"title": "Dune", "year": 1965})")}, kLlamaServer};
    ASSERT_OK(structured<Book>(server, thinking, book_schema(), to_book, {}, {}));
    EXPECT_EQ(server.requests().at(0).thinking, true) << "an explicit choice stands";
}

TEST(Structured, SendsAnInvalidAnswerBackWithItsProblems) {
    const ScriptedProvider model{
        {answer(R"({"title": "It", "year": "1986"})"), answer(R"j({"title": "It (novel)", "year": 1986})j")},
        kLlamaServer};
    std::vector<std::string> events;
    const StreamSink sink{.on_content = [&](std::string_view) { events.emplace_back("text"); },
                          .on_restart = [&] { events.emplace_back("<restart>"); }};
    const auto book = unwrap(structured<Book>(model, ask("a book"), book_schema(), to_book, sink, {}));
    EXPECT_EQ(book.value.title, "It (novel)");
    EXPECT_EQ(book.repairs, 1);
    EXPECT_EQ(events, (std::vector<std::string>{"text", "<restart>", "text"}));

    const ChatRequest repair = model.requests().at(1);
    ASSERT_EQ(repair.messages.size(), 3U);
    EXPECT_EQ(repair.messages[1].role, MessageRole::assistant);
    EXPECT_EQ(repair.messages[1].text(), R"({"title": "It", "year": "1986"})") << "its own answer";
    const std::string instruction = repair.messages[2].text();
    EXPECT_THAT(instruction, HasSubstr("- /title (minLength)")) << "the full schema's rules count";
    EXPECT_THAT(instruction, HasSubstr("- /year (type)"));
    EXPECT_THAT(instruction, HasSubstr("corrected JSON only"));
}

TEST(Structured, GivesUpWithEveryProblemListed) {
    const ScriptedProvider model{{answer("I think it is Dune."), answer(R"({"title": "Dune"})")},
                                 kLlamaServer};
    const auto result = structured<Book>(model, ask("a book"), book_schema(), to_book, {}, {});
    ASSERT_ERRC(result, Errc::model_output_invalid);
    EXPECT_THAT(result.error().message(), HasSubstr("2 attempt(s)"));
    EXPECT_THAT(result.error().message(), HasSubstr("(required)"));
    EXPECT_THAT(model.requests().at(1).messages.back().text(), HasSubstr("it is not JSON"));
}

TEST(Structured, TheCallersChecksAndTheConversionAreProblemsToo) {
    StructuredOptions options;
    options.check = [](const json::Json& value) {
        return value["title"] == "Untitled" ? std::vector<std::string>{"/title: a real title, please"}
                                            : std::vector<std::string>{};
    };
    const ScriptedProvider model{
        {answer(R"({"title": "Untitled", "year": 2000})"), answer(R"({"title": "Foundation", "year": 3000})"),
         answer(R"({"title": "Foundation", "year": 1951})")},
        kLlamaServer};
    options.repairs = 2;
    const auto book = unwrap(structured<Book>(model, ask("a book"), book_schema(), to_book, {}, {}, options));
    EXPECT_EQ(book.value.year, 1951);
    EXPECT_EQ(book.repairs, 2);
    EXPECT_THAT(model.requests().at(1).messages.back().text(), HasSubstr("a real title, please"));
    EXPECT_THAT(model.requests().at(2).messages.back().text(), HasSubstr("year is in the future"));
}

TEST(Structured, SaysWhenAnAnswerWasCutShort) {
    ChatResponse cut = answer(R"({"title": "War and Pea)");
    cut.finish = FinishReason::length;
    const ScriptedProvider model{{cut, answer(R"({"title": "War and Peace", "year": 1869})")}, kLlamaServer};
    ASSERT_OK(structured<Book>(model, ask("a book"), book_schema(), to_book, {}, {}));
    EXPECT_THAT(model.requests().at(1).messages.back().text(), HasSubstr("cut off at the token limit"));

    const ScriptedProvider fine{{answer(R"({"title": 1})"), answer(R"({"title": "Ulysses", "year": 1922})")},
                                kLlamaServer};
    ASSERT_OK(structured<Book>(fine, ask("a book"), book_schema(), to_book, {}, {}));
    EXPECT_THAT(fine.requests().at(1).messages.back().text(), Not(HasSubstr("token limit")));
}

TEST(Structured, ProviderFailuresPassThrough) {
    const ScriptedProvider model{{fail(Errc::unavailable, "pod stopped")}, kLlamaServer};
    EXPECT_ERRC(structured<Book>(model, ask("a book"), book_schema(), to_book, {}, {}), Errc::unavailable);
}

}  // namespace
}  // namespace rr::llm
