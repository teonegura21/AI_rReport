# rr_library — a library's database (ADR-0008): its schema (migrations/,
# compiled in) and the repositories over it: the model-call log, a job's memo
# of its calls (ADR-0020), the battle log of its missions (ADR-0011), and the
# pipeline's record: jobs, artifacts, the stage cache (ADR-0004); the eval's
# runs, their scores and their judgements (ADR-0019).
# Hub only.
rr_add_embedded_library(library_migrations
    NAMESPACE rr::embedded::library_migrations
    FILES
        migrations/0001_accounting.sql
        migrations/0002_call_memo.sql
        migrations/0003_missions.sql
        migrations/0004_pipeline.sql
        migrations/0005_job_kind.sql
        migrations/0006_eval.sql
        migrations/0007_judgement.sql)

rr_add_module(library
    SOURCES
        call_log.cpp
        call_memo.cpp
        jobs.cpp
        library.cpp
        mission_log.cpp
        stage_store.cpp
    PUBLIC_DEPS
        rr::agents
        rr::core
        rr::db
        rr::json
        rr::llm
    PRIVATE_DEPS
        rr::embedded_library_migrations)
