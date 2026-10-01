// rr/knowledge/artifacts.hpp — the schemas of the artifacts the stages write.
//
// Why this exists
//   Every stage writes one immutable, schema-validated JSON artifact, and the
//   artifact is the truth: SQL tables only project it (ADR-0004). Its schema
//   is part of the build (schemas/artifacts/*.json, compiled in), so a binary
//   never reads an artifact by a schema other than its own. Each schema's text
//   will enter the stage cache key (plan P15): a changed schema is never
//   served an artifact written under the old one.
//
//   Reading an artifact (transcript_from_json(), atom_ledger_from_json(),
//   article_from_json()) validates it against its schema first, then checks
//   what a schema cannot say, such as IDs that run S1, S2, … in order.
//
// Thread-safety: the registry is built once, on first use, and only read.
#pragma once

#include <rr/json/schema.hpp>

namespace rr::knowledge {

// The artifact schemas, compiled in: "artifacts/source" (written by the
// pipeline's source stage, rr/pipeline/source.hpp), "artifacts/transcript",
// "artifacts/atoms" and "artifacts/article".
[[nodiscard]] const json::SchemaRegistry& artifact_schemas();

}  // namespace rr::knowledge
