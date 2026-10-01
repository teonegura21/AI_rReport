#include <rr/json/schema.hpp>

#include <algorithm>
#include <string>
#include <string_view>

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

// A judge's answer, written in the order a model should think: the quote,
// then the reasoning, then the decision. Alphabetically, the decision would
// come first, and the model would decide before it had read its evidence.
constexpr std::string_view kJudgeSchema = R"({
  "type": "object",
  "additionalProperties": false,
  "required": ["sentences", "overall"],
  "properties": {
    "sentences": {"type": "array", "items": {"$ref": "#/$defs/sentence"}},
    "overall": {"type": "string", "enum": ["keep", "revise"]}
  },
  "$defs": {
    "sentence": {
      "type": "object",
      "additionalProperties": false,
      "required": ["quote", "rationale", "decision"],
      "properties": {
        "quote": {"type": "string", "minLength": 1},
        "rationale": {"type": "string"},
        "decision": {"anyOf": [{"type": "string", "maxLength": 12}, {"type": "null"}]}
      }
    }
  }
})";

// kJudgeSchema for the model: in the order written, without the keywords
// decoders do not all support (minLength, maxLength), and compact.
constexpr std::string_view kJudgeModelFacing =
    R"({"type":"object","additionalProperties":false,"required":["sentences","overall"],)"
    R"("properties":{"sentences":{"type":"array","items":{"$ref":"#/$defs/sentence"}},)"
    R"("overall":{"type":"string","enum":["keep","revise"]}},)"
    R"("$defs":{"sentence":{"type":"object","additionalProperties":false,)"
    R"("required":["quote","rationale","decision"],)"
    R"("properties":{"quote":{"type":"string"},"rationale":{"type":"string"},)"
    R"("decision":{"anyOf":[{"type":"string"},{"type":"null"}]}}}}})";

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
    EXPECT_EQ(Schema::compile_text(R"({"$ref": "#/$defs/missing"})").error().code(), Errc::invalid_argument);
    EXPECT_EQ(Schema::compile_text("{not json").error().code(), Errc::invalid_argument);
}

TEST(Schema, TheModelGetsTheFieldsInTheOrderWritten) {
    const Schema schema = Schema::compile_text(kJudgeSchema).value();
    EXPECT_EQ(schema.model_facing(), kJudgeModelFacing)
        << "every object, nested and in $defs, keeps its authored order";
    EXPECT_EQ(Schema::compile_ordered(parse_ordered(kJudgeSchema).value()).value().model_facing(),
              kJudgeModelFacing)
        << "a document built in code keeps its order too";

    EXPECT_EQ(schema.document(), parse(kJudgeSchema).value()) << "the document itself is canonical";
    const Json judgement = parse(R"({"sentences": [{"quote": "", "rationale": "r", "decision": "keep"}],
                                     "overall": "keep"})")
                               .value();
    EXPECT_FALSE(schema.check(judgement).empty()) << "validation keeps the full rules (minLength)";
}

TEST(Schema, ACanonicalDocumentHasNoOrderToKeep) {
    // Json sorted the members as it parsed them: "overall" before "sentences",
    // "decision" before "quote". That is why schemas are compiled from text.
    const std::string facing = Schema::compile(parse(kJudgeSchema).value()).value().model_facing();
    EXPECT_LT(facing.find(R"("overall")"), facing.find(R"("sentences")")) << facing;
    EXPECT_LT(facing.find(R"("decision")"), facing.find(R"("quote")")) << facing;
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

TEST(SchemaRegistry, SchemasLoadedFromTextKeepTheirOrder) {
    SchemaRegistry registry;
    ASSERT_TRUE(registry.add_text("llm/judge", kJudgeSchema).has_value());
    ASSERT_NE(registry.find("llm/judge"), nullptr);
    EXPECT_EQ(registry.find("llm/judge")->model_facing(), kJudgeModelFacing);
    EXPECT_EQ(registry.add_text("llm/judge", kJudgeSchema).error().code(), Errc::conflict);
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

TEST(ToModelFacing, KeepsTheOrderOfAnOrderedDocument) {
    EXPECT_EQ(dump(to_model_facing(parse_ordered(kJudgeSchema).value())), kJudgeModelFacing);
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
