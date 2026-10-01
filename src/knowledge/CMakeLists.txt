# rr_knowledge — the knowledge model as the stages write it (ADR-0004,
# ADR-0005, ADR-0006): the short IDs S#, A#, V#; the transcript; Ledger A, the
# atoms and fillers every sentence became; the article and its [A#] markers,
# read by a parser that is fuzzed. Each artifact has a strict JSON schema
# (schemas/artifacts/, compiled in) that reading it enforces
# (docs/design/knowledge.md). Hub only.
rr_add_embedded_library(artifact_schemas
    NAMESPACE rr::embedded::artifact_schemas
    FILES
        schemas/artifacts/article.json
        schemas/artifacts/atoms.json
        schemas/artifacts/transcript.json)

rr_add_module(knowledge
    SOURCES
        article.cpp
        artifacts.cpp
        atoms.cpp
        transcript.cpp
    PUBLIC_DEPS
        rr::core
        rr::json
        rr::text
    PRIVATE_DEPS
        rr::embedded_artifact_schemas)
