# rr_pipeline — a job's stages, run or found done, and kept (ADR-0004): the
# Stage interface and its cache key, the Executor that commits each stage's
# artifact with its cache entry and the job's record in one transaction, and
# the stages: `source` (the video, or a caption file, read once),
# `transcript` (captions into numbered sentences, punctuated by the infantry
# when they are not), `outline` (the video's sections and its brief) and
# `atomize` (Ledger A: what became of every sentence); the windows a
# transcript is read in, each as large as its engine holds; and run_job(),
# a job taken through them and saved (docs/design/pipeline.md). Hub only.
rr_add_module(pipeline
    SOURCES
        asking.cpp
        atomize_stage.cpp
        executor.cpp
        job_run.cpp
        outline.cpp
        outline_stage.cpp
        punctuation.cpp
        readings.cpp
        source.cpp
        transcript_stage.cpp
        windows.cpp
    PUBLIC_DEPS
        rr::agents
        rr::core
        rr::db
        rr::json
        rr::knowledge
        rr::library
        rr::media
        rr::prompt
        rr::reason
        rr::transcript
    PRIVATE_DEPS
        rr::fidelity
        rr::llm
        rr::log
        rr::text)
