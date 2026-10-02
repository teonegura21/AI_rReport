// Where each value of a JSON text was written: exact spans on documents
// written to show each rule, and on every document, the property that ties
// them to parse(): each span's text parses to the value at its pointer.
#include <rr/json/spans.hpp>

#include <cstddef>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <rr/json/json.hpp>
#include <rr/testkit/result.hpp>

namespace rr::json {
namespace {

using ::testing::ElementsAre;
using testkit::unwrap;

// The text of each span, by pointer, as written.
std::vector<std::pair<std::string, std::string>> written(std::string_view text) {
    std::vector<std::pair<std::string, std::string>> found;
    for (const Span& span : unwrap(spans(text))) {
        found.emplace_back(span.pointer, std::string{text.substr(span.begin, span.end - span.begin)});
    }
    return found;
}

using Written = std::pair<std::string, std::string>;

// How many values `document` holds, itself included.
std::size_t values_in(const Json& document) {
    std::size_t count = 0;
    std::vector<const Json*> left{&document};  // a stack, not recursion: any depth
    while (!left.empty()) {
        const Json* value = left.back();
        left.pop_back();
        ++count;
        if (value->is_structured()) {
            for (const Json& child : *value) {
                left.push_back(&child);
            }
        }
    }
    return count;
}

TEST(Spans, AreEveryValueInTheOrderTheyBegin) {
    EXPECT_THAT(
        written(R"({"a": [1, {"b": "x"}], "c": null})"),
        ElementsAre(Written{"", R"({"a": [1, {"b": "x"}], "c": null})"}, Written{"/a", R"([1, {"b": "x"}])"},
                    Written{"/a/0", "1"}, Written{"/a/1", R"({"b": "x"})"}, Written{"/a/1/b", R"("x")"},
                    Written{"/c", "null"}));
}

TEST(Spans, CoverAValuesOwnBytesNeverTheSpaceAroundItNorItsKey) {
    const std::string document = "{ \"a\" :\r\n -1.5e+3 , \"b\":[ ] ,\"c\" : { } }";
    EXPECT_THAT(written("\xEF\xBB\xBF \n\t" + document + "  \n"),
                ElementsAre(Written{"", document}, Written{"/a", "-1.5e+3"}, Written{"/b", "[ ]"},
                            Written{"/c", "{ }"}))
        << "a byte order mark, as parse() skips it; whitespace of every kind";
    EXPECT_THAT(written(" true "), ElementsAre(Written{"", "true"})) << "a document of one scalar";
    EXPECT_THAT(written(R"([false, null, 0, "", [], {}])"),
                ElementsAre(Written{"", R"([false, null, 0, "", [], {}])"}, Written{"/0", "false"},
                            Written{"/1", "null"}, Written{"/2", "0"}, Written{"/3", R"("")"},
                            Written{"/4", "[]"}, Written{"/5", "{}"}));
}

TEST(Spans, ReadStringsToTheirClosingQuoteThroughEveryEscape) {
    EXPECT_THAT(written(R"({"q": "a \"quoted\" \\", "u": "é\n", "k": "}]"})"),
                ElementsAre(Written{"", R"({"q": "a \"quoted\" \\", "u": "é\n", "k": "}]"})"},
                            Written{"/q", R"("a \"quoted\" \\")"}, Written{"/u", R"("é\n")"},
                            Written{"/k", R"("}]")"}));
}

TEST(Spans, NameKeysAsJsonPointersDo) {
    EXPECT_EQ(pointer_token("a/b~c"), "a~1b~0c") << "RFC 6901: ~ first, then /";
    EXPECT_EQ(pointer_token("~1"), "~01") << "a key that looks escaped is escaped again";
    EXPECT_THAT(written(R"({"a/b": 1, "m~n": 2, "é\"": 3, "": 4})"),
                ElementsAre(Written{"", R"({"a/b": 1, "m~n": 2, "é\"": 3, "": 4})"}, Written{"/a~1b", "1"},
                            Written{"/m~0n", "2"}, Written{"/\xC3\xA9\"", "3"}, Written{"/", "4"}))
        << "a key is the text it stands for, its escapes read";
}

