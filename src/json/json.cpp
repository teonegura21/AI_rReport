#include <rr/json/json.hpp>

#include <format>

#include <rr/core/strings.hpp>

namespace rr::json {

namespace {

// nlohmann reports errors by throwing; this adapter is the one place where
// that exception is caught and turned into a Result (ADR-0007). Both document
// types throw the same parse_error.
template <class Document>
Result<Document> parse_as(std::string_view text) {
    try {
        return Document::parse(text);
    } catch (const Json::parse_error& error) {
        return fail(Errc::invalid_argument,
                    std::format("invalid JSON at byte {}: {}", error.byte, error.what()));
    }
}

template <class Document>
std::string compact(const Document& value) {
    return value.dump(-1, ' ', /*ensure_ascii=*/false, Document::error_handler_t::replace);
}

}  // namespace

Result<Json> parse(std::string_view text) {
    return parse_as<Json>(text);
}

Result<OrderedJson> parse_ordered(std::string_view text) {
    return parse_as<OrderedJson>(text);
}

std::string dump(const Json& value) {
    return compact(value);
}

std::string dump(const OrderedJson& value) {
    return compact(value);
}

std::string dump_pretty(const Json& value, int indent) {
    return value.dump(indent, ' ', /*ensure_ascii=*/false, Json::error_handler_t::replace);
}

Blake3Digest content_hash(const Json& value) {
    return blake3(dump(value));
}

Result<const Json*> find(const Json& document, std::string_view path) {
    const Json* node = &document;
    for (const std::string_view key : split(path, '.')) {
        if (!node->is_object()) {
            return fail(Errc::invalid_argument,
                        std::format("field '{}' is missing (parent is not an object)", path));
        }
        const auto it = node->find(key);
        if (it == node->end()) {
            return fail(Errc::invalid_argument, std::format("field '{}' is missing", path));
        }
        node = &*it;
    }
    return node;
}

namespace {

template <class T, class Check>
Result<T> get_typed(const Json& document, std::string_view path, std::string_view type_name, Check is_type) {
    RR_TRY_ASSIGN(const Json* node, find(document, path));
    if (!is_type(*node)) {
        return fail(Errc::invalid_argument,
                    std::format("field '{}' must be {}, got {}", path, type_name, node->type_name()));
    }
    return node->get<T>();
}

}  // namespace

Result<std::string> get_string(const Json& document, std::string_view path) {
    return get_typed<std::string>(document, path, "a string",
                                  [](const Json& node) { return node.is_string(); });
}

Result<std::int64_t> get_int(const Json& document, std::string_view path) {
    return get_typed<std::int64_t>(document, path, "an integer",
                                   [](const Json& node) { return node.is_number_integer(); });
}

Result<double> get_number(const Json& document, std::string_view path) {
    return get_typed<double>(document, path, "a number", [](const Json& node) { return node.is_number(); });
}

Result<bool> get_bool(const Json& document, std::string_view path) {
    return get_typed<bool>(document, path, "a boolean", [](const Json& node) { return node.is_boolean(); });
}

}  // namespace rr::json
