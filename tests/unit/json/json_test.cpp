#include <rr/json/json.hpp>

#include <gtest/gtest.h>

namespace rr::json {
namespace {

TEST(Parse, ValidDocument) {
    const auto parsed = parse(R"({"a": [1, 2, {"b": null}]})");
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ((*parsed)["a"][2]["b"], nullptr);
}

TEST(Parse, InvalidDocument_ReportsPositionWithoutThrowing) {
    const auto parsed = parse(R"({"a": 1,})");
    ASSERT_FALSE(parsed.has_value());
    EXPECT_EQ(parsed.error().code(), Errc::invalid_argument);
    EXPECT_NE(parsed.error().message().find("byte"), std::string::npos) << parsed.error().message();
}

TEST(Dump, IsCanonical_KeysSortedNoWhitespace) {
    EXPECT_EQ(dump(parse(R"({ "b": 1, "a": {"d": true, "c": [3, 2]} })").value()),
              R"({"a":{"c":[3,2],"d":true},"b":1})");
}

TEST(Dump, InvalidUtf8_IsReplacedNotThrown) {
    const Json value = std::string("ok \xff\xfe end");
    std::string text;
    ASSERT_NO_THROW(text = dump(value));
    EXPECT_NE(text.find("\xEF\xBF\xBD"), std::string::npos) << "expected U+FFFD in " << text;
}

TEST(OrderedJson, KeepsMembersInTheOrderWritten) {
    const OrderedJson document =
        parse_ordered(R"({ "verdict": "ok", "evidence": {"quote": "x", "at": 1}, "a": [3, 1] })").value();
    EXPECT_EQ(dump(document), R"({"verdict":"ok","evidence":{"quote":"x","at":1},"a":[3,1]})")
        << "compact, in the document's own order, nested objects included";
    EXPECT_EQ(dump(Json(document)), R"({"a":[3,1],"evidence":{"at":1,"quote":"x"},"verdict":"ok"})")
        << "as a Json, canonical again";
}

TEST(OrderedJson, ReportsErrorsAndBadBytesLikeJson) {
    const auto parsed = parse_ordered(R"({"a": 1,})");
    ASSERT_FALSE(parsed.has_value());
    EXPECT_EQ(parsed.error().code(), Errc::invalid_argument);
    EXPECT_NE(parsed.error().message().find("byte"), std::string::npos) << parsed.error().message();

    const OrderedJson broken = std::string("ok \xff end");
    std::string text;
    ASSERT_NO_THROW(text = dump(broken));
    EXPECT_NE(text.find("\xEF\xBF\xBD"), std::string::npos) << "expected U+FFFD in " << text;
}

TEST(ContentHash, EqualDocumentsHashEqually) {
    const Json a = parse(R"({"x": 1, "y": [true, "z"]})").value();
    const Json b = parse(R"({"y": [true, "z"], "x": 1})").value();
    const Json c = parse(R"({"x": 2, "y": [true, "z"]})").value();
    EXPECT_EQ(content_hash(a), content_hash(b));
    EXPECT_NE(content_hash(a), content_hash(c));
}

TEST(TypedAccess, NestedPathsAndClearErrors) {
    const Json doc =
        parse(R"({"usage": {"prompt_tokens": 12, "ratio": 0.5, "cached": false}, "id": "x"})").value();
    EXPECT_EQ(get_int(doc, "usage.prompt_tokens").value(), 12);
    EXPECT_DOUBLE_EQ(get_number(doc, "usage.ratio").value(), 0.5);
    EXPECT_FALSE(get_bool(doc, "usage.cached").value());
    EXPECT_EQ(get_string(doc, "id").value(), "x");

    const auto missing = get_int(doc, "usage.completion_tokens");
    ASSERT_FALSE(missing.has_value());
    EXPECT_NE(missing.error().message().find("usage.completion_tokens"), std::string::npos);

    const auto wrong_type = get_string(doc, "usage.prompt_tokens");
    ASSERT_FALSE(wrong_type.has_value());
    EXPECT_NE(wrong_type.error().message().find("must be a string"), std::string::npos);

    const auto through_scalar = get_int(doc, "id.length");
    ASSERT_FALSE(through_scalar.has_value());
}

}  // namespace
}  // namespace rr::json
