#include <rr/json/schema.hpp>

#include <algorithm>

#include <gtest/gtest.h>

namespace rr::json {
namespace {

// A small but realistic schema in the portable subset: an atomization entry.
const Json kAtomSchema = parse(R"({
  "type": "object",
  "additionalProperties": false,
  "required": ["id", "kind", "text", "quote"],
  "properties": {
    "id":    {"type": "string", "pattern": "^A[0-9]+$"},
    "kind":  {"type": "string", "enum": ["claim", "fact", "aside"]},
    "text":  {"type": "string", "minLength": 1},
    "quote": {"type": ["string", "null"]}
  }
})")
                             .value();

bool has_problem_containing(const std::vector<std::string>& problems, std::string_view needle) {
    return std::ranges::any_of(problems,
                               [&](const std::string& p) { return p.find(needle) != std::string::npos; });
}

TEST(Schema, ValidInstancePasses) {
    const Schema schema = Schema::compile(kAtomSchema).value();
    const Json atom =
        parse(R"({"id": "A12", "kind": "claim", "text": "Data beats models", "quote": null})").value();
    EXPECT_TRUE(schema.check(atom).empty());
    EXPECT_TRUE(schema.validate(atom).has_value());
}

TEST(Schema, ReportsEveryViolationWithItsPath) {
    const Schema schema = Schema::compile(kAtomSchema).value();
    const Json atom =
        parse(R"({"id": "X1", "kind": "rumor", "text": "", "quote": null, "extra": 1})").value();
    const std::vector<Violation> violations = schema.check(atom);
    auto at = [&](std::string_view path) {
        return std::ranges::any_of(violations, [&](const Violation& v) { return v.instance_path == path; });
    };
    EXPECT_TRUE(at("/id")) << describe(violations);            // pattern
    EXPECT_TRUE(at("/kind")) << describe(violations);          // enum
    EXPECT_TRUE(at("/text")) << describe(violations);          // minLength
    EXPECT_GE(violations.size(), 4U) << describe(violations);  // + additionalProperties

    const auto result = schema.validate(atom);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code(), Errc::invalid_argument);
    EXPECT_NE(result.error().message().find("/kind"), std::string::npos);
}

TEST(Schema, MissingRequiredProperty) {
    const Schema schema = Schema::compile(kAtomSchema).value();
    const auto violations = schema.check(parse(R"({"id": "A1", "kind": "fact", "text": "t"})").value());
    ASSERT_FALSE(violations.empty());
    EXPECT_TRUE(std::ranges::any_of(violations, [](const Violation& v) { return v.keyword == "required"; }))
        << describe(violations);
}

TEST(Schema, InvalidSchemaDocumentIsRejected) {
    const auto compiled = Schema::compile(parse(R"({"$ref": "#/$defs/missing"})").value());
    ASSERT_FALSE(compiled.has_value());
    EXPECT_EQ(compiled.error().code(), Errc::invalid_argument);
}

TEST(SchemaRegistry, AddFindValidateAndConflicts) {
    SchemaRegistry registry;
    ASSERT_TRUE(registry.add("llm/atom", kAtomSchema).has_value());
    EXPECT_EQ(registry.add("llm/atom", kAtomSchema).error().code(), Errc::conflict);
    EXPECT_EQ(registry.add_text("broken", "{not json").error().code(), Errc::invalid_argument);
    EXPECT_NE(registry.find("llm/atom"), nullptr);
    EXPECT_EQ(registry.find("nope"), nullptr);
    EXPECT_EQ(registry.validate("nope", Json::object()).error().code(), Errc::not_found);
    EXPECT_FALSE(registry.validate("llm/atom", Json::object()).has_value());
    EXPECT_EQ(registry.names(), (std::vector<std::string>{"llm/atom"}));
}

TEST(LintPortable, PortableSchemaHasNoProblems) {
    EXPECT_TRUE(lint_portable(kAtomSchema).empty());
}

TEST(LintPortable, FlagsNonPortableConstructs) {
    const Json schema = parse(R"({
      "type": "object",
      "required": ["a"],
      "properties": {
        "a": {"oneOf": [{"type": "string"}, {"type": "integer"}]},
        "b": {"$ref": "https://example.com/other.json"}
      }
    })")
                            .value();
    const auto problems = lint_portable(schema);
    EXPECT_TRUE(has_problem_containing(problems, "additionalProperties")) << problems.size();
    EXPECT_TRUE(has_problem_containing(problems, "property 'b' must be listed"));
    EXPECT_TRUE(has_problem_containing(problems, "'oneOf' is not in the portable subset"));
    EXPECT_TRUE(has_problem_containing(problems, "$ref may only point into #/$defs/"));
}

TEST(LintPortable, DetectsRecursionThroughDefs) {
    const Json schema = parse(R"({
      "$defs": {
        "node": {"type": "object", "additionalProperties": false, "required": ["children"],
                 "properties": {"children": {"type": "array", "items": {"$ref": "#/$defs/list"}}}},
        "list": {"type": "array", "items": {"$ref": "#/$defs/node"}}
      },
      "$ref": "#/$defs/node"
    })")
                            .value();
    EXPECT_TRUE(has_problem_containing(lint_portable(schema), "recursive definitions"));
}

TEST(LintPortable, PropertyNamesAreNotMistakenForKeywords) {
    // Properties literally named like keywords must not be reported.
    const Json schema = parse(R"({
      "type": "object", "additionalProperties": false, "required": ["type", "oneOf"],
      "properties": {"type": {"type": "string"}, "oneOf": {"type": "string"}}
    })")
                            .value();
    EXPECT_TRUE(lint_portable(schema).empty());
}

TEST(ToModelFacing, StripsValidationOnlyKeywordsButKeepsStructure) {
    const Json facing = to_model_facing(kAtomSchema);
    EXPECT_FALSE(facing["properties"]["id"].contains("pattern"));
    EXPECT_FALSE(facing["properties"]["text"].contains("minLength"));
    EXPECT_EQ(facing["properties"]["kind"]["enum"].size(), 3U);
    EXPECT_EQ(facing["additionalProperties"], false);
    EXPECT_EQ(facing["required"].size(), 4U);
}

TEST(ToModelFacing, KeepsPropertiesWhoseNamesLookLikeKeywords) {
    const Json schema =
        parse(R"({"type": "object", "properties": {"pattern": {"type": "string", "maxLength": 5}}})").value();
    const Json facing = to_model_facing(schema);
    ASSERT_TRUE(facing["properties"].contains("pattern"));
    EXPECT_FALSE(facing["properties"]["pattern"].contains("maxLength"));
}

}  // namespace
}  // namespace rr::json
