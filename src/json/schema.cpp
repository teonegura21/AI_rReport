#include <rr/json/schema.hpp>

#include <algorithm>
#include <array>
#include <format>
#include <functional>
#include <set>

#include <jsoncons/json.hpp>
#include <jsoncons_ext/jsonschema/jsonschema.hpp>

namespace rr::json {
namespace {

namespace jsch = jsoncons::jsonschema;

// Keywords every consumer understands: they shape generation.
constexpr std::array<std::string_view, 12> kStructuralKeywords{
    "type",  "properties", "required", "additionalProperties", "items", "enum", "const", "anyOf",
    "allOf", "$ref",       "$defs",    "description",
};

// Keywords enforced by post-generation validation but stripped for models.
constexpr std::array<std::string_view, 19> kValidationOnlyKeywords{
    "title",    "minLength", "maxLength",        "pattern",          "format",
    "minimum",  "maximum",   "exclusiveMinimum", "exclusiveMaximum", "multipleOf",
    "minItems", "maxItems",  "uniqueItems",      "minProperties",    "maxProperties",
    "$schema",  "$id",       "$comment",         "default",
};

bool contains(std::span<const std::string_view> list, std::string_view value) {
    return std::ranges::find(list, value) != list.end();
}

// Converts to jsoncons through text. Our documents are small (schemas and
// model outputs are kilobytes), so clarity beats a hand-written tree walk.
jsoncons::json to_jsoncons(const Json& value) {
    return jsoncons::json::parse(dump(value));
}

// Visits every subschema of `schema` with its JSON-Pointer-like location.
// Property and $defs *names* are never mistaken for keywords.
void for_each_subschema(const Json& schema, const std::string& location,
                        const std::function<void(const Json&, const std::string&)>& visit) {
    if (!schema.is_object()) {
        return;
    }
    visit(schema, location);
    for (const char* map_keyword : {"properties", "$defs"}) {
        if (const auto it = schema.find(map_keyword); it != schema.end() && it->is_object()) {
            for (const auto& [name, subschema] : it->items()) {
                for_each_subschema(subschema, std::format("{}/{}/{}", location, map_keyword, name), visit);
            }
        }
    }
    if (const auto it = schema.find("items"); it != schema.end()) {
        for_each_subschema(*it, location + "/items", visit);
    }
    for (const char* list_keyword : {"anyOf", "allOf"}) {
        if (const auto it = schema.find(list_keyword); it != schema.end() && it->is_array()) {
            for (std::size_t index = 0; index < it->size(); ++index) {
                for_each_subschema((*it)[index], std::format("{}/{}/{}", location, list_keyword, index),
                                   visit);
            }
        }
    }
}

// Collects the $defs names referenced (via "#/$defs/<name>") anywhere inside `schema`.
void collect_refs(const Json& schema, std::set<std::string>& out) {
    for_each_subschema(schema, "", [&](const Json& node, const std::string&) {
        if (const auto it = node.find("$ref"); it != node.end() && it->is_string()) {
            const auto& ref = it->get_ref<const std::string&>();
            constexpr std::string_view kPrefix = "#/$defs/";
            if (ref.starts_with(kPrefix)) {
                out.insert(ref.substr(kPrefix.size()));
            }
        }
    });
}

bool is_object_schema(const Json& node) {
    if (node.contains("properties")) {
        return true;
    }
    const auto type = node.find("type");
    if (type == node.end()) {
        return false;
    }
    if (type->is_string()) {
        return type->get_ref<const std::string&>() == "object";
    }
    return type->is_array() && std::ranges::find(*type, Json("object")) != type->end();
}

}  // namespace

// ---------------------------------------------------------------------------
// Violations
// ---------------------------------------------------------------------------
std::string describe(const std::vector<Violation>& violations) {
    std::string text;
    for (const Violation& violation : violations) {
        if (!text.empty()) {
            text += "; ";
        }
        text += std::format("{} ({}): {}", violation.instance_path.empty() ? "/" : violation.instance_path,
                            violation.keyword, violation.message);
    }
    return text;
}

// ---------------------------------------------------------------------------
// Schema
// ---------------------------------------------------------------------------
struct Schema::Impl {
    Json document;
    jsch::json_schema<jsoncons::json> compiled;
};

Schema::Schema(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
Schema::Schema(Schema&&) noexcept = default;
Schema& Schema::operator=(Schema&&) noexcept = default;
Schema::~Schema() = default;

Result<Schema> Schema::compile(const Json& document) {
    try {
        auto compiled = jsch::make_json_schema(to_jsoncons(document));
        return Schema{std::make_unique<Impl>(Impl{.document = document, .compiled = std::move(compiled)})};
    } catch (const std::exception& error) {
        return fail(Errc::invalid_argument, std::format("invalid JSON Schema: {}", error.what()));
    }
}

std::vector<Violation> Schema::check(const Json& instance) const {
    std::vector<Violation> violations;
    const jsoncons::json converted = to_jsoncons(instance);
    impl_->compiled.validate(converted, [&](const jsch::validation_message& message) -> jsch::walk_state {
        violations.push_back(Violation{
            .instance_path = message.instance_location().string(),
            .keyword = message.keyword(),
            .message = message.message(),
        });
        return jsch::walk_state::advance;
    });
    return violations;
}

Result<void> Schema::validate(const Json& instance) const {
    const std::vector<Violation> violations = check(instance);
    if (violations.empty()) {
        return ok();
    }
    return fail(Errc::invalid_argument,
                std::format("{} schema violation(s): {}", violations.size(), describe(violations)));
}

const Json& Schema::document() const noexcept {
    return impl_->document;
}

// ---------------------------------------------------------------------------
// SchemaRegistry
// ---------------------------------------------------------------------------
Result<void> SchemaRegistry::add(std::string name, const Json& document) {
    if (schemas_.contains(name)) {
        return fail(Errc::conflict, std::format("schema '{}' is already registered", name));
    }
    auto compiled = Schema::compile(document);
    if (!compiled) {
        return fail(
            std::move(compiled).error().with_context(std::format("while compiling schema '{}'", name)));
    }
    schemas_.emplace(std::move(name), std::move(*compiled));
    return ok();
}

Result<void> SchemaRegistry::add_text(std::string name, std::string_view json_text) {
    auto document = parse(json_text);
    if (!document) {
        return fail(std::move(document).error().with_context(std::format("while parsing schema '{}'", name)));
    }
    return add(std::move(name), *document);
}

const Schema* SchemaRegistry::find(std::string_view name) const noexcept {
    const auto it = schemas_.find(name);
    return it == schemas_.end() ? nullptr : &it->second;
}

Result<void> SchemaRegistry::validate(std::string_view name, const Json& instance) const {
    const Schema* schema = find(name);
    if (schema == nullptr) {
        return fail(Errc::not_found, std::format("no schema named '{}'", name));
    }
    return schema->validate(instance);
}

std::vector<std::string> SchemaRegistry::names() const {
    std::vector<std::string> result;
    result.reserve(schemas_.size());
    for (const auto& [name, schema] : schemas_) {
        result.push_back(name);
    }
    return result;
}

// ---------------------------------------------------------------------------
// Portable subset
// ---------------------------------------------------------------------------
namespace {

// Rule: only keywords from the known lists.
void check_keywords(const Json& node, const std::string& location, std::vector<std::string>& problems) {
    for (const auto& [keyword, value] : node.items()) {
        if (!contains(kStructuralKeywords, keyword) && !contains(kValidationOnlyKeywords, keyword)) {
            problems.push_back(
                std::format("{}: keyword '{}' is not in the portable subset", location, keyword));
        }
    }
}

// Rules: closed objects, and every property required.
void check_object(const Json& node, const std::string& location, std::vector<std::string>& problems) {
    if (!is_object_schema(node)) {
        return;
    }
    const auto additional = node.find("additionalProperties");
    if (additional == node.end() || *additional != Json(false)) {
        problems.push_back(std::format("{}: objects must set \"additionalProperties\": false", location));
    }
    const auto properties = node.find("properties");
    if (properties == node.end()) {
        return;
    }
    const auto required = node.find("required");
    const bool has_required = required != node.end() && required->is_array();
    for (const auto& [name, subschema] : properties->items()) {
        if (!has_required || std::ranges::find(*required, Json(name)) == required->end()) {
            problems.push_back(std::format(
                "{}: property '{}' must be listed in \"required\" (use a null type for optional values)",
                location, name));
        }
    }
}

// Rule: references stay inside the document's $defs.
void check_ref(const Json& node, const std::string& location, std::vector<std::string>& problems) {
    const auto ref = node.find("$ref");
    if (ref != node.end() &&
        (!ref->is_string() || !ref->get_ref<const std::string&>().starts_with("#/$defs/"))) {
        problems.push_back(std::format("{}: $ref may only point into #/$defs/", location));
    }
}

// Rule: no recursion. Builds the reference graph between $defs and reports
// every definition that can reach itself.
void check_recursion(const Json& schema, std::vector<std::string>& problems) {
    const auto defs = schema.find("$defs");
    if (defs == schema.end() || !defs->is_object()) {
        return;
    }
    std::map<std::string, std::set<std::string>> edges;
    for (const auto& [name, definition] : defs->items()) {
        collect_refs(definition, edges[name]);
    }
    for (const auto& [start, direct] : edges) {
        std::vector<std::string> stack(direct.begin(), direct.end());
        std::set<std::string> seen;
        while (!stack.empty()) {
            const std::string current = stack.back();
            stack.pop_back();
            if (current == start) {
                problems.push_back(std::format("#/$defs/{}: recursive definitions are not portable", start));
                break;
            }
            if (seen.insert(current).second) {
                stack.insert(stack.end(), edges[current].begin(), edges[current].end());
            }
        }
    }
}

}  // namespace

std::vector<std::string> lint_portable(const Json& schema) {
    std::vector<std::string> problems;
    if (!schema.is_object()) {
        problems.emplace_back("the schema must be a JSON object");
        return problems;
    }
    for_each_subschema(schema, "#", [&](const Json& node, const std::string& location) {
        check_keywords(node, location, problems);
        check_object(node, location, problems);
        check_ref(node, location, problems);
    });
    check_recursion(schema, problems);
    return problems;
}

Json to_model_facing(const Json& schema) {
    if (schema.is_array()) {
        Json result = Json::array();
        for (const Json& element : schema) {
            result.push_back(to_model_facing(element));
        }
        return result;
    }
    if (!schema.is_object()) {
        return schema;
    }
    Json result = Json::object();
    for (const auto& [key, value] : schema.items()) {
        if (key == "properties" || key == "$defs") {
            // Values are schemas keyed by *names*; recurse into each value only.
            Json mapped = Json::object();
            for (const auto& [name, subschema] : value.items()) {
                mapped[name] = to_model_facing(subschema);
            }
            result[key] = std::move(mapped);
        } else if (key == "items" || key == "anyOf" || key == "allOf") {
            result[key] = to_model_facing(value);
        } else if (!contains(kValidationOnlyKeywords, key)) {
            result[key] = value;
        }
    }
    return result;
}

}  // namespace rr::json
