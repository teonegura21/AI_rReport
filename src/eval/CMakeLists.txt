# rr_eval — the evaluation's data (ADR-0019, docs/design/eval.md): the sets of
# videos and how each may be used; snapshots of their captions, pinned by hash
# and read through the pipeline's own source stage; gold annotations anchored
# to the timeline, drafted by models as silver and corrected by a person; the
# review sheet a person corrects them with; runs of the set's videos through
# the article stages, scored by code and exported. Hub only.
rr_add_module(eval
    SOURCES
        annotate.cpp
        export.cpp
        gold.cpp
        run.cpp
        score.cpp
        set.cpp
        sheet.cpp
        snapshot.cpp
    PUBLIC_DEPS
        rr::agents
        rr::core
        rr::db
        rr::json
        rr::knowledge
        rr::library
        rr::media
        rr::pipeline
        rr::report
    PRIVATE_DEPS
        rr::llm
        rr::prompt
        rr::text
        rr::transcript
        tomlplusplus::tomlplusplus)
