# rr_library — a library's database (ADR-0008): its schema (migrations/,
# compiled in) and the repositories over it: the model-call log, and a job's
# memo of its calls (ADR-0020). Hub only.
rr_add_embedded_library(library_migrations
    NAMESPACE rr::embedded::library_migrations
    FILES
        migrations/0001_accounting.sql
        migrations/0002_call_memo.sql)

rr_add_module(library
    SOURCES
        call_log.cpp
        call_memo.cpp
        library.cpp
    PUBLIC_DEPS
        rr::core
        rr::db
        rr::json
        rr::llm
    PRIVATE_DEPS
        rr::embedded_library_migrations)
