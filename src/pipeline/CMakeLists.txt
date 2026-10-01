# rr_pipeline — a job's stages, run or found done, and kept (ADR-0004): the
# Stage interface and its cache key, the Executor that commits each stage's
# artifact with its cache entry and the job's record in one transaction, and
# the first stages: `source` (the video, or a caption file, read once) and
# `transcript` (captions into numbered sentences, punctuated by the infantry
# when they are not); and run_job(), a job taken through them and saved
# (docs/design/pipeline.md). Hub only.
rr_add_module(pipeline
    SOURCES
        executor.cpp
        job_run.cpp
        punctuation.cpp
        source.cpp
        transcript_stage.cpp
    PUBLIC_DEPS
        rr::agents
        rr::core
        rr::db
        rr::json
        rr::library
        rr::media
        rr::prompt
        rr::transcript
    PRIVATE_DEPS
        rr::knowledge
        rr::llm
        rr::log)