TEST(Spans, AreOnlyOfTheValuesParseKeeps) {
    const std::string text = R"({"a": {"x": [1]}, "b": 2, "a": {"y": 3}, "c": [{"k": 1, "k": 2}]})";
    EXPECT_EQ(dump(unwrap(parse(text))), R"({"a":{"y":3},"b":2,"c":[{"k":2}]})") << "the later of a key";
    EXPECT_THAT(written(text),
                ElementsAre(Written{"", text}, Written{"/b", "2"}, Written{"/a", R"({"y": 3})"},
                            Written{"/a/y", "3"}, Written{"/c", R"([{"k": 1, "k": 2}])"},
                            Written{"/c/0", R"({"k": 1, "k": 2})"}, Written{"/c/0/k", "2"}))
        << "the earlier value of a key, and everything in it, have none";
}

TEST(Spans, RefuseWhatParseRefusesAsItSays) {
    for (const std::string_view text : {"", "{", R"({"a": 1,})", "[1 2]", "nul", "1e999", R"("\x")"}) {
        const Result<std::vector<Span>> found = spans(text);
        ASSERT_FALSE(found.has_value()) << text;
        EXPECT_EQ(found.error().code(), Errc::invalid_argument) << text;
        EXPECT_EQ(found.error().message(), parse(text).error().message()) << text;
    }
}

// The reader keeps a stack of its own, so that no depth parse() reads can
// exhaust the thread's: here 5,000 levels, each value's pointer one "/0"
// longer than its parent's.
TEST(Spans, ReadDeepNesting) {
    const std::size_t depth = 5'000;
    const std::string text = std::string(depth, '[') + std::string(depth, ']');
    const std::vector<Span> found = unwrap(spans(text));
    ASSERT_EQ(found.size(), depth);
    EXPECT_EQ(found.back().pointer.size(), 2 * (depth - 1)) << "/0 a level";
    EXPECT_EQ(found.back().begin, depth - 1);
    EXPECT_EQ(found.back().end, depth + 1);
}

// The property the fuzz target (tests/fuzz/json_spans_fuzz.cpp) checks on
// any input, here on documents of every shape: each span's text parses to
// the value parse() puts at its pointer, and every value has one span.
TEST(Spans, EachParsesToTheValueAtItsPointer) {
    const std::vector<std::string_view> documents{
        R"({"a": {"x": [1]}, "b": 2, "a": {"y": 3}})",
        R"({"sentences": [{"sentence": "S1", "filler": null, "atoms": [{"also": [], "kind": "claim",
            "modality": "asserted", "must_quote": null}]}, {"sentence": "S2", "filler": "greeting",
            "atoms": []}]})",
        R"([[[1], [2, [3, {"x": [true]}]]], -0.0, 1E5, "😀", {"a": {"b": {"c": {}}}}])",
        R"({"a": 1, "b": {"a": 2}, "a": 3})",
        "[[[[[[]]]]]]",
        "\"just a string\"",
    };
    for (const std::string_view text : documents) {
        const Json document = unwrap(parse(text));
        const std::vector<Span> found = unwrap(spans(text));
        for (const Span& span : found) {
            ASSERT_LT(span.begin, span.end);
            ASSERT_LE(span.end, text.size());
            const Json::json_pointer at{span.pointer};
            ASSERT_TRUE(document.contains(at)) << text << " has no " << span.pointer;
            EXPECT_EQ(unwrap(parse(text.substr(span.begin, span.end - span.begin))), document.at(at))
                << span.pointer;
        }
        EXPECT_EQ(found.size(), values_in(document)) << text << ": one span a value";
    }
}

}  // namespace
}  // namespace rr::json
