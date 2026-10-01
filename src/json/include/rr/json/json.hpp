// rr/json/json.hpp — JSON as rReport's interchange format.
//
// Why this exists
//   Everything that crosses a process boundary is JSON: model requests and
//   responses, artifacts, the hub API, the runtime protocol. This header fixes
//   one representation (nlohmann::json) and three properties the rest of the
//   system relies on:
//
//   1. Parsing never throws. It returns Result<Json> with the byte offset of
//      the problem (ADR-0007).
//   2. Serialization is canonical. Object keys are always sorted (nlohmann's
//      default object type is a std::map) and there is no whitespace, so equal
//      documents produce identical bytes. Prompt caching and stage cache keys
//      depend on this (ADR-0004). The one exception is OrderedJson, below,
//      for the documents whose member order means something.
//   3. Serialization never throws on invalid UTF-8. Model output can contain
//      broken byte sequences; they are replaced with U+FFFD instead of
//      aborting a whole stage.
//
// Thread-safety: free functions; Json values follow standard container rules.
#pragma once

#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include <rr/core/hash.hpp>
#include <rr/core/result.hpp>

namespace rr::json {

using Json = nlohmann::json;

// JSON whose objects keep their members in the order they were written.
//
// Member order is not part of JSON's data model, and everything that is
// hashed, compared or stored uses the canonical Json above. OrderedJson is for
// the one place where order carries meaning: a schema sent to a model.
// llama.cpp builds its grammar from a schema's properties in the order it
// reads them, so the model writes its answer's fields in that order, and
// "quote the evidence, then decide" happens only if the evidence comes first
// (json::Schema::model_facing()).
//
// The two convert into each other implicitly: to Json sorts the members; from
// Json, they come in sorted order.
using OrderedJson = nlohmann::ordered_json;

// Parses one JSON document. Trailing non-whitespace is an error.
[[nodiscard]] Result<Json> parse(std::string_view text);

// Parses like parse(), keeping every object's members in document order.
[[nodiscard]] Result<OrderedJson> parse_ordered(std::string_view text);

// Canonical compact form: sorted keys, no whitespace, UTF-8 (invalid
// sequences replaced with U+FFFD).
[[nodiscard]] std::string dump(const Json& value);

// Compact form with every object's members in the value's own order. Not
// canonical: equal objects written in different orders give different text,
// which is the point. Invalid UTF-8 is replaced, as above.
[[nodiscard]] std::string dump(const OrderedJson& value);

// Human-readable form for files people read (reports, configs). Not canonical
// in whitespace; never use it for hashing.
[[nodiscard]] std::string dump_pretty(const Json& value, int indent = 2);

// BLAKE3 of the canonical form. Equal JSON values have equal hashes.
[[nodiscard]] Blake3Digest content_hash(const Json& value);

// Typed field access with a clear error ("field 'usage.prompt_tokens' is
// missing" rather than an exception from deep inside nlohmann).
// `path` uses dots for nesting: "usage.prompt_tokens".
[[nodiscard]] Result<const Json*> find(const Json& document, std::string_view path);
[[nodiscard]] Result<std::string> get_string(const Json& document, std::string_view path);
[[nodiscard]] Result<std::int64_t> get_int(const Json& document, std::string_view path);
[[nodiscard]] Result<double> get_number(const Json& document, std::string_view path);
[[nodiscard]] Result<bool> get_bool(const Json& document, std::string_view path);

}  // namespace rr::json
