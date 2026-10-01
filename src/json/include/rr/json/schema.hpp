// rr/json/schema.hpp — JSON Schema validation and the "portable subset".
//
// Why this exists
//   Every model output and every artifact is validated against a JSON Schema
//   before it is stored (plan §8). One schema serves two jobs:
//     * constraining generation: llama-server compiles it into a grammar (as
//       vLLM and SGLang would);
//     * validating the result afterwards, with the *full* rules.
//
//   The decoder and the validator support different keyword sets, so schemas
//   are written in a **portable subset**, checked by lint_portable():
//     * every object has "additionalProperties": false;
//     * every property is listed in "required" (optional values use a null
//       type instead, e.g. {"type": ["string", "null"]});
//     * no recursion: $ref may only point into #/$defs, and $defs may not
//       reference themselves, directly or through each other (trees are flat
//       lists with parent ids);
//     * only keywords from a known list.
//   Keywords that constrained decoders do not all support (minLength,
//   maximum, pattern, ...) are allowed in the schema but removed by
//   to_model_facing(). They are still enforced by validation after
//   generation.
//
//   **Field order is part of the prompt.** A constrained decoder writes an
//   object's properties in the schema's order: llama.cpp's grammar builder
//   walks them as it reads them. A schema that lists "evidence" before
//   "verdict" makes the model quote before it decides; the reverse makes it
//   decide first and justify afterwards. Json sorts object members, which
//   would silently put every schema's properties in alphabetical order. So a
//   schema keeps the order it was written in (compile_text(),
//   compile_ordered()), and model_facing() hands that order to the model.
//
// The validator (jsoncons, draft 2020-12) is hidden behind a pimpl, so only
// schema.cpp pays its compile-time cost.
//
// Thread-safety: a compiled Schema is immutable and safe to share across
//   threads. SchemaRegistry is not synchronized: populate it at startup,
//   then share it read-only.
#pragma once

#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <rr/core/result.hpp>
#include <rr/json/json.hpp>

namespace rr::json {

// One reason an instance failed validation.
struct Violation {
    std::string instance_path;  // JSON Pointer into the instance, e.g. "/atoms/3/type"
    std::string keyword;        // the schema keyword that failed, e.g. "enum"
    std::string message;        // human-readable explanation

    friend bool operator==(const Violation&, const Violation&) = default;
};

// Formats violations for an error message or a model repair prompt:
//   "/atoms/3/type (enum): must be one of ...; / (required): ..."
[[nodiscard]] std::string describe(const std::vector<Violation>& violations);

class Schema {
public:
    // Compiles a schema written as text (draft 2020-12), the usual way to load
    // one. Every object keeps its members' order, so model_facing() lists the
    // properties as the author wrote them. Errc::invalid_argument if the text
    // is not JSON, or not a valid schema.
    [[nodiscard]] static Result<Schema> compile_text(std::string_view text);

    // Compiles a schema built in code, keeping its members' order.
    // Errc::invalid_argument if it is not a valid schema.
    [[nodiscard]] static Result<Schema> compile_ordered(const OrderedJson& document);

    // Compiles a canonical document. Its objects are sorted, so model_facing()
    // lists properties alphabetically: fine for a schema that only validates,
    // wrong for one whose field order should guide a model.
    // Errc::invalid_argument if it is not a valid schema.
    [[nodiscard]] static Result<Schema> compile(const Json& document);

    Schema(Schema&&) noexcept;
    Schema& operator=(Schema&&) noexcept;
    Schema(const Schema&) = delete;
    Schema& operator=(const Schema&) = delete;
    ~Schema();

    // All violations; empty means valid.
    [[nodiscard]] std::vector<Violation> check(const Json& instance) const;

    // Errc::invalid_argument listing every violation; ok() if valid.
    [[nodiscard]] Result<void> validate(const Json& instance) const;

    // The document, canonical (members sorted).
    [[nodiscard]] const Json& document() const noexcept;

    // What constrained decoding gets: to_model_facing() of the document, as
    // compact JSON text with every object's members in their authored order.
    // Computed once, when the schema is compiled.
    [[nodiscard]] const std::string& model_facing() const noexcept;

private:
    struct Impl;
    explicit Schema(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;
};

// Named, compiled schemas (e.g. "llm/atomize", "artifacts/article").
class SchemaRegistry {
public:
    // Errc::conflict if `name` is already registered. add_text() keeps the
    // text's member order (Schema::compile_text()); add() takes a canonical
    // document (Schema::compile()).
    [[nodiscard]] Result<void> add(std::string name, const Json& document);
    [[nodiscard]] Result<void> add_text(std::string name, std::string_view json_text);

    [[nodiscard]] const Schema* find(std::string_view name) const noexcept;

    // Errc::not_found for an unknown schema name.
    [[nodiscard]] Result<void> validate(std::string_view name, const Json& instance) const;

    [[nodiscard]] std::vector<std::string> names() const;

private:
    [[nodiscard]] Result<void> add_compiled(std::string name, Result<Schema> compiled);

    std::map<std::string, Schema, std::less<>> schemas_;
};

// Problems that make a schema non-portable (empty = portable). See the header comment.
[[nodiscard]] std::vector<std::string> lint_portable(const Json& schema);

// A copy of `schema` without keywords that constrained decoders do not
// reliably support. Use it for generation, never for validation. Every object
// keeps its members in the input's order (sorted, for a Json).
[[nodiscard]] Json to_model_facing(const Json& schema);
[[nodiscard]] OrderedJson to_model_facing(const OrderedJson& schema);

}  // namespace rr::json
