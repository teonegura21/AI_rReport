# rr_eval — the evaluation's data (ADR-0019, docs/design/eval.md): the sets of
# videos and how each may be used; snapshots of their captions, pinned by hash
# and read through the pipeline's own source stage; gold annotations anchored
# to the timeline, drafted by models as silver and corrected by a person; the
# review sheet a person corrects them with. Hub only.
rr_add_module(eval
    SOURCES
        annotate.cpp
        gold.cpp
        set.cpp
        sheet.cpp
        snapshot.cpp
    PUBLIC_DEPS
        rr::agents
        rr::core
        rr::json
        rr::knowledge
        rr::library
        rr::media
        rr::pipeline
    PRIVATE_DEPS
        rr::llm
        rr::prompt
        rr::text
        rr::transcript
        tomlplusplus::tomlplusplus)
